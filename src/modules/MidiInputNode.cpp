#include "modules/MidiInputNode.h"
#include "core/Preferences.h"
#include <RtMidi.h>
#include <cstdio>
#include <vector>
#include <memory>

namespace oss {

namespace { const std::string kVirtual = "\x01virtual"; }   // sentinel for the virtual-port fallback

MidiInputNode::MidiInputNode() : Node("MIDI In") {
    addOutput("midi", PortType::Midi);
}

MidiInputNode::~MidiInputNode() { closeAll(); }

void MidiInputNode::closeAll() {
    for (RtMidiIn* mi : ins_) delete mi;   // RtMidiIn dtor closes the port
    ins_.clear();
    open_.clear();
}

static int portIndexByName(RtMidiIn& mi, const std::string& name) {
    unsigned int n = mi.getPortCount();
    for (unsigned int i = 0; i < n; ++i)
        if (mi.getPortName(i) == name) return (int)i;
    return -1;
}

void MidiInputNode::syncPorts(const Preferences* prefs) {
    std::vector<std::string> desired;
    if (prefs && !prefs->enabledMidiInputs.empty()) desired = prefs->enabledMidiInputs;
    else desired = { kVirtual };
    if (desired == open_) return;          // nothing changed
    closeAll();
    for (const std::string& name : desired) {
        try {
            auto mi = std::make_unique<RtMidiIn>(RtMidi::UNSPECIFIED, "shader-streamer");
            mi->ignoreTypes(true, true, true);
            if (name == kVirtual) {
                mi->openVirtualPort("shader-streamer in");
            } else {
                int idx = portIndexByName(*mi, name);
                if (idx < 0) continue;                   // port gone -> skip (unique_ptr frees mi)
                mi->openPort((unsigned int)idx, "in");
            }
            ins_.push_back(mi.release());                // ownership transfers only on success
        } catch (RtMidiError& err) {
            std::fprintf(stderr, "[MidiIn] open '%s' failed: %s\n", name.c_str(), err.getMessage().c_str());
        }
    }
    open_ = desired;
}

void MidiInputNode::evaluate(EvalContext& ctx) {
    events_.clear();
    syncPorts(ctx.prefs);
    std::vector<unsigned char> msg;
    for (RtMidiIn* mi : ins_) {
        for (;;) {
            mi->getMessage(&msg);
            if (msg.empty()) break;
            MidiEvent e{};
            e.status = msg[0];
            if (msg.size() > 1) e.data1 = msg[1];
            if (msg.size() > 2) e.data2 = msg[2];
            events_.push_back(e);
        }
    }
    ctx.out<MidiRef>(0, MidiRef{events_.data(), events_.size()});
}

} // namespace oss
