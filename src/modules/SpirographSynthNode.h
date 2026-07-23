#pragma once
#include <cmath>
#include <cstddef>
#include <vector>
#include "core/Node.h"
#include "core/Value.h"
#include "audio/Spirograph.h"
#include "audio/AudioBlock.h"

namespace oss {

// Stereo Spirograph oscillator node: audio generated directly from hypotrochoid /
// epitrochoid curves. x -> left, y -> right. Every control is an input port (CV-able);
// wire the LFO node into `ratio` to morph the timbre. GL-free.
//
// Inputs: 0 = curve type (choice), 1 = freq, 2 = ratio, 3 = pen, 4 = phase, 5 = level.
class SpirographSynthNode : public Node {
public:
    SpirographSynthNode()
        : Node("Spirograph Synth"),
          bufL_(kAudioMaxBlock, 0.0f), bufR_(kAudioMaxBlock, 0.0f) {
        addChoiceInput("curve type", {"Hypotrochoid", "Epitrochoid"}, 0);
        addInput("freq",  PortType::Float, 110.0f, 1.0f, 1000.0f);   // Hz
        addInput("ratio", PortType::Float, 3.0f,   2.0f, 12.0f);     // R/r
        addInput("pen",   PortType::Float, 0.5f,   0.0f, 1.0f);      // d, fraction
        addInput("phase", PortType::Float, 0.0f,   0.0f, 1.0f);      // turns
        addInput("level", PortType::Float, 0.8f,   0.0f, 1.0f);
        addOutput("left",  PortType::Audio);
        addOutput("right", PortType::Audio);
        voice_.setSampleRate(sampleRate_);
    }

    void evaluate(EvalContext& ctx) override {
        voice_.setCurveType((int)std::lround(ctx.in<float>(0)));
        voice_.setFrequency(ctx.in<float>(1));
        voice_.setRatio(ctx.in<float>(2));
        voice_.setPen(ctx.in<float>(3));
        voice_.setPhase(ctx.in<float>(4));
        voice_.setLevel(ctx.in<float>(5));

        int n = audioBlockFrames(sampleRate_, ctx.dt);
        voice_.process(bufL_.data(), bufR_.data(), n);
        ctx.out<AudioRef>(0, AudioRef{bufL_.data(), (std::size_t)n, sampleRate_});
        ctx.out<AudioRef>(1, AudioRef{bufR_.data(), (std::size_t)n, sampleRate_});
    }

private:
    int sampleRate_ = 48000;
    Spirograph voice_;
    std::vector<float> bufL_, bufR_;
};

} // namespace oss
