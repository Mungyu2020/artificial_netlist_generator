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

void Netlist::distMatching() {
    using Clock = std::chrono::system_clock;
    using Duration = std::chrono::duration<double>;
    auto distStart = Clock::now();
    double totalSinkBinTime = 0.0;
    double totalMaxGainTime = 0.0;

    int tarEdgeCnt = edgeDist_.totalCnt();
    int curEdgeCnt = 0;

    vector<Bin*> srcBins = bins_;
    vector<int> edgeSampling = edgeDist_.getSamplingVector();

    int MAX_INST = 0;
    for (Bin* bin : bins_) {
        MAX_INST = std::max(MAX_INST, bin->numNodes());
    }

    std::cout << "Total # of edges to create : " << tarEdgeCnt << std::endl;
    std::cout << "[INFO] Using getMaxGain_v4()" << std::endl;

    while (tarEdgeCnt > curEdgeCnt) {
        int rIdx = rand() % edgeSampling.size();
        int edgeLength = edgeSampling[rIdx];

        Gain maxG(-INT_MAX, nullptr, nullptr);
        std::random_shuffle(srcBins.begin(), srcBins.end());

        auto t0 = Clock::now();
        std::vector<std::pair<Bin*, Bin*>> binPairs;
        for (Bin* srcBin : srcBins) {
            vector<Bin*> sinkBins = getSinkBins(srcBin, edgeLength, true);
            for (Bin* sinkBin : sinkBins) {
                binPairs.emplace_back(srcBin, sinkBin);
            }
        }
        auto t1 = Clock::now();
        totalSinkBinTime += Duration(t1 - t0).count();

        auto t2 = Clock::now();
        maxG = getMaxGain_v4(binPairs, MAX_INST);
        auto t3 = Clock::now();
        totalMaxGainTime += Duration(t3 - t2).count();

        if (maxG.value() != -INT_MAX) {
            Node* srcNode = maxG.n1();
            Node* sinkNode = maxG.n2();
            connect(srcNode, sinkNode);
            curEdgeCnt++;

            if (curEdgeCnt % 1000 == 0 || curEdgeCnt == tarEdgeCnt) {
                double progress = 1.0 * curEdgeCnt / tarEdgeCnt;
                printf("distribution matching progress... [%2.2f%%]\n", 100 * progress);
            }
        }
    }

    auto distEnd = Clock::now();
    Duration totalRuntime = distEnd - distStart;

    std::cout << "\n[TIMING SUMMARY]" << std::endl;
    std::cout << "[TIME] distMatching total     : " << totalRuntime.count() << " sec" << std::endl;
    std::cout << "[TIME] getSinkBins total time : " << totalSinkBinTime << " sec" << std::endl;
    std::cout << "[TIME] getMaxGain_v4 total    : " << totalMaxGainTime << " sec" << std::endl;
    std::cout << "[TIME] other overhead         : " << (totalRuntime.count() - totalSinkBinTime - totalMaxGainTime) << " sec" << std::endl;
    exit(0);
    print();
}



