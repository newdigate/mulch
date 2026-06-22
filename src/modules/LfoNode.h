#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <random>
#include <string>
#include <vector>
#include "core/Node.h"
#include "core/Value.h"

namespace oss {

// LFO period per sync division, slow -> fast (bars per cycle). Index matches the
// "rate sync" choice labels in the LfoNode constructor.
static constexpr double kLfoDivisionBars[15] = {
    32.0, 24.0, 16.0, 12.0, 8.0, 6.0, 4.0, 2.0, 1.0,
    0.5, 0.25, 0.125, 0.0625, 0.03125, 0.015625
};

// One LFO cycle's normalised value in [0,1] at phase p in [0,1), for the
// deterministic waveforms. Sample & Hold (index 5) is stateful and supplied by the
// node, so it returns 0 here.
inline double lfoSample(int wf, double p) {
    constexpr double kTwoPi = 6.283185307179586;
    switch (wf) {
        case 0: return 0.5 + 0.5 * std::sin(kTwoPi * p);   // Sine
        case 1: return p < 0.5 ? 2.0 * p : 2.0 - 2.0 * p;  // Triangle (0 -> 1 -> 0)
        case 2: return p < 0.5 ? 1.0 : 0.0;                // Square (50% duty)
        case 3: return p;                                  // Ramp Up
        case 4: return 1.0 - p;                            // Ramp Down
        default: return 0.0;                               // Sample & Hold: node supplies
    }
}

// Low-frequency oscillator: a control-rate Float modulation source. Pick a waveform
// and run it free (rate in Hz) or synced to the transport (rate in bar divisions);
// the [0,1] waveform is mapped into a [min,max] output range. Every control is an
// input port, so waveform/rate/sync can each be driven by another node -- LFOs
// chain. Free mode integrates rate*dt every frame (runs even while stopped); sync
// mode derives phase from transport.bars() (locked to song position). GL-free.
class LfoNode : public Node {
public:
    LfoNode() : Node("LFO") {
        addChoiceInput("waveform",
            {"Sine", "Triangle", "Square", "Ramp Up", "Ramp Down", "Sample & Hold"}, 0);
        addInput("sync", PortType::Bool, false);
        addInput("rate Hz", PortType::Float, 1.0f, 0.01f, 40.0f);
        addChoiceInput("rate sync",
            {"32 bars", "24 bars", "16 bars", "12 bars", "8 bars", "6 bars", "4 bars",
             "2 bars", "1 bar", "1/2 bar", "1/4 bar", "1/8 bar", "1/16 bar",
             "1/32 bar", "1/64 bar"}, 8);
        addInput("min", PortType::Float, 0.0f, -1.0f, 1.0f);
        addInput("max", PortType::Float, 1.0f, -1.0f, 1.0f);
        addInput("amplify", PortType::Float, 1.0f, 0.0f, 4000.0f);   // gain for the amplified output
        addOutput("out", PortType::Float);
        addOutput("amplified", PortType::Float);                   // = out * amplify
        shVal_ = uni_(rng_);   // seed the first Sample & Hold value
    }

    void evaluate(EvalContext& ctx) override {
        int   wf   = std::clamp((int)std::lround(ctx.in<float>(0)), 0, 5);
        bool  sync = ctx.in<bool>(1);
        float lo   = ctx.in<float>(4);
        float hi   = ctx.in<float>(5);
        double phase01 = 0.0;
        bool   newCycle = false;

        if (sync) {
            int div = std::clamp((int)std::lround(ctx.in<float>(3)), 0, 14);
            double periodBars = kLfoDivisionBars[div];
            double bars   = ctx.transport ? ctx.transport->bars() : 0.0;
            double cycles = periodBars > 0.0 ? bars / periodBars : 0.0;
            long long cyc = (long long)std::floor(cycles);
            phase01 = cycles - (double)cyc;
            if (cyc != lastCycle_) { newCycle = true; lastCycle_ = cyc; }
        } else {
            double hz = (double)ctx.in<float>(2);
            phase_ += hz * (double)ctx.dt;
            if (phase_ >= 1.0) newCycle = true;
            phase_ -= std::floor(phase_);   // wrap to [0,1)
            phase01 = phase_;
        }

        if (newCycle) shVal_ = uni_(rng_);   // re-latch Sample & Hold each cycle

        double w01 = (wf == 5) ? shVal_ : lfoSample(wf, phase01);
        float out0 = (float)(lo + w01 * (hi - lo));
        ctx.out<float>(0, out0);                       // "out"       -- normal, exactly as today
        ctx.out<float>(1, out0 * ctx.in<float>(6));    // "amplified" = out0 * amplify
    }

private:
    double    phase_     = 0.0;     // free-run phase in [0,1)
    long long lastCycle_ = 0;       // sync cycle index, for Sample & Hold
    double    shVal_     = 0.0;      // held Sample & Hold value in [0,1]
    std::mt19937 rng_{0x9E3779B9u};  // fixed seed -> deterministic / testable
    std::uniform_real_distribution<double> uni_{0.0, 1.0};
};

} // namespace oss
