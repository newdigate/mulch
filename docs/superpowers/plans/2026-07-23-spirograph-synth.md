# Spirograph Synth Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a **Spirograph Synth** audio node that generates stereo audio directly from hypotrochoid / epitrochoid curves — θ advances at audio rate, curve x → left channel, y → right channel.

**Architecture:** A GL-free header-only DSP class `Spirograph` (`src/audio/Spirograph.h`) computes normalized, provably-bounded x/y per sample from a persisted θ accumulator. A header-only node `SpirographSynthNode` (`src/modules/SpirographSynthNode.h`) wraps one `Spirograph`, reads its input ports each frame, and publishes two mono `AudioRef` outputs. Unit-tested in `core_tests`; no GL, so no `gl_smoke`.

**Tech Stack:** C++17, doctest, CMake. Follows the existing `AcidVoice`/`AcidNode` and `SineWaveNode` patterns.

**Spec:** `docs/superpowers/specs/2026-07-23-spirograph-synth-design.md`

---

## File Structure

- **Create** `src/audio/Spirograph.h` — GL-free DSP class (curve math, θ accumulator, `process`).
- **Create** `src/modules/SpirographSynthNode.h` — header-only node wrapping one `Spirograph`.
- **Create** `tests/test_spirograph.cpp` — doctest unit tests for the DSP class + node.
- **Modify** `CMakeLists.txt` — add `tests/test_spirograph.cpp` to the `core_tests` sources.
- **Modify** `src/app/Application.cpp` — register the node in `makeNode()` + `nodeCategories()`.
- **Modify** `CLAUDE.md`, `README.md` — document the new node.

---

## Task 1: `Spirograph` DSP class (TDD)

**Files:**
- Create: `src/audio/Spirograph.h`
- Create: `tests/test_spirograph.cpp`
- Modify: `CMakeLists.txt` (add test to `core_tests`)

- [ ] **Step 1: Create the DSP header as a stub (compiles, emits silence)**

Create `src/audio/Spirograph.h`. This first version declares the full public interface but leaves `process` writing zeros, so the tests below compile and go red on the behavioral assertions (not on a compile error).

```cpp
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
        for (int i = 0; i < n; ++i) { outL[i] = 0.0f; outR[i] = 0.0f; }   // STUB (Step 4 implements)
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
```

- [ ] **Step 2: Write the failing tests**

Create `tests/test_spirograph.cpp` with the DSP tests (node tests are added in Task 2). Include only `audio/Spirograph.h` for now.

