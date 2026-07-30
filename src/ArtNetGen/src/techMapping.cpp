#include "artnetgen/artNetGen.h"
#include "netlist.h"
#include "bin.h"
#include "node.h"

#include <algorithm>
#include <math.h>
#include <limits.h>
#include <cfloat>
#include <queue>
#include "opendb/db.h"
//#include "opendb/dbSet.h"

#include <unordered_set>
#include <functional>

#include <fstream>

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
Netlist::initOnlyUseMasters() {
    int fiMax = fiDist_.xMax();
    fi2CombMasters_ = vector<vector<dbMaster*>>(fiMax+1);
    fi2SequMasters_ = vector<vector<dbMaster*>>(fiMax+1);

    vector<MasterInfo> masters = ang_->getMasterInfo();
    
    for(MasterInfo& info : masters) {
        dbMaster* master = info.master();
        float ratio = info.ratio();

        master2ratio_[master] = ratio;
        unordered_map<string, int> ioCount = getPortInfo(master);

        if(ioCount["output_signal"] == 0)
            continue;

        if(ioCount["input_scan"] > 0) {
            cout << "current version does not support scan-chain cell... (" << master->getName() << ")" <<  endl;
            continue;
        }

        if(ioCount["input_clock"] > 0) {
            // sequential cell
            fi2SequMasters_[ioCount["input_signal"]].push_back(master);       
        } else {
            // combinational cell
            fi2CombMasters_[ioCount["input_signal"]].push_back(master);
        }
    }
    
    for(int i=0; i <= fiMax; i++) {
        cout << "# of fanins : " << i << endl;
        vector<dbMaster*> masters = fi2SequMasters_[i];
        for(dbMaster* master : masters) //MasterInfo& info : masters) {
        {
            //dbMaster* master = info.master();
            cout << "   - " << master->getName() << endl;
        }
    }
}


