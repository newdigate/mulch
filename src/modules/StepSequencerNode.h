#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>
#include "core/Node.h"
#include "core/Value.h"
#include "core/StepSync.h"

namespace oss {

// 16-step drum sequencer -- a very basic TR-909 voice. A playhead advances through
// 16 on/off steps, triggering a single note on a single channel each time it lands
// on an enabled step. Outputs MIDI. GL-free.
//
// Two timing modes:
//   - free (sync off): the playhead runs off ctx.dt at the `tempo` BPM (16th notes).
//   - sync (sync on):  each step is derived from the transport bar position at the
//     selected `rate sync` division, so it locks to the project tempo, starts/stops
//     with the transport, and stays bar-aligned (loop-robust).
//
// Inputs: 0..15 = step toggles (Bool), 16 = tempo (BPM), 17 = note, 18 = channel,
//         19 = sync (Bool), 20 = rate sync (choice: step division).
class StepSequencerNode : public Node {
public:
    StepSequencerNode() : Node("Step Seq") {
        for (int i = 0; i < kSteps; ++i)
            addInput(std::to_string(i + 1), PortType::Bool, (i % 4) == 0);  // four-on-the-floor
        addInput("tempo",   PortType::Float, 120.0f, 40.0f, 240.0f);   // BPM (free mode)
        addInput("note",    PortType::Float, 36.0f,   0.0f, 127.0f);   // 36 = GM kick
        addInput("channel", PortType::Float, 9.0f,    0.0f,  15.0f);   // 9 = GM drums (ch 10)
        addInput("sync",    PortType::Bool, false);                    // lock to transport BPM
        addChoiceInput("rate sync", stepDivisionLabels(), kStepDivisionDefault);  // step length
        addOutput("midi", PortType::Midi);
    }

    void evaluate(EvalContext& ctx) override {
        out_.clear();

        int  note    = std::clamp((int)std::lround(ctx.in<float>(kSteps + 1)), 0, 127);
        int  channel = std::clamp((int)std::lround(ctx.in<float>(kSteps + 2)), 0, 15);
        bool sync    = ctx.in<bool>(kSteps + 3);
        int  div     = std::clamp((int)std::lround(ctx.in<float>(kSteps + 4)), 0, 7);

        if (sync) {
            // --- transport-synced: derive the step from the bar position ---
            const double barsPerStep = stepDivisionBars(div);
            if (ctx.transport && ctx.transport->playing && barsPerStep > 0.0) {
                double    stepPos  = ctx.transport->bars() / barsPerStep;
                long long stepAbs  = (long long)std::floor(stepPos);
                double    frac     = stepPos - (double)stepAbs;
                bool      boundary = !primed_ || stepAbs != lastStepAbs_;

                if (active_ >= 0 && (boundary || frac >= kGate)) {
                    out_.push_back(midiNoteOff(active_, activeCh_));
                    active_ = -1;
                }
                if (boundary) {
                    int s = (int)(((stepAbs % kSteps) + kSteps) % kSteps);
                    if (ctx.in<bool>((std::size_t)s)) {
                        out_.push_back(midiNoteOn(note, 100, channel));
                        active_ = note; activeCh_ = channel;
                    }
                    lastStepAbs_ = stepAbs;
                    primed_ = true;
                }
            } else {
                // paused/stopped: release any held note and re-prime so resume fires cleanly
                if (active_ >= 0) { out_.push_back(midiNoteOff(active_, activeCh_)); active_ = -1; }
                primed_ = false;
            }
        } else {
            // --- free-running: tempo-based clock (unchanged) ---
            float tempo = ctx.in<float>(kSteps + 0);
            if (tempo < 1.0f) tempo = 1.0f;
            const double period = 15.0 / tempo;   // seconds per 16th note (4 steps per beat)

            clock_ += ctx.dt;
            if (active_ >= 0 && clock_ >= noteOff_) {
                out_.push_back(midiNoteOff(active_, activeCh_));
                active_ = -1;
            }
            while (clock_ >= nextStep_) {
                if (active_ >= 0) { out_.push_back(midiNoteOff(active_, activeCh_)); active_ = -1; }
                if (ctx.in<bool>((std::size_t)step_)) {
                    out_.push_back(midiNoteOn(note, 100, channel));
                    active_   = note;
                    activeCh_ = channel;
                    noteOff_  = nextStep_ + kGate * period;
                }
                step_ = (step_ + 1) % kSteps;
                nextStep_ += period;
            }
        }

        ctx.out<MidiRef>(0, MidiRef{out_.data(), out_.size()});
    }

private:
    static constexpr int    kSteps = 16;
    static constexpr double kGate  = 0.5;   // note length as a fraction of a step
    std::vector<MidiEvent> out_;            // events produced this frame (owns MidiRef storage)
    double clock_    = 0.0;
    double nextStep_ = 0.0;
    double noteOff_  = 0.0;
    int    step_     = 0;                   // next step to trigger (free mode)
    int    active_   = -1;                  // currently sounding note, or -1
    int    activeCh_ = 0;
    long long lastStepAbs_ = 0;             // last synced step index (sync mode)
    bool      primed_      = false;         // synced playback has fired its first step
};

} // namespace oss
