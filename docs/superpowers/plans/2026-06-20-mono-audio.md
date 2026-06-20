# Mono Audio Edges + Stereo Bridge Nodes Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make every audio edge mono — `AudioRef` drops `channels`; nodes that were stereo expose separate `left`/`right` mono ports; add **Mono to Stereo** (pan) and **Stereo to Mono** (downmix) bridge nodes.

**Architecture:** Pan/balance math lives in a GL-free, unit-tested `core/AudioPan.h` shared by the mixer and bridges. The refactor is sequenced so each task compiles and tests green: add the helper + bridges, convert each node to mono I/O **while `AudioRef` still has `channels`** (defaulting to 1, so untouched code keeps working), then remove the field last.

**Tech Stack:** C++17, doctest (`core_tests`, GL-free), `gl_smoke` (headless GL), libsoundio (device I/O), CMake. Design: `docs/superpowers/specs/2026-06-20-mono-audio-design.md`.

**CRITICAL — two different `frames()`/`channels`:**
- `AudioRef` (`src/core/Value.h`) — the **edge** type. This is what becomes mono (loses `channels`/`frames()`).
- `AudioClip` (`src/audio/AudioFile.h`) — the file **decoder** buffer. Keeps its own `channels`/`frames()` and stays **stereo**. `AudioPlayerNode`'s `clip_.frames()` / `clip_.samples` and the `gl_smoke` `clip.channels==2` checks are `AudioClip` — **do NOT touch them.**

Header-only / GL-free files (no CMake source entry): `core/AudioPan.h`, `MonoToStereoNode.h`, `StereoToMonoNode.h`, `AudioMixerNode.h`, `OscilloscopeNode.h`. The `.cpp` nodes (`AudioPlayerNode`, `AudioInputNode`, `AudioOutputNode`, `RecorderNode`, `SpectrographNode`) are already in `APP_SOURCES`.

---

### Task 1: `core/AudioPan.h` — the pan/downmix laws

**Files:** Create `src/core/AudioPan.h`, `tests/test_audio_pan.cpp`; Modify `CMakeLists.txt`.

- [ ] **Step 1: Write the failing test**

Create `tests/test_audio_pan.cpp`:
```cpp
#include <doctest/doctest.h>
#include "core/AudioPan.h"

using namespace oss;

TEST_CASE("panGains: centre keeps both, hard pan mutes the far side") {
    CHECK(panGains(0.0f).l == doctest::Approx(1.0f));
    CHECK(panGains(0.0f).r == doctest::Approx(1.0f));
    CHECK(panGains(-1.0f).l == doctest::Approx(1.0f));   // hard left
    CHECK(panGains(-1.0f).r == doctest::Approx(0.0f));
    CHECK(panGains(1.0f).l == doctest::Approx(0.0f));     // hard right
    CHECK(panGains(1.0f).r == doctest::Approx(1.0f));
}

TEST_CASE("downmixGains: centre averages, hard balance keeps one side") {
    CHECK(downmixGains(0.0f).l == doctest::Approx(0.5f));
    CHECK(downmixGains(0.0f).r == doctest::Approx(0.5f));
    CHECK(downmixGains(-1.0f).l == doctest::Approx(1.0f));  // all left
    CHECK(downmixGains(-1.0f).r == doctest::Approx(0.0f));
    CHECK(downmixGains(1.0f).l == doctest::Approx(0.0f));   // all right
    CHECK(downmixGains(1.0f).r == doctest::Approx(1.0f));
}
```

- [ ] **Step 2: Wire into the build**

In `CMakeLists.txt`, in the `core_tests` source list, add after `tests/test_audio_mixer.cpp`:
```cmake
  tests/test_audio_pan.cpp
```

- [ ] **Step 3: Run to verify it fails**

Run: `cmake -S . -B build && cmake --build build -j --target core_tests`
Expected: FAIL — `'core/AudioPan.h' file not found`.

- [ ] **Step 4: Create `src/core/AudioPan.h`**

```cpp
#pragma once
#include <algorithm>

namespace oss {

// Per-channel gains for the audio pan/downmix nodes. GL-free.
struct PanGains { float l, r; };

// Mono -> stereo pan (the Audio Mix balance law): centre (pan 0) keeps both at
// unity; pan -1 mutes the right, pan +1 mutes the left. pan in [-1, 1].
inline PanGains panGains(float pan) {
    return { 1.0f - std::max(0.0f, pan), 1.0f + std::min(0.0f, pan) };
}

// Stereo -> mono downmix crossfade: balance 0 averages (0.5L + 0.5R);
// balance -1 keeps only L, +1 keeps only R. balance in [-1, 1].
inline PanGains downmixGains(float balance) {
    return { 0.5f * (1.0f - balance), 0.5f * (1.0f + balance) };
}

} // namespace oss
```

- [ ] **Step 5: Run to verify it passes**

Run: `cmake --build build -j --target core_tests && ./build/core_tests`
Expected: PASS — all green.

- [ ] **Step 6: Commit**

```bash
git add src/core/AudioPan.h tests/test_audio_pan.cpp CMakeLists.txt
git commit -m "$(cat <<'EOF'
feat(core): add AudioPan pan/downmix gain helpers

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
)"
```

---

### Task 2: Mono to Stereo + Stereo to Mono bridge nodes

**Files:** Create `src/modules/MonoToStereoNode.h`, `src/modules/StereoToMonoNode.h`, `tests/test_audio_bridges.cpp`; Modify `CMakeLists.txt`, `src/app/Application.cpp`.

- [ ] **Step 1: Write the failing test**

