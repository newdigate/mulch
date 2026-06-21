#pragma once
#include <cstddef>
#include <deque>

namespace oss {

// MIDI System Real-Time bytes for Beat Clock.
constexpr unsigned char kMidiClock = 0xF8, kMidiStart = 0xFA, kMidiContinue = 0xFB, kMidiStop = 0xFC;

// --- pure Beat Clock message helpers (GL-free, testable) ---
int  beatsToSixteenths(double beats);                  // round(beats*4), >= 0
void sppMessage(int sixteenths, unsigned char out[3]); // {0xF2, lsb, msb} (14-bit, 7 bits each)
int  sppToSixteenths(unsigned char lsb, unsigned char msb);

// Beat Clock RECEIVER: fed timestamped clock events, derives tempo + position + play state.
class BeatClockReader {
public:
    void onTick(double tSeconds);          // a 0xF8 at absolute time tSeconds
    void onStart();                        // playing = true, position = 0
    void onContinue();                     // playing = true
    void onStop();                         // playing = false
    void onSongPosition(int sixteenths);   // SPP: position = sixteenths/4 beats; resets tick phase
    void reset();

    double bpm() const { return bpm_; }                 // 0 until >= 2 ticks seen
    double positionBeats() const { return locateBeats_ + (double)ticksSinceLocate_ / 24.0; }
    bool   playing() const { return playing_; }

private:
    std::deque<double> tickTimes_;          // recent tick absolute times (tempo window)
    double bpm_ = 0.0;
    bool   playing_ = false;
    double locateBeats_ = 0.0;
    long   ticksSinceLocate_ = 0;
};

} // namespace oss
