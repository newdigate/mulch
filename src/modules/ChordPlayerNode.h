#pragma once
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>
#include "core/Node.h"
#include "core/Value.h"
#include "core/Chords.h"

namespace oss {

// Transport-synced chord sequencer: holds 8 patterns (root pitch-class + octave + chord
// name) and plays one at a time as a chord (simultaneous note-ons) on its MIDI output,
// switching on a quantized boundary (next Bar, or Beat). Patterns auto-progress
// (unitAbs % length) or are manually selected. Releases the prior chord's exact notes on
// every switch / stop, so nothing hangs (and the downstream Arpeggiator folds the
// note-ons into its held set). GL-free, header-only.
//
// Inputs: per pattern p (0..7): 3p root (choice), 3p+1 oct, 3p+2 chord (choice);
//   24 mode (choice Auto/Manual), 25 quantize (choice Bar/Beat), 26 length (1..8),
//   27 pattern (choice 1..8, manual selection). Output 0 = midi.
class ChordPlayerNode : public Node {
public:
    ChordPlayerNode() : Node("Chord Player") {
        for (int p = 0; p < kPatterns; ++p) {
            std::string s = std::to_string(p + 1);
            addChoiceInput("root " + s, rootNoteLabels(), 0);          // C
            addInput("oct " + s, PortType::Float, 4.0f, 0.0f, 8.0f);   // octave (C4=60)
            addChoiceInput("chord " + s, chordNames(), 0);             // maj
        }
        addChoiceInput("mode", {"Auto", "Manual"}, 0);
        addChoiceInput("quantize", {"Bar", "Beat"}, 0);
        addInput("length", PortType::Float, 8.0f, 1.0f, 8.0f);            // auto loop length
        addChoiceInput("pattern", {"1","2","3","4","5","6","7","8"}, 0);  // manual select
        addOutput("midi", PortType::Midi);
    }

    void evaluate(EvalContext& ctx) override {
        out_.clear();

        int mode      = std::clamp((int)std::lround(ctx.in<float>(kGlobals + 0)), 0, 1);
        int quantize  = std::clamp((int)std::lround(ctx.in<float>(kGlobals + 1)), 0, 1);
        int length    = std::clamp((int)std::lround(ctx.in<float>(kGlobals + 2)), 1, kPatterns);
        int manualSel = std::clamp((int)std::lround(ctx.in<float>(kGlobals + 3)), 0, kPatterns - 1);

        int beatsPerBar = (ctx.transport && ctx.transport->beatsPerBar > 0)
                              ? ctx.transport->beatsPerBar : 4;
        double unitBars = (quantize == 1) ? 1.0 / (double)beatsPerBar : 1.0;

        if (ctx.transport && ctx.transport->playing && unitBars > 0.0) {
            double    unitPos  = ctx.transport->bars() / unitBars;
            long long unitAbs  = (long long)std::floor(unitPos);
            bool      boundary = !primed_ || unitAbs != lastUnitAbs_;

            if (boundary) {
                int target = (mode == 1)
                    ? manualSel
                    : (int)(((unitAbs % length) + length) % length);
                if (!primed_ || target != activePattern_) {
                    for (int note : activeNotes_) out_.push_back(midiNoteOff(note));
                    activeNotes_.clear();
                    buildChordNotes(rootOf(ctx, target), octOf(ctx, target),
                                    chordOf(ctx, target), activeNotes_);
                    for (int note : activeNotes_) out_.push_back(midiNoteOn(note, 100));
                    activePattern_ = target;
                }
                lastUnitAbs_ = unitAbs;
                primed_ = true;
            }
        } else {
            // paused/stopped: release everything and re-prime so resume re-fires cleanly
            for (int note : activeNotes_) out_.push_back(midiNoteOff(note));
            activeNotes_.clear();
            activePattern_ = -1;
            primed_ = false;
        }

        // Status line: mode, the sounding chord, and pattern/length.
        std::string st = (mode == 1) ? "Manual" : "Auto";
        if (activePattern_ >= 0) {
            st += " · " + rootNoteLabels()[(std::size_t)rootOf(ctx, activePattern_)]
                + " " + chordNames()[(std::size_t)chordOf(ctx, activePattern_)]
                + " · ▸" + std::to_string(activePattern_ + 1)
                + "/" + std::to_string(length);
        }
        status_ = st;

        ctx.out<MidiRef>(0, MidiRef{out_.data(), out_.size()});
    }

    std::string statusLine() const override { return status_; }

private:
    int rootOf(EvalContext& ctx, int p) const {
        return std::clamp((int)std::lround(ctx.in<float>((std::size_t)(3 * p + 0))), 0, 11);
    }
    int octOf(EvalContext& ctx, int p) const {
        return std::clamp((int)std::lround(ctx.in<float>((std::size_t)(3 * p + 1))), 0, 8);
    }
    int chordOf(EvalContext& ctx, int p) const {
        return std::clamp((int)std::lround(ctx.in<float>((std::size_t)(3 * p + 2))),
                          0, (int)chordNames().size() - 1);
    }

    static constexpr int kPatterns = 8;
    static constexpr int kGlobals  = 3 * kPatterns;   // 24 (first global input index)
    std::vector<MidiEvent> out_;          // events produced this frame (owns MidiRef storage)
    std::vector<int>       activeNotes_;  // exact MIDI notes currently sounding
    int                    activePattern_ = -1;
    long long              lastUnitAbs_   = 0;
    bool                   primed_        = false;
    std::string            status_;
};

} // namespace oss
