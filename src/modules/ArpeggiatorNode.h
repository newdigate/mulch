#pragma once
#include <algorithm>
#include <cmath>
#include <set>
#include <vector>
#include "core/Node.h"
#include "core/Value.h"

namespace oss {

// Arpeggiator: tracks the notes currently held on its MIDI input and steps
// through them at a fixed rate, emitting generated note-on/note-off events on
// its MIDI output. GL-free and deterministic -- timing advances purely with
// ctx.dt, so the same input + dt sequence always yields the same output.
class ArpeggiatorNode : public Node {
public:
    ArpeggiatorNode() : Node("Arpeggiator") {
        addInput("midi",    PortType::Midi, MidiRef{});
        addInput("rate",    PortType::Float, 8.0f, 0.5f, 20.0f);   // steps per second
        addInput("gate",    PortType::Float, 0.5f, 0.05f, 1.0f);   // note length fraction
        addInput("octaves", PortType::Float, 1.0f, 1.0f, 4.0f);    // span (rounded)
        addInput("mode",    PortType::Float, 0.0f, 0.0f, 2.0f);    // 0 up, 1 down, 2 up-down
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

        float rate    = ctx.in<float>(1);
        float gate    = ctx.in<float>(2);
        int   octaves = std::clamp((int)std::lround(ctx.in<float>(3)), 1, 4);
        int   mode    = std::clamp((int)std::lround(ctx.in<float>(4)), 0, 2);
        if (rate < 0.01f) rate = 0.01f;
        gate = std::clamp(gate, 0.0f, 1.0f);
        const double period = 1.0 / rate;

        clock_ += ctx.dt;

        // 2) Release the current note once its gate time elapses.
        if (active_ >= 0 && clock_ >= noteOff_) {
            out_.push_back(midiNoteOff(active_));
            active_ = -1;
        }

        // 3) Cross step boundaries (a while-loop so one long frame spanning
        //    several steps still produces every note in order).
        std::vector<int> seq = buildSequence(octaves, mode);
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
    double clock_    = 0.0;                // seconds since start
    double nextStep_ = 0.0;                // clock time of the next note-on
    double noteOff_  = 0.0;                // clock time to release the active note
    int    step_     = 0;                  // index into the step sequence
    int    active_   = -1;                 // currently sounding note, or -1
};

} // namespace oss
