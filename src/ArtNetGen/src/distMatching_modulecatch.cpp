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

void
Netlist::distMatching() {
    using Clock = std::chrono::system_clock;
    using Duration = std::chrono::duration<double>;
    auto distStart = Clock::now();
    double totalSinkBinTime = 0.0;
    double totalMaxGainTime = 0.0;

    int tarEdgeCnt = edgeDist_.totalCnt();
    int curEdgeCnt = 0;

    vector<Bin*> srcBins = bins_;
    vector<int> edgeSampling = edgeDist_.getSamplingVector();

    cout << "Total # of edges to create : " << tarEdgeCnt << endl;
    while ( tarEdgeCnt > curEdgeCnt ) {
        int rIdx = rand() % edgeSampling.size();
        int edgeLength = edgeSampling[rIdx];

        Gain maxG(-INT_MAX, nullptr, nullptr);

        std::random_shuffle(srcBins.begin(), srcBins.end());
        // Iterate source-sink bins (distance(source, sink) == edgeLength)
        for (Bin* srcBin : srcBins) {
            auto t0 = Clock::now();
            vector<Bin*> sinkBins;
            for (int level = 1; level <= srcBin->getPath().size(); level++) {
                sinkBins = getSinkBins_v2(srcBin, edgeLength, level, true);
                if (!sinkBins.empty()) break;
            }
            auto t1 = Clock::now();
            totalSinkBinTime += Duration(t1 - t0).count();
            for(Bin* sinkBin : sinkBins) {
                auto t2 = Clock::now();
                Gain localMaxG = getMaxGain(srcBin, sinkBin);
                auto t3 = Clock::now();
                totalMaxGainTime += Duration(t3 - t2).count();
                if(maxG.value() < localMaxG.value()) {maxG = localMaxG;}
            }
        }
        if( maxG.value() == -INT_MAX ) {}
        else {            
            Node* srcNode = maxG.n1();
            Node* sinkNode = maxG.n2();
            connect(srcNode, sinkNode);
            curEdgeCnt++;
            if(curEdgeCnt % 1000 == 0 || curEdgeCnt == tarEdgeCnt) { 
                double progress = 1.0 * curEdgeCnt / tarEdgeCnt;
                printf("distribution matching progress... [%2.2f\%]\n", 100* progress); 
            }
        }
    }
    auto distEnd = Clock::now();
    Duration totalRuntime = distEnd - distStart;

    std::cout << "[TIME] distMatching total     : " << totalRuntime.count()    << " sec\n";
    std::cout << "[TIME] getSinkBins total time : " << totalSinkBinTime        << " sec\n";
    std::cout << "[TIME] getMaxGain total time  : " << totalMaxGainTime        << " sec\n";
    std::cout << "[TIME] other overhead         : " << (totalRuntime.count() - totalSinkBinTime - totalMaxGainTime) << " sec\n";
    print();
    exit(0);
}

// Source Bin과 Sink Bin에 속한 Node중 가장 큰 Gain을 가지는 Node쌍을 반환
Gain Netlist::getMaxGain(Bin* srcBin, Bin* sinkBin) {
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
        double foG = fanoutGain(fo, fo+1);
        
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
            double fiG = faninGain(fi, fi+1);
            
            //double totG = 1.0 * fiG + 1.0 * foG + 1.0 * bboxG + 1.0 * moduleG;
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
vector<Bin*> Netlist::getSinkBins_v2(Bin* srcBin, int edgeLength, int level, bool shuffle) {
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
    double reward = (fo1 == 0) ? 16 : 0; 
    double g = deltaErr + reward;
    return g;
}

// Fanin gain function
double Netlist::faninGain(int fi1, int fi2) {
    double currErr = 1.0 * abs(fiDist_.delta(fi1)) + 1.0 * abs(fiDist_.delta(fi2));
    double nextErr = 1.0 * abs(fiDist_.delta(fi1)-1) + 1.0 * abs(fiDist_.delta(fi2)+1);
    double deltaErr = currErr - nextErr;
    double reward = (fi1 == 0) ? 16 : 0; 
    double g = deltaErr + reward;
    return g;
}
}