Gain Netlist::getMaxGain_v4(const std::vector<std::pair<Bin*, Bin*>>& binPairs, const int MAX_INST) {
    const int NUM_BIN_PAIRS = binPairs.size();

    const int SIZE_FI = fiDist_.xMax() + 1;
    const int SIZE_FO = foDist_.xMax() + 1;
    const int SIZE_BBOX = bboxDist_.xMax() + 1;

    const int SIZE_LUT = SIZE_FI + SIZE_FO + SIZE_BBOX;
    const int SIZE_HEADER = 5; // N1 | N2 | moduleG | x | y
    const int SIZE_A = MAX_INST * 5;
    const int SIZE_B = MAX_INST;
    const int SIZE_GAIN = MAX_INST * MAX_INST;
    const int SIZE_MODULE = SIZE_HEADER + SIZE_A + SIZE_B + SIZE_GAIN;
    const int SIZE_TOTAL = SIZE_LUT + SIZE_MODULE * binPairs.size();

    // buf = [LUT | Header | A | B | GAIN | Header | A | B | GAIN | Header | A | B | GAIN | ...]. 총 binPairs.size() 만큼의 (Header, A,B,GAIN)  존재
    int* buf = (int*) malloc(sizeof(int) * SIZE_TOTAL);
    if (buf == nullptr) {
        std::cerr << "[ERROR] malloc failed for buf (SIZE_TOTAL = " << SIZE_TOTAL << ")\n";
        exit(1);
    }
    
    // Init LUT
    for (int i = 0; i < SIZE_LUT; i++) {
        if(i < SIZE_FI) {buf[i] = fiDist_.delta(i);}
        else if(i < SIZE_FI + SIZE_FO) {buf[i] = foDist_.delta(i - SIZE_FI);} 
        else {buf[i] = bboxDist_.delta(i - SIZE_FI - SIZE_FO);}
    }
    
    // Init [Header | A | B | Gain] for each n-th source-sink bin pair
    for (int n = 0; n < NUM_BIN_PAIRS; n++) {
        const int OFFSET_HEADER = SIZE_LUT + n * SIZE_MODULE;
        const int OFFSET_A = OFFSET_HEADER + SIZE_HEADER;
        const int OFFSET_B = OFFSET_A + SIZE_A;
        const int OFFSET_GAIN = OFFSET_B + SIZE_B;

        Bin* srcBin = binPairs[n].first;
        Bin* sinkBin = binPairs[n].second;
        if (!srcBin || !sinkBin) continue;

        const std::vector<Node*>& srcNodes = srcBin->getNodes();
        const std::vector<Node*>& sinkNodes = sinkBin->getNodes();
        int N1 = srcNodes.size();
        int N2 = sinkNodes.size();

        if (N1 == 0 || N2 == 0) continue;
        
        int moduleG = 0.0;
        if (srcBin->getSubmoduleId() == sinkBin->getSubmoduleId()) moduleG = 10.0;
        else if (srcBin->getModuleId() == sinkBin->getModuleId()) moduleG = 5.0;
        int x = sinkBin->x(), y = sinkBin->y();
        
        // Init header
        buf[OFFSET_HEADER + 0] = N1;
        buf[OFFSET_HEADER + 1] = N2;
        buf[OFFSET_HEADER + 2] = moduleG;
        buf[OFFSET_HEADER + 3] = x;
        buf[OFFSET_HEADER + 4] = y;

        // A: src info
        for (int i = 0; i < N1; ++i) {
            Node* node = srcNodes[i];
            buf[OFFSET_A + i * 5 + 0] = node->numFanouts();
            buf[OFFSET_A + i * 5 + 1] = node->lx();
            buf[OFFSET_A + i * 5 + 2] = node->ly();
            buf[OFFSET_A + i * 5 + 3] = node->ux();
            buf[OFFSET_A + i * 5 + 4] = node->uy();
        }

        // B: sink fanin
        for (int i = 0; i < N2; ++i) {
            buf[OFFSET_B + i] = sinkNodes[i]->numFanins();
        }

        // GAIN 초기값
        for (int i = 0; i < N1 * N2; ++i) {
            Node* src = srcNodes[i / N2];
            Node* sink = sinkNodes[i % N2];

            bool isValid = true;
            if (sink->getType() == NodeType::PrimaryIn) isValid = false;
            else if (sink->getType() == NodeType::PrimaryOut && sink->numFanins() >= 1) isValid = false;
            if (src->getType() == NodeType::PrimaryOut) isValid = false;
            if (srcBin == sinkBin && sink <= src) isValid = false;
            if (src->hasConnection(sink)) isValid = false;
            if (sink->numFanins() >= SIZE_FI - 1) isValid = false;
            if (src->numFanouts() >= SIZE_FO - 1) isValid = false;

            buf[OFFSET_GAIN + i] = isValid ? 0 : -INT_MAX;
        }
    }

    // 이부분을 CUDA KERNEL로 구현할 예정정
    for (int i = 0; i < NUM_BIN_PAIRS * SIZE_GAIN; i++){
        const int ORDER_OF_BIN_PAIR = i / SIZE_GAIN;
        const int ORDER_OF_NODE_PAIR = i % SIZE_GAIN;

        const int OFFSET_HEADER = SIZE_LUT + (ORDER_OF_BIN_PAIR * SIZE_MODULE);
        const int OFFSET_FI = 0;
        const int OFFSET_FO = OFFSET_FI + SIZE_FI;
        const int OFFSET_BBOX = OFFSET_FO + SIZE_FO;

        const int OFFSET_A = OFFSET_HEADER + SIZE_HEADER;
        const int OFFSET_B = OFFSET_A + SIZE_A;
        const int OFFSET_GAIN = OFFSET_B + SIZE_B;

        const int N1 = buf[OFFSET_HEADER + 0];
        const int N2 = buf[OFFSET_HEADER + 1];

        if (ORDER_OF_NODE_PAIR >= N1 * N2) {continue;} // N1 * N2 < SIZE_GAIN인 경우도 있음. SIZE_GAIN은 최대 크기.
        int idx1 = ORDER_OF_NODE_PAIR / N2;
        int idx2 = ORDER_OF_NODE_PAIR % N2;

        int fo = buf[OFFSET_A + idx1 * 5];
        int foG = abs(buf[fo + OFFSET_FO]) + abs(buf[fo + OFFSET_FO + 1]) - abs(buf[fo + OFFSET_FO] - 1) - abs(buf[fo + OFFSET_FO + 1] + 1);
        if (fo == 0) foG += 16;

        int lx = buf[OFFSET_A + idx1 * 5 + 1], ly = buf[OFFSET_A + idx1 * 5 + 2];
        int ux = buf[OFFSET_A + idx1 * 5 + 3], uy = buf[OFFSET_A + idx1 * 5 + 4];
        int x = buf[OFFSET_HEADER + 3], y = buf[OFFSET_HEADER + 4];
        int bbox1 = (ux - lx) + (uy - ly);
        int bbox2 = (std::max(ux, x) - std::min(lx, x)) + (std::max(uy, y) - std::min(ly, y));
        int boxG = abs(buf[OFFSET_BBOX + bbox1]) + abs(buf[OFFSET_BBOX + bbox2]) -
                abs(buf[OFFSET_BBOX + bbox1] - 1) - abs(buf[OFFSET_BBOX + bbox2] + 1);

        int fi = buf[OFFSET_B + idx2];
        int fiG = abs(buf[OFFSET_FI + fi]) + abs(buf[OFFSET_FI + fi + 1]) -
                abs(buf[OFFSET_FI + fi] - 1) - abs(buf[OFFSET_FI + fi + 1] + 1);
        if (fi == 0) fiG += 16;

        int moduleG = buf[OFFSET_HEADER + 2];
        int totalG = foG + fiG + boxG + moduleG;

        bool check = (buf[OFFSET_GAIN + ORDER_OF_NODE_PAIR] != -INT_MAX);
        buf[OFFSET_GAIN + ORDER_OF_NODE_PAIR] = check ? totalG : -INT_MAX;        
    }

    // Max gain에 해당하는 src, sink pair 찾기기
    int bestGain = -INT_MAX;
    int bestIdx = -1;
    int tieCount = 0;
    
    for (int i = 0; i < NUM_BIN_PAIRS * SIZE_GAIN; ++i) {
        const int ORDER_OF_BIN_PAIR = i / SIZE_GAIN;
        const int ORDER_OF_NODE_PAIR = i % SIZE_GAIN;
    
        const int OFFSET_HEADER = SIZE_LUT + ORDER_OF_BIN_PAIR * SIZE_MODULE;
        const int OFFSET_GAIN = OFFSET_HEADER + SIZE_HEADER + SIZE_A + SIZE_B;
    
        const int N1 = buf[OFFSET_HEADER + 0];
        const int N2 = buf[OFFSET_HEADER + 1];

        if (ORDER_OF_NODE_PAIR >= N1 * N2) continue;

        int gainVal = buf[OFFSET_GAIN + ORDER_OF_NODE_PAIR];
        if (gainVal == -INT_MAX) continue;
    
        if (gainVal > bestGain) {
            bestGain = gainVal;
            bestIdx = i;
            tieCount = 1;
        } else if (gainVal == bestGain) {
            tieCount++;
            if (rand() % tieCount == 0) {bestIdx = i;}
        }
    }

    Node* src = nullptr;
    Node* sink = nullptr;
    if (bestIdx != -1) {
        int binIdx = bestIdx / SIZE_GAIN;
        int nodeIdx = bestIdx % SIZE_GAIN;

        int OFFSET_HEADER = SIZE_LUT + binIdx * SIZE_MODULE;
        int N1 = buf[OFFSET_HEADER + 0];
        int N2 = buf[OFFSET_HEADER + 1];
        int idx1 = nodeIdx / N2;
        int idx2 = nodeIdx % N2;

        src = binPairs[binIdx].first->getNodes()[idx1];
        sink = binPairs[binIdx].second->getNodes()[idx2];
    }
    free(buf);
    if (src && sink)
        return Gain(bestGain, src, sink);
    else
        return Gain(-INT_MAX, nullptr, nullptr);
}



// 현재 Source Bin과 Distance가 edgeLength인 sinkBin 리스트를 반환
vector<Bin*> Netlist::getSinkBins(Bin* srcBin, int edgeLength, bool shuffle) {
    vector<Bin*> sinkBins;

    for(int dx=0; dx <= edgeLength; dx++) {
        int dy = edgeLength - dx;
        int x = srcBin->x() + dx;
        int y = srcBin->y() + dy;

        if( x >= layoutDimX() || y >= layoutDimY() ) 
            continue;
        
        Bin* sinkBin = getBin(x, y);
        sinkBins.push_back(sinkBin);
    }

    if( shuffle && sinkBins.size() > 1 )
        std::random_shuffle(sinkBins.begin(), sinkBins.end());

    return sinkBins;
}

}