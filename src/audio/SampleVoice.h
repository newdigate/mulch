#pragma once
#include <cstddef>
#include "audio/AudioFile.h"   // AudioClip (struct only; no decode dependency)

namespace oss {

// One monophonic, one-shot sample voice over an AudioClip. A source-frame playhead is advanced by
// `rate` per output frame; the clip is read downmixed to mono with linear interpolation. trigger()
// (re)starts it (at the end when `reverse`); it deactivates when the playhead leaves the clip (no
// loop). render() ADDS into the output buffers. GL-free; does not call decodeAudioFile.
class SampleVoice {
public:
    void trigger(const AudioClip& clip, bool reverse) {
        std::size_t f = clip.frames();
        if (f < 2) { active_ = false; return; }
        pos_    = reverse ? (double)(f - 1) : 0.0;
        active_ = true;
    }
    bool active() const { return active_; }

    // Mix `n` frames into outL/outR (ADDED). gL/gR are the final per-channel gains
    // (volume * accent * pan). A zero rate, short clip, or inactive voice is a no-op.
    void render(const AudioClip& clip, double rate, float gL, float gR,
                float* outL, float* outR, int n) {
        std::size_t f = clip.frames();
        if (!active_ || f < 2 || rate == 0.0 || clip.channels < 1) return;
        // Upper deactivation bound: for reverse (rate<0), allow pos up to f-1 inclusive
        // so the last sample is readable; for forward, deactivate at f-1 (last interp pair gone).
        double upperBound = (rate < 0.0) ? (double)f : (double)(f - 1);
        for (int i = 0; i < n; ++i) {
            if (pos_ < 0.0 || pos_ >= upperBound) { active_ = false; break; }
            int   i0 = (int)pos_;
            float fr = (float)(pos_ - (double)i0);
            // When pos_ is exactly at f-1 (reverse start), i0+1==f which would OOB; clamp.
            int   i1 = (i0 + 1 < (int)f) ? i0 + 1 : i0;
            float s  = monoAt(clip, i0) * (1.0f - fr) + monoAt(clip, i1) * fr;
            outL[i] += s * gL;
            outR[i] += s * gR;
            pos_ += rate;
        }
    }

private:
    static float monoAt(const AudioClip& clip, int frame) {
        int ch = clip.channels;
        std::size_t base = (std::size_t)frame * (std::size_t)ch;
        if (ch >= 2) return 0.5f * (clip.samples[base] + clip.samples[base + 1]);
        return clip.samples[base];
    }
    double pos_    = 0.0;
    bool   active_ = false;
};

} // namespace oss
