# Acid Bass Synth Voice Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A monophonic 303-style "Acid Bass" synth node — MIDI in → mono audio out — with a saw/square VCO + sub-osc, a 4-pole resonant ladder filter (envelope/accent/key-track/filter-FM modulated), note slide, and a bounded `tanh` distortion stage.

**Architecture:** A GL-free, unit-testable `AcidVoice` DSP class (with a public `LadderFilter` struct) in `src/audio/`, wrapped by a thin header-only `AcidNode` in `src/modules/` that folds MIDI into note events, pushes control-port values, and renders one block per frame.

**Tech Stack:** C++17, doctest. GL-free audio DSP.

**Spec:** `docs/superpowers/specs/2026-06-17-acid-synth-design.md`

---

## File Structure

| File | Responsibility | Change |
|---|---|---|
| `src/audio/AcidVoice.h` | `LadderFilter` struct + `AcidVoice` class declaration | **create** |
| `src/audio/AcidVoice.cpp` | `AcidVoice` method implementations | **create** |
| `src/modules/AcidNode.h` | the node (ports + MIDI fold + block render) | **create** (header-only) |
| `src/app/Application.cpp` | register `"Acid Bass"` (factory + Audio category) | modify |
| `src/main.cpp` | add an Acid Bass node to the screenshot demo | modify |
| `tests/test_acid_voice.cpp` | filter, voice, and node tests | **create** |
| `CMakeLists.txt` | wire `AcidVoice.cpp` + the test | modify |
| `README.md`, `CLAUDE.md` | document the node | modify |

Each task ends green (build + `ctest`).

---

### Task 1: `LadderFilter`

**Files:**
- Create: `src/audio/AcidVoice.h`
- Test: `tests/test_acid_voice.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Write the failing test**

Create `tests/test_acid_voice.cpp`:

```cpp
#include <doctest/doctest.h>
#include "audio/AcidVoice.h"
#include <algorithm>
#include <cmath>
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
```

- [ ] **Step 2: Wire the test into CMake**

In `CMakeLists.txt`, in the `add_executable(core_tests ...)` list, add after
`  tests/test_step_sync.cpp`:

```cmake
  tests/test_acid_voice.cpp
```

- [ ] **Step 3: Run to verify it fails**

Run: `cmake --build build -j 2>&1 | tail -5`
Expected: compile error — `audio/AcidVoice.h` does not exist.

- [ ] **Step 4: Create the header with `LadderFilter`**

Create `src/audio/AcidVoice.h`:

```cpp
#pragma once
#include <cmath>

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
        float x  = in - s4 * fb;
        x *= 0.35013f * (f * f) * (f * f);
        s1 = x  + 0.3f * d1 + (1.0f - f) * s1;  d1 = x;
        s2 = s1 + 0.3f * d2 + (1.0f - f) * s2;  d2 = s1;
        s3 = s2 + 0.3f * d3 + (1.0f - f) * s3;  d3 = s2;
        s4 = s3 + 0.3f * d4 + (1.0f - f) * s4;  d4 = s3;
        return s4;
    }
};

} // namespace oss
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `cmake --build build -j && ./build/core_tests --test-case="*ladder filter*"`
Expected: PASS (3 cases).

- [ ] **Step 6: Full suite**

Run: `ctest --test-dir build --output-on-failure`
Expected: all pass.

- [ ] **Step 7: Commit**

```bash
git add src/audio/AcidVoice.h tests/test_acid_voice.cpp CMakeLists.txt
git commit -m "feat(audio): add 4-pole resonant ladder filter (LadderFilter)"
```

---

### Task 2: `AcidVoice` DSP

**Files:**
- Modify: `src/audio/AcidVoice.h` (add the `AcidVoice` class)
- Create: `src/audio/AcidVoice.cpp`
- Test: `tests/test_acid_voice.cpp` (append voice cases)
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Append the voice tests**

Append to `tests/test_acid_voice.cpp` (after the ladder tests):

