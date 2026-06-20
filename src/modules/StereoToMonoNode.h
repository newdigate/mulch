#pragma once
#include <algorithm>
#include <cstddef>
#include <vector>
#include "core/Node.h"
#include "core/Value.h"
#include "core/AudioPan.h"

namespace oss {

// Bridge: downmixes a stereo pair (two mono inputs left, right) to one mono
// output. `balance` -1..1 (0 = equal average). Output clamped to [-1,1]. GL-free.
class StereoToMonoNode : public Node {
public:
    StereoToMonoNode() : Node("Stereo to Mono"), buf_(kMaxBlock, 0.0f) {
        addInput("left",    PortType::Audio, AudioRef{});
        addInput("right",   PortType::Audio, AudioRef{});
        addInput("balance", PortType::Float, 0.0f, -1.0f, 1.0f);
        addOutput("mono", PortType::Audio);
    }
    void evaluate(EvalContext& ctx) override {
        AudioRef L = ctx.in<AudioRef>(0);
        AudioRef R = ctx.in<AudioRef>(1);
        PanGains g = downmixGains(ctx.in<float>(2));
        std::size_t nL = L.samples ? L.count : 0;
        std::size_t nR = R.samples ? R.count : 0;
        std::size_t n  = std::min(std::max(nL, nR), (std::size_t)kMaxBlock);
        int sr = (L.samples && L.sampleRate > 0) ? L.sampleRate
               : (R.samples && R.sampleRate > 0) ? R.sampleRate : 48000;
        for (std::size_t i = 0; i < n; ++i) {
            float l = (i < nL) ? L.samples[i] : 0.0f;
            float r = (i < nR) ? R.samples[i] : 0.0f;
            buf_[i] = std::clamp(g.l * l + g.r * r, -1.0f, 1.0f);
        }
        ctx.out<AudioRef>(0, AudioRef{buf_.data(), n, sr});
    }
private:
    static constexpr int kMaxBlock = 8192;
    std::vector<float> buf_;
};

} // namespace oss