```cpp
#include <doctest/doctest.h>
#include "audio/Spirograph.h"
#include <cmath>
#include <vector>

using namespace oss;

TEST_CASE("Spirograph output is bounded to [-1,1] across the parameter space") {
    for (int curve = 0; curve <= 1; ++curve)
        for (float ratio = 2.0f; ratio <= 12.0f; ratio += 0.5f)
            for (float pen = 0.0f; pen <= 1.0f; pen += 0.1f) {
                Spirograph s;
                s.setSampleRate(48000); s.setCurveType(curve);
                s.setFrequency(220.0f); s.setRatio(ratio); s.setPen(pen); s.setLevel(1.0f);
                std::vector<float> L(256), R(256);
                s.process(L.data(), R.data(), 256);
                for (int i = 0; i < 256; ++i) {
                    CHECK(L[i] >= -1.0001f); CHECK(L[i] <= 1.0001f);
                    CHECK(R[i] >= -1.0001f); CHECK(R[i] <= 1.0001f);
                }
            }
}

TEST_CASE("Spirograph with pen=0 is quadrature sine (L=cos, R=sin, L^2+R^2 constant)") {
    Spirograph s;
    s.setSampleRate(48000); s.setCurveType(0);
    s.setFrequency(300.0f); s.setRatio(5.0f); s.setPen(0.0f); s.setLevel(1.0f);
    std::vector<float> L(512), R(512);
    s.process(L.data(), R.data(), 512);
    const double inc = Spirograph::kTwoPi * 300.0 / 48000.0;
    for (int i = 0; i < 512; ++i) {
        CHECK(L[i] == doctest::Approx(std::cos(inc * i)).epsilon(1e-3));
        CHECK(R[i] == doctest::Approx(std::sin(inc * i)).epsilon(1e-3));
        CHECK(L[i]*L[i] + R[i]*R[i] == doctest::Approx(1.0).epsilon(1e-3));
    }
}

TEST_CASE("Spirograph phase is continuous across process() blocks") {
    // Two 200-sample blocks must equal one 400-sample block, sample-for-sample.
    Spirograph split;
    split.setSampleRate(48000); split.setFrequency(440.0f);
    split.setRatio(4.0f); split.setPen(0.5f); split.setCurveType(0); split.setLevel(1.0f);
    std::vector<float> aL(200), aR(200), bL(200), bR(200);
    split.process(aL.data(), aR.data(), 200);
    split.process(bL.data(), bR.data(), 200);

    Spirograph whole;
    whole.setSampleRate(48000); whole.setFrequency(440.0f);
    whole.setRatio(4.0f); whole.setPen(0.5f); whole.setCurveType(0); whole.setLevel(1.0f);
    std::vector<float> wL(400), wR(400);
    whole.process(wL.data(), wR.data(), 400);

    for (int i = 0; i < 200; ++i) {
        CHECK(wL[i]       == doctest::Approx(aL[i]).epsilon(1e-4));
        CHECK(wL[200 + i] == doctest::Approx(bL[i]).epsilon(1e-4));
        CHECK(wR[i]       == doctest::Approx(aR[i]).epsilon(1e-4));
        CHECK(wR[200 + i] == doctest::Approx(bR[i]).epsilon(1e-4));
    }
}

TEST_CASE("Spirograph left channel oscillates at the requested frequency") {
    // pen=0 -> pure cos on L; count rising zero-crossings over 1 s ~= freq.
    Spirograph s;
    s.setSampleRate(48000); s.setFrequency(100.0f);
    s.setRatio(5.0f); s.setPen(0.0f); s.setCurveType(0); s.setLevel(1.0f);
    std::vector<float> L(48000), R(48000);
    s.process(L.data(), R.data(), 48000);
    int rising = 0;
    for (int i = 1; i < 48000; ++i)
        if (L[i-1] < 0.0f && L[i] >= 0.0f) ++rising;
    CHECK(rising >= 98);
    CHECK(rising <= 102);
}

TEST_CASE("Spirograph curve type changes the waveform") {
    Spirograph hypo;
    hypo.setSampleRate(48000); hypo.setFrequency(220.0f);
    hypo.setRatio(4.0f); hypo.setPen(0.7f); hypo.setCurveType(0); hypo.setLevel(1.0f);
    Spirograph epi;
    epi.setSampleRate(48000); epi.setFrequency(220.0f);
    epi.setRatio(4.0f); epi.setPen(0.7f); epi.setCurveType(1); epi.setLevel(1.0f);
    std::vector<float> hL(256), hR(256), eL(256), eR(256);
    hypo.process(hL.data(), hR.data(), 256);
    epi.process(eL.data(), eR.data(), 256);
    double diff = 0.0;
    for (int i = 0; i < 256; ++i) diff += std::abs(hL[i] - eL[i]);
    CHECK(diff > 1.0);   // materially different waveforms
}
```

- [ ] **Step 3: Wire the test into CMake**

In `CMakeLists.txt`, in the `add_executable(core_tests ...)` source list, add the new test file immediately after the `tests/test_panel_slot.cpp` line:

```cmake
  tests/test_panel_slot.cpp
  tests/test_spirograph.cpp
```

(`Spirograph.h` is header-only — do NOT add it to the sources; it is pulled in by the `#include`.)

- [ ] **Step 4: Build and run — verify RED**

Run:
```bash
cmake --build build --target core_tests -j && ./build/core_tests -tc="Spirograph*"
```
Expected: builds successfully, but the `pen=0 quadrature`, `frequency`, and `curve type` cases FAIL (stub emits zeros). The `bounded` and `continuity` cases pass trivially. This confirms the tests exercise real behavior.

- [ ] **Step 5: Implement `process` (make it green)**

Replace the STUB `process` body in `src/audio/Spirograph.h` with the real curve math:

```cpp
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
```

- [ ] **Step 6: Build and run — verify GREEN**

Run:
```bash
cmake --build build --target core_tests -j && ./build/core_tests -tc="Spirograph*"
```
Expected: all Spirograph test cases PASS.

- [ ] **Step 7: Commit**

```bash
git add src/audio/Spirograph.h tests/test_spirograph.cpp CMakeLists.txt
git commit -m "feat(audio): Spirograph stereo oscillator DSP + tests"
```

---

## Task 2: `SpirographSynthNode` (TDD)

**Files:**
- Create: `src/modules/SpirographSynthNode.h`
- Modify: `tests/test_spirograph.cpp` (add node test)

- [ ] **Step 1: Create the node header as a stub (ports declared, emits silence)**

Create `src/modules/SpirographSynthNode.h`. Ports and outputs are fully declared (so frame count is correct) but `evaluate` does NOT drive the voice yet — the buffers stay zero, so the node test goes red only on the "stereo differs" assertion.

