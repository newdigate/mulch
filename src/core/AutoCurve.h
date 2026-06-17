#pragma once
#include <cstddef>
#include <vector>

namespace oss {

// One automation breakpoint: a normalised value in [0,1] at a song position (bars).
struct AutoPoint { float bar = 0.0f; float value = 0.0f; };

// A piecewise-linear breakpoint curve over song bars, sampled to [0,1]. Points are
// kept sorted by bar (callers maintain the ordering); sample() holds the first and
// last value beyond the ends.
struct AutoCurve {
    std::vector<AutoPoint> points;

    float sample(float bar) const {
        if (points.empty()) return 0.0f;
        if (bar <= points.front().bar) return points.front().value;
        if (bar >= points.back().bar)  return points.back().value;
        for (std::size_t i = 0; i + 1 < points.size(); ++i) {
            if (bar >= points[i].bar && bar <= points[i + 1].bar) {
                float span = points[i + 1].bar - points[i].bar;
                float t = span > 0.0f ? (bar - points[i].bar) / span : 0.0f;
                return points[i].value + t * (points[i + 1].value - points[i].value);
            }
        }
        return points.back().value;
    }
};

} // namespace oss
