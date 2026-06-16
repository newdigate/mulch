#pragma once
#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>
#include "core/Node.h"
#include "core/Value.h"
#include "core/Transport.h"

namespace oss {

// One automation breakpoint: a value in [0, 1] at a song position (in bars).
struct AutoPoint { float bar = 0.0f; float value = 0.0f; };

// Parameter automation: kChannels float-output channels, each a piecewise-linear
// curve of breakpoints over song time (bars). Every frame each channel is sampled
// at the global transport's bar position (clamped to the song length) and emitted
// on its output, so wiring a channel into any Float input sequences that parameter
// over time. The normalised [0,1] curve is scaled to a per-channel [outMin,outMax]
// so a channel can drive any range (a 0..1 mix, a 20..2000 Hz frequency, ...).
// The breakpoints are edited with the mouse in the Automation window; they are
// kept sorted by bar, which sample() relies on.
class AutomationNode : public Node {
public:
    static constexpr int kChannels = 4;

    AutomationNode() : Node("Automation") {
        for (int c = 0; c < kChannels; ++c) {
            addOutput("ch " + std::to_string(c + 1), PortType::Float);
            outMin_[c] = 0.0f;
            outMax_[c] = 1.0f;
        }
    }

    void evaluate(EvalContext& ctx) override {
        double bar = ctx.transport ? ctx.transport->bars() : 0.0;
        bar = std::clamp(bar, 0.0, (double)lengthBars_);
        currentBar_ = (float)bar;
        for (int c = 0; c < kChannels; ++c) {
            float v = sample(c, (float)bar);                       // normalised [0,1]
            ctx.out<float>((std::size_t)c, outMin_[c] + v * (outMax_[c] - outMin_[c]));
        }
    }

    // --- UI access (the Automation window reads/edits these) ---
    int channelCount() const { return kChannels; }
    std::vector<AutoPoint>&       channel(int c)       { return ch_[c]; }
    const std::vector<AutoPoint>& channel(int c) const { return ch_[c]; }
    float lengthBars() const { return lengthBars_; }
    void  setLengthBars(float L) { lengthBars_ = std::max(1.0f, L); }
    float outMin(int c) const { return outMin_[c]; }
    float outMax(int c) const { return outMax_[c]; }
    void  setOutRange(int c, float lo, float hi) { outMin_[c] = lo; outMax_[c] = hi; }
    float currentBar() const { return currentBar_; }   // last sampled position (playhead)

    // Sample channel c's normalised curve at bar position b -> [0,1].
    float sample(int c, float b) const {
        const auto& p = ch_[c];
        if (p.empty()) return 0.0f;
        if (b <= p.front().bar) return p.front().value;
        if (b >= p.back().bar)  return p.back().value;
        for (std::size_t i = 0; i + 1 < p.size(); ++i) {
            if (b >= p[i].bar && b <= p[i + 1].bar) {
                float span = p[i + 1].bar - p[i].bar;
                float t = span > 0.0f ? (b - p[i].bar) / span : 0.0f;
                return p[i].value + t * (p[i + 1].value - p[i].value);
            }
        }
        return p.back().value;
    }

private:
    std::vector<AutoPoint> ch_[kChannels];
    float outMin_[kChannels];
    float outMax_[kChannels];
    float lengthBars_ = 8.0f;
    float currentBar_ = 0.0f;
};

} // namespace oss
