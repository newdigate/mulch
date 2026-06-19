#include <doctest/doctest.h>
#include "audio/AcidVoice.h"
#include "modules/AcidNode.h"
#include "core/Node.h"
#include "core/Value.h"
#include <algorithm>
#include <cmath>
#include <variant>
#include <vector>

using namespace oss;

static constexpr double kPi = 3.14159265358979323846;

static float rms(const float* x, int n) {
    double s = 0.0;
    for (int i = 0; i < n; ++i) s += (double)x[i] * x[i];
    return (float)std::sqrt(s / (n > 0 ? n : 1));
}

TEST_CASE("ladder filter attenuates content above its cutoff") {
    const int sr = 48000, n = 4800;
    std::vector<float> in(n), low(n), high(n);
    for (int i = 0; i < n; ++i) in[i] = std::sin(2.0 * kPi * 8000.0 * i / sr);  // 8 kHz
    LadderFilter f; f.reset();
    for (int i = 0; i < n; ++i) low[i]  = f.process(in[i], 200.0f, 0.2f, sr);
    f.reset();
    for (int i = 0; i < n; ++i) high[i] = f.process(in[i], 18000.0f, 0.2f, sr);
    // 8 kHz survives a 18 kHz cutoff but is crushed by a 200 Hz cutoff.
    CHECK(rms(low.data() + 480, n - 480) < 0.3f * rms(high.data() + 480, n - 480));
}

TEST_CASE("ladder filter resonance boosts energy near the cutoff") {
    const int sr = 48000, n = 9600;
    std::vector<float> in(n), lo(n), hi(n);
    for (int i = 0; i < n; ++i) in[i] = 0.5f * std::sin(2.0 * kPi * 1000.0 * i / sr);
    LadderFilter f1; for (int i = 0; i < n; ++i) lo[i] = f1.process(in[i], 1000.0f, 0.1f, sr);
    LadderFilter f2; for (int i = 0; i < n; ++i) hi[i] = f2.process(in[i], 1000.0f, 0.9f, sr);
    CHECK(rms(hi.data() + 960, n - 960) > rms(lo.data() + 960, n - 960));
}

TEST_CASE("ladder filter stays finite and bounded at max resonance") {
    const int sr = 48000, n = 48000;
    LadderFilter f;
    f.process(1.0f, 1000.0f, 1.0f, sr);                 // a kick to perturb it
    float m = 0.0f; bool finite = true;
    for (int i = 0; i < n; ++i) {
        float y = f.process(0.0f, 1000.0f, 1.0f, sr);
        if (!std::isfinite(y)) finite = false;
        m = std::max(m, std::fabs(y));
    }
    CHECK(finite);
    CHECK(m < 10.0f);
}

static float noteFreq(int n) { return 440.0f * std::pow(2.0f, (n - 69) / 12.0f); }

// High-frequency content (first-difference energy) -- a simple brightness proxy.
static float hfEnergy(const std::vector<float>& x) {
    double s = 0.0;
    for (std::size_t i = 1; i < x.size(); ++i) { double d = (double)x[i] - x[i - 1]; s += d * d; }
    return (float)std::sqrt(s / (x.size() > 0 ? x.size() : 1));
}

TEST_CASE("voice: a note sounds and is brighter while the filter envelope is open") {
    AcidVoice v; v.setSampleRate(48000);
    v.setCutoff(300.0f); v.setEnvMod(0.9f); v.setResonance(0.3f);
    v.setDecay(0.2f); v.setAccent(0.0f);
    v.noteOn(48, 100, false);
    std::vector<float> warm(480); v.process(warm.data(), 480);   // 10 ms: amp env settles
    std::vector<float> a(2400);   v.process(a.data(), 2400);     // filter still open
    std::vector<float> b(2400);
    for (int k = 0; k < 10; ++k)  v.process(b.data(), 2400);     // ~0.5 s later (filter closed)
    CHECK(rms(a.data(), 2400) > 0.0f);                           // audible
    CHECK(hfEnergy(a) > hfEnergy(b));                            // brighter while open
}

