# Oscilloscope Node Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add an **Oscilloscope** node that turns an audio signal into a line-strip vertex buffer (`VertexRef`) — a triggered/free-running waveform or an X-Y vectorscope — to wire into the Wireframe renderer.

**Architecture:** A GL-free vertex builder (`src/core/Oscilloscope.{h,cpp}`) holds all the trace math (downmix, rising-edge trigger, X-Y mapping, resample) and is fully unit-tested in `core_tests`. A header-only node (`src/modules/OscilloscopeNode.h`) owns the rolling audio history + an internal `SignalGenerator` fallback + the VBO upload, and publishes the geometry on output 0. This mirrors `SpectrographNode` + `audio/FFT`.

**Tech Stack:** C++17, OpenGL 4.1 (glad), doctest, CMake. Design: `docs/superpowers/specs/2026-06-18-oscilloscope-node-design.md`.

**IMPORTANT for the implementer:** The node header (`OscilloscopeNode.h`) `#include`s `<glad/gl.h>`, so it **must not** be included from `core_tests` (which links no GL). The unit test includes **only** `core/Oscilloscope.h` and tests `buildScopeVertices` directly. The node itself is verified by building the app + a screenshot, exactly like the prior MIDI File node.

---

### Task 1: GL-free vertex builder + unit tests

**Files:**
- Create: `src/core/Oscilloscope.h`
- Create: `src/core/Oscilloscope.cpp`
- Create: `tests/test_oscilloscope.cpp`
- Modify: `CMakeLists.txt` (add `Oscilloscope.cpp` to `APP_SOURCES` and `core_tests`; add the test)

- [ ] **Step 1: Write the header**

Create `src/core/Oscilloscope.h`:

```cpp
#pragma once
#include <cstddef>
#include <vector>

namespace oss {

// How the oscilloscope plots the incoming audio.
enum class ScopeMode { Waveform, XY };

// Fill `out` with `pointCount` vec3 positions (x,y,z) describing a scope trace as a
// line strip. histL/histR are rolling sample histories ordered oldest..newest, length
// n (a mono source passes the same pointer for both). `windowSamples` is how many of
// the most-recent samples the trace spans (clamped to [2, n] internally). GL-free.
//
//  - Waveform: signal = 0.5*(L+R). The window is the most-recent `windowSamples`. When
//    `trigger`, the window start shifts back to the latest rising zero-crossing
//    (s[i-1] < 0 <= s[i]) within a one-window look-back so a steady tone stands still;
//    if none is found it stays free-running. Resampled to `pointCount` points (linear):
//        x = -1 + 2*j/(pointCount-1),  y = sample*gain,  z = 0.
//  - XY: `trigger` is ignored. The most-recent `windowSamples` (L,R) pairs are resampled
//    to `pointCount` points:  x = L*gain,  y = R*gain,  z = 0.
//
// `out` is resized to pointCount*3. With n < 2 (or a null pointer) it is filled with
// zeros (a flat trace).
void buildScopeVertices(const float* histL, const float* histR, std::size_t n,
                        int windowSamples, int pointCount, ScopeMode mode,
                        bool trigger, float gain, std::vector<float>& out);

} // namespace oss
```

- [ ] **Step 2: Write the failing tests**

Create `tests/test_oscilloscope.cpp`:

```cpp
#include <doctest/doctest.h>
#include "core/Oscilloscope.h"
#include <cmath>
#include <vector>

using namespace oss;

static constexpr double kPi = 3.14159265358979323846;

// A sine history of length n at `freq` Hz / `sr`, with an optional phase offset (rad).
static std::vector<float> sineHist(std::size_t n, double freq, double sr, double phase = 0.0) {
    std::vector<float> v(n);
    for (std::size_t i = 0; i < n; ++i)
        v[i] = (float)std::sin(2.0 * kPi * freq * (double)i / sr + phase);
    return v;
}

TEST_CASE("free-running waveform maps the window samples to a line strip") {
    const int N = 2000, P = 512;
    std::vector<float> ramp(N);
    for (int i = 0; i < N; ++i) ramp[i] = -1.0f + 2.0f * (float)i / (N - 1);   // -1..+1
    std::vector<float> out;
    buildScopeVertices(ramp.data(), ramp.data(), N, /*window*/ N, P,
                       ScopeMode::Waveform, /*trigger*/ false, /*gain*/ 1.0f, out);
    REQUIRE(out.size() == (std::size_t)P * 3);
    CHECK(out[0] == doctest::Approx(-1.0f));               // first x
    CHECK(out[(P - 1) * 3] == doctest::Approx(1.0f));      // last x
    CHECK(out[1] == doctest::Approx(-1.0f));               // first y == ramp[0]
    CHECK(out[(P - 1) * 3 + 1] == doctest::Approx(1.0f));  // last y == ramp[N-1]
    CHECK(out[2] == doctest::Approx(0.0f));                // z flat
}

TEST_CASE("gain scales the waveform amplitude") {
    const int N = 2000, P = 512;
    std::vector<float> ramp(N);
    for (int i = 0; i < N; ++i) ramp[i] = -1.0f + 2.0f * (float)i / (N - 1);
    std::vector<float> g1, g2;
    buildScopeVertices(ramp.data(), ramp.data(), N, N, P, ScopeMode::Waveform, false, 1.0f, g1);
    buildScopeVertices(ramp.data(), ramp.data(), N, N, P, ScopeMode::Waveform, false, 2.0f, g2);
    for (int j = 0; j < P; ++j)
        CHECK(g2[j * 3 + 1] == doctest::Approx(2.0f * g1[j * 3 + 1]));
}

TEST_CASE("trigger starts the waveform at a rising zero-crossing") {
    const int N = 1500, P = 300;
    auto sine = sineHist(N, 480.0, 48000.0);   // period 100 samples
    std::vector<float> out;
    buildScopeVertices(sine.data(), sine.data(), N, /*window*/ 300, P,
                       ScopeMode::Waveform, /*trigger*/ true, 1.0f, out);
    CHECK(std::abs(out[1]) < 0.1f);            // first y at/near zero
    CHECK(out[5 * 3 + 1] > out[1]);            // and rising
}

TEST_CASE("trigger keeps the trace phase-stable across input phase shifts") {
    const int N = 1500, P = 300;
    auto a = sineHist(N, 480.0, 48000.0, 0.0);
    auto b = sineHist(N, 480.0, 48000.0, 2.0 * kPi * 0.37);   // shifted ~0.37 of a period
    std::vector<float> oa, ob;
    buildScopeVertices(a.data(), a.data(), N, 300, P, ScopeMode::Waveform, true, 1.0f, oa);
    buildScopeVertices(b.data(), b.data(), N, 300, P, ScopeMode::Waveform, true, 1.0f, ob);
    CHECK(std::abs(oa[1]) < 0.1f);             // both lock to a zero-crossing...
    CHECK(std::abs(ob[1]) < 0.1f);
    int agree = 0;
    for (int j = 0; j < P; ++j)
        if (std::abs(oa[j * 3 + 1] - ob[j * 3 + 1]) < 0.12f) ++agree;
    CHECK(agree >= (int)(P * 0.9));            // ...so the displayed traces match
}

TEST_CASE("trigger falls back to free-running when there is no zero-crossing") {
    const int N = 1000, P = 256;
    std::vector<float> dc(N, 0.5f);            // all positive: no rising zero-crossing
    std::vector<float> trig, freerun;
    buildScopeVertices(dc.data(), dc.data(), N, 400, P, ScopeMode::Waveform, true, 1.0f, trig);
    buildScopeVertices(dc.data(), dc.data(), N, 400, P, ScopeMode::Waveform, false, 1.0f, freerun);
    for (int j = 0; j < P; ++j)
        CHECK(trig[j * 3 + 1] == doctest::Approx(freerun[j * 3 + 1]));
}

TEST_CASE("X-Y mode maps L to x and R to y and ignores the trigger") {
    const int N = 1000, P = 256;
    std::vector<float> l(N, 0.5f), r(N, -0.25f);   // distinct constants prove no swap
    std::vector<float> xy, xyTrig;
    buildScopeVertices(l.data(), r.data(), N, 400, P, ScopeMode::XY, false, 2.0f, xy);
    buildScopeVertices(l.data(), r.data(), N, 400, P, ScopeMode::XY, true,  2.0f, xyTrig);
    for (int j = 0; j < P; ++j) {
        CHECK(xy[j * 3 + 0] == doctest::Approx(1.0f));    // 0.5 * gain 2
        CHECK(xy[j * 3 + 1] == doctest::Approx(-0.5f));   // -0.25 * gain 2
        CHECK(xy[j * 3 + 2] == doctest::Approx(0.0f));
        CHECK(xyTrig[j * 3 + 0] == doctest::Approx(xy[j * 3 + 0]));   // trigger ignored
        CHECK(xyTrig[j * 3 + 1] == doctest::Approx(xy[j * 3 + 1]));
    }
}

TEST_CASE("X-Y traces the unit circle for sine/cosine inputs") {
    const int N = 1500, P = 300;
    auto s = sineHist(N, 480.0, 48000.0, 0.0);
    auto c = sineHist(N, 480.0, 48000.0, kPi / 2.0);   // cosine
    std::vector<float> out;
    buildScopeVertices(s.data(), c.data(), N, 300, P, ScopeMode::XY, false, 1.0f, out);
    for (int j = 0; j < P; ++j) {
        float x = out[j * 3 + 0], y = out[j * 3 + 1];
        CHECK(x * x + y * y == doctest::Approx(1.0f).epsilon(0.02));
    }
}
```

