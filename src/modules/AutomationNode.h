#pragma once
#include <cstddef>
#include <string>
#include "core/Node.h"
#include "core/Value.h"
#include "core/AutoCurve.h"

namespace oss {

// Parameter automation: kChannels float-output channels, each an AutoCurve of
// breakpoints over song time (bars). Every frame each channel is sampled at the
// global transport's bar position and emitted on its output (scaled to a
// per-channel [outMin,outMax]), so wiring a channel into any Float input sequences
// that parameter over time. These are the "stream" automation channels; they reach
// their target through an edge. The curves are mouse-edited in the Automation
// window. The editor's time axis length is global (AutomationStore::lengthBars),
// not per-node; sampling holds at the curve's end points.
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
        float bar = ctx.transport ? (float)ctx.transport->bars() : 0.0f;
        for (int c = 0; c < kChannels; ++c) {
            float v = curve_[c].sample(bar);                       // normalised [0,1]
            ctx.out<float>((std::size_t)c, outMin_[c] + v * (outMax_[c] - outMin_[c]));
        }
    }

    // --- UI access (the Automation window reads/edits these) ---
    int channelCount() const { return kChannels; }
    std::vector<AutoPoint>&       channel(int c)       { return curve_[c].points; }
    const std::vector<AutoPoint>& channel(int c) const { return curve_[c].points; }
    float outMin(int c) const { return outMin_[c]; }
    float outMax(int c) const { return outMax_[c]; }
    void  setOutRange(int c, float lo, float hi) { outMin_[c] = lo; outMax_[c] = hi; }

private:
    AutoCurve curve_[kChannels];
    float     outMin_[kChannels];
    float     outMax_[kChannels];
};

} // namespace oss