```cpp
static float noteFreq(int n) { return 440.0f * std::pow(2.0f, (n - 69) / 12.0f); }

TEST_CASE("voice: a note sounds and its brightness falls as the envelope decays") {
    AcidVoice v; v.setSampleRate(48000);
    v.setCutoff(300.0f); v.setEnvMod(0.9f); v.setResonance(0.5f);
    v.setDecay(0.2f); v.setAccent(0.0f);
    v.noteOn(48, 100, false);
    std::vector<float> a(2400), b(2400);
    v.process(a.data(), 2400);                          // first ~50 ms (filter open)
    for (int k = 0; k < 8; ++k) v.process(b.data(), 2400);  // ~0.4 s later (filter closed)
    CHECK(rms(a.data(), 2400) > 0.0f);
    CHECK(rms(a.data(), 2400) > rms(b.data(), 2400));
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
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build build -j 2>&1 | tail -5`
Expected: compile error — `AcidVoice` is not declared.

- [ ] **Step 3: Add the `AcidVoice` class to the header**

In `src/audio/AcidVoice.h`, add these includes at the top (after `#include <cmath>`):

```cpp
#include <cstddef>
#include <vector>
```

Then, inside `namespace oss {` and **after** the `LadderFilter` struct (before the
closing `}` of the namespace), add:

```cpp
// Monophonic 303-style "acid bass" voice. MIDI-driven (last-note priority); renders
// mono samples. Saw/square VCO + sub-osc -> resonant ladder filter (env/accent/
// keytrack/filterFM modulated) -> VCA -> bounded tanh distortion. GL-free.
class AcidVoice {
public:
    void setSampleRate(int sr);
    void noteOn(int midiNote, int velocity, bool slide);
    void noteOff(int midiNote);

    void setWaveform(int w)     { waveform_ = w < 0 ? 0 : (w > 1 ? 1 : w); }
    void setCutoff(float hz)    { cutoff_ = hz < 20.0f ? 20.0f : (hz > 18000.0f ? 18000.0f : hz); }
    void setResonance(float r)  { resonance_ = clamp01(r); }
    void setEnvMod(float a)     { envMod_ = clamp01(a); }
    void setDecay(float s)      { decay_ = s < 1e-3f ? 1e-3f : s; updateCoefs(); }
    void setAccent(float a)     { accent_ = clamp01(a); }
    void setSubLevel(float a)   { subLevel_ = clamp01(a); }
    void setSlideTime(float s)  { slideTime_ = s < 1e-3f ? 1e-3f : s; updateCoefs(); }
    void setFilterFM(float a)   { filterFM_ = clamp01(a); }
    void setKeyTrack(float a)   { keyTrack_ = clamp01(a); }
    void setDistortion(float a) { distortion_ = clamp01(a); }

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
```

- [ ] **Step 4: Create the implementation**

Create `src/audio/AcidVoice.cpp`:

```cpp
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
        lastOut_ = s;
        out[i] = std::tanh(s * (1.0f + distortion_ * 9.0f));
    }
}

} // namespace oss
```

- [ ] **Step 5: Wire the source into CMake**

In `CMakeLists.txt`:
- In `set(APP_SOURCES ...)`, add after `  src/audio/SignalGenerator.cpp`:
```cmake
  src/audio/AcidVoice.cpp
```
- In `add_executable(core_tests ...)`, add after its `  src/audio/SignalGenerator.cpp` line:
```cmake
  src/audio/AcidVoice.cpp
```

- [ ] **Step 6: Run the voice tests**

Run: `cmake --build build -j && ./build/core_tests --test-case="voice:*"`
Expected: PASS (6 cases).

- [ ] **Step 7: Full suite**

Run: `ctest --test-dir build --output-on-failure`
Expected: all pass.

- [ ] **Step 8: Commit**

```bash
git add src/audio/AcidVoice.h src/audio/AcidVoice.cpp tests/test_acid_voice.cpp CMakeLists.txt
git commit -m "feat(audio): add AcidVoice 303-style monophonic synth DSP"
```

---

### Task 3: `AcidNode` + registration

**Files:**
- Create: `src/modules/AcidNode.h`
- Modify: `src/app/Application.cpp`, `src/main.cpp`
- Test: `tests/test_acid_voice.cpp` (append a node case)

- [ ] **Step 1: Append the node test**

Add these includes to the top of `tests/test_acid_voice.cpp` (after
`#include "audio/AcidVoice.h"`):

