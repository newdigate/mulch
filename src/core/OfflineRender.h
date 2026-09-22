#pragma once
#include <climits>
#include <cmath>
#include <cstdlib>
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

// parseRenderArgs sentinels for "not given on the command line" -- the driver (main.cpp,
// not yet written) fills these in once the project is loaded / Preferences are read.
inline constexpr double kRenderEndBarFromProject   = -1.0;  // -> the project's Automation song length
inline constexpr int    kRenderSizeFromPreferences = 0;     // -> the Preferences texture size

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

// The supported rates as prose ("24, 25, 30, 50 or 60"), BUILT FROM kRenderFrameRates rather
// than spelled out beside it: a rate added to (or dropped from) the array used to leave the
// rejection message behind, quietly telling the user something the validator does not enforce.
inline std::string renderFrameRateList() {
    std::string s;
    for (int i = 0; i < kRenderFrameRateCount; ++i) {
        if (i) s += (i + 1 == kRenderFrameRateCount) ? " or " : ", ";
        s += std::to_string(kRenderFrameRates[i]);
    }
    return s;
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

// One reason a render cannot start, or true with `err` cleared. `hasOutputNode` is supplied
// by the caller (core knows no node types). Even dimensions: the H.264 yuv420p encode needs them.
inline bool validateRenderSettings(const RenderSettings& s, bool hasOutputNode, std::string& err) {
    if (!hasOutputNode)            { err = "add an Output node"; return false; }
    if (!(s.endBar > s.startBar))  { err = "finish bar must be after start bar"; return false; }
    if (s.startBar < 0.0)          { err = "start bar must be 0 or later"; return false; }
    if (s.prerollBars < 0.0)       { err = "pre-roll must be 0 or more bars"; return false; }
    if (!isRenderFrameRate(s.fps)) { err = "frame rate must be " + renderFrameRateList(); return false; }
    if (s.width  < kRenderMinSize || s.width  > kRenderMaxSize ||
        s.height < kRenderMinSize || s.height > kRenderMaxSize) {
        // Same rule as the frame rates above: the bounds are named by the constants that enforce
        // them, so changing one cannot leave the message asserting the old pair.
        err = "width and height must be between " + std::to_string(kRenderMinSize) +
              " and " + std::to_string(kRenderMaxSize);
        return false;
    }
    if ((s.width % 2) || (s.height % 2)) { err = "width and height must be even"; return false; }
    if (s.outPath.empty())         { err = "choose an output file"; return false; }
    err.clear();
    return true;
}

// `--render <project.oss> <out.mp4> [--start B] [--end B] [--fps N] [--size WxH] [--preroll B]`
// (`args` = everything after `--render`). Fields not given are left as SENTINELS (see
// kRenderEndBarFromProject / kRenderSizeFromPreferences above) as the parser's one place
// defining "not given"; the driver must NOT compare settings.endBar/width/height back against
// those sentinels to decide whether to fill them in -- a user who happens to type the sentinel
// (`--end -1`, `--size 0x100`) would then be silently reinterpreted as "not given" instead of
// rejected by validateRenderSettings. endGiven/sizeGiven are the structural signal: true only
// when parseRenderArgs actually saw that option on the command line.
struct RenderCliArgs {
    std::string    projectPath;
    RenderSettings settings;
    bool           endGiven  = false;   // false -> settings.endBar is the sentinel; driver fills from the project
    bool           sizeGiven = false;   // false -> settings.width/height are the sentinel; driver fills from Preferences
};

inline const char* renderUsage() {
    return "usage: --render <project.oss> <out.mp4> [--start B] [--end B] [--fps N] [--size WxH] [--preroll B]";
}

inline bool parseRenderArgs(const std::vector<std::string>& args, RenderCliArgs& out, std::string& err) {
    out = RenderCliArgs{};
    out.settings.endBar = kRenderEndBarFromProject;
    out.settings.width  = kRenderSizeFromPreferences;
    out.settings.height = kRenderSizeFromPreferences;
    std::vector<std::string> positional;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a.size() < 2 || a[0] != '-' || a[1] != '-') { positional.push_back(a); continue; }
        if (a != "--start" && a != "--end" && a != "--fps" && a != "--size" && a != "--preroll") {
            err = "unknown option " + a; return false;
        }
        if (i + 1 >= args.size()) { err = a + " needs a value"; return false; }
        const std::string& v = args[++i];
        const char* c = v.c_str();
        char* end = nullptr;
        if (a == "--size") {
            // strtol saturates to LONG_MIN/LONG_MAX on overflow rather than failing to parse
            // (`end` still advances past the digits), so an absurd value like a 20-digit
            // width would otherwise reach the `(int)` cast below and truncate to garbage
            // (or be UB outright) instead of being rejected here.
            long w = std::strtol(c, &end, 10);
            if (end == c || *end != 'x' || w < INT_MIN || w > INT_MAX) {
                err = "--size expects WxH, got " + v; return false;
            }
            const char* hs = end + 1;
            long h = std::strtol(hs, &end, 10);
            if (end == hs || *end != '\0' || h < INT_MIN || h > INT_MAX) {
                err = "--size expects WxH, got " + v; return false;
            }
            out.settings.width = (int)w; out.settings.height = (int)h;
            out.sizeGiven = true;
            continue;
        }
        if (a == "--fps") {
            // Parsed as an integer, not through the generic double branch below: `(int)d`
            // on a double is only well-defined once you already know it is finite and in
            // range, and a bogus "59.94" should be reported as-typed rather than silently
            // truncated to 59 and then diagnosed as an unsupported frame rate.
            long n = std::strtol(c, &end, 10);
            if (end == c || *end != '\0' || n < 1 || n > 1000) {
                err = "bad value for --fps: " + v; return false;
            }
            out.settings.fps = (int)n;
            continue;
        }
        double d = std::strtod(c, &end);
        // strtod happily parses "inf"/"infinity"/"nan"/huge exponents; casting or comparing
        // those downstream is undefined or nonsensical, so require finite here.
        if (end == c || *end != '\0' || !std::isfinite(d)) {
            err = "bad value for " + a + ": " + v; return false;
        }
        if      (a == "--start")   out.settings.startBar    = d;
        else if (a == "--end")   { out.settings.endBar      = d; out.endGiven = true; }
        else /* --preroll */       out.settings.prerollBars = d;
    }
    if (positional.size() != 2) {
        err = renderUsage();
        return false;
    }
    out.projectPath      = positional[0];
    out.settings.outPath = positional[1];
    return true;
}

} // namespace oss
