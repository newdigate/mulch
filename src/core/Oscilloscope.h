#pragma once
#include <cstddef>
#include <vector>

namespace oss {

// How the oscilloscope plots the incoming audio.
enum class ScopeMode { Waveform, XY };

// Fill `out` with `pointCount` vec3 positions (x,y,z) describing a scope trace as a
// line strip. histL/histR are rolling sample histories ordered oldest..newest, length
// n (a mono source passes the same pointer for both). `windowSamples` is how many of
// the most-recent samples the trace spans (clamped to [2, n] internally). GL-free.
//
//  - Waveform: signal = 0.5*(L+R). The window is the most-recent `windowSamples`. When
//    `trigger`, the window start shifts back to the latest rising zero-crossing
//    (s[i-1] < 0 <= s[i]) within a one-window look-back so a steady tone stands still;
//    if none is found it stays free-running. Resampled to `pointCount` points (linear):
//        x = -1 + 2*j/(pointCount-1),  y = sample*gain,  z = 0.
//  - XY: `trigger` is ignored. The most-recent `windowSamples` (L,R) pairs are resampled
//    to `pointCount` points:  x = L*gain,  y = R*gain,  z = 0.
//
// `out` is resized to pointCount*3. With n < 2 (or a null pointer) it is filled with
// zeros (a flat trace).
void buildScopeVertices(const float* histL, const float* histR, std::size_t n,
                        int windowSamples, int pointCount, ScopeMode mode,
                        bool trigger, float gain, std::vector<float>& out);

} // namespace oss