void
Netlist::technologyMapping() {

    int fiMax = fiDist_.xMax();
    vector<vector<Node*>> fi2CombNodes(fiMax+1);
    vector<vector<Node*>> fi2SequNodes(fiMax+1);

    for(Node* node : nodes_) {
        if(node->getType() == NodeType::Combinational)
            fi2CombNodes[node->numFanins()].push_back(node);
        else if(node->getType() == NodeType::Sequential)
            fi2SequNodes[node->numFanins()].push_back(node);
    }

    Node* clkIn = new Node();
    Node* rstIn = new Node();
    nodes_.push_back(clkIn);
    nodes_.push_back(rstIn);

    primIns_.push_back(clkIn);
    primIns_.push_back(rstIn);

    clkIn->setName("clk");
    clkIn->setType(NodeType::ClockIn);
    clkIn->setBin(getBin(0,0)); 
    rstIn->setName("rst");
    rstIn->setType(NodeType::ResetIn);
    rstIn->setBin(getBin(0,0)); 

    // Nangate Lef --> CLOCK 포트에대한 정의가 없음.
    // clock port이름을 input argument로 받고 dbMTerm::getName() 과 비교하여 sequential MACRO를 찾아야 할듯. 
    // 혹은 lef파일 PIN-USE --> clock 추가
    int totNodeCnt=0;
    int beginIdx = 0;
    int endIdx = 0;
    float denominator;

    for(int fi = 0; fi <= fiMax; fi++) {
        cout << "# of inputs == " << fi << endl;
        // <Sequential>
        denominator = 0.0;
        totNodeCnt = fi2SequNodes[fi].size();
        beginIdx = 0;

        unordered_map<dbMaster*, int> targetCnt;
        unordered_map<dbMaster*, int> currentCnt;
        
        for(dbMaster* master : fi2SequMasters_[fi]) {
            denominator += master2ratio_[master];
            // ONLY_USE AT LEAST ONCE!
            //Node* target = fi2SequNodes[fi][beginIdx];
            //target->setDbMaster(master);
            //beginIdx++;
            //if(beginIdx >= totNodeCnt)
            //    break;
        }

        for(dbMaster* master : fi2SequMasters_[fi]) {
            master2ratio_[master] /= denominator;
            float ratio = master2ratio_[master];
            //cout << "   " << master->getName() << " " << 100 * master2ratio_[master] << "\%" << endl;
            targetCnt[master] = ceil(ratio * totNodeCnt);
            currentCnt[master] = 0;
        }

        int numNodes = fi2SequNodes[fi].size();
        int numMasters = fi2SequMasters_[fi].size();

        int idxIter = 0;

        for(int i=0; i < numNodes; i++) {
            Node* target = fi2SequNodes[fi][i];
            dbMaster* master;
            
            while(true) {
                master = fi2SequMasters_[fi][idxIter];
                idxIter = (idxIter + 1) % numMasters;
                if(targetCnt[master] - currentCnt[master] > 0) {
                    currentCnt[master]++;
                    break;
                }
            }
            target->setDbMaster(master);
        }
        
        cout << "<Sequantial>" << endl;
        for(dbMaster* master : fi2SequMasters_[fi]) {
            printf("    %2.2f (%d/%d) - ", master2ratio_[master], currentCnt[master], targetCnt[master]);
            cout << master->getName() << endl;
        }

        // <Combinational> 
        denominator = 0.0;
        totNodeCnt = fi2CombNodes[fi].size();
        beginIdx = 0;
        targetCnt.clear();

        for(dbMaster* master : fi2CombMasters_[fi]) {
            denominator += master2ratio_[master];
            // ONLY_USE AT LEAST ONCE!
            //Node* target = fi2CombNodes[fi][beginIdx];
            //target->setDbMaster(master);
            //beginIdx++;
            //if(beginIdx >= totNodeCnt)
            //    break;
        }

        for(dbMaster* master : fi2CombMasters_[fi]) {
            master2ratio_[master] /= denominator;
            float ratio = master2ratio_[master];
            //cout << "   " << master->getName() << " " << 100 * master2ratio_[master] << "\%" << endl;
            targetCnt[master] = ceil(ratio * totNodeCnt);
            currentCnt[master] = 0;
        }
        /*
        for (fi = 0 ; fi < fiDist_.xMax() ;fi++){
            cout << "fi2CombNodes["<<fi<<"].size(): "<<fi2CombNodes[fi].size()<<endl;
            cout << "fi2CombMasters_["<<fi<<"].size(): "<<fi2CombMasters_[fi].size()<<endl;
        }exit(0);
        */
        numNodes = fi2CombNodes[fi].size();
        numMasters = fi2CombMasters_[fi].size();
        idxIter = 0;

        for(int i=0; i < numNodes; i++) {
            Node* target = fi2CombNodes[fi][i];
            dbMaster* master;
            while(true) {
                master = fi2CombMasters_[fi][idxIter];
                idxIter = (idxIter+1) % numMasters;
  
                if(targetCnt[master] - currentCnt[master] > 0) {
                    currentCnt[master]++;
                    break;
                }
            }
            target->setDbMaster(master);
        }

        cout << "<Combinational>" << endl;
        for(dbMaster* master : fi2CombMasters_[fi]) {
            string name = master->getName();
            printf("    %2.2f (%d/%d) - ", master2ratio_[master], currentCnt[master], targetCnt[master]);
            cout << master->getName() << endl; //, name);
        }
    }

    for(Node* node : nodes_) {node->mappingTerms(clkIn, rstIn);}
    
    int netCnt=0;
    nets_.reserve(2 * ang_->getInstanceCnt());
    unordered_map<string, Net> name2net;
    unordered_map<string, string> out2name;
    
    for(int i = 0; i < nodes_.size(); i++) {
        Node* source = nodes_[i];
        unordered_map<string, vector<Node*>> out2sinks;

        for(Node* sink : source->getSinks()) {
            string outTermName = source->getTerm(sink);
            out2sinks[outTermName].push_back(sink);
        }

        string netName = "";
        // check whether output term is connected to IO
        for(auto p : out2sinks) {
            string outTermName = p.first;
            vector<Node*> sinks = p.second;

            if( source->getType() == NodeType::PrimaryIn || 
                source->getType() == NodeType::ClockIn ||
                source->getType() == NodeType::ResetIn) {
                netName = source->getName();
            } else {
                bool containPrimOut = false;
                netName = "net" + to_string(netCnt);
                for(Node* sink : sinks) {
                    if(sink->getType()==NodeType::PrimaryOut) {
                        netName = sink->getName();
                        containPrimOut = true;
                    }
                }
                if(!containPrimOut) {netCnt++;}   
            }
            out2name[outTermName] = netName;

            if(name2net.find(netName) == name2net.end()) {
                name2net[netName] = Net();
                name2net[netName].setName(netName);
                name2net[netName].addTerm(source, outTermName);
            }
            for(Node* sink : sinks) {
                string inTermName = sink->getTerm(source);
                name2net[netName].addTerm(sink, inTermName);
            }
        }
        for(auto p : out2sinks) {
            string outTermName = p.first;
            string netName = out2name[outTermName];
            vector<Node*> sinks = p.second;
        }
    }
    for(auto it = name2net.begin(); it != name2net.end(); it++) {
        netStor_.push_back(it->second);
        Net* net = &netStor_.back(); 
    }
    for(int i=0; i < netStor_.size(); i++) {
        Net* net = &netStor_[i];
        nets_.push_back(net);
    }
    cout << "technology mapping is finished" << endl;
}

int Netlist::getMaxTopologicalOrder() {
    int maxTopoOrder = 0;
    unordered_map<Node*, int> topoOrder = topologicalSort();
    for(Node* node : nodes_) {maxTopoOrder = max(maxTopoOrder, topoOrder[node]);}
    return maxTopoOrder;
}

