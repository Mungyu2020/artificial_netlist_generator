#ifndef __ARTNETGEN_BIN_HEADER__
#define __ARTNETGEN_BIN_HEADER__

#include <vector>
#include <iostream>
#include <unordered_map>
#include <map>

namespace artnetgen {
class Node;
class Bin
{
  private:
    int x_, y_;
    int moduleId_; // MK
    int submoduleId_; // MK
    std::vector<int> path_; // MK
    std::vector<Node*> nodes_;
    std::vector<std::vector<Node*>> fi2Nodes_;
    std::vector<std::vector<Node*>> fo2Nodes_;
    std::unordered_map<Node*,int> fi_;
    std::unordered_map<Node*,int> fo_;
    
  public:
    Bin();
    ~Bin();
    void setPath(const std::vector<int>& path) {path_ = path;}//MK
    const std::vector<int>& getPath() const {return path_;} //MK
    void setModuleId(int id) { moduleId_ = id; }  //MK
    int getModuleId() const { return moduleId_; } //MK
    void setSubmoduleId(int id) { submoduleId_ = id; } //MK
    int getSubmoduleId() const { return submoduleId_; } //MK
    const std::vector<Node*>& getNodes() const { return nodes_; } //MK

    void init(int maxFi, int maxFo);
    void setCoord(int x, int y);
    void addNode(Node* node);
    void update(Node* node);
    int x() const;
    int y() const;
    int numNodes() const;
    std::vector<Node*> fi2Nodes(int fi);
    std::vector<Node*> fo2Nodes(int fo);
    Node* getRandomNode();
};

};

#endif
