#include "core/Graph.h"
#include <algorithm>
#include <queue>

namespace oss {

int Graph::addNode(std::unique_ptr<Node> node) {
    int id = nextId_++;
    node->id_ = id;
    nodes_.push_back(std::move(node));
    markDirty();
    return id;
}

void Graph::removeNode(int nodeId) {
    connections_.erase(std::remove_if(connections_.begin(), connections_.end(),
        [&](const Connection& c){ return c.srcNode == nodeId || c.dstNode == nodeId; }),
        connections_.end());
    nodes_.erase(std::remove_if(nodes_.begin(), nodes_.end(),
        [&](const std::unique_ptr<Node>& n){ return n->id() == nodeId; }),
        nodes_.end());
    outputs_.erase(nodeId);
    markDirty();
}

Node* Graph::findNode(int nodeId) const {
    for (auto& n : nodes_) if (n->id() == nodeId) return n.get();
    return nullptr;
}

bool Graph::isInputConnected(int nodeId, int portIndex) const {
    for (auto& c : connections_)
        if (c.dstNode == nodeId && c.dstPort == portIndex) return true;
    return false;
}

bool Graph::wouldCreateCycle(int srcNode, int dstNode) const {
    // Adding src->dst creates a cycle iff dst can already reach src.
    if (srcNode == dstNode) return true;
    std::vector<int> stack{dstNode};
    std::unordered_map<int, bool> seen;
    while (!stack.empty()) {
        int cur = stack.back(); stack.pop_back();
        if (cur == srcNode) return true;
        if (seen[cur]) continue;
        seen[cur] = true;
        for (auto& c : connections_) if (c.srcNode == cur) stack.push_back(c.dstNode);
    }
    return false;
}

bool Graph::connect(int srcNode, int srcPort, int dstNode, int dstPort) {
    Node* s = findNode(srcNode);
    Node* d = findNode(dstNode);
    if (!s || !d) return false;
    if (srcPort < 0 || srcPort >= (int)s->outputs().size()) return false;
    if (dstPort < 0 || dstPort >= (int)d->inputs().size())  return false;
    if (s->outputs()[srcPort].type != d->inputs()[dstPort].type) return false;
    if (isInputConnected(dstNode, dstPort)) return false;
    if (wouldCreateCycle(srcNode, dstNode)) return false;
    connections_.push_back({srcNode, srcPort, dstNode, dstPort});
    markDirty();
    return true;
}

void Graph::disconnect(int dstNode, int dstPort) {
    connections_.erase(std::remove_if(connections_.begin(), connections_.end(),
        [&](const Connection& c){ return c.dstNode == dstNode && c.dstPort == dstPort; }),
        connections_.end());
    markDirty();
}

std::vector<int> Graph::topologicalOrder() const {
    std::unordered_map<int, int> indeg;
    for (auto& n : nodes_) indeg[n->id()] = 0;
    for (auto& c : connections_) indeg[c.dstNode]++;
    std::queue<int> q;
    for (auto& n : nodes_) if (indeg[n->id()] == 0) q.push(n->id());
    std::vector<int> order;
    while (!q.empty()) {
        int cur = q.front(); q.pop();
        order.push_back(cur);
        for (auto& c : connections_)
            if (c.srcNode == cur && --indeg[c.dstNode] == 0) q.push(c.dstNode);
    }
    if ((int)order.size() != (int)nodes_.size()) return {};  // cycle present
    return order;
}

} // namespace oss
