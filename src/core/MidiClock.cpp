#include "core/MidiClock.h"
#include <cmath>

namespace oss {

int beatsToSixteenths(double beats) {
    int s = (int)std::lround(beats * 4.0);
    return s < 0 ? 0 : s;
}
void sppMessage(int sixteenths, unsigned char out[3]) {
    if (sixteenths < 0) sixteenths = 0;
    if (sixteenths > 0x3FFF) sixteenths = 0x3FFF;
    out[0] = 0xF2;
    out[1] = (unsigned char)(sixteenths & 0x7F);
    out[2] = (unsigned char)((sixteenths >> 7) & 0x7F);
}
int sppToSixteenths(unsigned char lsb, unsigned char msb) {
    return ((int)(msb & 0x7F) << 7) | (int)(lsb & 0x7F);
}

namespace { constexpr std::size_t kTempoWindow = 24; }   // average over ~1 quarter note

void BeatClockReader::reset() {
    tickTimes_.clear(); bpm_ = 0.0; playing_ = false; locateBeats_ = 0.0; ticksSinceLocate_ = 0;
}
void BeatClockReader::onTick(double tSeconds) {
    tickTimes_.push_back(tSeconds);
    while (tickTimes_.size() > kTempoWindow) tickTimes_.pop_front();
    if (tickTimes_.size() >= 2) {
        double span = tickTimes_.back() - tickTimes_.front();
        double intervals = (double)(tickTimes_.size() - 1);
        if (span > 0.0) bpm_ = 60.0 / ((span / intervals) * 24.0);
    }
    if (playing_) ++ticksSinceLocate_;
}
void BeatClockReader::onStart()    { playing_ = true; locateBeats_ = 0.0; ticksSinceLocate_ = 0; }
void BeatClockReader::onContinue() { playing_ = true; }
void BeatClockReader::onStop()     { playing_ = false; }
void BeatClockReader::onSongPosition(int sixteenths) {
    locateBeats_ = sixteenths / 4.0;
    ticksSinceLocate_ = 0;
}

} // namespace oss
