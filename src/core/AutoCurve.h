#pragma once
#include <cstddef>
#include <string>
#include <vector>

namespace oss {

// One automation breakpoint: a normalised value in [0,1] at a song position (bars).
// Tangent handles are stored as (bar, value) offsets from the point: out* shapes the
// cubic segment toward the next point, in* toward the previous. All-zero handles => a
// straight (linear) segment. 'broken' lets the editor move the two handles independently;
// otherwise it keeps them colinear. New points default to retracted (linear).
struct AutoPoint {
    float bar = 0.0f, value = 0.0f;
    float outDBar = 0.0f, outDValue = 0.0f;   // handle toward the next point (shapes the segment to the right)
    float inDBar  = 0.0f, inDValue  = 0.0f;   // handle toward the previous point (shapes the segment to the left)
    bool  broken  = false;                    // false = editor keeps in/out colinear (aligned)
};

// One Bézier control point in (bar, value) space.
struct CurvePt { float bar = 0.0f, value = 0.0f; };

// The four cubic-Bézier control points for the segment between a (left) and b (right),
// with the control *bars* clamped monotonic (c[0].bar <= c[1].bar <= c[2].bar <= c[3].bar)
// so x(t) is non-decreasing and the curve is a single-valued function of bar. Shared by
// sampling and the editor's drawing so the drawn curve always matches the sampled one.
inline void bezierControls(const AutoPoint& a, const AutoPoint& b, CurvePt c[4]) {
    float b1 = a.bar + a.outDBar;
    if (b1 < a.bar) b1 = a.bar;
    if (b1 > b.bar) b1 = b.bar;
    float b2 = b.bar + b.inDBar;
    if (b2 < b1)    b2 = b1;
    if (b2 > b.bar) b2 = b.bar;
    c[0] = { a.bar, a.value };
    c[1] = { b1,    a.value + a.outDValue };
    c[2] = { b2,    b.value + b.inDValue };
    c[3] = { b.bar, b.value };
}

// Sample the cubic segment between a and b at song position `bar` (a.bar <= bar <= b.bar).
// Linear fast-path when both facing handles are retracted (bit-exact with the old linear
// curve); otherwise solve x(t)=bar by bisection on the monotonic control polygon, return y(t).
inline float bezierSampleSegment(const AutoPoint& a, const AutoPoint& b, float bar) {
    float span = b.bar - a.bar;
    if (span <= 0.0f) return a.value;
    if (a.outDBar == 0.0f && a.outDValue == 0.0f && b.inDBar == 0.0f && b.inDValue == 0.0f)
        return a.value + (bar - a.bar) / span * (b.value - a.value);
    CurvePt c[4];
    bezierControls(a, b, c);
    auto cubic = [](float p0, float p1, float p2, float p3, float t) {
        float u = 1.0f - t;
        return u*u*u*p0 + 3.0f*u*u*t*p1 + 3.0f*u*t*t*p2 + t*t*t*p3;
    };
    float lo = 0.0f, hi = 1.0f;
    for (int i = 0; i < 24; ++i) {
        float t = 0.5f * (lo + hi);
        float x = cubic(c[0].bar, c[1].bar, c[2].bar, c[3].bar, t);
        if (x < bar) lo = t; else hi = t;
    }
    float t = 0.5f * (lo + hi);
    return cubic(c[0].value, c[1].value, c[2].value, c[3].value, t);
}

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
            if (bar >= points[i].bar && bar <= points[i + 1].bar)
                return bezierSampleSegment(points[i], points[i + 1], bar);
        }
        return points.back().value;
    }
};

// Inline text codec for an AutoCurve. New form: "b1;" then 7 comma-joined numbers per
// point (bar,value,outDBar,outDValue,inDBar,inDValue,broken). Empty curve -> "". decode
// sniffs the "b1;" prefix and otherwise falls back to the legacy "bar,value,..." parser,
// so old projects + saved node state still load (with retracted handles). The encoding
// contains no spaces / ':' / '|', so the ProjectFile line format and AutomationNode
// saveState block format are unaffected. GL-free.
inline std::string encodeCurve(const AutoCurve& c) {
    if (c.points.empty()) return "";
    std::string s = "b1;";
    for (std::size_t i = 0; i < c.points.size(); ++i) {
        const AutoPoint& p = c.points[i];
        if (i) s += ',';
        s += std::to_string(p.bar)      + ',' + std::to_string(p.value)
           + ',' + std::to_string(p.outDBar) + ',' + std::to_string(p.outDValue)
           + ',' + std::to_string(p.inDBar)  + ',' + std::to_string(p.inDValue)
           + ',' + (p.broken ? "1" : "0");
    }
    return s;
}
inline AutoCurve decodeCurve(const std::string& s) {
    AutoCurve c;
    if (s.empty()) return c;
    bool bezier = s.rfind("b1;", 0) == 0;            // starts with the version tag
    const std::string body = bezier ? s.substr(3) : s;
    std::vector<float> nums;
    std::size_t pos = 0;
    while (pos <= body.size()) {
        std::size_t comma = body.find(',', pos);
        std::string tok = body.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        if (!tok.empty()) { try { nums.push_back(std::stof(tok)); } catch (...) {} }
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    if (bezier) {
        for (std::size_t i = 0; i + 6 < nums.size(); i += 7) {
            AutoPoint p;
            p.bar = nums[i]; p.value = nums[i + 1];
            p.outDBar = nums[i + 2]; p.outDValue = nums[i + 3];
            p.inDBar  = nums[i + 4]; p.inDValue  = nums[i + 5];
            p.broken  = nums[i + 6] != 0.0f;
            c.points.push_back(p);
        }
    } else {
        for (std::size_t i = 0; i + 1 < nums.size(); i += 2)
            c.points.push_back({ nums[i], nums[i + 1] });
    }
    return c;
}

} // namespace oss
