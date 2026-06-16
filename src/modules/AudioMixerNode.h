#pragma once
#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>
#include "core/Node.h"
#include "core/Value.h"

namespace oss {

// Four-channel audio mixer: sums four audio inputs, each scaled by its own gain,
// into one audio output. GL-free. Each frame the output length is the longest
// connected input (shorter inputs contribute silence past their end); the summed
// signal is clamped to [-1, 1] so it can't overdrive a downstream sink.
//
// Input ports interleave audio + gain so each level sits beside its source:
//   0: in 1   1: gain 1   2: in 2   3: gain 2   4: in 3 ...
class AudioMixerNode : public Node {
public:
    AudioMixerNode() : Node("Audio Mix"), buffer_(kMaxBlock, 0.0f) {
        for (int c = 0; c < kChannels; ++c) {
            addInput("in " + std::to_string(c + 1),   PortType::Audio, AudioRef{});
            addInput("gain " + std::to_string(c + 1), PortType::Float, 1.0f, 0.0f, 2.0f);
        }
        addOutput("out", PortType::Audio);
    }

    void evaluate(EvalContext& ctx) override {
        AudioRef    in[kChannels];
        float       gain[kChannels];
        std::size_t n  = 0;
        int         sr = 0;
        for (int c = 0; c < kChannels; ++c) {
            in[c]   = ctx.in<AudioRef>((std::size_t)(c * 2));
            gain[c] = ctx.in<float>((std::size_t)(c * 2 + 1));
            if (in[c].samples) {
                if (in[c].count > n) n = in[c].count;
                if (sr == 0 && in[c].sampleRate > 0) sr = in[c].sampleRate;
            }
        }
        if (sr == 0) sr = 48000;
        if (n > (std::size_t)kMaxBlock) n = (std::size_t)kMaxBlock;

        for (std::size_t i = 0; i < n; ++i) {
            float acc = 0.0f;
            for (int c = 0; c < kChannels; ++c)
                if (in[c].samples && i < in[c].count)
                    acc += gain[c] * in[c].samples[i];
            buffer_[i] = std::clamp(acc, -1.0f, 1.0f);
        }
        ctx.out<AudioRef>(0, AudioRef{buffer_.data(), n, sr});
    }

private:
    static constexpr int kChannels = 4;
    static constexpr int kMaxBlock = 8192;
    std::vector<float> buffer_;   // owns the samples the output AudioRef points at
};

} // namespace oss
