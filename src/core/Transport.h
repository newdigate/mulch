#pragma once
#include <algorithm>

namespace oss {

// Global transport / clock shared by the whole graph: a tempo and a song
// position, advanced once per frame while `playing`. The toolbar drives it, and
// nodes can read it from EvalContext to sync to the beat. The position is stored
// in seconds; beats, bars, and milliseconds are derived from the tempo (4/4).
struct Transport {
    double bpm         = 120.0;   // tempo in beats per minute (decimals allowed)
    bool   playing     = false;
    double seconds     = 0.0;     // song position in seconds (never negative)
    int    beatsPerBar = 4;       // time-signature numerator (assumes 4/4)

    // Advance the position by `dt` seconds when playing. Clamped at zero.
    void advance(double dt) {
        if (playing) seconds += dt;
        if (seconds < 0.0) seconds = 0.0;
    }

    // Tempo-derived conversions. Guard against a zero/negative tempo so the
    // toolbar can never divide by zero mid-edit.
    double secondsPerBeat() const { return 60.0 / (bpm > 0.0 ? bpm : 120.0); }
    double secondsPerBar()  const { return beatsPerBar * secondsPerBeat(); }

    double millis() const { return seconds * 1000.0; }
    double beats()  const { return seconds / secondsPerBeat(); }
    double bars()   const { return beats() / beatsPerBar; }

    // 1-based musical position for display: the start is bar 1, beat 1.
    int barNumber() const { return (int)bars() + 1; }
    int beatInBar() const { return (int)beats() % beatsPerBar + 1; }

    // Transport controls (used by the toolbar buttons).
    void play()       { playing = true; }
    void pause()      { playing = false; }
    void stop()       { playing = false; seconds = 0.0; }
    void rewindBar()  { seconds = std::max(0.0, seconds - secondsPerBar()); }
    void forwardBar() { seconds += secondsPerBar(); }
};

} // namespace oss
