#pragma once
#include <cmath>
#include <iterator>
#include <string>
#include <vector>

namespace oss {

// Offline render settings + the GL-free math behind them (frame counts, the fixed clock,
// validation, command-line parsing). The OfflineRenderer (app/) drives the graph from these;
// the RenderDialog (ui/) edits them. Bars follow the Loop-field convention: 0 = the first bar,
// the finish bar is exclusive, fractions allowed.

constexpr int    kRenderFrameRates[]   = {24, 25, 30, 50, 60};   // every one divides 48 000
constexpr int    kRenderFrameRateCount = (int)std::size(kRenderFrameRates);   // never drifts from the array above
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

// Frames whose start time lies in [0, durationSeconds): the half-open interval makes this
// ceil(dur*fps), with a small epsilon so float noise (0.2 s * 60 = 12.000000000000002) never
// adds a frame. That definition already yields >= 1 for any 0 < dur*fps <= 1e-6, so the clamp
// below is not a separate "at least one frame" rule -- it exists purely to undo the epsilon at
// that boundary. 0 for an empty/negative range. A non-finite or absurdly large product is
// rejected before the cast -- casting it would be undefined -- and also returns 0.
inline long long renderFramesOver(double durationSeconds, int fps) {
    if (durationSeconds <= 0.0 || fps <= 0) return 0;
    double raw = std::ceil(durationSeconds * fps - 1e-6);
    if (!(raw > -9.0e18 && raw < 9.0e18)) return 0;   // also false for NaN
    long long n = (long long)raw;
    return n < 1 ? 1 : n;
}

// Captured frames for [startBar, endBar).
inline long long renderFrameCount(const RenderSettings& s, double secondsPerBar) {
    return renderFramesOver((s.endBar - s.startBar) * secondsPerBar, s.fps);
}

// Discarded pre-roll frames before startBar.
inline long long prerollFrameCount(const RenderSettings& s, double secondsPerBar) {
    return renderFramesOver(s.prerollBars * secondsPerBar, s.fps);
}

// Transport position for frame k (k < 0 during pre-roll). fps <= 0 has no clock to advance by,
// so it degenerates to the start bar; either way the result is clamped to 0 so a render that
// starts at bar 0 pre-rolls sitting at the start.
inline double renderFrameSeconds(const RenderSettings& s, double secondsPerBar, long long k) {
    double start = s.startBar * secondsPerBar;
    double t = s.fps > 0 ? start + (double)k / (double)s.fps : start;
    return t < 0.0 ? 0.0 : t;
}

// Interleaved-stereo FRAMES of audio per video frame. Integer division: exact for every rate in
// kRenderFrameRates at 48 kHz. A rate that does not divide sampleRate truncates, and because the
// caller pads/trims each frame to this count the shortfall ACCUMULATES (44100/24 -> 1837 not
// 1837.5, ~2.2 s of audio lost per hour). Add a fractional accumulator before allowing one.
inline int audioSamplesPerFrame(int sampleRate, int fps) {
    return (sampleRate > 0 && fps > 0) ? sampleRate / fps : 0;
}

} // namespace oss