double Netlist::getAvgTopologicalOrder() {
    double avgTopoOrder = 0;
    unordered_map<Node*, int> topoOrder = topologicalSort();
    for(Node* node : nodes_) {avgTopoOrder += topoOrder[node];}
    avgTopoOrder /= nodes_.size();
    return avgTopoOrder;
}

unordered_map<Node*, int> Netlist::topologicalSort() {
    const int MAX_FO = foDist_.xMax();
    for (Node* dstNode : nodes_) {
        if (dstNode->numFanins() == 0 && dstNode->getType() == NodeType::Combinational) {
            Bin* dstBin = dstNode->getBin();
            const std::vector<int>& dstPath = dstBin->getPath();
    
            Node* bestSrc = nullptr;
            int maxCommonPrefix = -1;

            for (Node* srcNode : nodes_) {
                if (srcNode->numFanouts() >= MAX_FO) continue;
                if (srcNode == dstNode) continue;
    
                Bin* srcBin = srcNode->getBin();
                const std::vector<int>& srcPath = srcBin->getPath();
    
                int commonLen = 0;
                while (commonLen < srcPath.size() && commonLen < dstPath.size() &&
                       srcPath[commonLen] == dstPath[commonLen]) {
                    commonLen++;
                }
    
                if (commonLen > maxCommonPrefix) {
                    maxCommonPrefix = commonLen;
                    bestSrc = srcNode;
                }
            }
            if (bestSrc) { connect(bestSrc, dstNode);}    
            else {cout << "[ERROR] No suitable source found for comb node with fanin == 0 " << endl; exit(0);}
        }
    }

    queue<Node*> Q;
    unordered_map<Node*, int> inDegree;
    unordered_map<Node*, int> topoOrder;

    for(int i = 0; i < nodes_.size(); i++) {
        Node* node = nodes_[i];
        topoOrder[node] = -1;

        switch(node->getType()) {
            case NodeType::Sequential:
                inDegree[node] = 0; break;
            case NodeType::PrimaryIn:
                inDegree[node] = 0; break;
            default:
                inDegree[node] = node->numFanins(); break; 
        }

        if(inDegree[node] == 0) {
            topoOrder[node] = 0;
            if(node->getType() == NodeType::ClockIn || node->getType() == NodeType::ResetIn)
                continue;
            Q.push(node);
        }
    }

    while(!Q.empty()) {
        Node* n1 = Q.front(); Q.pop();
        // Exception
        if(n1->getType() == NodeType::ClockIn || n1->getType() == NodeType::ResetIn) { cout << "???????" << endl; exit(0);}
        for(Node* n2 : n1->getSinks()) {
            if( inDegree[n2] == 0 ) continue;
            if( --inDegree[n2] == 0 ) {
                topoOrder[n2] = topoOrder[n1] + 1;
                Q.push(n2);
            }
        }
    }
 
    return topoOrder;
}

int
Netlist::getCountNodeType(int type) {
    int count =0;
    for(Node* node : nodes_) {if(node->getType() == type) count++;}
    return count;
}

double
Netlist::getCombinationalRatio() {
    int numCombNodes = 0;
    int numSequNodes = 0;

    for(Node* node : nodes_) {
        if(node->getType() == NodeType::Sequential)
            numSequNodes++;
        else if(node->getType() == NodeType::Combinational)
            numCombNodes++;
    }
    double currentRatio = 1.0 * numCombNodes / (numSequNodes + numCombNodes);
    return currentRatio;
}