Create `tests/test_audio_bridges.cpp`:
```cpp
#include <doctest/doctest.h>
#include "modules/MonoToStereoNode.h"
#include "modules/StereoToMonoNode.h"
#include "core/Node.h"
#include "core/Value.h"
#include <vector>

using namespace oss;

TEST_CASE("Mono to Stereo: centre duplicates, hard left mutes right") {
    MonoToStereoNode n;
    std::vector<float> m = {0.2f, -0.4f, 0.6f};
    SUBCASE("centre") {
        std::vector<Value> in = { AudioRef{m.data(), m.size(), 48000}, 0.0f };
        std::vector<Value> out(2);
        EvalContext ctx{in, out, 0.016f};
        n.evaluate(ctx);
        AudioRef L = std::get<AudioRef>(out[0]), R = std::get<AudioRef>(out[1]);
        REQUIRE(L.count == 3); REQUIRE(R.count == 3);
        CHECK(L.samples[0] == doctest::Approx(0.2f));
        CHECK(R.samples[0] == doctest::Approx(0.2f));
        CHECK(L.samples[2] == doctest::Approx(0.6f));
        CHECK(R.samples[2] == doctest::Approx(0.6f));
    }
    SUBCASE("hard left") {
        std::vector<Value> in = { AudioRef{m.data(), m.size(), 48000}, -1.0f };
        std::vector<Value> out(2);
        EvalContext ctx{in, out, 0.016f};
        n.evaluate(ctx);
        AudioRef L = std::get<AudioRef>(out[0]), R = std::get<AudioRef>(out[1]);
        CHECK(L.samples[1] == doctest::Approx(-0.4f));
        CHECK(R.samples[1] == doctest::Approx(0.0f));
    }
}

TEST_CASE("Stereo to Mono: centre averages, balance picks a side, output clamps") {
    StereoToMonoNode n;
    std::vector<float> l = {0.2f, 0.8f}, r = {0.6f, 0.4f};
    SUBCASE("centre averages") {
        std::vector<Value> in = { AudioRef{l.data(), 2, 48000}, AudioRef{r.data(), 2, 48000}, 0.0f };
        std::vector<Value> out(1);
        EvalContext ctx{in, out, 0.016f};
        n.evaluate(ctx);
        AudioRef o = std::get<AudioRef>(out[0]);
        REQUIRE(o.count == 2);
        CHECK(o.samples[0] == doctest::Approx(0.4f));   // 0.5*(0.2+0.6)
        CHECK(o.samples[1] == doctest::Approx(0.6f));   // 0.5*(0.8+0.4)
    }
    SUBCASE("balance -1 keeps left only") {
        std::vector<Value> in = { AudioRef{l.data(), 2, 48000}, AudioRef{r.data(), 2, 48000}, -1.0f };
        std::vector<Value> out(1);
        EvalContext ctx{in, out, 0.016f};
        n.evaluate(ctx);
        AudioRef o = std::get<AudioRef>(out[0]);
        CHECK(o.samples[0] == doctest::Approx(0.2f));
        CHECK(o.samples[1] == doctest::Approx(0.8f));
    }
    SUBCASE("output clamps to [-1,1]") {
        std::vector<float> big = {2.0f, 2.0f};
        std::vector<Value> in = { AudioRef{big.data(), 2, 48000}, AudioRef{big.data(), 2, 48000}, 0.0f };
        std::vector<Value> out(1);
        EvalContext ctx{in, out, 0.016f};
        n.evaluate(ctx);
        AudioRef o = std::get<AudioRef>(out[0]);
        CHECK(o.samples[0] == doctest::Approx(1.0f));
    }
}
```

- [ ] **Step 2: Wire into the build**

In `CMakeLists.txt`, in the `core_tests` source list, add after `tests/test_audio_pan.cpp`:
```cmake
  tests/test_audio_bridges.cpp
```

- [ ] **Step 3: Run to verify it fails**

Run: `cmake --build build -j --target core_tests`
Expected: FAIL — `'modules/MonoToStereoNode.h' file not found`.

- [ ] **Step 4: Create `src/modules/MonoToStereoNode.h`**

```cpp
#pragma once
#include <algorithm>
#include <cstddef>
#include <vector>
#include "core/Node.h"
#include "core/Value.h"
#include "core/AudioPan.h"

namespace oss {

// Bridge: places a mono signal in the stereo field, producing two mono outputs
// (left, right). `pan` -1..1 (0 = centre). GL-free.
class MonoToStereoNode : public Node {
public:
    MonoToStereoNode() : Node("Mono to Stereo"), bufL_(kMaxBlock, 0.0f), bufR_(kMaxBlock, 0.0f) {
        addInput("mono", PortType::Audio, AudioRef{});
        addInput("pan",  PortType::Float, 0.0f, -1.0f, 1.0f);
        addOutput("left",  PortType::Audio);
        addOutput("right", PortType::Audio);
    }
    void evaluate(EvalContext& ctx) override {
        AudioRef m = ctx.in<AudioRef>(0);
        PanGains g = panGains(ctx.in<float>(1));
        std::size_t n = m.samples ? std::min(m.count, (std::size_t)kMaxBlock) : 0;
        int sr = (m.samples && m.sampleRate > 0) ? m.sampleRate : 48000;
        for (std::size_t i = 0; i < n; ++i) {
            float s = m.samples[i];
            bufL_[i] = g.l * s;
            bufR_[i] = g.r * s;
        }
        ctx.out<AudioRef>(0, AudioRef{bufL_.data(), n, sr});
        ctx.out<AudioRef>(1, AudioRef{bufR_.data(), n, sr});
    }
private:
    static constexpr int kMaxBlock = 8192;
    std::vector<float> bufL_, bufR_;
};

} // namespace oss
```

- [ ] **Step 5: Create `src/modules/StereoToMonoNode.h`**

```cpp
#pragma once
#include <algorithm>
#include <cstddef>
#include <vector>
#include "core/Node.h"
#include "core/Value.h"
#include "core/AudioPan.h"

namespace oss {

// Bridge: downmixes a stereo pair (two mono inputs left, right) to one mono
// output. `balance` -1..1 (0 = equal average). Output clamped to [-1,1]. GL-free.
class StereoToMonoNode : public Node {
public:
    StereoToMonoNode() : Node("Stereo to Mono"), buf_(kMaxBlock, 0.0f) {
        addInput("left",    PortType::Audio, AudioRef{});
        addInput("right",   PortType::Audio, AudioRef{});
        addInput("balance", PortType::Float, 0.0f, -1.0f, 1.0f);
        addOutput("mono", PortType::Audio);
    }
    void evaluate(EvalContext& ctx) override {
        AudioRef L = ctx.in<AudioRef>(0);
        AudioRef R = ctx.in<AudioRef>(1);
        PanGains g = downmixGains(ctx.in<float>(2));
        std::size_t nL = L.samples ? L.count : 0;
        std::size_t nR = R.samples ? R.count : 0;
        std::size_t n  = std::min(std::max(nL, nR), (std::size_t)kMaxBlock);
        int sr = (L.samples && L.sampleRate > 0) ? L.sampleRate
               : (R.samples && R.sampleRate > 0) ? R.sampleRate : 48000;
        for (std::size_t i = 0; i < n; ++i) {
            float l = (i < nL) ? L.samples[i] : 0.0f;
            float r = (i < nR) ? R.samples[i] : 0.0f;
            buf_[i] = std::clamp(g.l * l + g.r * r, -1.0f, 1.0f);
        }
        ctx.out<AudioRef>(0, AudioRef{buf_.data(), n, sr});
    }
private:
    static constexpr int kMaxBlock = 8192;
    std::vector<float> buf_;
};

} // namespace oss
```