```cpp
#include "modules/AcidNode.h"
#include "core/Node.h"
#include "core/Value.h"
#include <variant>
```

Then append this test case to the end of the file:

```cpp
TEST_CASE("AcidNode renders audio for a MIDI note and decays after note-off") {
    AcidNode node;
    auto eval = [&](const std::vector<MidiEvent>& ev, float dt) {
        std::vector<Value> in(13);
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
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build build -j 2>&1 | tail -5`
Expected: compile error — `modules/AcidNode.h` does not exist.

- [ ] **Step 3: Create `AcidNode.h`**

Create `src/modules/AcidNode.h`:

```cpp
#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>
#include "core/Node.h"
#include "core/Value.h"
#include "audio/AcidVoice.h"

namespace oss {

// 303-style monophonic "acid bass" synth voice: MIDI in -> mono audio out. Wraps the
// GL-free AcidVoice DSP; every control is an input port (wire any for CV/automation).
//
// Inputs: 0 = midi, 1 = waveform (choice), 2 = cutoff, 3 = resonance, 4 = env mod,
//   5 = decay, 6 = accent, 7 = sub level, 8 = slide, 9 = slide time, 10 = filter FM,
//   11 = key track, 12 = distortion.
class AcidNode : public Node {
public:
    AcidNode() : Node("Acid Bass"), buffer_(kMaxBlock, 0.0f) {
        addInput("midi", PortType::Midi, MidiRef{});
        addChoiceInput("waveform", {"Saw", "Square"}, 0);
        addInput("cutoff",     PortType::Float, 800.0f, 20.0f,  12000.0f);
        addInput("resonance",  PortType::Float, 0.7f,   0.0f,   1.0f);
        addInput("env mod",    PortType::Float, 0.6f,   0.0f,   1.0f);
        addInput("decay",      PortType::Float, 0.3f,   0.03f,  2.0f);
        addInput("accent",     PortType::Float, 0.4f,   0.0f,   1.0f);
        addInput("sub level",  PortType::Float, 0.0f,   0.0f,   1.0f);
        addInput("slide",      PortType::Float, 0.0f,   0.0f,   1.0f);
        addInput("slide time", PortType::Float, 0.08f,  0.005f, 0.5f);
        addInput("filter FM",  PortType::Float, 0.0f,   0.0f,   1.0f);
        addInput("key track",  PortType::Float, 0.0f,   0.0f,   1.0f);
        addInput("distortion", PortType::Float, 0.0f,   0.0f,   1.0f);
        addOutput("audio", PortType::Audio);
        voice_.setSampleRate(sampleRate_);
    }

    void evaluate(EvalContext& ctx) override {
        bool slide = ctx.in<float>(8) >= 0.5f;
        MidiRef midi = ctx.in<MidiRef>(0);
        for (std::size_t i = 0; i < midi.count; ++i) {
            const MidiEvent& e = midi.events[i];
            if (midiIsNoteOn(e))       voice_.noteOn(e.data1, e.data2, slide);
            else if (midiIsNoteOff(e)) voice_.noteOff(e.data1);
        }
        voice_.setWaveform((int)std::lround(ctx.in<float>(1)));
        voice_.setCutoff(ctx.in<float>(2));
        voice_.setResonance(ctx.in<float>(3));
        voice_.setEnvMod(ctx.in<float>(4));
        voice_.setDecay(ctx.in<float>(5));
        voice_.setAccent(ctx.in<float>(6));
        voice_.setSubLevel(ctx.in<float>(7));
        voice_.setSlideTime(ctx.in<float>(9));
        voice_.setFilterFM(ctx.in<float>(10));
        voice_.setKeyTrack(ctx.in<float>(11));
        voice_.setDistortion(ctx.in<float>(12));

        int n = std::clamp((int)std::lround(sampleRate_ * (double)ctx.dt), 1, kMaxBlock);
        voice_.process(buffer_.data(), n);
        ctx.out<AudioRef>(0, AudioRef{buffer_.data(), (std::size_t)n, sampleRate_, 1});
    }

private:
    static constexpr int kMaxBlock = 2048;
    int sampleRate_ = 48000;
    AcidVoice voice_;
    std::vector<float> buffer_;   // owns the samples the AudioRef points at
};

} // namespace oss
```