void
Netlist::timingPathConstruction_v1() {
    // TODO
    //double synClkPeriod = ang_->getSynClkPeriod();
    double avgGateDelay = ang_->getAvgGateDelay();
    double avgTopoOrder = ang_->getAvgTopoOrder();

    int targetMaxOrder = ceil(avgTopoOrder) + 2; //ceil( synClkPeriod / avgGateDelay );
    //cout << "timing path construction (target clk : " << synClkPeriod << " avg. gate delay : " << avgGateDelay << ")" << endl;
    cout << "timing path construction (target avg. topo. order : " << avgTopoOrder << ")" << endl;

    int numIter = 0;
    while(true) {
        unordered_map<Node*, int> topoOrder = topologicalSort();
        bool updated = false;
        for(Node* n : nodes_) {
            if(topoOrder[n] == targetMaxOrder) {
                if(n->getType() == NodeType::Combinational) {
                    n->setType(NodeType::Sequential);
                    updated = true;
                }
            }
        }
        // MK
        /*
        int i=0;
        for(Node* n : nodes_){
            if(topoOrder[n] != -1){
                i++;
                cout<<topoOrder[n]<<endl;
            }
        }cout<<"i: "<<i<<endl; */
        
        if(numIter++ % 5 == 0) {
            cout << numIter++ << "-iteration target " << avgTopoOrder;
            cout << " cur_avg " << getAvgTopologicalOrder();
            cout << " cur_max " << getMaxTopologicalOrder() << endl;
        }
        if(!updated) break;
    }

    // fit to comb_ratio (input arg)
    int numTotNodes = nodes_.size();
    int numCombNodes = 0;
    int numSequNodes = 0;
    vector<vector<Node*>> targets(layoutDimX_ * layoutDimY_);
    vector<int> idx;

    for(int i=0; i < layoutDimX_ * layoutDimY_; i++) {idx.push_back(i);}
    for(Node* node : nodes_) {
        if(node->getType() == NodeType::Sequential) {
            int id = node->x() + node->y() * layoutDimX_;
            targets[id].push_back(node);
            numSequNodes++;
        } else if (node->getType() == NodeType::Combinational) {
            numCombNodes++;
        }
    }
    
    double currentRatio = 1.0 * numCombNodes / (numCombNodes + numSequNodes);
    cout << "Target combination ratio   : " << ang_->getCombRatio() << endl;
    cout << "Current combination ratio  : " << currentRatio << endl;
    cout << "Current max topo order     : " << getMaxTopologicalOrder() << endl;
    cout << "Current avg topo order     : " << getAvgTopologicalOrder() << endl;

    int edgeLength = 0;
    bool finish = false;
    int maxLen = ceil(0.2 * (layoutDimX_ + layoutDimY_));
    cout << "Max len : " << maxLen << endl;
    // 2단계: Sequential 노드를 2개씩 골라서 → 1개의 Combinational로 merge
    // 병합 기준은 fanin/fanout 수가 너무 많지 않은 노드끼리
    for(int edgeLength = 0; edgeLength < max(3, maxLen); edgeLength++) {
        if(currentRatio >= ang_->getCombRatio()) {break;}
        random_shuffle(idx.begin(), idx.end());
        for(int id : idx) {
            // edgeLength는 탐색 범위 반경 (맨해튼 거리 기준)
            // lx, ly, ux, uy는 탐색할 사각형 영역의 좌표 범위
            int cx = id % layoutDimX_;
            int cy = id / layoutDimX_;
            int lx = max(cx - edgeLength, 0);
            int ly = max(cy - edgeLength, 0);
            int ux = min(cx + edgeLength, layoutDimX_-1);
            int uy = min(cy + edgeLength, layoutDimY_-1);

            // 탐색 영역 내에 Sequential node들을 병합 후보로 선택
            // targets[tIdx]: tIdx에 해당하는 bin에 있는 sequential node들의 vector
            vector<Node*> candidates;
            for(int x = lx; x <= ux; x++) {
                for(int y = ly; y <= uy; y++) {
                    int tIdx = x + y * layoutDimX_;
                    candidates.insert(candidates.end(), targets[tIdx].begin(), targets[tIdx].end());
                }
            }

            if(candidates.size() < 2)
                continue;
            else {
                // 후보들을 fanin 개수가 적은 순으로 정렬
                sort(candidates.begin(), candidates.end(), [](Node* left, Node* right) {return left->numFanins() < right->numFanins();});

                bool found = false;
                Node *n1, *n2;
                for(int i=0; i < candidates.size() - 1; i++) {
                    n1 = candidates[i];
                    n2 = candidates[i + 1];
                    int totFanins = n1->numFanins() + n2->numFanins();
                    int totFanouts = n1->numFanouts() + n2->numFanouts();
                    // (총 fanin 수 ≤ 최대 fanin 제한) && (총 fanout 수 < 최대 fanout 제한) 이면 병합 가능
                    if(totFanins <= fiDist_.xMax() && totFanouts < foDist_.xMax()) {
                        found=true;
                        break;
                    } 
                }
                if(!found) continue;

                // 병합 가능한 쌍을 찾았으면, createMergeNode()를 통해 새로운 combinational 노드 n3 생성
                // 기존 n1, n2는 comb 노드로 재설정됨
                Node* n3 = createMergeNode(n1, n2, true);
                n1->setType(NodeType::Combinational);
                n2->setType(NodeType::Combinational);

                int idx1 = n1->x() + n1->y() * layoutDimX_;
                int idx2 = n2->x() + n2->y() * layoutDimX_;
                int idx3 = n3->x() + n3->y() * layoutDimX_;
                targets[idx1].erase(find(targets[idx1].begin(), targets[idx1].end(), n1));
                targets[idx2].erase(find(targets[idx2].begin(), targets[idx2].end(), n2));
                targets[idx3].push_back(n3);

                numCombNodes += 2;
                numSequNodes -= 1; 
                currentRatio = 1.0 * (numCombNodes+numSequNodes) / (numCombNodes + 2*numSequNodes); 
            }
            if(currentRatio >= ang_->getCombRatio()) { break; }
        }
    }

    // 3단계: 1단계로 만족 못했을 경우, fanout 기준으로 병합 재시도
    // 위와 똑같은 merge 작업인데 이번엔 fanout이 적은 순서로 sorting해서 시도함
    for(int edgeLength=0; edgeLength < max(3, maxLen); edgeLength++) {

        if(currentRatio >= ang_->getCombRatio())
            break;
        
        random_shuffle(idx.begin(), idx.end());
        for(int id : idx) {
            int cx = id % layoutDimX_;
            int cy = id / layoutDimX_;
            int lx = max(cx - edgeLength, 0);
            int ly = max(cy - edgeLength, 0);
            int ux = min(cx + edgeLength, layoutDimX_-1);
            int uy = min(cy + edgeLength, layoutDimY_-1);

            vector<Node*> candidates;

            for(int x=lx; x<=ux; x++) {
                for(int y=ly; y<=uy; y++) {
                    int tIdx = x + y*layoutDimX_;
                    candidates.insert(candidates.end(), targets[tIdx].begin(), targets[tIdx].end());
                }
            }

            if(candidates.size() < 2)
                continue;
            else {
                sort(candidates.begin(), candidates.end(), [](Node* left, Node* right) {
                    return left->numFanouts() < right->numFanouts();
                        });

                bool found=false;
                Node *n1, *n2, *n3;

                for(int i=0; i < candidates.size()-1; i++) {
                    n1 = candidates[i];
                    n2 = candidates[i+1];
                    int totFanouts = n1->numFanouts() + n2->numFanouts();
                    if(totFanouts < foDist_.xMax()) {
                        found=true;
                    }
                    break;
                }
               
                if(found) {
                    Node* n3 = createMergeNode(n1, n2, false);
                    n1->setType(NodeType::Combinational);
                    n2->setType(NodeType::Combinational);

                    int idx1 = n1->x() + n1->y() * layoutDimX_;
                    int idx2 = n2->x() + n2->y() * layoutDimX_;
                    int idx3 = n3->x() + n3->y() * layoutDimX_;
                    targets[idx1].erase(find(targets[idx1].begin(), targets[idx1].end(), n1));
                    targets[idx2].erase(find(targets[idx2].begin(), targets[idx2].end(), n2));
                    targets[idx3].push_back(n3);

                    numCombNodes += 2;
                    numSequNodes -= 1;
                    currentRatio = 1.0 * (numCombNodes+numSequNodes) / (numCombNodes + 2*numSequNodes); 
                } 
            }

            if(currentRatio >= ang_->getCombRatio())
                break;
        }


        if(currentRatio >= ang_->getCombRatio())
            break;
    }

    cout << "Current max topo order     : " << getMaxTopologicalOrder() << endl;
    cout << "Current comb_ratio : " << currentRatio << " (" << ang_->getCombRatio() << ")" << endl;
    cout << getCombinationalRatio() << endl;
    cout << "#comb : " << getCountNodeType(NodeType::Combinational) << endl;
    cout << "#sequ : " << getCountNodeType(NodeType::Sequential) << endl;
    
    // 4단계: Sequential 노드 중 사용할 수 없는 master가 할당된 경우 제거
    int len = nodes_.size();
    for(int i=0; i < len; i++) {
        Node* node = nodes_[i];
        if(node->getType() == NodeType::Sequential) {
            if(!hasMaster(node->numFanins(), true)) {
                node->setType(NodeType::Combinational);
                insertSequentialNode(node, false);
            }
        }
    }

    numCombNodes = 0;
    numSequNodes = 0;
    for(Node* node : nodes_) {
        if(node->getType() == NodeType::Sequential) {numSequNodes++;}
        else if(node->getType() == NodeType::Combinational) {numCombNodes++;}
    }
    currentRatio = 1.0 * numCombNodes / (numSequNodes + numCombNodes);
    cout << "Current comb_ratio : " << currentRatio << " (" << ang_->getCombRatio() << ")" << endl;
    cout << getCombinationalRatio() << endl;
    cout << "#comb : " << getCountNodeType(NodeType::Combinational) << endl;
    cout << "#sequ : " << getCountNodeType(NodeType::Sequential) << endl;
    cout << "Max topological order : " << getMaxTopologicalOrder() << " (target max order -> " << targetMaxOrder << ")" << endl;
}

