#include "artnetgen/artNetGen.h"
#include "netlist.h"
#include "bin.h"
#include "node.h"

#include <stdio.h>
#include <algorithm>
#include <math.h>
#include <limits.h>
#include <cfloat>
#include <queue>
#include "opendb/db.h"
#include <chrono> //mk
#include <omp.h> //mk
#include <tuple>

namespace artnetgen {
using std::cout;
using std::endl;
using std::vector;
using std::max;
using std::min;
using std::string;
using std::find;
using std::unordered_map;
using std::queue;
using std::make_pair;
using std::ceil;
using std::to_string;

using namespace odb;
/* VERSION 1
1. Determine bbox of each net from Bbox dist.
2. get a sample k from Fanout dist. -> Determine # of candidate sink
3. Phase1: determine outer edge 
        - determine src-sink pair usign fanoutGain(fo, fo+k) faninGain(fi, fi+1) bboxGain( bbox1, bbox2 )
        - in this stage, faninGain & bboxGain are determined 
   Phase2: determine inner edge
        - there are k-1 edges left, and source node is determined
        - cand. sinks are located in the bbox area -> limit search space
        - calculate faninGain(fi, fi+1) for each cand. sink. And select top k-1 cand. 
*/
/* VERSION 2
1. get a sample k from Fanout dist. -> Determine # of candidate sink
2. get k samples S from edge dist.
3. Phase1: determine outer edge
        - determine src-sink pair usign fanoutGain(fo, fo+k) faninGain(fi, fi+1) bboxGain( bbox1, bbox2 )
        - in this stage, faninGain & bboxGain are determined 
   Phase2: determine inner edge
        - there are k-1 edges left, and source node is determined
        - cand. sinks are located in the bbox area -> limit search space
        - for each k-1 sample, determine sink using faninGain(fi, fi+1)
*/
void
Netlist::distMatching() {
    using Clock = std::chrono::system_clock;
    using Duration = std::chrono::duration<double>;
    auto distStart = Clock::now();


    int tarBboxCnt = bboxDist_.totalCnt();
    int curBboxCnt = 0;

    vector<Bin*> srcBins = bins_;
    // Coonect a net for each loop
    while ( tarBboxCnt > curBboxCnt ){
        // 1. Determine bbox of each net from Bbox dist.
        int bboxOfNet = bboxDist_.sample();
        // 2. get a sample k(fanout) from Fanout dist. -> Determine # of candidate sink
        int fanout = foDist_.sample();

        // 3. Phase1: determine outer edge
        //  - determine a src-sink pair usign fanoutGain(fo, fo+k) faninGain(fi, fi+1) bboxGain( bbox1, bbox2 )
        //  - in this stage, faninGain & bboxGain are determined
        Gain maxG(-INT_MAX, nullptr, nullptr);
        std::random_shuffle(srcBins.begin(), srcBins.end());
        // Iterate source-sink bins (distance(source, sink) == edgeLength)
        for (Bin* srcBin : srcBins) {
            vector<Bin*> sinkBins;
            for (int level = 1; level <= srcBin->getPath().size(); level++) {
                sinkBins = getSinkBins_outer(srcBin, bboxOfNet, level, true);
                if (!sinkBins.empty()) break;
            }
            for(Bin* sinkBin : sinkBins) {
                Gain localMaxG = getMaxGain_outer(srcBin, sinkBin, fanout);
                if(maxG.value() < localMaxG.value()) {maxG = localMaxG;}
            }
        }
        if( maxG.value() == -INT_MAX ) {}
        else {            
            Node* srcNode = maxG.n1();
            Node* sinkNode = maxG.n2();
            connect(srcNode, sinkNode);
        }

        //3. Phase2: determine inner edge
        //  - there are k-1 edges left, and source node is determined
        //  - cand. sinks are located in the bbox area -> limit search space
        //  - for each k-1 sample, determine sink using faninGain(fi, fi+1)
        Node* srcNode = maxG.n1();
        Node* sinkNode_outer = maxG.n2();
        vector<Bin*> sinkBins;
        sinkBins = getSinkBins_inner(srcNode, sinkNode_outer, true);

        std::vector<std::tuple<Node*, double>> nodeGainList;
        for(Bin* sinkBin : sinkBins) {
            for(int fi = 0; fi < fiDist_.xMax(); fi++) {
                vector<Node*> candiSinks = sinkBin->fi2Nodes(fi);
                if(candiSinks.size() == 0) continue;
                double fiG = faninGain(fi, fi+1);
                for(Node* candiSink : candiSinks) {
                    if(candiSink->getType() == NodeType::PrimaryIn) {continue;} 
                    else if((candiSink->getType() == NodeType::PrimaryOut) && candiSink->numFanins() >= 1) {continue;} 
                    else if( candiSink <= srcNode || srcNode->hasConnection(candiSink) ) {continue;}
                    nodeGainList.push_back(std::make_tuple(candiSink, fiG));
                }
            }
        }
        std::sort(nodeGainList.begin(), nodeGainList.end(),
            [](const std::tuple<Node*, double>& a, const std::tuple<Node*, double>& b) {
                return std::get<1>(a) > std::get<1>(b);
            });
        std::vector<Node*> candidateSinkNode;
        int k = fanout - 1;
        for (int i = 0; i < std::min(k, (int)nodeGainList.size()); ++i) {
            Node* sinkNode = std::get<0>(nodeGainList[i]);
            candidateSinkNode.push_back(sinkNode);
        }
        // 3. connect 호출
        for (Node* sinkNode : candidateSinkNode) {
            connect(srcNode, sinkNode);
        }
        curBboxCnt++;
        if(curBboxCnt % 1000 == 0 || curBboxCnt == tarBboxCnt) { 
            double progress = 1.0 * curBboxCnt / tarBboxCnt;
            printf("distribution matching progress... [%2.2f\%]\n", 100* progress); 
        }
    }

    auto distEnd = Clock::now();
    Duration totalRuntime = distEnd - distStart;
    std::cout << "[TIME] distMatching total     : " << totalRuntime.count()    << " sec\n";
    print();
    exit(0);
}

// Source Bin과 Sink Bin에 속한 Node중 가장 큰 Gain을 가지는 Node쌍을 반환
Gain Netlist::getMaxGain_outer(Bin* srcBin, Bin* sinkBin, int fanout) {
    double maxG = -INT_MAX;
    Node *srcNode, *sinkNode;

    // GAIN1 : Module
    double moduleG = 0.0;
    if (srcBin->getSubmoduleId() == sinkBin->getSubmoduleId()) moduleG = 10.0;
    else if (srcBin->getModuleId() == sinkBin->getModuleId()) moduleG = 5.0;

    for(int fo=0; fo < foDist_.xMax(); fo++) {
        vector<Node*> candiSrcs = srcBin->fo2Nodes(fo);
        if(candiSrcs.size() == 0) {continue;}

        // GAIN2 : Fanout
        if(fo + fanout > foDist_.xMax()) {continue;}
        double foG = fanoutGain(fo, fo+fanout);
        
        // srcBin에 속한 node중 sinkBin과 연결됬을때, netBbox gain이 가장큰 src node를 반환
        std::random_shuffle(candiSrcs.begin(), candiSrcs.end());

        double bboxG = -INT_MAX;
        Node* localMaxSrcNode;
        for(Node* candiSrc : candiSrcs) {
            if(candiSrc->getType() == NodeType::PrimaryOut) {continue;}

            int lx, ly, ux, uy;
            int x, y;
            lx = candiSrc->lx(), ly = candiSrc->ly();
            ux = candiSrc->ux(), uy = candiSrc->uy();
            x = sinkBin->x(), y = sinkBin->y();
          
            int bbox1 = (ux - lx) + (uy - ly);
            int bbox2 = (max(ux, x) - min(lx, x)) + (max(uy, y) - min(ly, y));
            double localBboxG = 0;
            // GAIN3 : BBOX 
            if( bbox1 < bbox2 ) { localBboxG = bboxGain( bbox1, bbox2 ); }
            if( bboxG < localBboxG ) { bboxG = localBboxG; localMaxSrcNode = candiSrc; } 
        }
        if( bboxG == -INT_MAX ) continue;

        // 여기까지 srcBin에 속한 Node중 fanout, bbox Gain의 기댓값이 가장큰 srcNode를 찾음.
        // 이제부터 sinkBin에 속한 Node중 fanin Gain값이 가장큰 sinkNode를 찾아야됨.
        for(int fi = 0; fi < fiDist_.xMax(); fi++) {
            vector<Node*> candiSinks = sinkBin->fi2Nodes(fi);
            if(candiSinks.size() == 0) continue;

            // GAIN4: Fanout
            double fiG = faninGain(fi, fi+fanout);
            
            double totG = 1.0 * fiG + 1.0 * foG + 1.0 * bboxG;

            if( totG > maxG ) {
                std::random_shuffle(candiSinks.begin(), candiSinks.end());
                for(Node* candiSink : candiSinks) {
                    // exception case handling
                    if(candiSink->getType() == NodeType::PrimaryIn) {continue;} 
                    else if((candiSink->getType() == NodeType::PrimaryOut) && candiSink->numFanins() >= 1) {continue;} 
                    else { 
                        if( srcBin == sinkBin && candiSink <= localMaxSrcNode ) {continue;}
                        if( localMaxSrcNode->hasConnection(candiSink) ) {continue;}
                    } 
                    srcNode = localMaxSrcNode;
                    sinkNode = candiSink;
                    maxG = totG;
                }
            }
        }
    }
    if(maxG == -INT_MAX) {} else {}
    return Gain(maxG, srcNode, sinkNode);
}


// 현재 Source Bin과 Distance가 edgeLength인 sinkBin 리스트를 반환
vector<Bin*> Netlist::getSinkBins_outer(Bin* srcBin, int edgeLength, int level, bool shuffle) {
    vector<Bin*> sinkBins;
    const vector<int>& srcPath = srcBin->getPath();

    for(int dx=0; dx <= edgeLength; dx++) {
        int dy = edgeLength - dx;
        int x = srcBin->x() + dx;
        int y = srcBin->y() + dy;

        if (x >= layoutDimX() || y >= layoutDimY()) continue;
        
        Bin* sinkBin = getBin(x, y);
        const vector<int>& sinkPath = sinkBin->getPath();
        // 특정 level에 대해서 같은 group이면 sinkBin candidate로 추가
        if (srcPath[srcPath.size() - level] == sinkPath[sinkPath.size() - level]) {
            sinkBins.push_back(sinkBin);
        }
    }
    if( shuffle && sinkBins.size() > 1 )
        std::random_shuffle(sinkBins.begin(), sinkBins.end());

    return sinkBins;
}


vector<Bin*> Netlist::getSinkBins_inner(Node* srcNode, Node* sinkNode, bool shuffle) {
    vector<Bin*> sinkBins;
    int x1 = srcNode->getBin()->x(), y1 = srcNode->getBin()->y();
    int x2 = sinkNode->getBin()->x(), y2 = sinkNode->getBin()->y();


    int minX = std::min(x1, x2);
    int maxX = std::max(x1, x2);
    int minY = std::min(y1, y2);
    int maxY = std::max(y1, y2);
    
    for (int x = minX; x <= maxX; ++x) {
        for (int y = minY; y <= maxY; ++y) {
            Bin* sinkBin = getBin(x, y);
            sinkBins.push_back(sinkBin);
        }
    }

    if( shuffle && sinkBins.size() > 1 )
        std::random_shuffle(sinkBins.begin(), sinkBins.end());

    return sinkBins;
}

// Bbox gain function
double Netlist::bboxGain(int bbox1, int bbox2) {
    double currErr = 1.0 * abs(bboxDist_.delta(bbox1)) + 1.0 * abs(bboxDist_.delta(bbox2));
    double nextErr = 1.0 * abs(bboxDist_.delta(bbox1)-1) + 1.0 * abs(bboxDist_.delta(bbox2)+1);
    double deltaErr = currErr - nextErr;
    return deltaErr;
}

// Fanout gain function
double Netlist::fanoutGain(int fo1, int fo2) {
    double currErr = 1.0 * abs(foDist_.delta(fo1)) + 1.0 * abs(foDist_.delta(fo2));
    double nextErr = 1.0 * abs(foDist_.delta(fo1)-1) + 1.0 * abs(foDist_.delta(fo2)+1);
    double deltaErr = currErr - nextErr;
    //double reward = (fo1 == 0) ? 16 : 0; 
    double reward = 0;
    double g = deltaErr + reward;
    return g;
}

// Fanin gain function
double Netlist::faninGain(int fi1, int fi2) {
    double currErr = 1.0 * abs(fiDist_.delta(fi1)) + 1.0 * abs(fiDist_.delta(fi2));
    double nextErr = 1.0 * abs(fiDist_.delta(fi1)-1) + 1.0 * abs(fiDist_.delta(fi2)+1);
    double deltaErr = currErr - nextErr;
    //double reward = (fi1 == 0) ? 16 : 0; 
    double reward = 0;
    double g = deltaErr + reward;
    return g;
}
}
