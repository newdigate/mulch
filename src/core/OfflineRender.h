#pragma once
#include <cmath>
#include <string>
#include <vector>

namespace oss {

// Offline render settings + the GL-free math behind them (frame counts, the fixed clock,
// validation, command-line parsing). The OfflineRenderer (app/) drives the graph from these;
// the RenderDialog (ui/) edits them. Bars follow the Loop-field convention: 0 = the first bar,
// the finish bar is exclusive, fractions allowed.

constexpr int    kRenderFrameRates[]   = {24, 25, 30, 50, 60};   // every one divides 48 000
constexpr int    kRenderFrameRateCount = 5;
constexpr int    kRenderMinSize        = 16;
constexpr int    kRenderMaxSize        = 8192;
constexpr double kRenderLoadTimeoutSeconds = 30.0;   // give up waiting for a node's async load

struct RenderSettings {
    double startBar    = 0.0;    // first captured bar (>= 0)
    double endBar      = 8.0;    // exclusive
    double prerollBars = 1.0;    // bars evaluated but not captured before startBar (>= 0)
    int    fps         = 60;     // one of kRenderFrameRates
    int    width       = 1280;   // render size (own bounds, independent of the Preferences clamp)
    int    height      = 720;
    std::string outPath;         // the .mp4 to write
};

inline bool isRenderFrameRate(int fps) {
    for (int r : kRenderFrameRates) if (r == fps) return true;
    return false;
}

// Frames over `durationSeconds`: those whose start time lies in [0, duration) = ceil(dur*fps),
// with a small epsilon so float noise (0.2 s * 60 = 12.000000000000002) never adds a frame,
// and a floor of 1 so a non-empty range always yields a frame. 0 for an empty range.
inline long framesOver(double durationSeconds, int fps) {
    if (durationSeconds <= 0.0 || fps <= 0) return 0;
    long n = (long)std::ceil(durationSeconds * fps - 1e-6);
    return n < 1 ? 1 : n;
}

// Captured frames for [startBar, endBar).
inline long renderFrameCount(const RenderSettings& s, double secondsPerBar) {
    return framesOver((s.endBar - s.startBar) * secondsPerBar, s.fps);
}

// Discarded pre-roll frames before startBar.
inline long prerollFrameCount(const RenderSettings& s, double secondsPerBar) {
    return framesOver(s.prerollBars * secondsPerBar, s.fps);
}

// Transport position for frame k (k < 0 during pre-roll), clamped at 0 so a render that starts
// at bar 0 pre-rolls sitting at the start.
inline double renderFrameSeconds(const RenderSettings& s, double secondsPerBar, long k) {
    double t = s.startBar * secondsPerBar + (double)k / (double)s.fps;
    return t < 0.0 ? 0.0 : t;
}

// Interleaved-stereo FRAMES of audio per video frame (exact at 48 kHz for the listed rates).
inline int audioSamplesPerFrame(int sampleRate, int fps) {
    return fps > 0 ? sampleRate / fps : 0;
}

} // namespace oss
