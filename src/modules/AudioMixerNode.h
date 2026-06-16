#pragma once
#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>
#include "core/Node.h"
#include "core/Value.h"

namespace oss {

// Four-channel stereo mixer: sums four audio inputs, each with its own gain and
// pan, into one interleaved stereo output (L,R,L,R). GL-free. Panning a mono
// input places it across the stereo field, so this is how mono sources become a
// real stereo signal. The summed signal is clamped to [-1, 1] per channel.
//
// Input ports per channel are in / gain / pan, so each control sits by its source:
//   0: in 1   1: gain 1   2: pan 1   3: in 2   4: gain 2   5: pan 2 ...
class AudioMixerNode : public Node {
public:
    AudioMixerNode() : Node("Audio Mix"), buffer_(kMaxBlock * 2, 0.0f) {
        for (int c = 0; c < kChannels; ++c) {
            addInput("in " + std::to_string(c + 1),   PortType::Audio, AudioRef{});
            addInput("gain " + std::to_string(c + 1), PortType::Float, 1.0f, 0.0f, 2.0f);
            addInput("pan " + std::to_string(c + 1),  PortType::Float, 0.0f, -1.0f, 1.0f);
        }
        addOutput("out", PortType::Audio);   // interleaved stereo
    }

    void evaluate(EvalContext& ctx) override {
        AudioRef in[kChannels];
        float    gain[kChannels], pan[kChannels];
        std::size_t n = 0;        // longest input, in frames
        int sr = 0;
        for (int c = 0; c < kChannels; ++c) {
            in[c]   = ctx.in<AudioRef>((std::size_t)(c * 3));
            gain[c] = ctx.in<float>((std::size_t)(c * 3 + 1));
            pan[c]  = ctx.in<float>((std::size_t)(c * 3 + 2));
            if (in[c].samples) {
                n = std::max(n, in[c].frames());
                if (sr == 0 && in[c].sampleRate > 0) sr = in[c].sampleRate;
            }
        }
        if (sr == 0) sr = 48000;
        n = std::min(n, (std::size_t)kMaxBlock);

        for (std::size_t i = 0; i < n; ++i) {
            float l = 0.0f, r = 0.0f;
            for (int c = 0; c < kChannels; ++c) {
                if (!in[c].samples || i >= in[c].frames()) continue;
                // Balance pan: centre keeps both, hard pan mutes the far side.
                float lg = gain[c] * (1.0f - std::max(0.0f, pan[c]));
                float rg = gain[c] * (1.0f + std::min(0.0f, pan[c]));
                if (in[c].channels == 2) {
                    l += lg * in[c].samples[i * 2];
                    r += rg * in[c].samples[i * 2 + 1];
                } else {                                  // mono -> placed by pan
                    float s = in[c].samples[i];
                    l += lg * s;
                    r += rg * s;
                }
            }
            buffer_[i * 2]     = std::clamp(l, -1.0f, 1.0f);
            buffer_[i * 2 + 1] = std::clamp(r, -1.0f, 1.0f);
        }
        ctx.out<AudioRef>(0, AudioRef{buffer_.data(), n * 2, sr, 2});
    }

private:
    static constexpr int kChannels = 4;
    static constexpr int kMaxBlock = 8192;   // frames
    std::vector<float> buffer_;               // interleaved stereo the output points at
};

} // namespace oss
