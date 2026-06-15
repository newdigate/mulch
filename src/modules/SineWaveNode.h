#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>
#include "core/Node.h"
#include "core/Value.h"

namespace oss {

// Pure sine-wave audio source. Each frame it generates a phase-continuous block
// of samples into an owned buffer and publishes it as an AudioRef on output 0,
// so it can drive any audio input (e.g. the Spectrograph). GL-free: no shaders,
// no framebuffers -- it only produces samples.
class SineWaveNode : public Node {
public:
    SineWaveNode() : Node("Sine"), buffer_(kMaxBlock, 0.0f) {
        addInput("freq", PortType::Float, 220.0f, 20.0f, 2000.0f);  // Hz
        addInput("amp",  PortType::Float, 0.8f,   0.0f,  1.0f);
        addOutput("audio", PortType::Audio);
    }

    void evaluate(EvalContext& ctx) override {
        const float  freq = ctx.in<float>(0);
        const float  amp  = ctx.in<float>(1);
        // Generate one frame's worth of samples; cap to the buffer so a long
        // frame can't overrun it (the consumer windows what it needs).
        const int n = std::clamp(
            (int)std::lround(sampleRate_ * (double)ctx.dt), 1, kMaxBlock);
        const double inc = kTwoPi * (double)freq / sampleRate_;
        for (int i = 0; i < n; ++i) {
            buffer_[i] = static_cast<float>(amp * std::sin(phase_));
            phase_ += inc;                       // phase persists -> no clicks
            while (phase_ >= kTwoPi) phase_ -= kTwoPi;
        }
        ctx.out<AudioRef>(0, AudioRef{buffer_.data(), (std::size_t)n, sampleRate_});
    }

private:
    static constexpr int    kMaxBlock = 1024;   // max samples produced per frame
    static constexpr double kTwoPi    = 6.283185307179586;
    int                sampleRate_ = 48000;
    double             phase_      = 0.0;        // radians, accumulated for continuity
    std::vector<float> buffer_;                  // owns the samples AudioRef points at
};

} // namespace oss
