#include "core/MidiTimecode.h"
#include <cmath>

namespace oss {

double nominalFps(MtcRate r) {
    switch (r) {
        case MtcRate::Fps24:     return 24.0;
        case MtcRate::Fps25:     return 25.0;
        case MtcRate::Fps2997df: return 30000.0 / 1001.0;
        case MtcRate::Fps30:     return 30.0;
    }
    return 30.0;
}
double frameDuration(MtcRate r) { return 1.0 / nominalFps(r); }

static int countFps(MtcRate r) {   // integer fps used for the frame count (df counts at 30)
    switch (r) {
        case MtcRate::Fps24: return 24;
        case MtcRate::Fps25: return 25;
        default:             return 30;
    }
}

double smpteToSeconds(const SmpteTime& tc) {
    if (tc.rate == MtcRate::Fps2997df) {
        long totalMin = 60L * tc.h + tc.m;
        long dropped  = 2 * (totalMin - totalMin / 10);
        long frames   = ((long)tc.h * 3600 + (long)tc.m * 60 + tc.s) * 30 + tc.f - dropped;
        return (double)frames * 1001.0 / 30000.0;
    }
    int fps = countFps(tc.rate);
    long frames = ((long)tc.h * 3600 + (long)tc.m * 60 + tc.s) * fps + tc.f;
    return (double)frames / (double)fps;
}

SmpteTime secondsToSmpte(double seconds, MtcRate r) {
    if (seconds < 0.0) seconds = 0.0;
    SmpteTime tc; tc.rate = r;
    if (r == MtcRate::Fps2997df) {
        long d   = (long)std::llround(seconds * 30000.0 / 1001.0);   // real frames
        long ten = d / 17982;                                        // real frames per 10 min (18000-18)
        long rem = d % 17982;
        long drop = (rem >= 2) ? (18 * ten + 2 * ((rem - 2) / 1798)) // 1 min df = 1800-2 = 1798
                               : (18 * ten);
        long tf  = d + drop;                                         // timecode frame number (with gaps)
        tc.f = (int)(tf % 30);
        tc.s = (int)((tf / 30) % 60);
        tc.m = (int)((tf / 30 / 60) % 60);
        tc.h = (int)((tf / 30 / 60 / 60) % 24);
        return tc;
    }
    int fps = countFps(r);
    long frames = (long)std::llround(seconds * (double)fps);
    tc.f = (int)(frames % fps);
    long ts = frames / fps;
    tc.s = (int)(ts % 60);
    tc.m = (int)((ts / 60) % 60);
    tc.h = (int)((ts / 3600) % 24);
    return tc;
}

unsigned char quarterFrameByte(int piece, const SmpteTime& tc) {
    int rate = (int)tc.rate & 0x03;
    int nib = 0;
    switch (piece & 7) {
        case 0: nib = tc.f & 0x0F; break;
        case 1: nib = (tc.f >> 4) & 0x01; break;
        case 2: nib = tc.s & 0x0F; break;
        case 3: nib = (tc.s >> 4) & 0x03; break;
        case 4: nib = tc.m & 0x0F; break;
        case 5: nib = (tc.m >> 4) & 0x03; break;
        case 6: nib = tc.h & 0x0F; break;
        case 7: nib = ((tc.h >> 4) & 0x01) | (rate << 1); break;
    }
    return (unsigned char)(((piece & 7) << 4) | (nib & 0x0F));
}

void fullFrameMessage(const SmpteTime& tc, std::vector<unsigned char>& out) {
    int rate = (int)tc.rate & 0x03;
    out.clear();
    out.push_back(0xF0); out.push_back(0x7F); out.push_back(0x7F);
    out.push_back(0x01); out.push_back(0x01);
    out.push_back((unsigned char)(((rate << 5) | (tc.h & 0x1F))));   // 0rrh hhhh
    out.push_back((unsigned char)(tc.m & 0x3F));
    out.push_back((unsigned char)(tc.s & 0x3F));
    out.push_back((unsigned char)(tc.f & 0x1F));
    out.push_back(0xF7);
}

bool parseFullFrame(const unsigned char* data, std::size_t n, SmpteTime& out) {
    if (n < 10) return false;
    if (data[0] != 0xF0 || data[1] != 0x7F || data[3] != 0x01 || data[4] != 0x01 || data[n - 1] != 0xF7)
        return false;
    out.rate = (MtcRate)((data[5] >> 5) & 0x03);
    out.h = data[5] & 0x1F;
    out.m = data[6] & 0x3F;
    out.s = data[7] & 0x3F;
    out.f = data[8] & 0x1F;
    return true;
}

void MtcReader::reset() {
    for (int i = 0; i < 8; ++i) { nib_[i] = 0; haveNib_[i] = false; }
    seconds_ = 0.0; playing_ = false; idle_ = 0.0; rate_ = MtcRate::Fps30;
}
void MtcReader::onQuarterFrame(unsigned char data) {
    int piece = (data >> 4) & 0x07;
    nib_[piece] = (unsigned char)(data & 0x0F);
    haveNib_[piece] = true;
    playing_ = true; idle_ = 0.0;
    seconds_ += frameDuration(rate_) / 4.0;   // 4 quarter-frames per frame
    if (piece == 7) {
        bool all = true;
        for (int i = 0; i < 8; ++i) if (!haveNib_[i]) all = false;
        if (all) assemble();
        for (int i = 0; i < 8; ++i) haveNib_[i] = false;
    }
}
void MtcReader::assemble() {
    SmpteTime tc;
    tc.f = (nib_[1] << 4) | nib_[0];
    tc.s = (nib_[3] << 4) | nib_[2];
    tc.m = (nib_[5] << 4) | nib_[4];
    tc.h = ((nib_[7] & 0x01) << 4) | nib_[6];
    tc.rate = (MtcRate)((nib_[7] >> 1) & 0x03);
    rate_ = tc.rate;
    seconds_ = smpteToSeconds(tc) + 2.0 * frameDuration(tc.rate);   // assembled time is 2 frames old
}
void MtcReader::onFullFrame(const SmpteTime& tc) {
    rate_ = tc.rate;
    seconds_ = smpteToSeconds(tc);   // a locate; does not by itself imply playing
}
void MtcReader::onIdle(double dt) {
    idle_ += dt;
    if (idle_ > 0.1) playing_ = false;   // ~no QF for 100 ms (a few frames) -> stopped
}

} // namespace oss