void
Netlist::timingPathConstruction_v2() {
    // TODO
    //double synClkPeriod = ang_->getSynClkPeriod();
    double avgGateDelay = ang_->getAvgGateDelay();
    double avgTopoOrder = ang_->getAvgTopoOrder();


    int targetMaxOrder = ceil(avgTopoOrder) + 2; //ceil( synClkPeriod / avgGateDelay );
    //cout << "timing path construction (target clk : " << synClkPeriod << " avg. gate delay : " << avgGateDelay << ")" << endl;
    cout << "timing path construction (target avg. topo. order : " << avgTopoOrder << ")" << endl;

    int numIter = 0;

    while(true) {
        unordered_map<Node*, int> topoOrder = topologicalSort();
        bool updated = false;
        for(Node* n : nodes_) {
            if(topoOrder[n] == targetMaxOrder) {
                if(n->getType() == NodeType::Combinational) {
                    n->setType(NodeType::Sequential);
                    updated = true;
                }
            }
        }
  
     
        cout << numIter++ << "-iteration target " << avgTopoOrder << " cur_avg " << getAvgTopologicalOrder() << " cur_max " << getMaxTopologicalOrder() << endl;


        if(!updated)
            break;
    }


    // TODO 
    // fit to comb_ratio (input arg)
    int numTotNodes = nodes_.size();
    int numCombNodes = 0;
    int numSequNodes = 0;
   


    double currentRatio = 1.0 * numCombNodes / (numCombNodes + numSequNodes);
    
    cout << "Target combination ratio   : " << ang_->getCombRatio() << endl;
    cout << "Current combination ratio  : " << currentRatio << endl;
    cout << "Current max topo order     : " << getMaxTopologicalOrder() << endl;
    cout << "Current avg topo order     : " << getAvgTopologicalOrder() << endl;
    int edgeLength = 0;
    bool finish = false;

    vector<int> idx;
    for(int i=0; i < layoutDimX_ * layoutDimY_; i++)
        idx.push_back(i);

    //for(int edgeLength=0; edgeLength < layoutDimX_+layoutDimY_; edgeLength++) {
    //for(int edgeLength=0; edgeLength < 3; edgeLength++) {
    int maxLen = ceil(0.5 * (layoutDimX_ + layoutDimY_));
    cout << "Max len : " << maxLen << endl;
    for(int edgeLength=0; edgeLength < max(3, maxLen); edgeLength++) {

        if(currentRatio >= ang_->getCombRatio())
            break;
        
        vector<vector<Node*>> targets(layoutDimX_ * layoutDimY_);

        for(Node* node : nodes_) {
            if(node->getType() == NodeType::Sequential) {
                int x = node->x();
                int y = node->y();
                int id = x + y * layoutDimX_;
                targets[id].push_back(node);
                numSequNodes++;
            } else if (node->getType() == NodeType::Combinational) {
                numCombNodes++;
            }

        }
        
        random_shuffle(idx.begin(), idx.end());
        for(int id : idx) {
            int cx = id % layoutDimX_;
            int cy = id / layoutDimX_;
            int lx = max(cx - edgeLength, 0);
            int ly = max(cy - edgeLength, 0);
            int ux = min(cx + edgeLength, layoutDimX_-1);
            int uy = min(cy + edgeLength, layoutDimY_-1);

            vector<Node*> candidates;
            for(int x=lx; x<=ux; x++) {
                for(int y=ly; y<=uy; y++) {
                    int tIdx = x + y*layoutDimX_;
                    candidates.insert(candidates.end(), targets[tIdx].begin(), targets[tIdx].end());
                }
            }


            if(candidates.size() < 2)
                continue;
            
            bool isDone = false;
            Node *n1, *n2, *n3;

            vector<Node*> faninSorted = candidates;
            sort(faninSorted.begin(), faninSorted.end(), [](Node* left, Node* right) {
                return left->numFanins() < right->numFanins();
                    });
            
            int i=0;
            while(i<faninSorted.size()-1) {
                n1 = faninSorted[i];
                n2 = faninSorted[i+1];
                int totFanins = n1->numFanins() + n2->numFanins();
                //cout << "# fanins to merge = " << totFanins << endl;
                if(totFanins <= fiDist_.xMax()) {
                    n3 = createMergeNode(n1,n2,true);
                    //cout << "merged!" << endl;
                    n1->setType(NodeType::Combinational);
                    n2->setType(NodeType::Combinational);

                    int idx1 = n1->x() + n1->y() * layoutDimX_;
                    int idx2 = n2->x() + n2->y() * layoutDimX_;
                    int idx3 = n3->x() + n3->y() * layoutDimX_;
                    candidates.erase(find(candidates.begin(), candidates.end(), n1));
                    candidates.erase(find(candidates.begin(), candidates.end(), n2));
                    candidates.push_back(n3);


                    currentRatio = getCombinationalRatio();
                    //cout << "current ratio : " << currentRatio << " (" << ang_->getCombRatio() << ")" << endl; 
                    
                    i += 2;
                    if(currentRatio >= ang_->getCombRatio())
                        break;
                
                } else {
                    break;
                }
            }
                
            if(candidates.size() < 2)
                continue;

            vector<Node*> fanoutSorted = candidates;
            sort(fanoutSorted.begin(), fanoutSorted.end(), [](Node* left, Node* right) {
                return left->numFanouts() < right->numFanouts();
                    });
            i=0;
            while(i<fanoutSorted.size()-1) {
                n1 = fanoutSorted[i];
                n2 = fanoutSorted[i+1];
                int totFanouts = n1->numFanouts() + n2->numFanouts();
                //cout << "# fanouts to merge = " << totFanouts << endl;
                if(totFanouts <= foDist_.xMax()) {
                    n3 = createMergeNode(n1,n2,false);
                    //cout << "merged!" << endl;
                    n1->setType(NodeType::Combinational);
                    n2->setType(NodeType::Combinational);

                    int idx1 = n1->x() + n1->y() * layoutDimX_;
                    int idx2 = n2->x() + n2->y() * layoutDimX_;
                    int idx3 = n3->x() + n3->y() * layoutDimX_;

                    

                    candidates.erase(find(candidates.begin(), candidates.end(), n1));
                    candidates.erase(find(candidates.begin(), candidates.end(), n2));
                    candidates.push_back(n3);
                    currentRatio = getCombinationalRatio();
                    //cout << "current ratio : " << currentRatio << " (" << ang_->getCombRatio() << ")" << endl; 
                    i+=2;
                    if(currentRatio >= ang_->getCombRatio())
                        break;
                } else {
                    break;
                }

            }

            if(currentRatio >= ang_->getCombRatio())
                break;
        }
    }

    cout << "Current max topo order     : " << getMaxTopologicalOrder() << endl;
    cout << "Current comb_ratio : " << currentRatio << " (" << ang_->getCombRatio() << ")" << endl;
    cout << getCombinationalRatio() << endl;

    cout << "#comb : " << getCountNodeType(NodeType::Combinational) << endl;
    cout << "#sequ : " << getCountNodeType(NodeType::Sequential) << endl;

    cout << "HERE1" << endl;
    for(int i=0; i < nodes_.size(); i++) {
        Node* node = nodes_[i];
        if(node->getType() == NodeType::Sequential) {
            //if(node->numFanins() > 1) {
            //    node->setType(NodeType::Combinational);
            //    insertSequentialNode(node, false);
            //}
            if(!hasMaster(node->numFanins(), true)) {
                //cout << node->getName() << " " << node->numFanins() << endl;
                node->setType(NodeType::Combinational);
                insertSequentialNode(node, false);
            }
        }
    }
    cout << "HERE2" << endl;

    numCombNodes = 0;
    numSequNodes = 0;

    for(Node* node : nodes_) {
        if(node->getType() == NodeType::Sequential)
            numSequNodes++;
        else if(node->getType() == NodeType::Combinational)
            numCombNodes++;
    }

    currentRatio = 1.0 * numCombNodes / (numSequNodes + numCombNodes);

    cout << "Current comb_ratio : " << currentRatio << " (" << ang_->getCombRatio() << ")" << endl;
    cout << getCombinationalRatio() << endl;
    cout << "#comb : " << getCountNodeType(NodeType::Combinational) << endl;
    cout << "#sequ : " << getCountNodeType(NodeType::Sequential) << endl;
    cout << "Max topological order : " << getMaxTopologicalOrder() << " (target max order -> " << targetMaxOrder << ")" << endl;

}


