# 3-Band Crossover Filter Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a **Crossover Filter** audio node — one mono input split into bass / mid / treble mono outputs by two cascaded resonant crossovers, with per-crossover cutoff + resonance Float inputs.

**Architecture:** A new GL-free, header-only TPT state-variable filter (`audio/StateVariableFilter.h`) yields lowpass/bandpass/highpass from one cutoff + resonance. A new header-only node (`modules/CrossoverFilterNode.h`) holds two of them wired as a cascade: SVF@low-cutoff → `bass` (its lowpass) + remainder (its highpass); SVF@high-cutoff on the remainder → `mid` (lowpass) + `treble` (highpass). Registered in the app's `"Audio"` category.

**Tech Stack:** C++17, doctest (`core_tests`), CMake FetchContent. No GL, no persistence, no editor change.

**Spec:** `docs/superpowers/specs/2026-07-01-crossover-filter-design.md`

---

### Task 1: `StateVariableFilter` DSP primitive

The GL-free 2-pole TPT state-variable filter (Zavalishin/Cytomic). Produces low/band/high taps from one cutoff + resonance; unconditionally stable. Unit-tested for frequency response and stability.

**Files:**
- Create: `src/audio/StateVariableFilter.h`
- Create: `tests/test_state_variable_filter.cpp`
- Modify: `CMakeLists.txt` (add the test source to `core_tests`, after line 311 `tests/test_asset_library_file.cpp`)

- [ ] **Step 1: Write the failing test**

Create `tests/test_state_variable_filter.cpp`:

```cpp
#include <doctest/doctest.h>
#include "audio/StateVariableFilter.h"
#include <cmath>

using namespace oss;

static const float kPi = 3.14159265358979323846f;

// RMS of one band tap (0=low, 1=band, 2=high) after driving a sine of `hz` and
// letting the filter settle. Uses the filter's current coefficients.
static float bandRms(StateVariableFilter& f, float hz, int sr, int which) {
    double sumsq = 0.0;
    int warm = sr / 10;   // ~100 ms warm-up
    int meas = sr / 10;   // ~100 ms measurement
    int phase = 0;
    for (int i = 0; i < warm + meas; ++i, ++phase) {
        float in = std::sin(2.0f * kPi * hz * (float)phase / (float)sr);
        SvfOut o = f.process(in);
        float y = which == 0 ? o.low : (which == 1 ? o.band : o.high);
        if (i >= warm) sumsq += (double)y * y;
    }
    return (float)std::sqrt(sumsq / (double)meas);
}

static float measureRms(float cutoff, float res, float hz, int sr, int which) {
    StateVariableFilter f;
    f.setCoefficients(cutoff, res, sr);
    return bandRms(f, hz, sr, which);
}

TEST_CASE("SVF: DC passes the lowpass, is blocked by the highpass") {
    StateVariableFilter f;
    f.setCoefficients(1000.0f, 0.0f, 48000);
    SvfOut o{};
    for (int i = 0; i < 48000; ++i) o = f.process(1.0f);   // settle on a constant
    CHECK(o.low  == doctest::Approx(1.0f).epsilon(0.01));
    CHECK(o.high == doctest::Approx(0.0f).epsilon(0.01));
}

TEST_CASE("SVF: a tone below cutoff passes the lowpass, not the highpass") {
    int sr = 48000;
    float inRms = 1.0f / std::sqrt(2.0f);            // RMS of a unit sine
    float lp = measureRms(2000.0f, 0.0f, 100.0f, sr, 0);   // 100 Hz << 2 kHz
    float hp = measureRms(2000.0f, 0.0f, 100.0f, sr, 2);
    CHECK(lp == doctest::Approx(inRms).epsilon(0.1));       // LP passes
    CHECK(hp < 0.15f * inRms);                              // HP blocks
}

TEST_CASE("SVF: a tone above cutoff passes the highpass, not the lowpass") {
    int sr = 48000;
    float inRms = 1.0f / std::sqrt(2.0f);
    float hp = measureRms(500.0f, 0.0f, 8000.0f, sr, 2);   // 8 kHz >> 500 Hz
    float lp = measureRms(500.0f, 0.0f, 8000.0f, sr, 0);
    CHECK(hp == doctest::Approx(inRms).epsilon(0.15));      // HP passes
    CHECK(lp < 0.15f * inRms);                              // LP blocks
}

TEST_CASE("SVF: stays finite and bounded at maximum resonance") {
    StateVariableFilter f;
    f.setCoefficients(1000.0f, 1.0f, 48000);
    for (int i = 0; i < 96000; ++i) {
        float in = (i == 0) ? 1.0f : 0.5f * std::sin(0.3f * (float)i);   // impulse then drive
        SvfOut o = f.process(in);
        REQUIRE(std::isfinite(o.low));
        REQUIRE(std::isfinite(o.band));
        REQUIRE(std::isfinite(o.high));
        REQUIRE(std::fabs(o.low)  < 100.0f);
        REQUIRE(std::fabs(o.band) < 100.0f);
        REQUIRE(std::fabs(o.high) < 100.0f);
    }
}
```

