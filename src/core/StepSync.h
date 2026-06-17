#pragma once
#include <string>
#include <vector>

namespace oss {

// Per-step musical divisions for transport-synced sequencers/arps (length of one
// step). Index matches stepDivisionBars(). Assumes 4/4 (beatsPerBar = 4).
inline const std::vector<std::string>& stepDivisionLabels() {
    static const std::vector<std::string> labels = {
        "1/4", "1/8", "1/8.", "1/8T", "1/16", "1/16.", "1/16T", "1/32"
    };
    return labels;
}

// Length of one step in bars for division index `idx` (clamped to a valid index).
inline double stepDivisionBars(int idx) {
    static const double bars[8] = {
        0.25,                 // 1/4   (a beat)
        0.125,                // 1/8
        0.1875,               // 1/8.  (dotted = 1.5x)
        0.125 * 2.0 / 3.0,    // 1/8T  (triplet = 2/3x)
        0.0625,               // 1/16
        0.09375,              // 1/16. (dotted)
        0.0625 * 2.0 / 3.0,   // 1/16T (triplet)
        0.03125               // 1/32
    };
    if (idx < 0) idx = 0;
    if (idx > 7) idx = 7;
    return bars[idx];
}

inline constexpr int kStepDivisionDefault = 4;   // "1/16"

} // namespace oss