bool Netlist::hasMaster(int numFanins, bool isSequential) {
    if(isSequential) 
        return fi2SequMasters_[numFanins].size() > 0 ? true : false;
    else
        return fi2CombMasters_[numFanins].size() > 0 ? true : false;

}

Node* Netlist::createMergeNode(Node* n1, Node* n2, bool front) {

    string cellName = "merge" + to_string(nodes_.size());
    
    Node* n3 = new Node();
    nodes_.push_back(n3);

    n3->setType(NodeType::Sequential);
    n3->setBin(n1->getBin());
    int lx = min(n1->lx(), n2->lx());
    int ly = min(n1->ly(), n2->ly());
    int ux = max(n1->ux(), n2->ux());
    int uy = max(n1->uy(), n2->uy());
    n3->setBbox(lx, ly, ux, uy);
    n3->setName(cellName);
    n3->getBin()->addNode(n3);

    if(front) {
        vector<Node*> sources;
        vector<Node*>::iterator it;

        for(Node* node: n1->getSources()) {
            it = find(sources.begin(), sources.end(), node);
            if(it == sources.end()) {
                sources.push_back(node);
            }
        }
        for(Node* node: n2->getSources()) {
            it = find(sources.begin(), sources.end(), node);
            if(it == sources.end()) {
                sources.push_back(node);
            }
        }

        n3->addSources(sources);
        n3->addSink(n1);
        n3->addSink(n2);
        n1->removeAllSources();
        n2->removeAllSources();
        n1->addSource(n3);
        n2->addSource(n3);


    } else {
        vector<Node*> sinks;
        vector<Node*>::iterator it;

        for(Node* node : n1->getSinks()) {
            it = find(sinks.begin(), sinks.end(), node);
            if(it == sinks.end()) {
                sinks.push_back(node);
            }
        }

        for(Node* node : n2->getSinks()) {
            it = find(sinks.begin(), sinks.end(), node);
            if(it == sinks.end()) {
                sinks.push_back(node);
            }
        }

        n3->addSinks(sinks);
        n3->addSource(n1);
        n3->addSource(n2);
        n1->removeAllSinks();
        n2->removeAllSinks();
        n1->addSink(n3);
        n2->addSink(n3);
    }
    n1->getBin()->update(n1);
    n2->getBin()->update(n2);
    n3->getBin()->update(n3);

    return n3;
}