- [ ] **Step 2: Register the test and verify it fails**

In `CMakeLists.txt`, add this line inside `add_executable(core_tests ...)` immediately after `tests/test_asset_library_file.cpp` (currently line 311):

```cmake
  tests/test_state_variable_filter.cpp
```

Run:
```bash
cmake --build build -j --target core_tests
```
Expected: FAIL — compile error, `audio/StateVariableFilter.h` file not found (the header doesn't exist yet).

- [ ] **Step 3: Write the implementation**

Create `src/audio/StateVariableFilter.h`:

```cpp
#pragma once
#include <cmath>

namespace oss {

// One evaluation of the filter yields all three band taps from the same state.
struct SvfOut { float low, band, high; };

// 2-pole topology-preserving-transform state-variable filter (Zavalishin/Cytomic).
// Coefficients are set per control-block from cutoff (Hz) + resonance (0..1); process()
// runs per audio sample. Integrator state persists across calls (and thus across frames).
// GL-free. Unconditionally stable for all cutoff/resonance in range.
struct StateVariableFilter {
    float ic1 = 0.0f, ic2 = 0.0f;                               // integrator memory
    float g = 0.0f, k = 2.0f, a1 = 0.0f, a2 = 0.0f, a3 = 0.0f;  // derived coefficients

    void reset() { ic1 = ic2 = 0.0f; }

    // fc in Hz (clamped to [20, 0.45*sr]); res in [0,1] (clamped). res maps to the
    // damping k = 1/Q: res 0 -> k=2 (Q=0.5, gentle), res 1 -> k~=0.02 (Q~=50, sharp peak).
    void setCoefficients(float fc, float res, int sr) {
        float ny = 0.45f * (float)sr;
        fc  = fc  < 20.0f ? 20.0f : (fc  > ny   ? ny   : fc);
        res = res < 0.0f  ? 0.0f  : (res > 1.0f ? 1.0f : res);
        g  = std::tan(3.14159265f * fc / (float)sr);
        k  = 2.0f - 1.98f * res;
        a1 = 1.0f / (1.0f + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
    }

    SvfOut process(float in) {
        float v3 = in - ic2;
        float v1 = a1 * ic1 + a2 * v3;
        float v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2.0f * v1 - ic1;
        ic2 = 2.0f * v2 - ic2;
        return { v2, v1, in - k * v1 - v2 };   // low, band, high
    }
};

} // namespace oss
```

- [ ] **Step 4: Build and run the test — verify it passes**

Run:
```bash
cmake --build build -j --target core_tests && ./build/core_tests -tc="SVF*"
```
Expected: PASS — all 4 `SVF: ...` cases green (`[doctest] Status: SUCCESS!`).

- [ ] **Step 5: Commit**

```bash
git add src/audio/StateVariableFilter.h tests/test_state_variable_filter.cpp CMakeLists.txt
git commit -m "feat(audio): TPT state-variable filter primitive"
```

---

### Task 2: `CrossoverFilterNode` + registration

The node: two `StateVariableFilter` members cascaded, three mono outputs. Registered so it appears in the editor's Audio category and round-trips through the makeNode factory.

**Files:**
- Create: `src/modules/CrossoverFilterNode.h`
- Create: `tests/test_crossover_filter.cpp`
- Modify: `CMakeLists.txt` (add the test source to `core_tests`, after the line added in Task 1)
- Modify: `src/app/Application.cpp` (include; `makeNode()`; `nodeCategories()` Audio list)

- [ ] **Step 1: Write the failing test**

Create `tests/test_crossover_filter.cpp`:

```cpp
#include <doctest/doctest.h>
#include "modules/CrossoverFilterNode.h"
#include "core/Node.h"
#include "core/Value.h"
#include <cmath>
#include <vector>

using namespace oss;

static const float kPi = 3.14159265358979323846f;

// Drive the node with a continuous sine of `hz` for several blocks (default cutoffs
// 200/2000 Hz, resonance 0) and return the summed squared energy of each band over
// the final settled block.
static void bandEnergies(float hz, double& bass, double& mid, double& treble) {
    CrossoverFilterNode n;
    int sr = 48000, block = 2048, phase = 0;
    std::vector<float> sig(block);
    bass = mid = treble = 0.0;
    for (int b = 0; b < 12; ++b) {
        for (int i = 0; i < block; ++i, ++phase)
            sig[i] = std::sin(2.0f * kPi * hz * (float)phase / (float)sr);
        std::vector<Value> in = { AudioRef{sig.data(), (std::size_t)block, sr},
                                  200.0f, 0.0f, 2000.0f, 0.0f };
        std::vector<Value> out(3);
        EvalContext ctx{in, out, 0.0426667};   // ~2048/48000 s
        n.evaluate(ctx);
        if (b == 11) {
            AudioRef B = std::get<AudioRef>(out[0]);
            AudioRef M = std::get<AudioRef>(out[1]);
            AudioRef T = std::get<AudioRef>(out[2]);
            for (std::size_t i = 0; i < B.count; ++i) bass   += (double)B.samples[i] * B.samples[i];
            for (std::size_t i = 0; i < M.count; ++i) mid    += (double)M.samples[i] * M.samples[i];
            for (std::size_t i = 0; i < T.count; ++i) treble += (double)T.samples[i] * T.samples[i];
        }
    }
}

TEST_CASE("Crossover Filter: a low tone lands mostly in the bass band") {
    double bass, mid, treble;
    bandEnergies(80.0f, bass, mid, treble);        // 80 Hz, below the 200 Hz low cutoff
    CHECK(bass > mid);
    CHECK(bass > treble);
    CHECK(treble < 0.1 * bass);
}

TEST_CASE("Crossover Filter: a high tone lands mostly in the treble band") {
    double bass, mid, treble;
    bandEnergies(8000.0f, bass, mid, treble);      // 8 kHz, above the 2 kHz high cutoff
    CHECK(treble > mid);
    CHECK(treble > bass);
    CHECK(bass < 0.1 * treble);
}

TEST_CASE("Crossover Filter: outputs are mono AudioRefs matching the input block") {
    CrossoverFilterNode n;
    std::vector<float> sig = {0.1f, -0.2f, 0.3f, -0.4f};
    std::vector<Value> in = { AudioRef{sig.data(), sig.size(), 44100},
                              200.0f, 0.0f, 2000.0f, 0.0f };
    std::vector<Value> out(3);
    EvalContext ctx{in, out, 0.016f};
    n.evaluate(ctx);
    for (int k = 0; k < 3; ++k) {
        AudioRef o = std::get<AudioRef>(out[k]);
        CHECK(o.count == sig.size());
        CHECK(o.sampleRate == 44100);
        REQUIRE(o.samples != nullptr);
    }
}

TEST_CASE("Crossover Filter: an unconnected input yields three empty blocks") {
    CrossoverFilterNode n;
    std::vector<Value> in = { AudioRef{}, 200.0f, 0.0f, 2000.0f, 0.0f };
    std::vector<Value> out(3);
    EvalContext ctx{in, out, 0.016f};
    n.evaluate(ctx);
    for (int k = 0; k < 3; ++k) {
        AudioRef o = std::get<AudioRef>(out[k]);
        CHECK(o.count == 0);
    }
}
```

- [ ] **Step 2: Register the test and verify it fails**

In `CMakeLists.txt`, add this line inside `add_executable(core_tests ...)` immediately after `tests/test_state_variable_filter.cpp` (the line added in Task 1):

```cmake
  tests/test_crossover_filter.cpp
```

Run:
```bash
cmake --build build -j --target core_tests
```
Expected: FAIL — compile error, `modules/CrossoverFilterNode.h` file not found.

- [ ] **Step 3: Write the node implementation**

Create `src/modules/CrossoverFilterNode.h`:

```cpp
#pragma once
#include <algorithm>
#include <cstddef>
#include <vector>
#include "core/Node.h"
#include "core/Value.h"
#include "audio/AudioBlock.h"
#include "audio/StateVariableFilter.h"

namespace oss {

// 3-band crossover filter: one mono input split into bass / mid / treble mono outputs
// by two cascaded state-variable crossovers. Float inputs: low cutoff + its resonance
// (bass | rest split), high cutoff + its resonance (mid | treble split). GL-free;
// filter state persists across per-frame blocks like a real-time filter.
class CrossoverFilterNode : public Node {
public:
    CrossoverFilterNode()
        : Node("Crossover Filter"),
          bass_(kAudioMaxBlock, 0.0f), mid_(kAudioMaxBlock, 0.0f), treble_(kAudioMaxBlock, 0.0f) {
        addInput("mono",           PortType::Audio, AudioRef{});
        addInput("low cutoff",     PortType::Float, 200.0f,  20.0f, 20000.0f);
        addInput("low resonance",  PortType::Float, 0.2f,    0.0f,  1.0f);
        addInput("high cutoff",    PortType::Float, 2000.0f, 20.0f, 20000.0f);
        addInput("high resonance", PortType::Float, 0.2f,    0.0f,  1.0f);
        addOutput("bass",   PortType::Audio);
        addOutput("mid",    PortType::Audio);
        addOutput("treble", PortType::Audio);
    }

    void evaluate(EvalContext& ctx) override {
        AudioRef m = ctx.in<AudioRef>(0);
        std::size_t n = m.samples ? std::min(m.count, (std::size_t)kAudioMaxBlock) : 0;
        int sr = (m.samples && m.sampleRate > 0) ? m.sampleRate : 48000;

        low_.setCoefficients (ctx.in<float>(1), ctx.in<float>(2), sr);   // low cutoff / low res
        high_.setCoefficients(ctx.in<float>(3), ctx.in<float>(4), sr);   // high cutoff / high res

        for (std::size_t i = 0; i < n; ++i) {
            SvfOut a = low_.process(m.samples[i]);   // split at the low cutoff
            bass_[i] = a.low;                         // below low cutoff
            SvfOut b = high_.process(a.high);         // split the remainder at the high cutoff
            mid_[i]    = b.low;                        // between the two cutoffs
            treble_[i] = b.high;                       // above high cutoff
        }

        ctx.out<AudioRef>(0, AudioRef{bass_.data(),   n, sr});
        ctx.out<AudioRef>(1, AudioRef{mid_.data(),    n, sr});
        ctx.out<AudioRef>(2, AudioRef{treble_.data(), n, sr});
    }

private:
    StateVariableFilter low_, high_;
    std::vector<float> bass_, mid_, treble_;
};

} // namespace oss
```

- [ ] **Step 4: Build and run the test — verify it passes**

Run:
```bash
cmake --build build -j --target core_tests && ./build/core_tests -tc="Crossover Filter*"
```
Expected: PASS — all 4 `Crossover Filter: ...` cases green.

- [ ] **Step 5: Register the node in the app**

In `src/app/Application.cpp`:

1. Add the include alongside the other `modules/` includes (after `#include "modules/StereoToMonoNode.h"`, currently line 12):

```cpp
#include "modules/CrossoverFilterNode.h"
```

2. In `makeNode()`, add after the `"Stereo to Mono"` line (currently line 58):

```cpp
    if (type == "Crossover Filter") return std::make_unique<CrossoverFilterNode>();
```

3. In `nodeCategories()`, add `"Crossover Filter"` to the `"Audio"` list, after `"Stereo to Mono"`:

```cpp
        { "Audio",   { "Sine", "Acid Bass", "Audio File", "Audio In", "Audio Mix", "Mono to Stereo", "Stereo to Mono", "Crossover Filter", "Spectrograph", "Oscilloscope", "Drum Machine", "Audio Out" } },
```

- [ ] **Step 6: Build the whole app and run the full test suite**

Run:
```bash
cmake --build build -j && ctest --test-dir build --output-on-failure
```
Expected: the app links, and all tests pass (`core_tests` includes the new SVF + Crossover cases).

- [ ] **Step 7: Commit**

```bash
git add src/modules/CrossoverFilterNode.h tests/test_crossover_filter.cpp CMakeLists.txt src/app/Application.cpp
git commit -m "feat(audio): Crossover Filter node (bass/mid/treble split)"
```

---

### Task 3: Documentation

Document the node in `CLAUDE.md` (architecture bullet) and `README.md` (node table row).

**Files:**
- Modify: `CLAUDE.md` (add a bullet after the "Acid Bass synth voice" bullet)
- Modify: `README.md` (add a row after the "Stereo to Mono" row, currently line 74)

- [ ] **Step 1: Add the CLAUDE.md architecture bullet**

In `CLAUDE.md`, immediately after the bullet that begins "**Acid Bass synth voice** — `AcidNode`" (it ends "...the node is header-only and GL-free."), insert:

```markdown
- **Crossover Filter** — `CrossoverFilterNode` (`src/modules/CrossoverFilterNode.h`, header-only,
  GL-free) splits one mono input into **bass / mid / treble** mono outputs with two cascaded
  crossovers: a state-variable filter at the `low cutoff` sends its lowpass to `bass` and its
  highpass on to a second filter at the `high cutoff`, whose lowpass is `mid` and highpass is
  `treble`. Each crossover has its own `cutoff` + `resonance` Float inputs. The GL-free
  `audio/StateVariableFilter.h` is the primitive — a 2-pole TPT (Zavalishin/Cytomic) filter that
  yields lowpass/bandpass/highpass from one cutoff + resonance and is unconditionally stable
  (unlike the lowpass-only `LadderFilter`), so no output clamp is needed. It's a musical filter,
  not a phase-flat mastering crossover — the bands cover the spectrum continuously and sum to
  approximately the input, and resonance emphasizes each crossover. Filter state persists across
  per-frame blocks. The primitive and the node's band-split are unit-tested in `core_tests`.
```

- [ ] **Step 2: Add the README.md node-table row**

In `README.md`, immediately after the `| **Stereo to Mono** | ...` row (currently line 74), insert:

```markdown
| **Crossover Filter** | split one mono signal into `bass` / `mid` / `treble` mono outputs by two cascaded resonant crossovers; each crossover has its own `cutoff` + `resonance` (musical state-variable filter, not a phase-flat mastering crossover) |
```

- [ ] **Step 3: Commit**

```bash
git add CLAUDE.md README.md
git commit -m "docs: document the Crossover Filter node"
```

---

## Notes for the implementer

- **Both new files are header-only** (`.h`), so only the two `tests/test_*.cpp` lines are added to `CMakeLists.txt` — no new `.cpp` in any source list.
- **C++ TDD "red" state** is a compile failure (the test includes a header that doesn't exist yet). That's expected at Step 2 of Tasks 1 and 2 — proceed to write the implementation.
- **`src/audio/` and `src/core/` stay GL-free** — neither new file includes any GL header (they don't).
- **Do not** `git add -A` / `git add .`. Stage only the files listed in each commit step. Leave the untracked `build.sh`, `preferences.oss`, `project.oss`, `examples/` alone.
- The `EvalContext{in, out, dt}` test-driving pattern mirrors `tests/test_audio_bridges.cpp`; input `Value`s are supplied in port order (`mono`, `low cutoff`, `low resonance`, `high cutoff`, `high resonance`).
