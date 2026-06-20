#pragma once
#include <cstddef>
#include <cstdlib>
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

    // Persist the 4 channels as "outMin:outMax:curve" blocks joined by '|'.
    std::string saveState() const override {
        std::string s;
        for (int c = 0; c < kChannels; ++c) {
            if (c) s += '|';
            s += std::to_string(outMin_[c]) + ':' + std::to_string(outMax_[c]) + ':' + encodeCurve(curve_[c]);
        }
        return s;
    }
    void loadState(const std::string& s) override {
        std::size_t pos = 0;
        for (int c = 0; c < kChannels; ++c) {
            if (pos > s.size()) break;
            std::size_t bar = s.find('|', pos);
            std::string block = s.substr(pos, bar == std::string::npos ? std::string::npos : bar - pos);
            std::size_t a = block.find(':');
            std::size_t b = (a == std::string::npos) ? std::string::npos : block.find(':', a + 1);
            if (a != std::string::npos && b != std::string::npos) {
                try { outMin_[c] = std::stof(block.substr(0, a));
                      outMax_[c] = std::stof(block.substr(a + 1, b - a - 1)); } catch (...) {}
                curve_[c] = decodeCurve(block.substr(b + 1));
            }
            if (bar == std::string::npos) break;
            pos = bar + 1;
        }
    }

private:
    AutoCurve curve_[kChannels];
    float     outMin_[kChannels];
    float     outMax_[kChannels];
};

} // namespace oss
