#pragma once
namespace oss {

// Directed edge: output (srcNode, srcPort) -> input (dstNode, dstPort).
struct Connection {
    int srcNode;
    int srcPort;
    int dstNode;
    int dstPort;
};

} // namespace oss