- [ ] **Step 6: Register the nodes**

In `src/app/Application.cpp`: add includes alongside the other `#include "modules/..."` lines:
```cpp
#include "modules/MonoToStereoNode.h"
#include "modules/StereoToMonoNode.h"
```
In `makeNode(...)`, add near the other audio nodes:
```cpp
    if (type == "Mono to Stereo") return std::make_unique<MonoToStereoNode>();
    if (type == "Stereo to Mono") return std::make_unique<StereoToMonoNode>();
```
In `nodeCategories()`, add `"Mono to Stereo"` and `"Stereo to Mono"` to the existing **Audio** category's list (read the file to find it; append them to that braced list).

- [ ] **Step 7: Build everything + run tests**

Run: `cmake --build build -j && ./build/core_tests`
Expected: clean build; all core_tests pass (including the new bridge tests).

- [ ] **Step 8: Commit**

```bash
git add src/modules/MonoToStereoNode.h src/modules/StereoToMonoNode.h tests/test_audio_bridges.cpp CMakeLists.txt src/app/Application.cpp
git commit -m "$(cat <<'EOF'
feat(modules): add Mono to Stereo + Stereo to Mono bridge nodes

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
)"
```

---

### Task 3: Audio Mix → two mono outputs

**Files:** Modify `src/modules/AudioMixerNode.h`, `tests/test_audio_mixer.cpp`.

- [ ] **Step 1: Rewrite the test for two mono outputs**

Replace the entire body of `tests/test_audio_mixer.cpp` with:
```cpp
#include <doctest/doctest.h>
#include "modules/AudioMixerNode.h"
#include "core/Node.h"
#include "core/Value.h"
#include <vector>

using namespace oss;

// Input ports per channel: in / gain / pan -> 12 ports for 4 channels.
static std::vector<Value> makeInputs() {
    std::vector<Value> in(12);
    for (int c = 0; c < 4; ++c) {
        in[c * 3]     = AudioRef{};   // unconnected audio
        in[c * 3 + 1] = 1.0f;         // unity gain
        in[c * 3 + 2] = 0.0f;         // centre pan
    }
    return in;
}

// out[0] = left mono, out[1] = right mono.
TEST_CASE("centred mono inputs sum equally into both outputs") {
    AudioMixerNode mix;
    std::vector<float> a = {0.1f, 0.2f, 0.3f};
    std::vector<float> b = {0.4f, 0.4f, 0.4f};
    auto in = makeInputs();
    in[0] = AudioRef{a.data(), a.size(), 48000};
    in[3] = AudioRef{b.data(), b.size(), 48000};
    std::vector<Value> out(2);
    EvalContext ctx{in, out, 1.0f / 60.0f};
    mix.evaluate(ctx);

    AudioRef L = std::get<AudioRef>(out[0]), R = std::get<AudioRef>(out[1]);
    REQUIRE(L.count == 3); REQUIRE(R.count == 3);
    CHECK(L.samples[0] == doctest::Approx(0.5f));
    CHECK(R.samples[0] == doctest::Approx(0.5f));
    CHECK(L.samples[2] == doctest::Approx(0.7f));
    CHECK(R.samples[2] == doctest::Approx(0.7f));
}

TEST_CASE("pan places a mono input in the stereo field") {
    AudioMixerNode mix;
    std::vector<float> s = {0.5f};
    SUBCASE("hard left") {
        auto in = makeInputs();
        in[0] = AudioRef{s.data(), 1, 48000};
        in[2] = -1.0f;
        std::vector<Value> out(2);
        EvalContext ctx{in, out, 0.016f};
        mix.evaluate(ctx);
        CHECK(std::get<AudioRef>(out[0]).samples[0] == doctest::Approx(0.5f));   // L
        CHECK(std::get<AudioRef>(out[1]).samples[0] == doctest::Approx(0.0f));   // R muted
    }
    SUBCASE("hard right") {
        auto in = makeInputs();
        in[0] = AudioRef{s.data(), 1, 48000};
        in[2] = 1.0f;
        std::vector<Value> out(2);
        EvalContext ctx{in, out, 0.016f};
        mix.evaluate(ctx);
        CHECK(std::get<AudioRef>(out[0]).samples[0] == doctest::Approx(0.0f));   // L muted
        CHECK(std::get<AudioRef>(out[1]).samples[0] == doctest::Approx(0.5f));   // R
    }
}

TEST_CASE("mixer applies per-channel gain") {
    AudioMixerNode mix;
    std::vector<float> a = {0.5f}, b = {0.5f};
    auto in = makeInputs();
    in[0] = AudioRef{a.data(), 1, 48000}; in[1] = 0.5f;
    in[3] = AudioRef{b.data(), 1, 48000}; in[4] = 0.25f;
    std::vector<Value> out(2);
    EvalContext ctx{in, out, 0.016f};
    mix.evaluate(ctx);
    CHECK(std::get<AudioRef>(out[0]).samples[0] == doctest::Approx(0.5f * 0.5f + 0.5f * 0.25f));
}

TEST_CASE("mixer clamps each output to [-1, 1]") {
    AudioMixerNode mix;
    std::vector<float> s = {0.9f};
    auto in = makeInputs();
    for (int c = 0; c < 4; ++c) in[c * 3] = AudioRef{s.data(), 1, 48000};
    std::vector<Value> out(2);
    EvalContext ctx{in, out, 0.016f};
    mix.evaluate(ctx);
    CHECK(std::get<AudioRef>(out[0]).samples[0] == doctest::Approx(1.0f));
    CHECK(std::get<AudioRef>(out[1]).samples[0] == doctest::Approx(1.0f));
}

TEST_CASE("mixer output length is the longest input (in samples)") {
    AudioMixerNode mix;
    std::vector<float> a = {0.1f, 0.1f, 0.1f, 0.1f};
    std::vector<float> b = {0.2f, 0.2f};
    auto in = makeInputs();
    in[0] = AudioRef{a.data(), 4, 48000};
    in[3] = AudioRef{b.data(), 2, 48000};
    std::vector<Value> out(2);
    EvalContext ctx{in, out, 0.016f};
    mix.evaluate(ctx);
    AudioRef L = std::get<AudioRef>(out[0]);
    REQUIRE(L.count == 4);
    CHECK(L.samples[0] == doctest::Approx(0.3f));   // a+b
    CHECK(L.samples[2] == doctest::Approx(0.1f));   // a only (b exhausted)
}

TEST_CASE("mixer with nothing connected outputs empty blocks") {
    AudioMixerNode mix;
    auto in = makeInputs();
    std::vector<Value> out(2);
    EvalContext ctx{in, out, 0.016f};
    mix.evaluate(ctx);
    CHECK(std::get<AudioRef>(out[0]).count == 0);
    CHECK(std::get<AudioRef>(out[1]).count == 0);
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build build -j --target core_tests`
Expected: FAIL — the mixer still has one output / interleaved buffer (`out` index 1 out of range, or wrong values).

