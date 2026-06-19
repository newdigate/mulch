#pragma once
#include <cmath>
#include <cstddef>
#include <vector>

namespace oss {

// Compact 4-pole (24 dB/oct) resonant low-pass -- the classic "simplified Moog"
// ladder (Stilson/Smith). Stateful; GL-free. res in [0,1] self-oscillates near 1.
struct LadderFilter {
    float s1 = 0, s2 = 0, s3 = 0, s4 = 0;   // stage outputs
    float d1 = 0, d2 = 0, d3 = 0, d4 = 0;   // one-sample delays
    void reset() { s1 = s2 = s3 = s4 = d1 = d2 = d3 = d4 = 0.0f; }

    float process(float in, float cutoffHz, float res, int sr) {
        float fc = cutoffHz / (0.5f * (float)sr);
        if (fc < 0.0f) fc = 0.0f;
        if (fc > 0.99f) fc = 0.99f;
        float f  = fc * 1.16f;
        float fb = res * 4.0f * (1.0f - 0.15f * f * f);
        // tanh on the feedback tap saturates self-oscillation: keeps the filter
        // bounded (BIBO-stable) even at res = 1 under hard audio drive, and adds the
        // gentle "transistor" character. Near-linear for small signals.
        float x  = in - std::tanh(s4) * fb;
        x *= 0.35013f * (f * f) * (f * f);
        s1 = x  + 0.3f * d1 + (1.0f - f) * s1;  d1 = x;
        s2 = s1 + 0.3f * d2 + (1.0f - f) * s2;  d2 = s1;
        s3 = s2 + 0.3f * d3 + (1.0f - f) * s3;  d3 = s2;
        s4 = s3 + 0.3f * d4 + (1.0f - f) * s4;  d4 = s3;
        return s4;
    }
};

// Monophonic 303-style "acid bass" voice. MIDI-driven (last-note priority); renders
// mono samples. Saw/square VCO + sub-osc -> resonant ladder filter (env/accent/
// keytrack/filterFM modulated) -> VCA -> bounded tanh distortion. GL-free.
class AcidVoice {
public:
    AcidVoice() { updateCoefs(); }   // valid coefficients from the default params/sr

    void setSampleRate(int sr);
    void noteOn(int midiNote, int velocity, bool slide);
    void noteOff(int midiNote);

    void setWaveform(int w)     { waveform_ = w < 0 ? 0 : (w > 1 ? 1 : w); }
    void setCutoff(float hz)    { cutoff_ = hz < 20.0f ? 20.0f : (hz > 12000.0f ? 12000.0f : hz); }
    void setResonance(float r)  { resonance_ = clamp01(r); }
    void setEnvMod(float a)     { envMod_ = clamp01(a); }
    void setDecay(float s)      { decay_ = s < 1e-3f ? 1e-3f : s; updateCoefs(); }
    void setAccent(float a)     { accent_ = clamp01(a); }
    void setSubLevel(float a)   { subLevel_ = clamp01(a); }
    void setSlideTime(float s)  { slideTime_ = s < 1e-3f ? 1e-3f : s; updateCoefs(); }
    void setFilterFM(float a)   { filterFM_ = clamp01(a); }
    void setKeyTrack(float a)   { keyTrack_ = clamp01(a); }
    void setDistortion(float a) { distortion_ = clamp01(a); }
    void setLevel(float a)      { level_ = clamp01(a); }

    void process(float* out, int n);

    float currentFreq() const { return curFreq_; }   // for glide tests
    float filtEnv()     const { return filtEnv_; }    // for envelope tests

private:
    void updateCoefs();
    static float clamp01(float x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }
    static float midiToFreq(int n) { return 440.0f * std::pow(2.0f, (n - 69) / 12.0f); }

    int sr_ = 48000;
    // parameters
    int   waveform_   = 0;
    float cutoff_     = 800.0f;
    float resonance_  = 0.7f;
    float envMod_     = 0.6f;
    float decay_      = 0.3f;
    float accent_     = 0.4f;
    float subLevel_   = 0.0f;
    float slideTime_  = 0.08f;
    float filterFM_   = 0.0f;
    float keyTrack_   = 0.0f;
    float distortion_ = 0.0f;
    float level_      = 0.7f;   // output gain (post-distortion volume trim)
    // derived coefficients
    float decayCoef_   = 0.0f;
    float glideCoef_   = 0.0f;
    float attackCoef_  = 0.0f;
    float releaseCoef_ = 0.0f;
    // state
    std::vector<int> held_;        // held notes, back() = sounding (mono last-note)
    int   curNote_ = 60;
    int   curVel_  = 100;
    bool  gateOn_  = false;
    bool  gliding_ = false;
    double phase_    = 0.0;
    double subPhase_ = 0.0;
    float curFreq_    = 0.0f;
    float targetFreq_ = 0.0f;
    float filtEnv_ = 0.0f;
    float ampEnv_  = 0.0f;
    float lastOut_ = 0.0f;
    LadderFilter filter_;
};

} // namespace oss
