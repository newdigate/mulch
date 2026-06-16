#include "modules/MidiOutputNode.h"
#include <RtMidi.h>
#include <cstdio>
#include <vector>

namespace oss {

MidiOutputNode::MidiOutputNode() : Node("MIDI Out") {
    addInput("midi", PortType::Midi, MidiRef{});
}

MidiOutputNode::~MidiOutputNode() {
    if (midiout_ && lazy_.ok()) {
        // Politely silence anything still sounding (all-notes-off, channels 1-16).
        try {
            for (int ch = 0; ch < 16; ++ch) {
                std::vector<unsigned char> m = {(unsigned char)(0xB0u | ch), 123, 0};
                midiout_->sendMessage(&m);
            }
        } catch (...) {}
    }
    delete midiout_;
}

void MidiOutputNode::evaluate(EvalContext& ctx) {
    if (!ensureStarted()) return;
    MidiRef in = ctx.in<MidiRef>(0);
    std::vector<unsigned char> m(3);
    for (std::size_t i = 0; i < in.count; ++i) {
        const MidiEvent& e = in.events[i];
        m[0] = e.status; m[1] = e.data1; m[2] = e.data2;
        try { midiout_->sendMessage(&m); } catch (...) {}
    }
}

bool MidiOutputNode::ensureStarted() {
    return lazy_.ensure([this] { return openDevice(); });
}

bool MidiOutputNode::openDevice() {
    try {
        midiout_ = new RtMidiOut(RtMidi::UNSPECIFIED, "shader-streamer");
        if (midiout_->getPortCount() > 0) {
            midiout_->openPort(0, "out");
            std::fprintf(stderr, "[MidiOut] opened port 0: %s\n",
                         midiout_->getPortName(0).c_str());
        } else {
            midiout_->openVirtualPort("shader-streamer out");
            std::fprintf(stderr, "[MidiOut] no ports found; opened a virtual output port\n");
        }
        return true;
    } catch (RtMidiError& err) {
        std::fprintf(stderr, "[MidiOut] init failed: %s\n", err.getMessage().c_str());
        return false;
    }
}

} // namespace oss
