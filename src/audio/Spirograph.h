#pragma once
#include <cmath>

namespace oss {

// GL-free stereo Spirograph oscillator: theta advances at audio rate; a hypotrochoid /
// epitrochoid curve's x -> left channel, y -> right channel. Output is normalized so
// |x|,|y| <= 1 for ALL parameters (triangle inequality), then scaled by level -- no clamp
// needed (same spirit as StateVariableFilter). theta persists across process() blocks for
// click-free continuity (like SineWaveNode).
class Spirograph {
public:
    static constexpr double kTwoPi = 6.283185307179586;

    void setSampleRate(int sr) { sr_ = sr > 0 ? sr : 48000; }
    void setCurveType(int c)   { curve_ = (c == 1) ? 1 : 0; }   // 0 = hypo, 1 = epi
    void setFrequency(float hz){ freq_ = hz; }
    void setRatio(float r)     { ratio_ = clampf(r, 2.0f, 12.0f); }   // R/r
    void setPen(float d)       { pen_ = clampf(d, 0.0f, 1.0f); }      // d, fraction 0..1
    void setPhase(float turns) { phase_ = turns; }                    // 0..1 turns, global theta offset
    void setLevel(float a)     { level_ = clampf(a, 0.0f, 1.0f); }

    // Fill n frames of both channels. outL == x waveform, outR == y waveform.
    void process(float* outL, float* outR, int n) {
        const double inc    = kTwoPi * (double)freq_ / (double)sr_;
        // k (partial rate) and the big-circle arm depend on the curve family.
        const double k       = (curve_ == 1) ? (ratio_ + 1.0) : (ratio_ - 1.0);
        const double bigArm  = (curve_ == 1) ? (ratio_ + 1.0) : (ratio_ - 1.0);   // > 0 (ratio >= 2)
        const double penSign = (curve_ == 1) ? -1.0 : 1.0;    // hypo: +pen*cos ; epi: -pen*cos
        const double norm    = 1.0 / (bigArm + (double)pen_); // bigArm + pen >= 1, never zero
        const double off     = kTwoPi * (double)phase_;
        for (int i = 0; i < n; ++i) {
            const double t = theta_ + off;
            const double x = bigArm * std::cos(t) + penSign * (double)pen_ * std::cos(k * t);
            const double y = bigArm * std::sin(t) -            (double)pen_ * std::sin(k * t);
            outL[i] = (float)((double)level_ * x * norm);
            outR[i] = (float)((double)level_ * y * norm);
            theta_ += inc;
            // Robust wrap to [0, 2pi) for any inc sign/magnitude (CV can push freq hard).
            theta_ -= kTwoPi * std::floor(theta_ / kTwoPi);
        }
    }

private:
    static float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }

    int    sr_    = 48000;
    int    curve_ = 0;
    float  freq_  = 110.0f;
    float  ratio_ = 3.0f;
    float  pen_   = 0.5f;
    float  phase_ = 0.0f;
    float  level_ = 0.8f;
    double theta_ = 0.0;   // persisted phase accumulator, radians, in [0, 2pi)
};

} // namespace oss