```cpp
#pragma once
#include <cmath>
#include <cstddef>
#include <vector>
#include "core/Node.h"
#include "core/Value.h"
#include "audio/Spirograph.h"
#include "audio/AudioBlock.h"

namespace oss {

// Stereo Spirograph oscillator node: audio generated directly from hypotrochoid /
// epitrochoid curves. x -> left, y -> right. Every control is an input port (CV-able);
// wire the LFO node into `ratio` to morph the timbre. GL-free.
//
// Inputs: 0 = curve type (choice), 1 = freq, 2 = ratio, 3 = pen, 4 = phase, 5 = level.
class SpirographSynthNode : public Node {
public:
    SpirographSynthNode()
        : Node("Spirograph Synth"),
          bufL_(kAudioMaxBlock, 0.0f), bufR_(kAudioMaxBlock, 0.0f) {
        addChoiceInput("curve type", {"Hypotrochoid", "Epitrochoid"}, 0);
        addInput("freq",  PortType::Float, 110.0f, 1.0f, 1000.0f);   // Hz
        addInput("ratio", PortType::Float, 3.0f,   2.0f, 12.0f);     // R/r
        addInput("pen",   PortType::Float, 0.5f,   0.0f, 1.0f);      // d, fraction
        addInput("phase", PortType::Float, 0.0f,   0.0f, 1.0f);      // turns
        addInput("level", PortType::Float, 0.8f,   0.0f, 1.0f);
        addOutput("left",  PortType::Audio);
        addOutput("right", PortType::Audio);
        voice_.setSampleRate(sampleRate_);
    }

    void evaluate(EvalContext& ctx) override {
        int n = audioBlockFrames(sampleRate_, ctx.dt);
        // STUB: buffers left at zero (Step 4 drives the voice).
        ctx.out<AudioRef>(0, AudioRef{bufL_.data(), (std::size_t)n, sampleRate_});
        ctx.out<AudioRef>(1, AudioRef{bufR_.data(), (std::size_t)n, sampleRate_});
    }

private:
    int sampleRate_ = 48000;
    Spirograph voice_;
    std::vector<float> bufL_, bufR_;
};

} // namespace oss
```

- [ ] **Step 2: Add the failing node test**

At the top of `tests/test_spirograph.cpp`, add the node include next to the existing include:

```cpp
#include "audio/Spirograph.h"
#include "modules/SpirographSynthNode.h"
#include "core/Node.h"
#include "core/Value.h"
```

Then append this test case to the end of the file:

```cpp
TEST_CASE("SpirographSynthNode emits two bounded AudioRefs of round(sampleRate*dt) frames") {
    SpirographSynthNode node;
    // ports: curve, freq, ratio, pen, phase, level
    std::vector<Value> in = { Value(0.0f), Value(220.0f), Value(4.0f),
                              Value(0.6f), Value(0.0f),   Value(1.0f) };
    std::vector<Value> out(2);
    EvalContext ctx{ in, out, 1.0f / 60.0f };
    node.evaluate(ctx);

    AudioRef l = std::get<AudioRef>(out[0]);
    AudioRef r = std::get<AudioRef>(out[1]);
    CHECK(l.sampleRate == 48000);
    CHECK(r.sampleRate == 48000);
    CHECK(l.count == (std::size_t)std::lround(48000.0 / 60.0));   // 800
    CHECK(r.count == l.count);
    bool stereo = false;
    for (std::size_t i = 0; i < l.count; ++i) {
        CHECK(l.samples[i] >= -1.0001f); CHECK(l.samples[i] <= 1.0001f);
        CHECK(r.samples[i] >= -1.0001f); CHECK(r.samples[i] <= 1.0001f);
        if (std::abs(l.samples[i] - r.samples[i]) > 1e-4f) stereo = true;
    }
    CHECK(stereo);   // left (x) and right (y) are genuinely different signals
}
```

- [ ] **Step 3: Build and run — verify RED**

Run:
```bash
cmake --build build --target core_tests -j && ./build/core_tests -tc="SpirographSynthNode*"
```
Expected: builds, but `CHECK(stereo)` FAILS (stub emits zeros → L == R everywhere). Counts/bounds pass.

- [ ] **Step 4: Drive the voice from `evaluate` (make it green)**

Replace the `evaluate` body in `src/modules/SpirographSynthNode.h`:

