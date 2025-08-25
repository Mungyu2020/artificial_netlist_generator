#include "artnetgen/artNetGen.h"
#include "netlist.h"
#include "bin.h"
#include "node.h"
#include "opendb/db.h"

#include <algorithm>
#include <math.h>
#include <limits.h>
#include <cfloat>
#include <queue>
#include <chrono>
//#include "opendb/dbSet.h"
#include "sta/Liberty.hh"
#include "sta/TimingArc.hh"
#include "sta/TableModel.hh"
#include "db_sta/dbSta.hh"
#include "db_sta/dbNetwork.hh"

#include <unordered_set>
#include <functional>

namespace artnetgen {
//using std::cout;
//using std::endl;
//using std::vector;
//using std::max;
//using std::min;
//using std::string;
//using std::find;
//using std::unordered_map;
//using std::queue;
//using std::make_pair;
//using std::ceil;
//using std::to_string;
using namespace std;
using namespace odb;

void
Netlist::generate() {
    chrono::system_clock::time_point start;
    chrono::duration<double> runtime;
    
    // 1. initialize
    start = chrono::system_clock::now();
    initialize();
    runtime = chrono::system_clock::now() - start;
    cout << "[Info] Graph initialization finished (" << runtime.count() << " sec)" << endl;
    // 2. set primary I/O nodes
    start = chrono::system_clock::now();
    setPrimaryIO();
    // 3. create edge considering target distributions
    distMatching();
    runtime = chrono::system_clock::now() - start;
    cout << "[Info] Graph construction finished (" << runtime.count() << " sec)" << endl;

    start = chrono::system_clock::now();
    resolveUnconnectedGraphs(); // MK
    runtime = chrono::system_clock::now() - start;
    cout << "[Info] Graph resolving finished (" << runtime.count() << " sec)" << endl; 

    //checkUnconnected();
    // 4. insert sequential logic cells
    start = chrono::system_clock::now();
    initOnlyUseMasters();
    timingPathConstruction_v1();
    runtime = chrono::system_clock::now() - start;
    cout << "[Info] Timing path construction finished (" << runtime.count() << " sec)" << endl;
    //checkUnconnected();

    // 5. map standard cell
    start = chrono::system_clock::now();
    technologyMapping();
    runtime = chrono::system_clock::now() - start;
    cout << "[Info] Technology mapping finished (" << runtime.count() << " sec)" << endl;

    summaryDesign();
}

// Perform DFS traversal to extract a disconnected component
// It collects all nodes in the same component and checks for cycles.
void Netlist::dfsDisconnectedComponent(Node* startNode, std::unordered_map<Node*, int> topoOrder,
    std::unordered_set<Node*>& globalVisited, std::vector<Node*>& component, bool& hasCycle)
{
    std::unordered_set<Node*> localVisited;
    std::unordered_set<Node*> recursionStack;

    std::function<void(Node*)> dfsVisit = [&](Node* node) {
        globalVisited.insert(node);     // 전체 그래프에서 node가 visited 됐는지
        localVisited.insert(node);      // 탐색중인 component에서 node가 visited 됐는지
        recursionStack.insert(node);
        component.push_back(node);

        for (Node* sink : node->getSinks()) {
            if (topoOrder[sink] == -1) {
                if (!localVisited.count(sink)) {dfsVisit(sink);}    
                else if (recursionStack.count(sink)) {hasCycle = true;}
            }
        }
        recursionStack.erase(node);
    };
    dfsVisit(startNode);
}


void Netlist::resolveUnconnectedGraphs(){
    unordered_map<Node*, int> topoOrder = topologicalSort();
    std::unordered_set<Node*> visited;
    std::vector<std::vector<Node*>> components;
    std::vector<bool> componentHasCycle;
    
    // Number of disconnected components = components.size()
    // Whether the conponent has cycle = componentHasCycle[i]
    for (Node* node : nodes_) {
        if (topoOrder[node] == -1 && !visited.count(node)) {
            std::vector<Node*> component;
            bool loop = false;
            dfsDisconnectedComponent(node, topoOrder, visited, component, loop);
            components.push_back(component);
            componentHasCycle.push_back(loop);
        }
    }
    
    for (int i = 0; i < components.size(); ++i) {
        std::unordered_set<Node*> compSet(components[i].begin(), components[i].end());

        // 1. Disconnected componenet 상에 존재하는 모든 loop 제거
        bool removedEdge = true;
        while (removedEdge) {
            removedEdge = false;
            std::unordered_set<Node*> visited ,recursionStack;
            std::function<bool(Node*)> dfsCycleBreakAll = [&](Node* curr) {
                visited.insert(curr);
                recursionStack.insert(curr);

                for (Node* sink : curr->getSinks()) {
                    if (!compSet.count(sink)) continue;
                    if (!visited.count(sink)) {if (dfsCycleBreakAll(sink)) return true;} 
                    else if (recursionStack.count(sink)) {
                        disconnect(curr, sink);
                        removedEdge = true;
                        return true;
                    }
                }
                recursionStack.erase(curr);
                return false;
            };
            for (Node* node : components[i]) {if (dfsCycleBreakAll(node)) break;}
        }

        // 2. 각 component에 PI 연결
        for (Node* node : components[i]) {
            if (node->numFanins() == 0) {
                Node* pi = primIns_[rand() % primIns_.size()];
                connect(pi, node);
                break;
            }
        }
    }
}


void 
Netlist::summaryDesign() {
    vector<Node*> inUnconnected;
    vector<Node*> outUnconnected;

    unordered_map<Node*, int> topoOrder = topologicalSort();

    int maxTopoOrder = 0;
    int numCCs = 0;
    int numFFs = 0;
    int numPIs = 0;
    int numPOs = 0;

    for(int i=0; i < nodes_.size(); i++) {
        Node* node = nodes_[i];
        if( node->getType() == NodeType::PrimaryIn) {
            if( node->numFanins() != 0 ) {
                cout << "#fanin of primary input is not zero! (" << node->numFanins() << ")" <<  endl;
                exit(0);
            }
        }else if( node->getType() == NodeType::PrimaryOut) {
            if( node->numFanins() != 1 ) {
                cout << "#fanin of primary output is not one! (" << node->numFanins() << ")" <<  endl;
                exit(0);
            }
            if( node->numFanouts() != 0 ) {
                cout << "#fanout of primary output is not zero! (" << node->numFanouts() << ")" << endl;
                exit(0);
            }
        } else {
            if( node->numFanins() == 0 ) {inUnconnected.push_back(node);}
            if( node->numFanouts() == 0 ) {outUnconnected.push_back(node);}
        }

        int x = node->x();
        int y = node->y();
        int order = topoOrder[node];

        switch(node->getType()) {
            case NodeType::Combinational: 
                numCCs++; break;
            case NodeType::Sequential:
                numFFs++; break;
            case NodeType::PrimaryIn:
                numPIs++; break;
            case NodeType::PrimaryOut:
                numPOs++; break;
            default: break;
        }
        maxTopoOrder = max(maxTopoOrder, order);    
    }
    float seqRatio = 1.0 * numFFs / (numCCs + numFFs);
    cout << "# input unconnected nodes : " << inUnconnected.size() << endl;
    cout << "# output unconnected nodes : " << outUnconnected.size() << endl;
    cout << "Maximum topological order : " << getMaxTopologicalOrder() << endl;
    cout << "Average topological order : " << getAvgTopologicalOrder() << endl;
    cout << "# of combinational nodes : " << numCCs << endl;
    cout << "# of sequential nodes : " << numFFs << endl;
    cout << "# of primary input nodes : " << numPIs << endl;
    cout << "# of primary output nodes : " << numPOs << endl;
    cout << "Sequential ratio : " << seqRatio << " (" << 1- ang_->getCombRatio() << ")" << endl;
    //cout << "primary input/output will be connected to the unconnected nodes, first" << endl;
}

void 
Netlist::setPrimaryIO() {
    int inputPinCnt = ang_->getInputPinCnt();
    int outputPinCnt = ang_->getOutputPinCnt();
    int x,y;
    bool done;

    x = 0, y = 0;
    done = false;

    while(true) {
        Bin* headBin = getBin(x,y);
        vector<Node*> candidates = headBin->fi2Nodes(0);

        for(int i=0; i < candidates.size(); i++) {
            Node* node = candidates[i];
            node->setType(NodeType::PrimaryIn);

            primIns_.push_back(node);
            if(primIns_.size() >= inputPinCnt) {
                done = true;
                break;
            }
        }

        if(!done) {
            x++;
            if( x >= layoutDimX_) {
                cout << "# of primary inputs is too large!" << endl;
                exit(0);
            }
        } else {break;}
    }

    x = layoutDimX_-1, y=layoutDimY_-1;
    done = false;

    while(true) {
        Bin* tailBin = getBin(x,y);
        vector<Node*> candidates = tailBin->fo2Nodes(0);

        for(int i=0; i < candidates.size(); i++) {
            Node* node = candidates[i];
            node->setType(NodeType::PrimaryOut);

            primOuts_.push_back(node);

            if(primOuts_.size() >= outputPinCnt) {
                done = true;
                break;
            }
        }
        
        if(!done) {
            //y--; //Original
            x--; // by MK
            if( x < 0 ) {
                cout << "# of primary outputs is too large!" << endl;
                exit(0);
            }
        } else {break;}
    }

    cout << "# of primary inputs is " << inputPinCnt << endl;
    for(int i=0; i < primIns_.size(); i++) {
        Node* primIn = primIns_[i]; 
        string ioName = "in" + to_string(i);
        primIn->setName(ioName);
        if(i < 5) {
            cout << " - " << i << "-th primary input is in (" 
                << primIn->x() << " " << primIn->y() << ")" << endl;
            if (i==4)
                cout << " - ..." << endl;
        }
    }

    cout << "# of primary outputs is " << outputPinCnt << endl;
    for(int i=0; i < primOuts_.size(); i++) {
        Node* primOut = primOuts_[i];
        string ioName = "out" + to_string(i);
        primOut->setName(ioName);
        if(i < 5) {
            cout << " - " << i << "-th primary output is in (" 
                << primOut->x() << " " << primOut->y() << ")" << endl;
            if (i==4)
                cout << " - ..." << endl;
        }
    }
}

void
Netlist::initialize() {
    //ang_ = ang;
    //layoutDimX_ = ang_->binSqrt(); // BIN 2D Grid 너비 (x-축 bin count), binSqrt --> sqrt(#bins) 라고 생각하면됨
    //layoutDimY_ = ang_->binSqrt(); // BIN 2D Grid 높이 (y-축 bin count)

    int instanceCnt = ang_->getInstanceCnt();                   // Total instance 개수 ( = CC + FF )
    double combRatio = ang_->getCombRatio();                    // Nonclocking Cell 비율
    double discountFactor = 0.80;
    int combCellCnt = std::ceil(instanceCnt * combRatio);
    int flipflopCnt = instanceCnt * (1 - combRatio);
    int totalBinCnt = ceil(sqrt(instanceCnt));

    // Init layout dimension
    layoutDimX_ = 1;
    layoutDimY_ = 1;

    while(layoutDimX_ * layoutDimY_ < totalBinCnt) {
        if(layoutDimX_ > layoutDimY_) layoutDimY_++;
        else layoutDimX_++;
    }
    cout << "Layout dimension (" << layoutDimX_ << " " << layoutDimY_ << ")" << endl;
    
    int binCnt = layoutDimX_ * layoutDimY_;
    int initCellCnt = 0.80 * instanceCnt - 0.2 * (1 - combRatio) * instanceCnt;
    int avgNodeCnt = ceil( 1.0 * initCellCnt / binCnt );              // BIN당 평균 instance (node) 개수
   
    int maxFanin = ang_->getMaxFanin();
    int maxFanout = ang_->getMaxFanout();
    int maxEdgeLength = layoutDimX_ + layoutDimY_;

    // copy (확장성을위해 netlist의 spec은 독립적으로 할당)
    fiDist_.setDescription("fanin distribution");
    foDist_.setDescription("fanout distribution");
    bboxDist_.setDescription("net bbox distribution");
    fiDist_.init(0, maxFanin, initCellCnt, ang_->getFaninDistInfo());
    foDist_.init(0, maxFanout, initCellCnt, ang_->getFanoutDistInfo());
    bboxDist_.init(0, maxEdgeLength, initCellCnt, ang_->getBboxDistInfo());
    int totEdgeCnt = ceil(1.0*initCellCnt*fiDist_.targetAvg());
    edgeDist_.init(0, maxEdgeLength, totEdgeCnt, ang_->getEdgeDistInfo());

    // 1. Bin 생성 (Stor) & (Pointer)
    binStor_ = vector<Bin>(binCnt);
    for(int i=0; i < binCnt; i++) {
        int x = i % layoutDimX_;
        int y = i / layoutDimX_;

        bins_.push_back(&binStor_[i]);
        bins_.back()->setCoord(x,y); 
        bins_.back()->init(fiDist_.xMax(), foDist_.xMax());
    }

    // 2. 모듈 계층 트리 + binPaths 계산 by MK ----------------------------------------------------------------------------
    std::queue<int> childQueue;
    std::queue<int> parentQueue;
    std::unordered_map<int, int> parentOf;
    std::unordered_map<int, std::vector<int>> binPaths; // bin → root까지의 path
    
    int nextInternalId = binCnt; // 전역 parent ID는 binCnt 이후부터 시작
    
    // 2-1. leaf bin 초기화
    for (int i = 0; i < binCnt; ++i) {childQueue.push(i);}
    
    // 2-2. 트리 생성 (전역 parent ID 사용)
    while (true) {
        int initChildQueueSize = childQueue.size();
        double minRatio = 0.05;
        double maxRatio = 0.20;
        int minGroupSize = std::max(2, int(initChildQueueSize * minRatio));
        int maxGroupSize = std::max(minGroupSize, int(initChildQueueSize * maxRatio));

        while (!childQueue.empty()) {
            int groupSize = std::min((int)childQueue.size(), minGroupSize + rand() % (maxGroupSize - minGroupSize + 1));
    
            std::vector<int> group;
            for (int i = 0; i < groupSize; ++i) {
                group.push_back(childQueue.front());
                childQueue.pop();
            }
    
            int thisParentId = nextInternalId++;
            for (int childId : group) { parentOf[childId] = thisParentId; }
            parentQueue.push(thisParentId);
        }
    
        if (parentQueue.size() == 1)
            break;
    
        while (!parentQueue.empty()) {
            childQueue.push(parentQueue.front());
            parentQueue.pop();
        }
    }

    for (int binId = 0; binId < binCnt; ++binId) {
        std::vector<int> path;
        int cur = binId;
    
        while (parentOf.find(cur) != parentOf.end()) {
            path.push_back(cur);
            cur = parentOf[cur];
        }
        path.push_back(cur); // root
        std::reverse(path.begin(), path.end()); // root → leaf 순서
        binPaths[binId] = path;
    }
    
    auto joinPath = [](const std::vector<int>& path) -> std::string {
        std::string result;
        for (size_t i = 0; i < path.size(); ++i) {
            result += std::to_string(path[i]);
            if (i != path.size() - 1)
                result += "/";
        }
        return result;
    };
    //for (int i = 0; i < binCnt; ++i) {std::cout << "bin " << i << " path = " << joinPath(binPaths[i]) << std::endl;}
    // ----------------------------------------------------------------------------------------------------------------------

    // 3. Node 생성 (Pointer)
    for(int i=0; i < initCellCnt; i++) {
        Node* n = new Node();
        nodes_.push_back(n);
    }

    // 4. x-y coordinate 할당 (Node -> Bin)  + prefix 이름 부여
    for(int i=0; i < binCnt; i++) {
        int x = i % layoutDimX_;
        int y = i / layoutDimX_;

        Bin* bin = getBin(x, y);
        bin->setPath(binPaths[i]);
        std::string prefix = joinPath(binPaths[i]);
        for (int j = avgNodeCnt * i; j < avgNodeCnt * (i + 1) && j < initCellCnt; j++) {
            string cellName = "c" + to_string(j);
            Node* node = nodes_[j];
            node->setType(NodeType::Combinational);
            node->setName(prefix + "/" + cellName);
            bin->addNode(node);
        }
    }
    /* ***************************************************************************** */
    cout << "layout dimension: ("<<layoutDimX_<<","<<layoutDimY_<<")"<<endl;
    int moduleDim = 4;      // 8 / 2
    int subDim = 2;         // 4 / 2 = each module is 2x2 submodules
    
    int submodulePerRow = 4 * 2;  // 8 / 2
    
    for (Bin& bin : binStor_) {
        int x = bin.x(), y = bin.y();
    
        // coarse module (2x2 of 4x4 grid)
        int moduleX = x / (layoutDimX_ / 2);
        int moduleY = y / (layoutDimY_ / 2);
        int moduleId = moduleY * 2 + moduleX;
        bin.setModuleId(moduleId);
    
        // fine submodule (within each module)
        int localX = x % (layoutDimX_ / 2);  // local bin x inside module
        int localY = y % (layoutDimY_ / 2);
        int subX = localX / subDim;
        int subY = localY / subDim;
    
        int submoduleId = moduleId * 4 + (subY * 2 + subX);  // 4 submodules per module
        bin.setSubmoduleId(submoduleId);
    }    
    /* *********************************************** */

    cout << "finished initialize" << endl;
    print();
}

void Netlist::setArtNetGen(ArtNetGen* ang) {
    ang_ = ang;
}

Netlist::Netlist() {}
Netlist::~Netlist() {}

Bin* Netlist::getBin(int x, int y) {
    int idx = x + y * layoutDimX_;
   
    Bin* bin = bins_[idx];
    if (bin->x() != x || bin->y() != y) {
        cout << "idx : " << idx << "(" << bin->x() << " " << bin->y() <<") (" << x << " " << y << ")" << endl;
        exit(0);
    }
    return bins_[idx];
}

void
Netlist::print() {
    foDist_.print(5);
    fiDist_.print(5);
    bboxDist_.print(5);

    int multiPortConn = 0;
    for(Node* node : nodes_) {
        if(node->hasMultiPortConn())
            multiPortConn++;
    }
    cout << "# of multiple port connection cells = " << multiPortConn << endl;
}

void
Netlist::connect(Node* srcNode, Node* sinkNode) {
    int edgeLen = abs(srcNode->x() - sinkNode->x()) + abs(srcNode->y() - sinkNode->y());
    // decrement previous fanin, fanout 
    fiDist_.decr(sinkNode->numFanins());
    foDist_.decr(srcNode->numFanouts());
    bboxDist_.decr(srcNode->bboxSize());
    // add source and sink for each node
    srcNode->addSink(sinkNode);
    sinkNode->addSource(srcNode);
    // increment current fanin, fanout
    fiDist_.incr(sinkNode->numFanins());
    foDist_.incr(srcNode->numFanouts());
    bboxDist_.incr(srcNode->bboxSize());
    edgeDist_.incr(edgeLen);
    
    // update information of srcNode in the bin
    Bin* srcBin = srcNode->getBin();
    Bin* sinkBin = sinkNode->getBin();
    srcBin->update(srcNode);
    sinkBin->update(sinkNode);
}

void 
Netlist::disconnect(Node* srcNode, Node* sinkNode) {
    int edgeLen = abs(srcNode->x() - sinkNode->x()) + abs(srcNode->y() - sinkNode->y());

    // decrement previous fanin, fanout 
    fiDist_.decr(sinkNode->numFanins());
    foDist_.decr(srcNode->numFanouts());
    bboxDist_.decr(srcNode->bboxSize());
    edgeDist_.decr(edgeLen);

    // remove source and sink for each node
    srcNode->removeSink(sinkNode);
    sinkNode->removeSource(srcNode);

    // increment current fanin, fanout
    fiDist_.incr(sinkNode->numFanins());
    foDist_.incr(srcNode->numFanouts());
    bboxDist_.incr(srcNode->bboxSize());

    // update information of srcNode in the bin
    Bin* srcBin = srcNode->getBin();
    Bin* sinkBin = sinkNode->getBin();
    srcBin->update(srcNode);
    sinkBin->update(sinkNode);
}


vector<Net*> Netlist::getNetlist() {return nets_;}
vector<Node*> Netlist::getPrimaryInputs() {return primIns_;}
vector<Node*> Netlist::getPrimaryOutputs() {return primOuts_;}
vector<Node*> Netlist::getNodes() {return nodes_;}
};