- [ ] **Step 3: Wire the build (so the test compiles and fails to link)**

In `CMakeLists.txt`, add `src/core/Oscilloscope.cpp` to `APP_SOURCES` right after `src/core/MidiFile.cpp` (line ~195):

```cmake
  src/core/MidiFile.cpp
  src/core/Oscilloscope.cpp
```

In the `core_tests` target, add the test file right after `tests/test_midi_clip.cpp` (line ~263):

```cmake
  tests/test_midi_clip.cpp
  tests/test_oscilloscope.cpp
```

and add the source right after `src/core/MidiFile.cpp` in that same target (line ~267):

```cmake
  src/core/MidiFile.cpp
  src/core/Oscilloscope.cpp
```

- [ ] **Step 4: Run the tests to verify they fail**

Run: `cmake -S . -B build && cmake --build build -j --target core_tests`
Expected: FAIL — link error (`undefined reference to buildScopeVertices`) or a compile error, because `Oscilloscope.cpp` has no implementation yet.

- [ ] **Step 5: Write the implementation**

Create `src/core/Oscilloscope.cpp`:

```cpp
#include "core/Oscilloscope.h"
#include <algorithm>
#include <cmath>

namespace oss {
namespace {

// Linear interpolation of a channel at fractional position p in [0, n-1].
float sampleAt(const float* s, std::size_t n, double p) {
    if (p <= 0.0) return s[0];
    if (p >= (double)(n - 1)) return s[n - 1];
    std::size_t i = (std::size_t)p;
    float frac = (float)(p - (double)i);
    return s[i] * (1.0f - frac) + s[i + 1] * frac;
}

} // namespace

void buildScopeVertices(const float* histL, const float* histR, std::size_t n,
                        int windowSamples, int pointCount, ScopeMode mode,
                        bool trigger, float gain, std::vector<float>& out) {
    if (pointCount < 1) pointCount = 1;
    out.assign((std::size_t)pointCount * 3, 0.0f);
    if (n < 2 || histL == nullptr || histR == nullptr) return;   // flat trace

    int win = windowSamples;
    if (win < 2) win = 2;
    if ((std::size_t)win > n) win = (int)n;

    if (mode == ScopeMode::XY) {
        std::size_t start = n - (std::size_t)win;
        for (int j = 0; j < pointCount; ++j) {
            double t = (pointCount > 1) ? (double)j / (pointCount - 1) : 0.0;
            double p = (double)start + t * (double)(win - 1);
            out[(std::size_t)j * 3 + 0] = sampleAt(histL, n, p) * gain;
            out[(std::size_t)j * 3 + 1] = sampleAt(histR, n, p) * gain;
            out[(std::size_t)j * 3 + 2] = 0.0f;
        }
        return;
    }

    // Waveform: downmix to mono so the trigger search and the resample read one signal.
    std::vector<float> s(n);
    for (std::size_t i = 0; i < n; ++i) s[i] = 0.5f * (histL[i] + histR[i]);

    std::size_t start = n - (std::size_t)win;                    // free-running default
    if (trigger && start >= 1) {                                 // need s[i-1]; room to look back
        std::size_t lo = (start > (std::size_t)win) ? (start - (std::size_t)win) : 1;
        for (std::size_t i = start; ; --i) {
            if (s[i - 1] < 0.0f && s[i] >= 0.0f) { start = i; break; }   // latest rising ZC
            if (i == lo) break;
        }
    }

    for (int j = 0; j < pointCount; ++j) {
        double t = (pointCount > 1) ? (double)j / (pointCount - 1) : 0.0;
        double p = (double)start + t * (double)(win - 1);
        out[(std::size_t)j * 3 + 0] = -1.0f + 2.0f * (float)t;
        out[(std::size_t)j * 3 + 1] = sampleAt(s.data(), n, p) * gain;
        out[(std::size_t)j * 3 + 2] = 0.0f;
    }
}

} // namespace oss
```