```cpp
    void evaluate(EvalContext& ctx) override {
        voice_.setCurveType((int)std::lround(ctx.in<float>(0)));
        voice_.setFrequency(ctx.in<float>(1));
        voice_.setRatio(ctx.in<float>(2));
        voice_.setPen(ctx.in<float>(3));
        voice_.setPhase(ctx.in<float>(4));
        voice_.setLevel(ctx.in<float>(5));

        int n = audioBlockFrames(sampleRate_, ctx.dt);
        voice_.process(bufL_.data(), bufR_.data(), n);
        ctx.out<AudioRef>(0, AudioRef{bufL_.data(), (std::size_t)n, sampleRate_});
        ctx.out<AudioRef>(1, AudioRef{bufR_.data(), (std::size_t)n, sampleRate_});
    }
```

- [ ] **Step 5: Build and run — verify GREEN**

Run:
```bash
cmake --build build --target core_tests -j && ./build/core_tests -tc="Spirograph*"
```
Expected: all Spirograph + SpirographSynthNode cases PASS.

- [ ] **Step 6: Commit**

```bash
git add src/modules/SpirographSynthNode.h tests/test_spirograph.cpp
git commit -m "feat(audio): Spirograph Synth node (stereo curve oscillator)"
```

---

## Task 3: Register the node in the app

**Files:**
- Modify: `src/app/Application.cpp`

- [ ] **Step 1: Add the include**

In `src/app/Application.cpp`, with the other `#include "modules/..."` lines (near `#include "modules/SineWaveNode.h"`), add:

```cpp
#include "modules/SpirographSynthNode.h"
```

- [ ] **Step 2: Register in `makeNode()`**

In `makeNode()`, next to the other audio entries (after the `"Acid Bass"` line), add:

```cpp
    if (type == "Spirograph Synth") return std::make_unique<SpirographSynthNode>();
```

- [ ] **Step 3: Add to the Audio category in `nodeCategories()`**

In `nodeCategories()`, add `"Spirograph Synth"` to the `"Audio"` category list (after `"Acid Bass"`):

```cpp
        { "Audio",   { "Sine", "Acid Bass", "Spirograph Synth", "Audio File", "Audio In", "Audio Mix", "Mono to Stereo", "Stereo to Mono", "Crossover Filter", "Spectrograph", "Oscilloscope", "Drum Machine", "Audio Out" } },
```

- [ ] **Step 4: Build the app to confirm it links**

Run:
```bash
cmake --build build --target shader_streamer -j
```
Expected: builds and links with no errors.

- [ ] **Step 5: Commit**

```bash
git add src/app/Application.cpp
git commit -m "feat(app): register Spirograph Synth node (Audio category)"
```

---

## Task 4: Documentation

**Files:**
- Modify: `CLAUDE.md`
- Modify: `README.md`

- [ ] **Step 1: Add a node bullet to `CLAUDE.md`**

In `CLAUDE.md`, in the Architecture section's node list, add a bullet after the **Acid Bass synth voice** bullet:

```markdown
- **Spirograph Synth** — `SpirographSynthNode` (`src/modules/SpirographSynthNode.h`,
  header-only) is a stereo audio oscillator generated directly from Spirograph curves. It
  wraps the GL-free `audio/Spirograph.h` DSP, advancing θ at audio rate so a
  hypotrochoid/epitrochoid curve's x → `left` and y → `right` mono outputs. `ratio` (R/r
  2..12) sets the bright partial's harmonic (k = ratio∓1), `pen` (d 0..1) fades it in,
  `curve type` picks hypo/epi; output is normalized by (bigArm+pen) so |x|,|y| ≤ 1 for all
  params (provably bounded, no clamp — like `StateVariableFilter`). Every control is an
  input port, so wiring an LFO into `ratio` morphs the timbre (the spec's multi-spiral
  engine / feedback / built-in morph LFO are deferred extensions). Unit-tested in
  `core_tests` (bounded, pen=0 quadrature, phase continuity, frequency, hypo≠epi); GL-free,
  no `gl_smoke`.
```

- [ ] **Step 2: Add a row to the `README.md` node table**

In `README.md`, add a row to the audio node table immediately after the **Acid Bass** row (line ~90):

```markdown
| **Spirograph Synth** | stereo oscillator straight from Spirograph curves: θ at audio rate, curve `x` → `left` / `y` → `right`. `curve type` (hypotrochoid / epitrochoid), `freq`, `ratio` (R/r, sets the bright partial), `pen` (fades that partial in), `phase`, `level`. Output is normalized so it can't clip. Every control is an input port — wire an **LFO** into `ratio` to morph |
```

- [ ] **Step 3: Commit**

```bash
git add CLAUDE.md README.md
git commit -m "docs: Spirograph Synth node"
```

---

## Final Verification

- [ ] **Full test suite passes**

Run:
```bash
cmake --build build -j && ctest --test-dir build --output-on-failure
```
Expected: `100% tests passed` (both `core_tests` and `gl_smoke`).