- [ ] **Step 3: Rewrite `src/modules/AudioMixerNode.h`**

Replace the include block to add AudioPan, the buffers, the output ports, and the evaluate body. The full file:
```cpp
#pragma once
#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>
#include "core/Node.h"
#include "core/Value.h"
#include "core/AudioPan.h"

namespace oss {

// Four-channel mixer: sums four mono audio inputs, each with its own gain and
// pan, into two mono outputs (left, right). GL-free. Panning places each mono
// source across the stereo field. Each output is clamped to [-1, 1].
//
// Input ports per channel are in / gain / pan, so each control sits by its source:
//   0: in 1   1: gain 1   2: pan 1   3: in 2   4: gain 2   5: pan 2 ...
class AudioMixerNode : public Node {
public:
    AudioMixerNode()
        : Node("Audio Mix"), bufL_(kMaxBlock, 0.0f), bufR_(kMaxBlock, 0.0f) {
        for (int c = 0; c < kChannels; ++c) {
            addInput("in " + std::to_string(c + 1),   PortType::Audio, AudioRef{});
            addInput("gain " + std::to_string(c + 1), PortType::Float, 1.0f, 0.0f, 2.0f);
            addInput("pan " + std::to_string(c + 1),  PortType::Float, 0.0f, -1.0f, 1.0f);
        }
        addOutput("left",  PortType::Audio);
        addOutput("right", PortType::Audio);
    }

    void evaluate(EvalContext& ctx) override {
        AudioRef in[kChannels];
        float    gain[kChannels], pan[kChannels];
        std::size_t n = 0;        // longest input, in samples (mono)
        int sr = 0;
        for (int c = 0; c < kChannels; ++c) {
            in[c]   = ctx.in<AudioRef>((std::size_t)(c * 3));
            gain[c] = ctx.in<float>((std::size_t)(c * 3 + 1));
            pan[c]  = ctx.in<float>((std::size_t)(c * 3 + 2));
            if (in[c].samples) {
                n = std::max(n, in[c].count);
                if (sr == 0 && in[c].sampleRate > 0) sr = in[c].sampleRate;
            }
        }
        if (sr == 0) sr = 48000;
        n = std::min(n, (std::size_t)kMaxBlock);

        for (std::size_t i = 0; i < n; ++i) {
            float l = 0.0f, r = 0.0f;
            for (int c = 0; c < kChannels; ++c) {
                if (!in[c].samples || i >= in[c].count) continue;
                PanGains g = panGains(pan[c]);
                float s = in[c].samples[i];
                l += gain[c] * g.l * s;
                r += gain[c] * g.r * s;
            }
            bufL_[i] = std::clamp(l, -1.0f, 1.0f);
            bufR_[i] = std::clamp(r, -1.0f, 1.0f);
        }
        ctx.out<AudioRef>(0, AudioRef{bufL_.data(), n, sr});
        ctx.out<AudioRef>(1, AudioRef{bufR_.data(), n, sr});
    }

private:
    static constexpr int kChannels = 4;
    static constexpr int kMaxBlock = 8192;   // samples
    std::vector<float> bufL_, bufR_;
};

} // namespace oss
```

- [ ] **Step 4: Run to verify it passes**

Run: `cmake --build build -j --target core_tests && ./build/core_tests`
Expected: PASS. Then check nothing else broke: `grep -rn "Audio Mix" src/main.cpp tests/gl_smoke.cpp` — if any code wires the mixer's old single `out` (port 0) into a downstream stereo consumer, note it; the port-0 output is now `left` (still valid as a mono edge). No action unless a build/test fails.

- [ ] **Step 5: Build all + full tests**

Run: `cmake --build build -j && ctest --test-dir build --output-on-failure`
Expected: clean build, all tests pass.

- [ ] **Step 6: Commit**

```bash
git add src/modules/AudioMixerNode.h tests/test_audio_mixer.cpp
git commit -m "$(cat <<'EOF'
refactor(audio): Audio Mix emits two mono outputs (left/right)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
)"
```

---

### Task 4: Split Audio Player + Audio Input outputs into left/right

**Files:** Modify `src/modules/AudioPlayerNode.{h,cpp}`, `src/modules/AudioInputNode.{h,cpp}`, `tests/gl_smoke.cpp`.

- [ ] **Step 1: Audio Player — two mono outputs**

In `src/modules/AudioPlayerNode.h`:
- Replace the two output-buffer/accessor members. Change the comment line "as 48 kHz stereo" to "as 48 kHz, deinterleaved to two mono outputs (left, right)".
- Replace the accessors and buffers:
```cpp
    // Test/inspection accessors.
    double   playhead() const { return playhead_; }
    AudioRef leftOut()  const { return AudioRef{outL_.data(), (std::size_t)lastN_, sampleRate_}; }
    AudioRef rightOut() const { return AudioRef{outR_.data(), (std::size_t)lastN_, sampleRate_}; }
```
- Replace the `outBuf_` member with two mono buffers:
```cpp
    std::vector<float> outL_, outR_;           // mono left / right (kMaxBlock each)
    int         lastN_ = 0;                    // samples emitted last evaluate
```

