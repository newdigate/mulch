#pragma once
#include <cstddef>
#include <string>
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

// Inline text codec for an AutoCurve: comma-joined "bar,value" pairs (empty curve -> "").
// Used by the project file and the Automation node's saveState. GL-free.
inline std::string encodeCurve(const AutoCurve& c) {
    std::string s;
    for (std::size_t i = 0; i < c.points.size(); ++i) {
        if (i) s += ',';
        s += std::to_string(c.points[i].bar) + ',' + std::to_string(c.points[i].value);
    }
    return s;
}
inline AutoCurve decodeCurve(const std::string& s) {
    AutoCurve c;
    std::vector<float> nums;
    std::size_t pos = 0;
    while (pos <= s.size()) {
        std::size_t comma = s.find(',', pos);
        std::string tok = s.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        if (!tok.empty()) { try { nums.push_back(std::stof(tok)); } catch (...) {} }
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    for (std::size_t i = 0; i + 1 < nums.size(); i += 2) c.points.push_back({nums[i], nums[i + 1]});
    return c;
}

} // namespace oss
