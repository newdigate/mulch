#pragma once
#include <string>
#include <vector>
#include "core/Node.h"
#include "core/Value.h"

class RtMidiOut;   // opaque; <RtMidi.h> stays out of this header

namespace oss {

struct Preferences;

// MIDI sink: sends the channel messages on its MIDI input to a hardware or
// virtual MIDI output port via RtMidi (synchronous, on the graph thread). The
// port is opened lazily; a no-op if none can be opened. On destruction it sends
// an all-notes-off so nothing is left hanging.
class MidiOutputNode : public Node {
public:
    MidiOutputNode();
    ~MidiOutputNode() override;
    void evaluate(EvalContext& ctx) override;

private:
    void syncPorts(const Preferences* prefs);
    void closeAll();
    std::vector<RtMidiOut*>  outs_;
    std::vector<std::string> open_;
};

} // namespace oss
