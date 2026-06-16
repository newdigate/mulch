#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>
#include "core/Node.h"
#include "core/Value.h"

namespace oss {

// 16-step drum sequencer -- a very basic TR-909 voice. A playhead advances
// through 16 on/off steps at a tempo (16th notes), triggering a single note on
// a single channel each time it lands on an enabled step. Outputs MIDI. GL-free
// and deterministic (timing advances with ctx.dt).
//
// Inputs: 0..15 = step toggles (Bool), 16 = tempo (BPM), 17 = note, 18 = channel.
// The 16 step toggles get inline checkboxes from the standard Bool widget.
class StepSequencerNode : public Node {
public:
    StepSequencerNode() : Node("Step Seq") {
        for (int i = 0; i < kSteps; ++i)
            addInput(std::to_string(i + 1), PortType::Bool, (i % 4) == 0);  // four-on-the-floor
        addInput("tempo",   PortType::Float, 120.0f, 40.0f, 240.0f);   // BPM
        addInput("note",    PortType::Float, 36.0f,   0.0f, 127.0f);   // 36 = GM kick
        addInput("channel", PortType::Float, 9.0f,    0.0f,  15.0f);   // 9 = GM drums (ch 10)
        addOutput("midi", PortType::Midi);
    }

    void evaluate(EvalContext& ctx) override {
        out_.clear();

        float tempo   = ctx.in<float>(kSteps + 0);
        int   note    = std::clamp((int)std::lround(ctx.in<float>(kSteps + 1)), 0, 127);
        int   channel = std::clamp((int)std::lround(ctx.in<float>(kSteps + 2)), 0, 15);
        if (tempo < 1.0f) tempo = 1.0f;
        const double period = 15.0 / tempo;   // seconds per 16th note (4 steps per beat)

        clock_ += ctx.dt;

        // Release the active note once its gate elapses.
        if (active_ >= 0 && clock_ >= noteOff_) {
            out_.push_back(midiNoteOff(active_, activeCh_));
            active_ = -1;
        }

        // Advance the playhead across step boundaries (while-loop so one long
        // frame spanning several steps still triggers each enabled one).
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

        ctx.out<MidiRef>(0, MidiRef{out_.data(), out_.size()});
    }

private:
    static constexpr int    kSteps = 16;
    static constexpr double kGate  = 0.5;   // note length as a fraction of a step
    std::vector<MidiEvent> out_;            // events produced this frame (owns MidiRef storage)
    double clock_    = 0.0;
    double nextStep_ = 0.0;
    double noteOff_  = 0.0;
    int    step_     = 0;                   // next step to trigger
    int    active_   = -1;                  // currently sounding note, or -1
    int    activeCh_ = 0;
};

} // namespace oss
