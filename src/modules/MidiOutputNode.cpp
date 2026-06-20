#include "modules/MidiOutputNode.h"
#include "core/Preferences.h"
#include <RtMidi.h>
#include <cstdio>
#include <vector>
#include <memory>

namespace oss {

namespace { const std::string kVirtual = "\x01virtual"; }

MidiOutputNode::MidiOutputNode() : Node("MIDI Out") {
    addInput("midi", PortType::Midi, MidiRef{});
}

MidiOutputNode::~MidiOutputNode() { closeAll(); }

void MidiOutputNode::closeAll() {
    for (RtMidiOut* mo : outs_) {
        try {                                        // all-notes-off so nothing hangs
            for (int ch = 0; ch < 16; ++ch) {
                std::vector<unsigned char> m = {(unsigned char)(0xB0u | ch), 123, 0};
                mo->sendMessage(&m);
            }
        } catch (...) {}
        delete mo;
    }
    outs_.clear();
    open_.clear();
}

static int portIndexByName(RtMidiOut& mo, const std::string& name) {
    unsigned int n = mo.getPortCount();
    for (unsigned int i = 0; i < n; ++i)
        if (mo.getPortName(i) == name) return (int)i;
    return -1;
}

void MidiOutputNode::syncPorts(const Preferences* prefs) {
    std::vector<std::string> desired;
    if (prefs && !prefs->enabledMidiOutputs.empty()) desired = prefs->enabledMidiOutputs;
    else desired = { kVirtual };
    if (desired == open_) return;
    closeAll();
    for (const std::string& name : desired) {
        try {
            auto mo = std::make_unique<RtMidiOut>(RtMidi::UNSPECIFIED, "shader-streamer");
            if (name == kVirtual) {
                mo->openVirtualPort("shader-streamer out");
            } else {
                int idx = portIndexByName(*mo, name);
                if (idx < 0) continue;                   // port gone -> skip (unique_ptr frees mo)
                mo->openPort((unsigned int)idx, "out");
            }
            outs_.push_back(mo.release());               // ownership transfers only on success
        } catch (RtMidiError& err) {
            std::fprintf(stderr, "[MidiOut] open '%s' failed: %s\n", name.c_str(), err.getMessage().c_str());
        }
    }
    open_ = desired;
}

void MidiOutputNode::evaluate(EvalContext& ctx) {
    syncPorts(ctx.prefs);
    MidiRef in = ctx.in<MidiRef>(0);
    std::vector<unsigned char> m(3);
    for (std::size_t i = 0; i < in.count; ++i) {
        const MidiEvent& e = in.events[i];
        m[0] = e.status; m[1] = e.data1; m[2] = e.data2;
        for (RtMidiOut* mo : outs_) { try { mo->sendMessage(&m); } catch (...) {} }
    }
}

} // namespace oss
