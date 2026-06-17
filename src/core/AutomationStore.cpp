#include "core/AutomationStore.h"
#include "core/Graph.h"
#include "core/Node.h"
#include "core/Transport.h"
#include "core/Value.h"
#include <algorithm>
#include <variant>

namespace oss {

UiAutomationChannel* AutomationStore::find(int nodeId, int port) {
    for (auto& c : channels_)
        if (c.nodeId == nodeId && c.port == port) return &c;
    return nullptr;
}

UiAutomationChannel* AutomationStore::add(Graph& graph, int nodeId, int port) {
    if (auto* ex = find(nodeId, port)) return ex;
    Node* n = graph.findNode(nodeId);
    if (!n) return nullptr;
    if (port < 0 || port >= (int)n->inputs().size()) return nullptr;
    const Port& p = n->inputs()[port];
    if (p.type != PortType::Float) return nullptr;

    float cur   = std::get<float>(p.defaultValue);
    float lo    = p.minVal, hi = p.maxVal;
    float denom = hi - lo;
    float norm  = denom != 0.0f ? (cur - lo) / denom : 0.0f;

    UiAutomationChannel ch;
    ch.nodeId = nodeId; ch.port = port; ch.outMin = lo; ch.outMax = hi;
    ch.curve.points.push_back({0.0f, norm});
    channels_.push_back(ch);
    return &channels_.back();
}

void AutomationStore::remove(int nodeId, int port) {
    channels_.erase(std::remove_if(channels_.begin(), channels_.end(),
        [&](const UiAutomationChannel& c){ return c.nodeId == nodeId && c.port == port; }),
        channels_.end());
}

void AutomationStore::removeNode(int nodeId) {
    channels_.erase(std::remove_if(channels_.begin(), channels_.end(),
        [&](const UiAutomationChannel& c){ return c.nodeId == nodeId; }),
        channels_.end());
}

void AutomationStore::apply(Graph& graph, const Transport& transport) {
    float bar = (float)transport.bars();
    for (auto& ch : channels_) {
        Node* n = graph.findNode(ch.nodeId);
        if (!n) continue;
        if (ch.port < 0 || ch.port >= (int)n->inputs().size()) continue;
        if (n->inputs()[ch.port].type != PortType::Float) continue;
        if (graph.isInputConnected(ch.nodeId, ch.port)) continue;
        float v = ch.curve.sample(bar);
        n->inputDefault(ch.port) = Value(ch.outMin + v * (ch.outMax - ch.outMin));
    }
}

} // namespace oss
