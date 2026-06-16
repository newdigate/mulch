#include "modules/MidiInputNode.h"
#include <RtMidi.h>
#include <cstdio>
#include <vector>

namespace oss {

MidiInputNode::MidiInputNode() : Node("MIDI In") {
    addOutput("midi", PortType::Midi);
}

MidiInputNode::~MidiInputNode() {
    delete midiin_;   // RtMidiIn's destructor closes the port
}

void MidiInputNode::evaluate(EvalContext& ctx) {
    events_.clear();
    if (!ensureStarted()) { ctx.out<MidiRef>(0, MidiRef{}); return; }

    // Drain RtMidi's internal queue (getMessage is non-blocking and returns an
    // empty message once the queue is empty).
    std::vector<unsigned char> msg;
    for (;;) {
        midiin_->getMessage(&msg);
        if (msg.empty()) break;
        MidiEvent e{};
        e.status = msg[0];
        if (msg.size() > 1) e.data1 = msg[1];
        if (msg.size() > 2) e.data2 = msg[2];
        events_.push_back(e);
    }
    ctx.out<MidiRef>(0, MidiRef{events_.data(), events_.size()});
}

bool MidiInputNode::ensureStarted() {
    if (initTried_) return ok_;
    initTried_ = true;
    try {
        midiin_ = new RtMidiIn(RtMidi::UNSPECIFIED, "shader-streamer");
        midiin_->ignoreTypes(true, true, true);   // skip sysex / timing / active sensing
        if (midiin_->getPortCount() > 0) {
            midiin_->openPort(0, "in");
            std::fprintf(stderr, "[MidiIn] opened port 0: %s\n",
                         midiin_->getPortName(0).c_str());
        } else {
            midiin_->openVirtualPort("shader-streamer in");
            std::fprintf(stderr, "[MidiIn] no ports found; opened a virtual input port\n");
        }
        ok_ = true;
    } catch (RtMidiError& err) {
        std::fprintf(stderr, "[MidiIn] init failed: %s\n", err.getMessage().c_str());
        ok_ = false;
    }
    return ok_;
}

} // namespace oss