- [ ] **Step 6: Run the tests to verify they pass**

Run: `cmake --build build -j --target core_tests && ./build/core_tests`
Expected: PASS — all `core_tests` green, including the 7 oscilloscope cases.

- [ ] **Step 7: Commit**

```bash
git add src/core/Oscilloscope.h src/core/Oscilloscope.cpp tests/test_oscilloscope.cpp CMakeLists.txt
git commit -m "$(cat <<'EOF'
feat(core): add a GL-free oscilloscope vertex builder

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
)"
```

---

### Task 2: Oscilloscope node + registration + screenshot demo

**Files:**
- Create: `src/modules/OscilloscopeNode.h`
- Modify: `src/app/Application.cpp` (include + `makeNode` + Audio category)
- Modify: `src/main.cpp` (add the node to the `--screenshot` demo)

- [ ] **Step 1: Write the node**

Create `src/modules/OscilloscopeNode.h`:

```cpp
#pragma once
#include <glad/gl.h>
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>
#include "core/Node.h"
#include "core/Value.h"
#include "core/Oscilloscope.h"
#include "audio/SignalGenerator.h"

namespace oss {

// Turns an audio signal into an oscilloscope trace as streamed geometry: a line strip of
// vec3 positions published on output 0 (wire into the Wireframe node). Two modes: a
// triggered/free-running waveform (mono L+R downmix) or an X-Y vectorscope (L->x, R->y).
// The trace math is the GL-free buildScopeVertices; this node owns the rolling audio
// history, an internal synth fallback when `audio` is unconnected, and the VBO upload.
class OscilloscopeNode : public Node {
public:
    OscilloscopeNode()
        : Node("Oscilloscope"), gen_(48000, 220.0f),
          histL_(kHistory, 0.0f), histR_(kHistory, 0.0f),
          verts_((std::size_t)kPoints * 3, 0.0f) {
        addInput("audio", PortType::Audio, AudioRef{});       // unconnected -> internal synth
        addChoiceInput("mode", {"Waveform", "X-Y"}, 0);
        addInput("trigger", PortType::Bool, true);            // rising-edge lock (waveform only)
        addInput("window", PortType::Float, 20.0f, 1.0f, 100.0f);   // milliseconds
        addInput("gain", PortType::Float, 1.0f, 0.1f, 4.0f);
        addOutput("geometry", PortType::Vertex);
    }
    ~OscilloscopeNode() override { if (vbo_) glDeleteBuffers(1, &vbo_); }

    void initGL() override { glGenBuffers(1, &vbo_); }

    void evaluate(EvalContext& ctx) override {
        AudioRef a = ctx.in<AudioRef>(0);
        int sr = (a.samples && a.sampleRate > 0) ? a.sampleRate : gen_.sampleRate();
        int ch = (a.samples && a.channels > 0) ? a.channels : 1;
        std::size_t frames = a.samples ? a.frames() : 0;

        // Advance the rolling history by one frame's worth of samples.
        int adv = a.samples ? std::clamp((int)frames, 1, kHistory)
                            : std::clamp((int)std::lround(sr * (double)ctx.dt), 1, kHistory);
        std::move(histL_.begin() + adv, histL_.end(), histL_.begin());
        std::move(histR_.begin() + adv, histR_.end(), histR_.begin());
        float* tailL = histL_.data() + (kHistory - adv);
        float* tailR = histR_.data() + (kHistory - adv);

        if (a.samples && frames >= (std::size_t)adv) {
            std::size_t start = frames - (std::size_t)adv;
            for (int i = 0; i < adv; ++i) {
                std::size_t f = start + (std::size_t)i;
                tailL[i] = a.samples[f * (std::size_t)ch];
                tailR[i] = (ch == 2) ? a.samples[f * 2 + 1] : a.samples[f * (std::size_t)ch];
            }
        } else {
            scratch_.resize((std::size_t)adv);                 // no audio / underrun -> synth
            gen_.generate(scratch_.data(), (std::size_t)adv);
            for (int i = 0; i < adv; ++i) { tailL[i] = scratch_[i]; tailR[i] = scratch_[i]; }
        }

        int windowSamples = std::clamp((int)std::lround(ctx.in<float>(3) / 1000.0 * sr),
                                       2, kHistory / 2);
        ScopeMode mode = ((int)std::lround(ctx.in<float>(1)) == 1) ? ScopeMode::XY
                                                                   : ScopeMode::Waveform;
        buildScopeVertices(histL_.data(), histR_.data(), (std::size_t)kHistory,
                           windowSamples, kPoints, mode, ctx.in<bool>(2), ctx.in<float>(4), verts_);

        glBindBuffer(GL_ARRAY_BUFFER, vbo_);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(verts_.size() * sizeof(float)),
                     verts_.data(), GL_DYNAMIC_DRAW);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        ctx.out<VertexRef>(0, VertexRef{vbo_, kPoints, Primitive::LineStrip, VertexFormat::Pos3});

        mode_ = mode;
        windowMs_ = (int)std::lround(ctx.in<float>(3));
    }

    std::string statusLine() const override {
        return mode_ == ScopeMode::XY ? std::string("X-Y")
                                      : ("waveform · " + std::to_string(windowMs_) + " ms");
    }

private:
    static constexpr int kHistory = 16384;   // rolling sample history (per channel)
    static constexpr int kPoints  = 512;     // fixed vertex count of the trace
    SignalGenerator    gen_;
    std::vector<float> histL_, histR_;
    std::vector<float> verts_;               // kPoints*3 floats (owns the VertexRef storage)
    std::vector<float> scratch_;             // synth fill buffer
    GLuint             vbo_ = 0;
    ScopeMode          mode_ = ScopeMode::Waveform;
    int                windowMs_ = 20;
};

} // namespace oss
```

