#pragma once
#include <cmath>

namespace oss {

// Source-time position (seconds) for a clip time-warped to span `lengthBars` bars,
// bar-locked to the transport's absolute bar position `bars`. The clip aligns to bar 1
// and repeats every `lengthBars` bars: within each length-bar window the position sweeps
// linearly from 0 to `durationSec` (slope durationSec/lengthBars per bar). Returns 0 for a
// non-positive length or duration. GL-free.
inline double barSyncPlayhead(double bars, int lengthBars, double durationSec) {
    if (lengthBars < 1 || durationSec <= 0.0) return 0.0;
    double win  = bars / (double)lengthBars;   // fractional count of length-bar windows
    double frac = win - std::floor(win);        // position within the current window [0,1)
    return frac * durationSec;                   // -> source seconds
}

} // namespace oss
