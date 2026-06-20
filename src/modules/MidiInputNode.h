#pragma once
#include <string>
#include <vector>
#include "core/Node.h"
#include "core/Value.h"

class RtMidiIn;   // opaque; <RtMidi.h> stays out of this header

namespace oss {

struct Preferences;

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
    void syncPorts(const Preferences* prefs);     // open/close to match the enabled set
    void closeAll();
    std::vector<RtMidiIn*>   ins_;                 // one per open port
    std::vector<std::string> open_;                // descriptor of the current open set
    std::vector<MidiEvent>   events_;
};

} // namespace oss