- [ ] **Step 2: Register the node**

In `src/app/Application.cpp`, add the include after `#include "modules/SpectrographNode.h"` (line ~21):

```cpp
#include "modules/SpectrographNode.h"
#include "modules/OscilloscopeNode.h"
```

Add the `makeNode` branch right after the Spectrograph line (line ~39):

```cpp
    if (type == "Spectrograph") return std::make_unique<SpectrographNode>();
    if (type == "Oscilloscope") return std::make_unique<OscilloscopeNode>();
```

Add `"Oscilloscope"` to the **Audio** category list (line ~64), right after `"Spectrograph"`:

```cpp
        { "Audio",   { "Sine", "Acid Bass", "Audio File", "Audio In", "Audio Mix", "Spectrograph", "Oscilloscope", "Audio Out" } },
```

- [ ] **Step 3: Add it to the screenshot demo**

In `src/main.cpp`, add this line right after the `"MIDI File"` demo node (line ~78):

```cpp
        app.addNodeOfType("MIDI File", glm::vec2(40.0f, 320.0f));
        app.addNodeOfType("Oscilloscope", glm::vec2(40.0f, 470.0f));
```

- [ ] **Step 4: Build the app and the smoke test**

Run: `cmake --build build -j`
Expected: clean build of `shader_streamer`, `core_tests`, and `gl_smoke` (no GL header leaks; `OscilloscopeNode.h` is only pulled in by `Application.cpp`).

