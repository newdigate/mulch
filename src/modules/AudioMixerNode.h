#pragma once
#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>
#include "core/Node.h"
#include "core/Value.h"
#include "core/AudioPan.h"
#include "audio/AudioBlock.h"

namespace oss {

// Four-channel mixer: sums four mono audio inputs, each with its own gain and
// pan, into two mono outputs (left, right). GL-free. Panning places each mono
// source across the stereo field. Each output is clamped to [-1, 1].
//
// Input ports per channel are in / gain / pan, so each control sits by its source:
//   0: in 1   1: gain 1   2: pan 1   3: in 2   4: gain 2   5: pan 2 ...
class AudioMixerNode : public Node {
public:
    AudioMixerNode()
        : Node("Audio Mix"), bufL_(kAudioMaxBlock, 0.0f), bufR_(kAudioMaxBlock, 0.0f) {
        for (int c = 0; c < kChannels; ++c) {
            addInput("in " + std::to_string(c + 1),   PortType::Audio, AudioRef{});
            addInput("gain " + std::to_string(c + 1), PortType::Float, 1.0f, 0.0f, 2.0f);
            addInput("pan " + std::to_string(c + 1),  PortType::Float, 0.0f, -1.0f, 1.0f);
        }
        addOutput("left",  PortType::Audio);
        addOutput("right", PortType::Audio);
    }

    void evaluate(EvalContext& ctx) override {
        AudioRef in[kChannels];
        float    gain[kChannels], pan[kChannels];
        std::size_t n = 0;        // longest input, in samples (mono)
        int sr = 0;
        for (int c = 0; c < kChannels; ++c) {
            in[c]   = ctx.in<AudioRef>((std::size_t)(c * 3));
            gain[c] = ctx.in<float>((std::size_t)(c * 3 + 1));
            pan[c]  = ctx.in<float>((std::size_t)(c * 3 + 2));
            if (in[c].samples) {
                n = std::max(n, in[c].count);
                if (sr == 0 && in[c].sampleRate > 0) sr = in[c].sampleRate;
            }
        }
        if (sr == 0) sr = 48000;
        n = std::min(n, (std::size_t)kAudioMaxBlock);

        for (std::size_t i = 0; i < n; ++i) {
            float l = 0.0f, r = 0.0f;
            for (int c = 0; c < kChannels; ++c) {
                if (!in[c].samples || i >= in[c].count) continue;
                PanGains g = panGains(pan[c]);
                float s = in[c].samples[i];
                l += gain[c] * g.l * s;
                r += gain[c] * g.r * s;
            }
            bufL_[i] = std::clamp(l, -1.0f, 1.0f);
            bufR_[i] = std::clamp(r, -1.0f, 1.0f);
        }
        ctx.out<AudioRef>(0, AudioRef{bufL_.data(), n, sr});
        ctx.out<AudioRef>(1, AudioRef{bufR_.data(), n, sr});
    }

private:
    static constexpr int kChannels = 4;
    std::vector<float> bufL_, bufR_;
};

} // namespace oss
