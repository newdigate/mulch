#pragma once
#include <algorithm>
#include <cmath>
#include <set>
#include <vector>
#include "core/Node.h"
#include "core/Value.h"
#include "core/StepSync.h"

namespace oss {

// Arpeggiator: tracks the notes currently held on its MIDI input and steps through
// them, emitting generated note-on/note-off events on its MIDI output. GL-free.
//
// Two timing modes:
//   - free (sync off): steps at `rate` steps per second off ctx.dt.
//   - sync (sync on):  each step is derived from the transport bar position at the
//     selected `rate sync` division, locking to the project tempo (loop-robust).
//
// Inputs: 0 = midi, 1 = rate, 2 = gate, 3 = octaves, 4 = mode,
//         5 = sync (Bool), 6 = rate sync (choice: step division).
class ArpeggiatorNode : public Node {
public:
    ArpeggiatorNode() : Node("Arpeggiator") {
        addInput("midi",    PortType::Midi, MidiRef{});
        addInput("rate",    PortType::Float, 8.0f, 0.5f, 20.0f);   // steps per second (free)
        addInput("gate",    PortType::Float, 0.5f, 0.05f, 1.0f);   // note length fraction
        addInput("octaves", PortType::Float, 1.0f, 1.0f, 4.0f);    // span (rounded)
        addInput("mode",    PortType::Float, 0.0f, 0.0f, 2.0f);    // 0 up, 1 down, 2 up-down
        addInput("sync",    PortType::Bool, false);                // lock to transport BPM
        addChoiceInput("rate sync", stepDivisionLabels(), kStepDivisionDefault);  // step length
        addOutput("midi", PortType::Midi);
    }

    void evaluate(EvalContext& ctx) override {
        out_.clear();

        // 1) Fold incoming note on/off into the held-note set.
        MidiRef in = ctx.in<MidiRef>(0);
        for (std::size_t i = 0; i < in.count; ++i) {
            const MidiEvent& e = in.events[i];
            if (midiIsNoteOn(e))       held_.insert(e.data1);
            else if (midiIsNoteOff(e)) held_.erase(e.data1);
        }

        float gate    = std::clamp(ctx.in<float>(2), 0.0f, 1.0f);
        int   octaves = std::clamp((int)std::lround(ctx.in<float>(3)), 1, 4);
        int   mode    = std::clamp((int)std::lround(ctx.in<float>(4)), 0, 2);
        bool  sync    = ctx.in<bool>(5);
        int   div     = std::clamp((int)std::lround(ctx.in<float>(6)), 0, 7);

        std::vector<int> seq = buildSequence(octaves, mode);

        if (sync) {
            // --- transport-synced: derive the step from the bar position ---
            const double barsPerStep = stepDivisionBars(div);
            if (ctx.transport && ctx.transport->playing && barsPerStep > 0.0) {
                double    stepPos  = ctx.transport->bars() / barsPerStep;
                long long stepAbs  = (long long)std::floor(stepPos);
                double    frac     = stepPos - (double)stepAbs;
                bool      boundary = !primed_ || stepAbs != lastStepAbs_;

                if (active_ >= 0 && (boundary || frac >= gate)) {
                    out_.push_back(midiNoteOff(active_));
                    active_ = -1;
                }
                if (boundary) {
                    if (!seq.empty()) {
                        int note = seq[step_ % seq.size()];
                        out_.push_back(midiNoteOn(note, 100));
                        active_ = note;
                        ++step_;
                    }
                    lastStepAbs_ = stepAbs;
                    primed_ = true;
                }
            } else {
                // paused/stopped: release any held note and re-prime so resume fires cleanly
                if (active_ >= 0) { out_.push_back(midiNoteOff(active_)); active_ = -1; }
                primed_ = false;
            }
        } else {
            // --- free-running: rate (steps/sec) clock (unchanged) ---
            float rate = ctx.in<float>(1);
            if (rate < 0.01f) rate = 0.01f;
            const double period = 1.0 / rate;

            clock_ += ctx.dt;
            if (active_ >= 0 && clock_ >= noteOff_) {
                out_.push_back(midiNoteOff(active_));
                active_ = -1;
            }
            while (clock_ >= nextStep_) {
                if (active_ >= 0) { out_.push_back(midiNoteOff(active_)); active_ = -1; }
                if (!seq.empty()) {
                    int note = seq[step_ % seq.size()];
                    out_.push_back(midiNoteOn(note, 100));
                    active_  = note;
                    noteOff_ = nextStep_ + gate * period;
                    ++step_;
                } else {
                    step_ = 0;          // nothing held -- keep the clock in sync, emit nothing
                }
                nextStep_ += period;
            }
        }

        ctx.out<MidiRef>(0, MidiRef{out_.data(), out_.size()});
    }

private:
    // The ordered note sequence for one arpeggio cycle, given the held notes.
    std::vector<int> buildSequence(int octaves, int mode) const {
        std::vector<int> up;
        for (int o = 0; o < octaves; ++o)
            for (int n : held_) up.push_back(n + 12 * o);   // held_ iterates ascending
        if (mode == 1) { std::reverse(up.begin(), up.end()); return up; }   // down
        if (mode == 2 && up.size() > 1) {                                   // up-down
            std::vector<int> ud = up;
            for (int i = (int)up.size() - 2; i >= 1; --i) ud.push_back(up[i]);
            return ud;
        }
        return up;   // up (mode 0), or a single note
    }

    std::set<int>          held_;          // MIDI note numbers currently held
    std::vector<MidiEvent> out_;           // events produced this frame (owns MidiRef storage)
    double clock_    = 0.0;                // seconds since start (free mode)
    double nextStep_ = 0.0;                // clock time of the next note-on (free mode)
    double noteOff_  = 0.0;                // clock time to release the active note (free mode)
    int    step_     = 0;                  // index into the step sequence
    int    active_   = -1;                 // currently sounding note, or -1
    long long lastStepAbs_ = 0;            // last synced step index (sync mode)
    bool      primed_      = false;        // synced playback has fired its first step
};

} // namespace oss