TEST_CASE("voice: the filter envelope decays to ~1/e after the decay time") {
    AcidVoice v; v.setSampleRate(48000); v.setDecay(0.1f);
    v.noteOn(60, 100, false);
    CHECK(v.filtEnv() == doctest::Approx(1.0f));        // just retriggered
    std::vector<float> buf(4800);
    v.process(buf.data(), 4800);                        // 0.1 s = one decay time
    CHECK(v.filtEnv() == doctest::Approx(std::exp(-1.0f)).epsilon(0.02));
}

TEST_CASE("voice: a slid note glides to the target pitch") {
    AcidVoice v; v.setSampleRate(48000); v.setSlideTime(0.05f);
    v.noteOn(48, 100, false);
    CHECK(v.currentFreq() == doctest::Approx(noteFreq(48)).epsilon(0.001));
    float f48 = v.currentFreq();
    v.noteOn(60, 100, true);                            // legato slide up an octave
    std::vector<float> mid(240); v.process(mid.data(), 240);   // 5 ms in
    CHECK(v.currentFreq() > f48);
    CHECK(v.currentFreq() < noteFreq(60));              // partway between
    std::vector<float> rest(30000); v.process(rest.data(), 30000);  // well past the glide
    CHECK(v.currentFreq() == doctest::Approx(noteFreq(60)).epsilon(0.01));
}

TEST_CASE("voice: an accented (high-velocity) note is louder than a soft one") {
    auto rmsAt = [](int vel) {
        AcidVoice v; v.setSampleRate(48000);
        v.setAccent(1.0f); v.setCutoff(2000.0f); v.setEnvMod(0.3f);
        v.noteOn(48, vel, false);
        std::vector<float> b(4800); v.process(b.data(), 4800);
        return rms(b.data(), 4800);
    };
    CHECK(rmsAt(127) > rmsAt(40));
}

TEST_CASE("voice: output is bounded for any distortion and changes the signal") {
    auto run = [](float dist) {
        AcidVoice v; v.setSampleRate(48000);
        v.setDistortion(dist); v.setCutoff(4000.0f); v.setResonance(0.6f);
        v.noteOn(48, 110, false);
        std::vector<float> b(4800); v.process(b.data(), 4800);
        return b;
    };
    auto clean = run(0.0f), dirty = run(0.9f);
    float mc = 0, md = 0; double diff = 0;
    for (int i = 0; i < 4800; ++i) {
        mc = std::max(mc, std::fabs(clean[i]));
        md = std::max(md, std::fabs(dirty[i]));
        diff += std::fabs(clean[i] - dirty[i]);
    }
    CHECK(mc <= 1.0f);
    CHECK(md <= 1.0f);
    CHECK(diff > 0.0);
}

TEST_CASE("voice: stays finite and bounded under an extreme-parameter sweep") {
    AcidVoice v; v.setSampleRate(48000);
    v.setCutoff(12000.0f); v.setResonance(1.0f); v.setFilterFM(1.0f);
    v.setDistortion(1.0f); v.setEnvMod(1.0f); v.setSubLevel(1.0f);
    v.noteOn(60, 127, false);
    std::vector<float> b(48000); v.process(b.data(), 48000);
    bool ok = true;
    for (int i = 0; i < 48000; ++i)
        if (!std::isfinite(b[i]) || std::fabs(b[i]) > 1.0f) { ok = false; break; }
    CHECK(ok);
}

TEST_CASE("voice: last-note priority falls back to the held note on release") {
    AcidVoice v; v.setSampleRate(48000);
    v.noteOn(48, 100, false);
    CHECK(v.currentFreq() == doctest::Approx(noteFreq(48)).epsilon(0.001));
    v.noteOn(60, 100, false);                      // newer note takes over
    CHECK(v.currentFreq() == doctest::Approx(noteFreq(60)).epsilon(0.001));
    v.noteOff(60);                                 // fall back to the still-held 48
    CHECK(v.currentFreq() == doctest::Approx(noteFreq(48)).epsilon(0.001));
    v.noteOff(99);                                 // unheld note -> no change
    CHECK(v.currentFreq() == doctest::Approx(noteFreq(48)).epsilon(0.001));
    v.noteOff(48);                                 // all released -> stays finite
    std::vector<float> b(480); v.process(b.data(), 480);
    bool ok = true; for (float x : b) if (!std::isfinite(x)) ok = false;
    CHECK(ok);
}