- [ ] **Step 5: Verify the node renders**

Run: `./build/shader_streamer --screenshot build/_ui.png`
Expected: exit 0, prints `wrote screenshot build/_ui.png`. Open the PNG and confirm an **Oscilloscope** node is visible with its `audio` input, the `mode` dropdown, `trigger` checkbox, `window` and `gain` sliders, a `geometry` output, and a `waveform · 20 ms` status line. If the node is clipped by the Automation panel at the bottom, move it to a clearly visible free spot (e.g. `glm::vec2(1060.0f, 60.0f)`) and re-run.

- [ ] **Step 6: Commit**

```bash
git add src/modules/OscilloscopeNode.h src/app/Application.cpp src/main.cpp
git commit -m "$(cat <<'EOF'
feat(modules): add the Oscilloscope node + register it

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
)"
```

---

### Task 3: Documentation

**Files:**
- Modify: `README.md`, `CLAUDE.md`

- [ ] **Step 1: Add the README module-table row**

In `README.md`, add this row immediately after the `| **Spectrograph** | ... |` row (search for `Spectrograph` in the module table; if the exact row text differs, place the new row directly beneath it):

```markdown
| **Oscilloscope** | turns audio into an oscilloscope trace as geometry → Vertex: `mode` (Waveform / X-Y vectorscope), a rising-edge `trigger` so a steady tone stands still, `window` (ms), and `gain`. Wire `geometry` into **Wireframe** to view it |
```

- [ ] **Step 2: Add the CLAUDE.md architecture bullet**

In `CLAUDE.md`, in the **Architecture** section, add this bullet immediately after the **MIDI File player** bullet (it ends with "... Output is a `MidiRef` → wire into a synth."):

```markdown
- **Oscilloscope** — `OscilloscopeNode` (`src/modules/OscilloscopeNode.h`, header-only)
  turns an audio signal into an oscilloscope trace as streamed geometry. The GL-free
  `buildScopeVertices` (`src/core/Oscilloscope.{h,cpp}`) does the trace math over a
  rolling sample history: a mono waveform locked to a rising zero-crossing `trigger`
  (so a steady tone stands still) or an X-Y vectorscope (L→x, R→y), resampled to a fixed
  512-point line strip and scaled by `gain`. The node owns the history + an internal
  `SignalGenerator` fallback + the VBO, and publishes a `VertexRef` on output 0 → wire
  into the Wireframe renderer. The trace math is unit-tested in `core_tests`.
```

- [ ] **Step 3: Sanity check + commit**

Run: `ctest --test-dir build --output-on-failure`
Expected: all pass (no code changed).

```bash
git add README.md CLAUDE.md
git commit -m "$(cat <<'EOF'
docs: document the Oscilloscope node

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
)"
```

---

## Final verification

- [ ] `cmake --build build -j` — clean build (`shader_streamer`, `core_tests`, `gl_smoke`)
- [ ] `ctest --test-dir build --output-on-failure` — all pass (`core_tests` incl. the 7 oscilloscope cases, `gl_smoke`)
- [ ] `./build/shader_streamer --screenshot build/_ui.png` — the Oscilloscope node renders with its ports
- [ ] Use superpowers:finishing-a-development-branch
</content>
