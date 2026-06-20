#pragma once
#include <vector>
#include "core/AutoCurve.h"

namespace oss {

class Graph;
struct Transport;

// A UI-automation channel: bound directly to one Float input control of a node
// (no edge). Its curve is sampled at the transport position and the scaled value
// is written straight into that control's input default by apply().
struct UiAutomationChannel {
    int       nodeId = 0;     // target node id
    int       port   = 0;     // target Float input port index
    AutoCurve curve;
    float     outMin = 0.0f;  // scales sample() [0,1] -> [outMin, outMax]
    float     outMax = 1.0f;
};

// Owns the graph's UI-automation channels plus one global song length (bars) shared
// by every lane's time axis in the Automation editor. GL-free.
class AutomationStore {
public:
    float lengthBars() const { return lengthBars_; }
    void  setLengthBars(float L) { lengthBars_ = L < 1.0f ? 1.0f : L; }

    // First existing channel for node+port, or nullptr. The returned pointer is
    // valid only until the next add/remove/removeNode (or any channels() mutation),
    // which may reallocate the underlying vector -- don't cache it across those.
    UiAutomationChannel* find(int nodeId, int port);

    // Create a channel for (nodeId, port). Idempotent: returns the existing channel
    // if one already targets that node+port. Seeds outMin/outMax from the port's
    // slider range and inserts one breakpoint at bar 0 equal to the control's
    // current normalised value (so creation never changes the live value). Returns
    // nullptr if the node/port is missing or the port is not a Float. Same pointer
    // lifetime caveat as find() -- valid only until the next add/remove/removeNode.
    UiAutomationChannel* add(Graph& graph, int nodeId, int port);

    void remove(int nodeId, int port);   // drop the channel for node+port (if any)
    void removeNode(int nodeId);          // drop all channels targeting a node
    void clear() { channels_.clear(); lengthBars_ = 8.0f; }   // drop all channels; reset length

    const std::vector<UiAutomationChannel>& channels() const { return channels_; }
    std::vector<UiAutomationChannel>&        channels()       { return channels_; }

    // Sample each channel at transport.bars() and write the scaled value into the
    // target node's input default -- but only when that input is unconnected (an
    // edge wins). Skips channels whose target node/port is gone or not a Float.
    void apply(Graph& graph, const Transport& transport);

private:
    std::vector<UiAutomationChannel> channels_;
    float lengthBars_ = 8.0f;
};

} // namespace oss