TEST_CASE("voice: the square waveform with key-track makes valid, bounded sound") {
    AcidVoice v; v.setSampleRate(48000);
    v.setWaveform(1); v.setKeyTrack(1.0f); v.setCutoff(1000.0f);
    v.noteOn(72, 110, false);
    std::vector<float> b(4800); v.process(b.data(), 4800);
    bool ok = true;
    for (float x : b) if (!std::isfinite(x) || std::fabs(x) > 1.0f) ok = false;
    CHECK(ok);
    CHECK(rms(b.data(), 4800) > 0.0f);
}

TEST_CASE("voice: an out-of-range MIDI note is clamped (no inf/NaN)") {
    AcidVoice v; v.setSampleRate(48000);
    v.noteOn(2000, 127, false);                    // absurd note -> clamped to 127
    std::vector<float> b(480); v.process(b.data(), 480);
    bool ok = true; for (float x : b) if (!std::isfinite(x)) ok = false;
    CHECK(ok);
}

// Peak |amplitude| of a held note rendered by a fresh voice; optionally setLevel first.
static float voicePeak(float level, bool applyLevel) {
    AcidVoice v; v.setSampleRate(48000);
    v.setCutoff(2000.0f); v.setResonance(0.3f); v.setEnvMod(0.3f); v.setDistortion(0.0f);
    if (applyLevel) v.setLevel(level);
    v.noteOn(48, 110, false);
    std::vector<float> b(4800); v.process(b.data(), 4800);
    float m = 0.0f; for (float x : b) m = std::max(m, std::fabs(x));
    return m;
}

TEST_CASE("voice: level scales the output amplitude linearly") {
    float full = voicePeak(1.0f, true);
    float half = voicePeak(0.5f, true);
    REQUIRE(full > 0.01f);                                  // audible reference
    CHECK(half == doctest::Approx(0.5f * full).epsilon(0.02));
}

TEST_CASE("voice: level 0 is silent") {
    AcidVoice v; v.setSampleRate(48000);
    v.setLevel(0.0f);
    v.noteOn(48, 110, false);
    std::vector<float> b(4800); v.process(b.data(), 4800);
    for (float x : b) CHECK(x == 0.0f);
}

TEST_CASE("voice: the default output level is 0.7") {
    float dflt  = voicePeak(0.0f, false);                   // fresh voice, no setLevel
    float unity = voicePeak(1.0f, true);
    REQUIRE(unity > 0.01f);
    CHECK(dflt == doctest::Approx(0.7f * unity).epsilon(0.02));
}

TEST_CASE("AcidNode renders audio for a MIDI note and decays after note-off") {
    AcidNode node;
    auto eval = [&](const std::vector<MidiEvent>& ev, float dt) {
        std::vector<Value> in(14);
        in[0]  = MidiRef{ ev.data(), ev.size() };
        in[1]  = 0.0f;   // waveform (Saw)
        in[2]  = 800.0f; // cutoff
        in[3]  = 0.7f;   // resonance
        in[4]  = 0.6f;   // env mod
        in[5]  = 0.3f;   // decay
        in[6]  = 0.4f;   // accent
        in[7]  = 0.0f;   // sub level
        in[8]  = 0.0f;   // slide
        in[9]  = 0.08f;  // slide time
        in[10] = 0.0f;   // filter FM
        in[11] = 0.0f;   // key track
        in[12] = 0.0f;   // distortion
        in[13] = 0.7f;   // level
        std::vector<Value> out(1);
        EvalContext ctx{ in, out, dt };
        node.evaluate(ctx);
        AudioRef a = std::get<AudioRef>(out[0]);
        return std::vector<float>(a.samples, a.samples + a.count);
    };
    auto first = eval({ midiNoteOn(48, 110) }, 0.04f);
    REQUIRE(first.size() > 0);
    CHECK(rms(first.data(), (int)first.size()) > 1e-3f);   // audible
    eval({ midiNoteOff(48) }, 0.04f);                      // release
    auto tail = eval({}, 0.04f);                           // a block later
    CHECK(rms(tail.data(), (int)tail.size()) < 1e-3f);     // decayed to ~silence
}
