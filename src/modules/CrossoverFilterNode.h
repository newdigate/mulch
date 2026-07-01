#pragma once
#include <algorithm>
#include <cstddef>
#include <vector>
#include "core/Node.h"
#include "core/Value.h"
#include "audio/AudioBlock.h"
#include "audio/StateVariableFilter.h"

namespace oss {

// 3-band crossover filter: one mono input split into bass / mid / treble mono outputs
// by two cascaded state-variable crossovers. Float inputs: low cutoff + its resonance
// (bass | rest split), high cutoff + its resonance (mid | treble split). GL-free;
// filter state persists across per-frame blocks like a real-time filter.
class CrossoverFilterNode : public Node {
public:
    CrossoverFilterNode()
        : Node("Crossover Filter"),
          bass_(kAudioMaxBlock, 0.0f), mid_(kAudioMaxBlock, 0.0f), treble_(kAudioMaxBlock, 0.0f) {
        addInput("mono",           PortType::Audio, AudioRef{});
        addInput("low cutoff",     PortType::Float, 200.0f,  20.0f, 20000.0f);
        addInput("low resonance",  PortType::Float, 0.2f,    0.0f,  1.0f);
        addInput("high cutoff",    PortType::Float, 2000.0f, 20.0f, 20000.0f);
        addInput("high resonance", PortType::Float, 0.2f,    0.0f,  1.0f);
        addOutput("bass",   PortType::Audio);
        addOutput("mid",    PortType::Audio);
        addOutput("treble", PortType::Audio);
    }

    void evaluate(EvalContext& ctx) override {
        AudioRef m = ctx.in<AudioRef>(0);
        std::size_t n = m.samples ? std::min(m.count, (std::size_t)kAudioMaxBlock) : 0;
        int sr = (m.samples && m.sampleRate > 0) ? m.sampleRate : 48000;

        low_.setCoefficients (ctx.in<float>(1), ctx.in<float>(2), sr);   // low cutoff / low res
        high_.setCoefficients(ctx.in<float>(3), ctx.in<float>(4), sr);   // high cutoff / high res

        for (std::size_t i = 0; i < n; ++i) {
            SvfOut a = low_.process(m.samples[i]);   // split at the low cutoff
            bass_[i] = a.low;                         // below low cutoff
            SvfOut b = high_.process(a.high);         // split the remainder at the high cutoff
            mid_[i]    = b.low;                        // between the two cutoffs
            treble_[i] = b.high;                       // above high cutoff
        }

        ctx.out<AudioRef>(0, AudioRef{bass_.data(),   n, sr});
        ctx.out<AudioRef>(1, AudioRef{mid_.data(),    n, sr});
        ctx.out<AudioRef>(2, AudioRef{treble_.data(), n, sr});
    }

private:
    StateVariableFilter low_, high_;
    std::vector<float> bass_, mid_, treble_;
};

} // namespace oss