- [ ] **Step 4: Register the node type**

In `src/app/Application.cpp`:
- Add the include after `#include "modules/AudioPlayerNode.h"`:
```cpp
#include "modules/AcidNode.h"
```
- In `makeNode()`, add after the `if (type == "Sine")` line:
```cpp
    if (type == "Acid Bass")   return std::make_unique<AcidNode>();
```
- In `nodeCategories()`, change the `Audio` line from:
```cpp
        { "Audio",   { "Sine", "Audio File", "Audio In", "Audio Mix", "Spectrograph", "Audio Out" } },
```
to:
```cpp
        { "Audio",   { "Sine", "Acid Bass", "Audio File", "Audio In", "Audio Mix", "Spectrograph", "Audio Out" } },
```

- [ ] **Step 5: Add an Acid Bass node to the screenshot demo**

In `src/main.cpp`, in `runScreenshot`, after the `app.addNodeOfType("LFO", ...)` line
(and before `app.graph().transport().bpm = 120.0;`), add:

```cpp
        app.addNodeOfType("Acid Bass", glm::vec2(740.0f, 320.0f));
```

- [ ] **Step 6: Build + run the tests**

Run: `cmake --build build -j && ./build/core_tests --test-case="AcidNode*" && ctest --test-dir build --output-on-failure 2>&1 | tail -4`
Expected: the node test passes; full suite green.

- [ ] **Step 7: Visual check**

Run: `./build/shader_streamer --screenshot build/_ui.png`
Expected: stderr prints `wrote screenshot build/_ui.png (...)`. Open it and confirm an
`Acid Bass` node renders with a `waveform` dropdown, the control sliders (cutoff,
resonance, env mod, decay, accent, sub level, slide, slide time, filter FM, key
track, distortion), the `midi` input, and an `audio` output. (If headless without a
GL context, note that this step was skipped.)

- [ ] **Step 8: Commit**

```bash
git add src/modules/AcidNode.h src/app/Application.cpp src/main.cpp tests/test_acid_voice.cpp
git commit -m "feat(modules): add Acid Bass synth node + register it"
```

---

### Task 4: Documentation

**Files:**
- Modify: `README.md`, `CLAUDE.md`

- [ ] **Step 1: Add the README module-table row**

In `README.md`, in the module table, add this row immediately after the `| **Sine** | ... |` row:

```markdown
| **Acid Bass** | 303-style monophonic synth: MIDI in → mono audio. Saw/square VCO + sub-osc → 4-pole resonant ladder filter (decay · env-mod · accent) → VCA → distortion, with note slide, filter FM (VCA → cutoff), and filter key-tracking. Every control is an input port |
```

- [ ] **Step 2: Note the synth voice in CLAUDE.md**

In `CLAUDE.md`, in the **Architecture** section, add a new bullet immediately after
the **Transport-synced sequencers** bullet (it ends with "... off their own
`tempo`/`rate`, unchanged."):

```markdown
- **Acid Bass synth voice** — `AcidNode` (`src/modules/AcidNode.h`, header-only) is
  the first MIDI-in → audio-out synth: it wraps the GL-free `AcidVoice` DSP
  (`src/audio/AcidVoice.{h,cpp}`) — a monophonic 303-style voice (saw/square VCO +
  sub-osc → a compact 4-pole resonant `LadderFilter` modulated by an envelope /
  accent / key-track / filter-FM → VCA → a `tanh` distortion stage, plus note slide).
  The output `tanh` keeps it bounded to `[-1,1]` regardless of resonance/FM. The
  voice is unit-tested in `core_tests`; the node is header-only and GL-free.
```

- [ ] **Step 3: Sanity check + commit**

Run: `ctest --test-dir build --output-on-failure`
Expected: all pass (no code changed).

```bash
git add README.md CLAUDE.md
git commit -m "docs: document the Acid Bass synth node"
```

---

## Final verification

- [ ] `cmake --build build -j` — clean build
- [ ] `ctest --test-dir build --output-on-failure` — all pass (`core_tests`, `gl_smoke`)
- [ ] `./build/shader_streamer --screenshot build/_ui.png` — the Acid Bass node renders with its ports
- [ ] Use superpowers:finishing-a-development-branch
