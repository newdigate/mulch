#pragma once
#include <cstddef>
#include <cmath>
#include <string>
#include <vector>
#include "core/Node.h"
#include "core/Value.h"
#include "core/MidiFile.h"
#include "core/MidiClip.h"

namespace oss {

// Streams a Standard MIDI File synced to the project transport: anchors the clip at a
// start offset (bars), optionally loops a region (bars), and mutes any of the 16 MIDI
// channels. Emits the events in each frame's beat window as a MidiRef (wire into a
// synth such as Acid Bass). The file's own tempo is ignored -- project BPM drives it.
//
// Inputs: 0 = file (String), 1 = start offset (bars), 2 = loop (Bool),
//   3 = loop length (bars), 4..19 = mute 1..16 (Bool, checked = channel muted).
class MidiFilePlayerNode : public Node {
public:
    MidiFilePlayerNode() : Node("MIDI File") {
        addAssetInput("file",    AssetType::Midi);
        addInput("start offset", PortType::Float, 0.0f, 0.0f, 64.0f);   // bars
        addInput("loop",         PortType::Bool, true);
        addIntInput("loop length", 4, 1, 64);                           // bars (whole numbers)
        for (int i = 0; i < 16; ++i)
            addInput("mute " + std::to_string(i + 1), PortType::Bool, false);
        addOutput("midi", PortType::Midi);
    }

    void evaluate(EvalContext& ctx) override {
        const std::string& path = ctx.in<std::string>(0);
        if (path != loadedPath_) {
            loadedPath_ = path;
            seq_ = loadMidiFile(path);
            status_ = seq_.ok ? ("loaded " + std::to_string(seq_.events.size()) + " events")
                              : (path.empty() ? std::string("no file") : ("error: " + seq_.error));
        }
        bool muted[16];
        for (int c = 0; c < 16; ++c) muted[c] = ctx.in<bool>(static_cast<std::size_t>(4 + c));

        double beats = ctx.transport ? ctx.transport->beats() : 0.0;
        double bpb   = ctx.transport ? (double)ctx.transport->beatsPerBar : 4.0;

        // loop length is a whole-number bars control; round so the loop is always whole bars
        // (even if a connected/automated value or an old fractional project value drives it).
        float loopLenBars = (float)std::lround(ctx.in<float>(3));
        out_ = player_.advance(seq_, beats, bpb, ctx.in<float>(1), ctx.in<bool>(2),
                               loopLenBars, muted);
        ctx.out<MidiRef>(0, MidiRef{out_.data(), out_.size()});
    }

    std::string statusLine() const override { return status_; }

private:
    std::string    loadedPath_;
    std::string    status_ = "no file";
    MidiSequence   seq_;
    MidiClipPlayer player_;
    std::vector<MidiEvent> out_;   // this frame's events (owns the MidiRef storage)
};

} // namespace oss