In `src/modules/AudioPlayerNode.cpp`:
- The constructor declares the output port. Find `addOutput("audio", ...)` and replace with:
```cpp
    addOutput("left",  PortType::Audio);
    addOutput("right", PortType::Audio);
```
- Find where `outBuf_` is sized (constructor, likely `outBuf_.assign(kMaxBlock * 2, 0.0f)` or `.resize`). Replace with two mono buffers:
```cpp
    outL_.assign(kMaxBlock, 0.0f);
    outR_.assign(kMaxBlock, 0.0f);
```
- Rewrite `emitAudio` to write the two mono buffers and emit two outputs (keep the clip read; the clip stays stereo):
```cpp
void AudioPlayerNode::emitAudio(EvalContext& ctx, double t0, double t1) {
    int n = std::clamp((int)std::lround(sampleRate_ * (double)ctx.dt), 1, kMaxBlock);
    std::size_t frames = clip_.frames();        // AudioClip (stereo) - unchanged
    if (!haveClip_ || frames < 2 || t0 == t1) {
        std::fill(outL_.begin(), outL_.begin() + n, 0.0f);
        std::fill(outR_.begin(), outR_.begin() + n, 0.0f);
    } else {
        for (int j = 0; j < n; ++j) {
            double s    = t0 + (t1 - t0) * ((double)j / n);
            double fidx = s * sampleRate_;
            float  L = 0.0f, R = 0.0f;
            if (fidx >= 0.0 && fidx < (double)(frames - 1)) {
                int   i0 = (int)fidx;
                float fr = (float)(fidx - i0);
                L = clip_.samples[(std::size_t)i0 * 2]     * (1 - fr) + clip_.samples[((std::size_t)i0 + 1) * 2]     * fr;
                R = clip_.samples[(std::size_t)i0 * 2 + 1] * (1 - fr) + clip_.samples[((std::size_t)i0 + 1) * 2 + 1] * fr;
            }
            outL_[(std::size_t)j] = L;
            outR_[(std::size_t)j] = R;
        }
    }
    lastN_ = n;
    ctx.out<AudioRef>(0, AudioRef{outL_.data(), (std::size_t)n, sampleRate_});
    ctx.out<AudioRef>(1, AudioRef{outR_.data(), (std::size_t)n, sampleRate_});
}
```

- [ ] **Step 2: Audio Input — two mono outputs**

In `src/modules/AudioInputNode.h`:
- Update the class comment: "publishes it as two mono outputs (left, right)".
- Add a second per-frame buffer + deinterleave scratch. Replace the `block_` member line with:
```cpp
    std::vector<float>    block_;           // interleaved capture (main thread only)
    std::vector<float>    outL_, outR_;     // deinterleaved mono left / right
```

