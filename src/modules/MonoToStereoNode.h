#pragma once
#include <algorithm>
#include <cstddef>
#include <vector>
#include "core/Node.h"
#include "core/Value.h"
#include "core/AudioPan.h"

namespace oss {

// Bridge: places a mono signal in the stereo field, producing two mono outputs
// (left, right). `pan` -1..1 (0 = centre). GL-free.
class MonoToStereoNode : public Node {
public:
    MonoToStereoNode() : Node("Mono to Stereo"), bufL_(kMaxBlock, 0.0f), bufR_(kMaxBlock, 0.0f) {
        addInput("mono", PortType::Audio, AudioRef{});
        addInput("pan",  PortType::Float, 0.0f, -1.0f, 1.0f);
        addOutput("left",  PortType::Audio);
        addOutput("right", PortType::Audio);
    }
    void evaluate(EvalContext& ctx) override {
        AudioRef m = ctx.in<AudioRef>(0);
        PanGains g = panGains(ctx.in<float>(1));
        std::size_t n = m.samples ? std::min(m.count, (std::size_t)kMaxBlock) : 0;
        int sr = (m.samples && m.sampleRate > 0) ? m.sampleRate : 48000;
        for (std::size_t i = 0; i < n; ++i) {
            float s = m.samples[i];
            bufL_[i] = g.l * s;
            bufR_[i] = g.r * s;
        }
        ctx.out<AudioRef>(0, AudioRef{bufL_.data(), n, sr});
        ctx.out<AudioRef>(1, AudioRef{bufR_.data(), n, sr});
    }
private:
    static constexpr int kMaxBlock = 8192;
    std::vector<float> bufL_, bufR_;
};

} // namespace oss