Node* Netlist::createMergeNode(Node* n1, Node* n2, Node* n3, bool front) {
    string cellName = "merge" + to_string(nodes_.size());
 

    int centerX = (n1->x() + n2->x() + n3->x())/3;
    int centerY = (n1->y() + n2->y() + n3->y())/3;


    Node* n4 = new Node();
    nodes_.push_back(n4);
    //nodeStor_.push_back(Node());
    //nodes_.push_back(&nodeStor_.back());
    //Node* n4 = nodes_.back();

    n4->setType(NodeType::Sequential);
    n4->setBin(getBin(centerX, centerY)); 
    
    int lx = min(min(n1->lx(), n2->lx()), n3->lx());
    int ly = min(min(n1->ly(), n2->ly()), n3->ly());
    int ux = max(max(n1->ux(), n2->ux()), n3->ux());
    int uy = max(max(n1->uy(), n2->uy()), n3->uy());

    n4->setBbox(lx, ly, ux, uy);
    n4->setName(cellName);
    n4->getBin()->addNode(n4);

    if(front) {
        vector<Node*> sources;
        vector<Node*>::iterator it;

        for(Node* node: n1->getSources()) {
            it = find(sources.begin(), sources.end(), node);
            if(it == sources.end()) {
                sources.push_back(node);
            }
        }

        for(Node* node: n2->getSources()) {
            it = find(sources.begin(), sources.end(), node);
            if(it == sources.end()) {
                sources.push_back(node);
            }
        }

        for(Node* node: n3->getSources()) {
            it = find(sources.begin(), sources.end(), node);
            if(it == sources.end()) {
                sources.push_back(node);
            }
        }
        n4->addSources(sources);
        n4->addSink(n1);
        n4->addSink(n2);
        n4->addSink(n3);
        n1->removeAllSources();
        n2->removeAllSources();
        n3->removeAllSources();
        n1->addSource(n4);
        n2->addSource(n4);
        n3->addSource(n4);


    } else {

        vector<Node*> sinks;
        vector<Node*>::iterator it;

        for(Node* node : n1->getSinks()) {
            it = find(sinks.begin(), sinks.end(), node);
            if(it == sinks.end()) {
                sinks.push_back(node);
            }
        }

        for(Node* node : n2->getSinks()) {
            it = find(sinks.begin(), sinks.end(), node);
            if(it == sinks.end()) {
                sinks.push_back(node);
            }
        }

        for(Node* node : n3->getSinks()) {
            it = find(sinks.begin(), sinks.end(), node);
            if(it == sinks.end()) {
                sinks.push_back(node);
            }
        }

        n4->addSinks(sinks);
        n4->addSource(n1);
        n4->addSource(n2);
        n4->addSource(n3);
        n1->removeAllSinks();
        n2->removeAllSinks();
        n3->removeAllSinks();
        n1->addSink(n4);
        n2->addSink(n4);
        n3->addSink(n4);
    }
    n1->getBin()->update(n1);
    n2->getBin()->update(n2);
    n3->getBin()->update(n3);
    n4->getBin()->update(n4);

    return n4;
}