In `src/modules/AudioInputNode.cpp`:
- In the constructor, replace `addOutput("audio", PortType::Audio);` with:
```cpp
    addOutput("left",  PortType::Audio);
    addOutput("right", PortType::Audio);
```
  (Also size `outL_`/`outR_` to `block_`'s size wherever `block_` is sized in the constructor — add `outL_.assign(block_.size(), 0.0f); outR_.assign(block_.size(), 0.0f);` next to it.)
- Rewrite `evaluate`:
```cpp
void AudioInputNode::evaluate(EvalContext& ctx) {
    if (!ensureStarted()) {                               // silence on both outputs
        ctx.out<AudioRef>(0, AudioRef{});
        ctx.out<AudioRef>(1, AudioRef{});
        return;
    }
    soundio_flush_events(soundio_);

    std::size_t n = ring_.pop(block_.data(), block_.size());
    n -= n % (std::size_t)channels_;
    std::size_t frames = n / (std::size_t)channels_;
    for (std::size_t f = 0; f < frames; ++f) {
        outL_[f] = block_[f * (std::size_t)channels_];
        outR_[f] = (channels_ == 2) ? block_[f * 2 + 1] : block_[f * (std::size_t)channels_];
    }
    ctx.out<AudioRef>(0, AudioRef{outL_.data(), frames, sampleRate_});
    ctx.out<AudioRef>(1, AudioRef{outR_.data(), frames, sampleRate_});
}
```

- [ ] **Step 3: Update the `gl_smoke` audio-player scenario**

In `tests/gl_smoke.cpp`, find the AudioPlayer scenario (around the `an->audioOut()` / `o.channels == 2` use, ~line 729). Replace the stereo check with the two mono accessors:
```cpp
            AudioRef oL = an->leftOut();
            AudioRef oR = an->rightOut();
            if (oL.count > 0 && oR.count > 0)
                for (std::size_t i = 0; i < oL.count; ++i)
                    if (std::fabs(oL.samples[i]) > 0.01f || std::fabs(oR.samples[i]) > 0.01f) { sawAudio = true; break; }
```
Adjust the surrounding loop/variable names to match what's there (it currently uses `o`/`o.channels`/`o.count`). Keep the `clip.channels == 2` checks elsewhere in the file (those are `AudioClip`). Ensure `<cmath>` is included (it is). If the failure message text mentions "stereo", you may keep it.

- [ ] **Step 4: Build + tests + gl_smoke**

Run: `cmake --build build -j && ctest --test-dir build --output-on-failure`
Expected: clean build; all pass (the gl_smoke audio-player scenario still detects advancing audio + reverse playhead via the two mono outputs).

- [ ] **Step 5: Commit**

```bash
git add src/modules/AudioPlayerNode.h src/modules/AudioPlayerNode.cpp src/modules/AudioInputNode.h src/modules/AudioInputNode.cpp tests/gl_smoke.cpp
git commit -m "$(cat <<'EOF'
refactor(audio): Audio Player + Audio Input emit left/right mono outputs

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
)"
```

---

### Task 5: Split Audio Out + Recorder inputs into left/right

**Files:** Modify `src/modules/AudioOutputNode.{h,cpp}`, `src/modules/RecorderNode.{h,cpp}`.

- [ ] **Step 1: Audio Out — two mono inputs, interleave with symmetric mirror**

In `src/modules/AudioOutputNode.h`: the `stereoScratch_` member stays (rename comment to "main thread: interleave left+right before push"). No new members needed.

In `src/modules/AudioOutputNode.cpp`:
- In the constructor, replace `addInput("audio", PortType::Audio, AudioRef{});` with:
```cpp
    addInput("left",  PortType::Audio, AudioRef{});
    addInput("right", PortType::Audio, AudioRef{});
```
- Replace the body of `evaluate` after `soundio_flush_events(...)`:
```cpp
    AudioRef l = ctx.in<AudioRef>(0);
    AudioRef r = ctx.in<AudioRef>(1);
    // Symmetric mirror: a single connected side feeds both speakers, so a lone
    // mono wire just works. The ring carries interleaved stereo (L,R,L,R).
    const AudioRef& effL = (l.samples && l.count > 0) ? l : r;
    const AudioRef& effR = (r.samples && r.count > 0) ? r : l;
    std::size_t nL = effL.samples ? effL.count : 0;
    std::size_t nR = effR.samples ? effR.count : 0;
    std::size_t n  = std::max(nL, nR);
    if (n == 0) return;                                  // nothing connected -> silence
    stereoScratch_.resize(n * 2);
    for (std::size_t i = 0; i < n; ++i) {
        stereoScratch_[i * 2]     = (i < nL) ? effL.samples[i] : 0.0f;
        stereoScratch_[i * 2 + 1] = (i < nR) ? effR.samples[i] : 0.0f;
    }
    ring_.push(stereoScratch_.data(), n * 2);            // overflow dropped, never blocks
```
- Add `#include <algorithm>` to the `.cpp` if `std::max` isn't already available (it includes `<cstddef>`, `<cstring>`; add `<algorithm>`).

- [ ] **Step 2: Recorder — two mono in/out, record interleaved stereo with mirror**

In `src/modules/RecorderNode.h`:
- Update the class comment (audio is now `left`(1)/`right`(2) in, mirrored to outputs).
- Change the `start` helper signature and add an interleave buffer:
```cpp
    void start(const std::string& file, const TexRef& vin, int sampleRate);
    ...
    std::vector<float> audioScratch_;           // interleave left+right before encoding
```

In `src/modules/RecorderNode.cpp`:
- Constructor: replace the audio in/out ports:
```cpp
    addInput("video",  PortType::Texture, TexRef{});
    addInput("left",   PortType::Audio,   AudioRef{});
    addInput("right",  PortType::Audio,   AudioRef{});
    addInput("record", PortType::Bool,    false);
    addInput("file",   PortType::String,  std::string("recording.mp4"));
    addOutput("video", PortType::Texture);   // mirrors input 0
    addOutput("left",  PortType::Audio);     // mirrors input 1
    addOutput("right", PortType::Audio);     // mirrors input 2
```
- Rewrite `evaluate` (the port indices shifted: record is now 3, file is 4; audio is left=1/right=2):
```cpp
void RecorderNode::evaluate(EvalContext& ctx) {
    TexRef   vin   = ctx.in<TexRef>(0);
    AudioRef lin   = ctx.in<AudioRef>(1);
    AudioRef rin   = ctx.in<AudioRef>(2);
    bool     rec   = ctx.in<bool>(3);
    const std::string& file = ctx.in<std::string>(4);

    // Pass video + audio straight through so the node is transparent in the graph.
    ctx.out<TexRef>(0, vin);
    ctx.out<AudioRef>(1, lin);
    ctx.out<AudioRef>(2, rin);

    // Symmetric mirror so a lone mono wire records on both channels.
    const AudioRef& effL = (lin.samples && lin.count > 0) ? lin : rin;
    const AudioRef& effR = (rin.samples && rin.count > 0) ? rin : lin;
    bool haveAudio = (effL.samples && effL.sampleRate > 0);
    int  sr = haveAudio ? effL.sampleRate : 0;

    if (rec && !recording_)  start(file, vin, sr);
    if (!rec && recording_)  stop();

    if (recording_ && enc_) {
        recordTime_ += ctx.dt;
        if (vin.id && vin.w == encW_ && vin.h == encH_) {
            pixbuf_.resize((std::size_t)encW_ * encH_ * 4);
            glBindTexture(GL_TEXTURE_2D, vin.id);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixbuf_.data());
            glBindTexture(GL_TEXTURE_2D, 0);
            enc_->addVideoFrame(pixbuf_.data(), recordTime_);
            ++frames_;
        }
        std::size_t nL = effL.samples ? effL.count : 0;
        std::size_t nR = effR.samples ? effR.count : 0;
        std::size_t n  = std::max(nL, nR);
        if (n > 0) {
            audioScratch_.resize(n * 2);
            for (std::size_t i = 0; i < n; ++i) {
                audioScratch_[i * 2]     = (i < nL) ? effL.samples[i] : 0.0f;
                audioScratch_[i * 2 + 1] = (i < nR) ? effR.samples[i] : 0.0f;
            }
            enc_->addAudio(audioScratch_.data(), (int)(n * 2));
        }
        char buf[96];
        std::snprintf(buf, sizeof(buf), "REC %5.1fs  %ld frames", recordTime_, frames_);
        status_ = buf;
    }
}
```
- Rewrite `start` to take a sample rate and always open a stereo (2ch) audio track when audio is present:
```cpp
void RecorderNode::start(const std::string& file, const TexRef& vin, int sampleRate) {
    if (!vin.id || vin.w <= 0 || vin.h <= 0) { status_ = "waiting for video input"; return; }

    enc_ = std::make_unique<VideoEncoder>();
    encW_ = vin.w; encH_ = vin.h;
    int arate = sampleRate;                       // 0 -> no audio track
    int achan = sampleRate > 0 ? 2 : 0;           // recorded interleaved stereo

    std::string err;
    if (enc_->open(file, encW_, encH_, 60, arate, achan, err)) {
        recording_ = true; recordTime_ = 0.0; frames_ = 0; file_ = file;
        status_ = "recording...";
        std::fprintf(stderr, "[Recorder] recording %s (%dx%d, %s)\n", file.c_str(), encW_, encH_,
                     achan == 2 ? "stereo" : "video only");
    } else {
        status_ = "record failed: " + err;
        std::fprintf(stderr, "[Recorder] %s\n", status_.c_str());
        enc_.reset();
    }
}
```
- Add `#include <algorithm>` to `RecorderNode.cpp` for `std::max`.

- [ ] **Step 3: Build + tests**

Run: `cmake --build build -j && ctest --test-dir build --output-on-failure`
Expected: clean build; all pass. (No unit tests cover these device/encoder nodes; they must still compile + link and not regress gl_smoke.) Then `grep -rn "Recorder\|Audio Out" tests/gl_smoke.cpp src/main.cpp` — if any code wires the old single audio port of these nodes by index, update it to the new left/right ports; otherwise no change.

- [ ] **Step 4: Commit**

```bash
git add src/modules/AudioOutputNode.h src/modules/AudioOutputNode.cpp src/modules/RecorderNode.h src/modules/RecorderNode.cpp
git commit -m "$(cat <<'EOF'
refactor(audio): Audio Out + Recorder take left/right mono inputs

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
)"
```

---

### Task 6: Oscilloscope (left/right) + Spectrograph (single mono)

**Files:** Modify `src/modules/OscilloscopeNode.h`, `src/modules/SpectrographNode.cpp`, `tests/gl_smoke.cpp`.

- [ ] **Step 1: Oscilloscope — two mono inputs**

In `src/modules/OscilloscopeNode.h`:
- Update the class comment: "Two mono inputs (left, right); waveform uses left, X-Y uses left->x, right->y."
- In the constructor, replace `addInput("audio", ...)` with two audio inputs, keeping the trailing controls (their indices shift by 1):
```cpp
        addInput("left",  PortType::Audio, AudioRef{});       // unconnected -> internal synth
        addInput("right", PortType::Audio, AudioRef{});
        addChoiceInput("mode", {"Waveform", "X-Y"}, 0);
        addInput("trigger", PortType::Bool, true);            // rising-edge lock (waveform only)
        addInput("window", PortType::Float, 20.0f, 1.0f, 100.0f);   // milliseconds
        addInput("gain", PortType::Float, 1.0f, 0.1f, 4.0f);
        addOutput("geometry", PortType::Vertex);
```
- Rewrite the head of `evaluate` (read two inputs; control indices are now mode=2, trigger=3, window=4, gain=5):
```cpp
    void evaluate(EvalContext& ctx) override {
        AudioRef aL = ctx.in<AudioRef>(0);
        AudioRef aR = ctx.in<AudioRef>(1);
        bool hasAudio = (aL.samples && aL.count > 0) || (aR.samples && aR.count > 0);
        const AudioRef& sL = (aL.samples && aL.count > 0) ? aL : aR;   // mirror lone side
        const AudioRef& sR = (aR.samples && aR.count > 0) ? aR : aL;
        int sr = hasAudio ? sL.sampleRate : gen_.sampleRate();
        std::size_t framesL = sL.samples ? sL.count : 0;
        std::size_t framesR = sR.samples ? sR.count : 0;
        std::size_t frames  = std::max(framesL, framesR);

        // Advance the rolling history by one frame's worth of samples.
        int adv = hasAudio ? std::clamp((int)frames, 1, kHistory)
                           : std::clamp((int)std::lround(sr * (double)ctx.dt), 1, kHistory);
        std::move(histL_.begin() + adv, histL_.end(), histL_.begin());
        std::move(histR_.begin() + adv, histR_.end(), histR_.begin());
        float* tailL = histL_.data() + (kHistory - adv);
        float* tailR = histR_.data() + (kHistory - adv);

        if (hasAudio && frames >= (std::size_t)adv) {
            std::size_t start = frames - (std::size_t)adv;
            for (int i = 0; i < adv; ++i) {
                std::size_t f = start + (std::size_t)i;
                tailL[i] = (f < framesL) ? sL.samples[f] : 0.0f;
                tailR[i] = (f < framesR) ? sR.samples[f] : 0.0f;
            }
        } else {
            scratch_.resize((std::size_t)adv);                 // no audio / underrun -> synth
            gen_.generate(scratch_.data(), (std::size_t)adv);
            for (int i = 0; i < adv; ++i) { tailL[i] = scratch_[i]; tailR[i] = scratch_[i]; }
        }

        float windowMs = ctx.in<float>(4);
        int windowSamples = std::clamp((int)std::lround(windowMs / 1000.0 * sr),
                                       2, kHistory / 2);
        ScopeMode mode = ((int)std::lround(ctx.in<float>(2)) == 1) ? ScopeMode::XY
                                                                   : ScopeMode::Waveform;
        buildScopeVertices(histL_.data(), histR_.data(), (std::size_t)kHistory,
                           windowSamples, kPoints, mode, ctx.in<bool>(3), ctx.in<float>(5), verts_);
        // ... (the rest of evaluate — VBO upload + ctx.out + status — is unchanged)
```
  Keep everything from the `glBindBuffer(...)` upload onward exactly as it was.

- [ ] **Step 2: Spectrograph — single mono input**

In `src/modules/SpectrographNode.cpp`, rewrite the channel/ingest part of `evaluate`. Replace the block that computes `ch`, `frames`, and the tail-fill downmix with:
```cpp
    AudioRef a = ctx.in<AudioRef>(0);

    int sr = (a.samples && a.sampleRate > 0) ? a.sampleRate : gen_.sampleRate();
    std::size_t frames = a.samples ? a.count : 0;
    int adv = std::clamp((int)std::lround(sr * (double)ctx.dt), 1, kWindow);
    if (a.samples) adv = std::clamp((int)frames, 1, kWindow);
    std::move(window_.begin() + adv, window_.end(), window_.begin());
    float* tail = window_.data() + (kWindow - adv);

    if (a.samples && frames >= (std::size_t)adv) {
        std::size_t start = frames - (std::size_t)adv;
        for (int i = 0; i < adv; ++i) tail[i] = a.samples[start + (std::size_t)i];
    } else {
        gen_.generate(tail, adv);
    }
```
  Leave the rest of `evaluate` (the `magnitudeSpectrum` / texture upload / geometry) unchanged. The `addInput("audio", ...)` port and the two outputs are unchanged.

- [ ] **Step 3: Update any gl_smoke scenarios for these nodes**

Run `grep -n "Oscilloscope\|Spectrograph" tests/gl_smoke.cpp`. For any scenario that sets an Oscilloscope input by index past the audio port (mode/trigger/window/gain were 1–4, now 2–5) or wires a stereo `AudioRef` into these nodes, update the indices/wiring. Spectrograph's single audio input is unchanged. Rebuild gl_smoke after.

- [ ] **Step 4: Build + tests**

Run: `cmake --build build -j && ctest --test-dir build --output-on-failure`
Expected: clean build; all pass.

- [ ] **Step 5: Commit**

```bash
git add src/modules/OscilloscopeNode.h src/modules/SpectrographNode.cpp tests/gl_smoke.cpp
git commit -m "$(cat <<'EOF'
refactor(audio): Oscilloscope takes left/right; Spectrograph takes mono

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
)"
```

---

### Task 7: Make `AudioRef` mono (remove `channels`/`frames()`)

**Files:** Modify `src/core/Value.h`; sweep for any remaining `AudioRef` `.channels`/`.frames()`.

- [ ] **Step 1: Find remaining AudioRef channel references**

Run:
```bash
grep -rn "\.channels\|\.frames()" src/ tests/ | grep -iv "AudioFile\|AudioClip\|clip\.\|clip_\.\|store\.channels\|automation"
```
Every remaining hit on an `AudioRef` must be gone (the node tasks above removed them). `AudioClip` hits (`AudioFile.h`, `AudioPlayerNode` `clip_.frames()`, `gl_smoke` `clip.channels`/`clip.frames()`) and `AutomationStore::channels()` are NOT AudioRef — leave them. If any AudioRef hit remains, fix it before changing the struct.

- [ ] **Step 2: Make `AudioRef` mono in `src/core/Value.h`**

Replace the struct + its comment:
```cpp
// Non-owning view of a node's latest audio samples. Audio edges are mono: `count`
// is the number of float samples in `samples`; `sampleRate` is the source rate.
// Stereo is carried as two separate mono edges (see the Mono to Stereo / Stereo to
// Mono bridges and nodes' left/right ports).
struct AudioRef {
    const float* samples    = nullptr;
    std::size_t  count      = 0;
    int          sampleRate = 0;
};
```
Delete the `channels` field and the `frames()` method.

- [ ] **Step 3: Build everything + full tests**

Run: `cmake -S . -B build && cmake --build build -j && ctest --test-dir build --output-on-failure`
Expected: clean build (no reference to the removed `AudioRef::channels`/`frames()`); ALL tests pass. If the build fails with a missing `channels`/`frames()`, that site was an `AudioRef` the node tasks missed — fix it (use `.count`), don't re-add the field.

- [ ] **Step 4: Commit**

```bash
git add src/core/Value.h
git commit -m "$(cat <<'EOF'
refactor(core): AudioRef is mono (drop channels/frames)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
)"
```

---

### Task 8: Demo rewiring + documentation

**Files:** Modify `src/main.cpp`, `README.md`, `CLAUDE.md`.

- [ ] **Step 1: Screenshot demo**

Read `src/main.cpp`. If it `connect`s any audio edge whose endpoint port changed (Audio Player out, Audio Mix out, Audio Out in, Recorder in/out, Oscilloscope in), update those `connect(...)` port indices to the new `left`/`right` ports. Add one of each new node so they appear in the demo:
```cpp
        app.addNodeOfType("Mono to Stereo", glm::vec2(740.0f, 470.0f));
        app.addNodeOfType("Stereo to Mono", glm::vec2(940.0f, 470.0f));
```
(Adjust coordinates if they overlap; this is the same demo-placement idiom used for other nodes.)

- [ ] **Step 2: Build + screenshot**

Run: `cmake --build build -j && ./build/shader_streamer --screenshot build/_ui.png`
Expected: exit 0. Open `build/_ui.png` with the Read tool and confirm the two new nodes render (Mono to Stereo: `mono`/`pan` in, `left`/`right` out; Stereo to Mono: `left`/`right`/`balance` in, `mono` out). Reposition + re-screenshot if clipped; report what you saw.

- [ ] **Step 3: README.md**

In the module table, update the audio rows whose ports changed and add two rows. Match the table's exact column layout. Content:
- **Mono to Stereo** — pan a mono signal into a `left`/`right` pair.
- **Stereo to Mono** — downmix a `left`/`right` pair to one mono signal (`balance` control).
- Note in the existing audio rows (Audio Player / Audio Input / Audio Mix / Audio Out / Recorder / Oscilloscope) that audio is now mono with `left`/`right` ports, where the row mentions audio I/O.

- [ ] **Step 4: CLAUDE.md**

Update the `AudioRef` Architecture bullet (it currently says "Audio is interleaved float in `AudioRef` (`channels` = 1 mono or 2 stereo...)"). Replace with: audio edges are **mono** — `AudioRef` is `{samples, count, sampleRate}`, `count` is the sample count; stereo is two separate `left`/`right` mono edges. Nodes split their stereo I/O into `left`/`right` ports; the GL-free `core/AudioPan.h` pan/downmix laws back the **Audio Mix** and the **Mono to Stereo** / **Stereo to Mono** bridges. Note `AudioClip`/`AudioFile` stay stereo internally (the Audio Player deinterleaves to its two outputs).

Also update the Recorder hard-rules bullet if it mentions "whatever channel count the input audio carries" → now records interleaved stereo from its `left`/`right` inputs.

- [ ] **Step 5: Verify + commit**

Run: `ctest --test-dir build --output-on-failure`
Expected: all pass (no code logic changed in this task beyond demo wiring).

```bash
git add src/main.cpp README.md CLAUDE.md
git commit -m "$(cat <<'EOF'
docs(audio): document mono audio edges + stereo bridges; rewire demo

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
)"
```

---

## Final verification

- [ ] `cmake --build build -j` — clean build (`shader_streamer`, `core_tests`, `gl_smoke`)
- [ ] `ctest --test-dir build --output-on-failure` — all pass (AudioPan, bridges, mixer L/R, sine, gl_smoke audio scenarios)
- [ ] `grep -rn "AudioRef" src/ | grep "channels\|\.frames()"` returns nothing (only `AudioClip` retains those)
- [ ] `./build/shader_streamer --screenshot build/_ui.png` — the two bridge nodes render
- [ ] Use superpowers:finishing-a-development-branch
</content>
