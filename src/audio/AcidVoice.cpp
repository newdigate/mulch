#include "audio/AcidVoice.h"
#include <algorithm>
#include <cmath>

namespace oss {

void AcidVoice::setSampleRate(int sr) {
    sr_ = sr > 0 ? sr : 48000;
    updateCoefs();
}

void AcidVoice::updateCoefs() {
    decayCoef_   = std::exp(-1.0f / (decay_ * sr_));
    glideCoef_   = 1.0f - std::exp(-1.0f / (slideTime_ * sr_));
    attackCoef_  = 1.0f - std::exp(-1.0f / (0.003f * sr_));   // ~3 ms
    releaseCoef_ = 1.0f - std::exp(-1.0f / (0.008f * sr_));   // ~8 ms
}

void AcidVoice::noteOn(int midiNote, int velocity, bool slide) {
    midiNote = midiNote < 0 ? 0 : (midiNote > 127 ? 127 : midiNote);   // no inf pitch
    held_.push_back(midiNote);
    curNote_ = midiNote;
    curVel_  = velocity;
    targetFreq_ = midiToFreq(midiNote);
    bool wasSounding = gateOn_;
    if (slide && wasSounding) {
        gliding_ = true;                 // legato glide; do NOT retrigger the envelope
    } else {
        curFreq_ = targetFreq_;          // jump
        gliding_ = false;
        filtEnv_ = 1.0f;                 // retrigger the filter envelope
    }
    gateOn_ = true;
}

void AcidVoice::noteOff(int midiNote) {
    midiNote = midiNote < 0 ? 0 : (midiNote > 127 ? 127 : midiNote);   // match noteOn clamp
    auto it = std::find(held_.rbegin(), held_.rend(), midiNote);
    if (it != held_.rend()) held_.erase(std::next(it).base());
    if (held_.empty()) {
        gateOn_ = false;                 // amp env releases to silence
    } else {
        curNote_ = held_.back();         // fall back to the still-held note
        targetFreq_ = midiToFreq(curNote_);
        curFreq_ = targetFreq_;
        gliding_ = false;
    }
}

void AcidVoice::process(float* out, int n) {
    const float ENV_OCT = 4.0f, FM_OCT = 2.0f;
    const float nyq = 0.45f * sr_;
    const float accentAmt = accent_ * (curVel_ / 127.0f);
    const float keyF = std::pow(2.0f, keyTrack_ * (curNote_ - 60) / 12.0f);
    for (int i = 0; i < n; ++i) {
        if (gliding_) {
            curFreq_ += (targetFreq_ - curFreq_) * glideCoef_;
            if (std::fabs(targetFreq_ - curFreq_) < 0.01f) { curFreq_ = targetFreq_; gliding_ = false; }
        }
        phase_    += curFreq_ / sr_;          if (phase_ >= 1.0)    phase_    -= std::floor(phase_);
        subPhase_ += 0.5 * curFreq_ / sr_;    if (subPhase_ >= 1.0) subPhase_ -= std::floor(subPhase_);
        float main = (waveform_ == 0) ? (float)(2.0 * phase_ - 1.0)
                                      : (phase_ < 0.5 ? 1.0f : -1.0f);
        float sub  = (subPhase_ < 0.5 ? 1.0f : -1.0f) * subLevel_;
        float osc  = main + sub;

        filtEnv_ *= decayCoef_;
        float ampTarget = gateOn_ ? (1.0f + 0.5f * accentAmt) : 0.0f;
        ampEnv_ += (ampTarget - ampEnv_) * (ampTarget > ampEnv_ ? attackCoef_ : releaseCoef_);

        float modOct = (envMod_ + accentAmt) * filtEnv_ * ENV_OCT + filterFM_ * lastOut_ * FM_OCT;
        float fcHz = cutoff_ * keyF * std::pow(2.0f, modOct);
        if (fcHz < 20.0f) fcHz = 20.0f;
        if (fcHz > nyq)   fcHz = nyq;

        float filtered = filter_.process(osc, fcHz, resonance_, sr_);
        float s = filtered * ampEnv_;
        lastOut_ = std::tanh(s);   // bounded VCA output -> stable filter-FM feedback
        out[i] = std::tanh(s * (1.0f + distortion_ * 9.0f));
    }
}

} // namespace oss