void
Netlist::insertSequentialNode(Node* target, bool front) {

    string cellName = "s" + to_string(nodes_.size());
    Node* seqn = new Node();
    nodes_.push_back(seqn);
    
    //Node n1;
    //nodeStor_.push_back(n1);
    //nodes_.push_back(&nodeStor_.back()); 
    //Node* seqn = nodes_.back();
    Bin* bin = target->getBin();
    seqn->setType(NodeType::Sequential);
    seqn->setBin(target->getBin());
    seqn->setBbox(target->x(), target->y(), target->x(), target->y());
    seqn->setName(cellName);
    bin->addNode(seqn);
    if(front) {
        vector<Node*> sources = target->getSources();
        if(sources.size() > 1) {
            Node* combn = new Node();
            nodes_.push_back(combn);
            //Node n2;
            //nodeStor_.push_back(n2);
            //nodes_.push_back(&nodeStor_.back()); 
            //Node* combn = nodes_.back(); //nodeStor_.back();
            combn->setType(NodeType::Combinational);
            combn->setBin(target->getBin());
            combn->setBbox(target->x(), target->y(), target->x(), target->y());
            combn->addSources(sources);
            combn->addSink(seqn);
            seqn->addSource(combn);
            target->getBin()->addNode(combn);
        } else {
           seqn->addSources(sources);
        }
        seqn->addSink(target);
        target->removeAllSources();
        target->addSource(seqn);
    } else {
        vector<Node*> sinks = target->getSinks();
        seqn->addSinks(sinks);
        seqn->addSource(target);
        target->removeAllSinks();
        target->addSink(seqn);
    }
    seqn->getBin()->update(seqn);
    target->getBin()->update(target);
}
};
