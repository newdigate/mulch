#pragma once
#include <memory>
#include <vector>
#include <unordered_map>
#include "core/Node.h"
#include "core/Connection.h"
#include "core/Transport.h"

namespace oss {

class Graph {
public:
    // Global transport (tempo + song position). The toolbar drives it; every
    // evaluate() advances it by dt and hands it to each node via EvalContext.
    Transport&       transport()       { return transport_; }
    const Transport& transport() const { return transport_; }

    // Takes ownership; returns the assigned (>=1) node id.
    int  addNode(std::unique_ptr<Node> node);
    void removeNode(int nodeId);

    // Returns false and makes no change if ports are invalid, types differ,
    // the input is already connected, or the edge would create a cycle.
    bool connect(int srcNode, int srcPort, int dstNode, int dstPort);
    void disconnect(int dstNode, int dstPort);
    bool isInputConnected(int nodeId, int portIndex) const;

    const std::vector<std::unique_ptr<Node>>& nodes() const { return nodes_; }
    const std::vector<Connection>&            connections() const { return connections_; }
    Node* findNode(int nodeId) const;

    // Evaluate every node once in topological order. dt = seconds this frame.
    void evaluate(float dt);

    // Topological order of node ids; empty if the graph is cyclic. (Testable.)
    std::vector<int> topologicalOrder() const;

private:
    bool wouldCreateCycle(int srcNode, int dstNode) const;
    void markDirty() { orderDirty_ = true; }

    std::vector<std::unique_ptr<Node>> nodes_;
    std::vector<Connection>            connections_;
    int nextId_ = 1;

    mutable std::vector<int> order_;
    mutable bool             orderDirty_ = true;

    Transport transport_;
    std::unordered_map<int, std::vector<Value>> outputs_;  // per-frame node outputs
};

} // namespace oss
