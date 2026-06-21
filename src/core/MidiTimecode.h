#pragma once
#include <cstddef>
#include <vector>

namespace oss {

// SMPTE frame-rate codes (the 2 bits carried in MTC's hour byte).
enum class MtcRate { Fps24 = 0, Fps25 = 1, Fps2997df = 2, Fps30 = 3 };

struct SmpteTime { int h = 0, m = 0, s = 0, f = 0; MtcRate rate = MtcRate::Fps30; };

double    nominalFps(MtcRate r);        // 24 / 25 / 29.97 / 30
double    frameDuration(MtcRate r);     // real seconds per frame
double    smpteToSeconds(const SmpteTime& tc);          // real wall-clock seconds
SmpteTime secondsToSmpte(double seconds, MtcRate r);    // inverse (frame-exact)

// Quarter-frame: piece 0..7 -> the data byte that follows 0xF1 (0nnn_dddd).
unsigned char quarterFrameByte(int piece, const SmpteTime& tc);
// Full-frame SysEx: F0 7F 7F 01 01 hh mm ss ff F7 (10 bytes).
void fullFrameMessage(const SmpteTime& tc, std::vector<unsigned char>& out);
bool parseFullFrame(const unsigned char* data, std::size_t n, SmpteTime& out);

// MTC RECEIVER: reassembles quarter-frame / full-frame messages into a position + play state.
class MtcReader {
public:
    void onQuarterFrame(unsigned char data);   // a 0xF1 data byte (piece index + nibble)
    void onFullFrame(const SmpteTime& tc);     // a full-frame locate
    void onIdle(double dtSeconds);             // advance an inactivity timer (no QF -> stop)
    void reset();
    double  seconds() const { return seconds_; }
    bool    playing() const { return playing_; }
    MtcRate rate()    const { return rate_; }

private:
    void assemble();
    unsigned char nib_[8]    = {0,0,0,0,0,0,0,0};
    bool          haveNib_[8] = {false,false,false,false,false,false,false,false};
    double        seconds_ = 0.0;
    bool          playing_ = false;
    double        idle_    = 0.0;
    MtcRate       rate_    = MtcRate::Fps30;
};

} // namespace oss
