#pragma once
#include <vector>
#include "core/Node.h"
#include "core/Value.h"

class RtMidiIn;   // opaque; <RtMidi.h> stays out of this header

namespace oss {

// MIDI source: reads channel messages from a hardware or virtual MIDI input port
// via RtMidi and publishes the events received this frame as a MidiRef. RtMidi
// queues incoming messages internally, so it is simply polled on the graph
// thread -- no callback thread or ring buffer needed. The port is opened lazily;
// the node emits nothing if no port can be opened.
class MidiInputNode : public Node {
public:
    MidiInputNode();
    ~MidiInputNode() override;
    void evaluate(EvalContext& ctx) override;

private:
    bool ensureStarted();
    RtMidiIn*              midiin_ = nullptr;
    std::vector<MidiEvent> events_;        // this frame's events (owns MidiRef storage)
    bool initTried_ = false;
    bool ok_        = false;
};

} // namespace oss
