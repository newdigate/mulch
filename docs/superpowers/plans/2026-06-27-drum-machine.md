# Drum Machine node — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A self-contained sample-based drum machine node: 4 sample voices sequenced on a 4×16 tri-state step grid with 8 pattern slots, mixed to a stereo (`left`/`right`) audio output with per-sample volume / rate / pan.

**Architecture:** A GL-free `DrumPatterns` store (the 8 grids + codec) and a GL-free `SampleVoice` (one-shot sampler) are unit-tested in `core_tests`. A header-only `DrumMachineNode` orchestrates them: 4 `AsyncLoader<AudioClip>` for sample files, Step-Seq-style transport stepping, per-step voice triggers, and `panGains` mixing. A new generic tri-state grid hook on `Node` is rendered by `NodeEditorPanel`.

**Tech Stack:** C++17, the existing `core/AsyncLoader`, `audio/AudioFile` (`AudioClip`/`decodeAudioFile`), `core/StepSync`, `core/AudioPan` (`panGains`), `audio/AudioBlock`, doctest, Dear ImGui + imgui-node-editor.

**Spec:** `docs/superpowers/specs/2026-06-27-drum-machine-design.md`

**Build/test commands:**
- Configure (once): `cmake -S . -B build`
- Build: `cmake --build build -j`
- Unit tests: `ctest --test-dir build -R core_tests --output-on-failure`
- GL/integration: `ctest --test-dir build -R gl_smoke --output-on-failure`
- Run individual doctest cases: `./build/core_tests -tc="*<wildcard>*"`

---

### Task 1: `core/DrumPattern.h` — 8-slot tri-state pattern store + codec

**Files:**
- Create: `src/core/DrumPattern.h`
- Create: `tests/test_drum_machine.cpp`
- Modify: `CMakeLists.txt` (add the test file to the `core_tests` source list, after `tests/test_midi_clock.cpp` at line ~288)

- [ ] **Step 1: Write the failing tests** — create `tests/test_drum_machine.cpp`:

```cpp
#include <doctest/doctest.h>
#include "core/DrumPattern.h"
#include <string>

using namespace oss;

TEST_CASE("DrumPatterns cells default off and cycle off->on->accent->off") {
    DrumPatterns p;
    CHECK(p.cell(0, 0) == 0);
    p.cycleCell(0, 0); CHECK(p.cell(0, 0) == 1);   // on
    p.cycleCell(0, 0); CHECK(p.cell(0, 0) == 2);   // accent
    p.cycleCell(0, 0); CHECK(p.cell(0, 0) == 0);   // back to off
    // out-of-range is a no-op (no crash)
    p.cycleCell(-1, 0); p.cycleCell(0, 99);
    CHECK(p.cell(0, 0) == 0);
}

TEST_CASE("DrumPatterns active slot selects an independent grid") {
    DrumPatterns p;
    p.setActive(0); p.cycleCell(1, 2);            // slot 0, row 1, col 2 -> on
    p.setActive(3); CHECK(p.cell(1, 2) == 0);     // slot 3 is empty
    p.cycleCell(1, 2); p.cycleCell(1, 2);         // slot 3 -> accent
    CHECK(p.cell(3, 1, 2) == 2);
    CHECK(p.cell(0, 1, 2) == 1);                  // slot 0 unchanged
    p.setActive(99); CHECK(p.active() == DrumPatterns::kSlots - 1);   // clamped
}

TEST_CASE("DrumPatterns encode/decode round-trips all slots + active index") {
    DrumPatterns a;
    a.setActive(5);
    a.setCell(0, 0, 0, 1); a.setCell(0, 1, 4, 2);
    a.setCell(7, 3, 15, 2);
    std::string s = a.encode();
    DrumPatterns b; b.decode(s);
    CHECK(b.active() == 5);
    CHECK(b.cell(0, 0, 0) == 1);
    CHECK(b.cell(0, 1, 4) == 2);
    CHECK(b.cell(7, 3, 15) == 2);
    CHECK(b.cell(2, 2, 2) == 0);
    CHECK(b.encode() == s);
}

TEST_CASE("DrumPatterns decode tolerates empty + malformed input") {
    DrumPatterns p; p.setCell(0, 0, 0, 2);
    p.decode("");                       // empty: no change, no throw
    CHECK(p.cell(0, 0, 0) == 2);
    DrumPatterns q;
    q.decode("notanumber;012");         // bad active -> 0; short grid -> rest off
    CHECK(q.active() == 0);
    CHECK(q.cell(0, 0, 0) == 0);
    CHECK(q.cell(0, 0, 1) == 1);
    CHECK(q.cell(0, 0, 2) == 2);
}
```

- [ ] **Step 2: Register the test file** — in `CMakeLists.txt`, add this line to the `core_tests` `add_executable(...)` list, immediately after `tests/test_midi_clock.cpp`:

```cmake
  tests/test_drum_machine.cpp
```

- [ ] **Step 3: Run to verify it fails** — `cmake --build build -j` → expected: compile error, `core/DrumPattern.h` not found.

- [ ] **Step 4: Implement** — create `src/core/DrumPattern.h`:

```cpp
#pragma once
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace oss {

// 8 pattern slots, each a 4x16 tri-state step grid. Cell values: 0 = off, 1 = on, 2 = accent.
// The active slot is the playback / edit source of truth. GL-free, header-only. The text codec
// uses no spaces or ';' inside a grid so it is safe in the project file's saveState string.
class DrumPatterns {
public:
    static constexpr int kRows = 4, kCols = 16, kSlots = 8;

    int  active() const { return active_; }
    void setActive(int s) { active_ = std::clamp(s, 0, kSlots - 1); }

    int  cell(int r, int c) const { return at(active_, r, c); }          // active slot
    int  cell(int slot, int r, int c) const { return at(slot, r, c); }   // explicit slot

    // Cycle the active slot's cell off -> on -> accent -> off.
    void cycleCell(int r, int c) {
        if (r < 0 || r >= kRows || c < 0 || c >= kCols) return;
        std::uint8_t& v = grids_[active_][r][c];
        v = (std::uint8_t)((v + 1) % 3);
    }
    void setCell(int slot, int r, int c, int v) {
        if (slot < 0 || slot >= kSlots || r < 0 || r >= kRows || c < 0 || c >= kCols) return;
        grids_[slot][r][c] = (std::uint8_t)std::clamp(v, 0, 2);
    }

    // "<active>;<g0>;...;<g7>", each grid 64 chars (row-major 4x16) of '0'/'1'/'2'.
    std::string encode() const {
        std::string s = std::to_string(active_);
        for (int slot = 0; slot < kSlots; ++slot) {
            s += ';';
            for (int r = 0; r < kRows; ++r)
                for (int c = 0; c < kCols; ++c)
                    s += (char)('0' + grids_[slot][r][c]);
        }
        return s;
    }
    void decode(const std::string& s) {
        if (s.empty()) return;
        std::vector<std::string> parts; std::string cur;
        for (char ch : s) { if (ch == ';') { parts.push_back(cur); cur.clear(); } else cur += ch; }
        parts.push_back(cur);
        if (parts.empty()) return;
        try { active_ = std::clamp(std::stoi(parts[0]), 0, kSlots - 1); } catch (...) { active_ = 0; }
        for (int slot = 0; slot < kSlots && slot + 1 < (int)parts.size(); ++slot) {
            const std::string& g = parts[(std::size_t)(slot + 1)];
            for (int r = 0; r < kRows; ++r)
                for (int c = 0; c < kCols; ++c) {
                    int idx = r * kCols + c;
                    int v = (idx < (int)g.size()) ? (g[(std::size_t)idx] - '0') : 0;
                    grids_[slot][r][c] = (std::uint8_t)std::clamp(v, 0, 2);
                }
        }
    }

private:
    int at(int slot, int r, int c) const {
        if (slot < 0 || slot >= kSlots || r < 0 || r >= kRows || c < 0 || c >= kCols) return 0;
        return grids_[slot][r][c];
    }
    std::uint8_t grids_[kSlots][kRows][kCols] = {};
    int          active_ = 0;
};

} // namespace oss
```

- [ ] **Step 5: Run to verify it passes** — `cmake --build build -j && ./build/core_tests -tc="*DrumPatterns*"` → expected: all 4 cases pass.

- [ ] **Step 6: Commit**

```bash
git add src/core/DrumPattern.h tests/test_drum_machine.cpp CMakeLists.txt
git commit -m "feat(core): add DrumPatterns 8-slot tri-state step store + codec"
```

---

### Task 2: `audio/SampleVoice.h` — one-shot monophonic sample voice

**Files:**
- Create: `src/audio/SampleVoice.h`
- Modify: `tests/test_drum_machine.cpp` (append cases)

- [ ] **Step 1: Write the failing tests** — append to `tests/test_drum_machine.cpp`:

```cpp
#include "audio/SampleVoice.h"

// A clip of `frames` stereo frames; L = R = the frame index (a 0,1,2,... ramp).
static AudioClip rampClip(int frames) {
    AudioClip c; c.ok = true; c.sampleRate = 48000; c.channels = 2;
    c.samples.resize((std::size_t)frames * 2);
    for (int i = 0; i < frames; ++i) { c.samples[(std::size_t)i*2] = (float)i; c.samples[(std::size_t)i*2+1] = (float)i; }
    return c;
}

TEST_CASE("SampleVoice plays a clip forward at rate 1 then deactivates") {
    AudioClip c = rampClip(5);              // values 0,1,2,3,4
    SampleVoice v; v.trigger(c, false);
    CHECK(v.active());
    float L[8] = {0}, R[8] = {0};
    v.render(c, 1.0, 1.0f, 1.0f, L, R, 8);  // 8 out frames, clip only 5
    CHECK(L[0] == doctest::Approx(0.0f));
    CHECK(L[1] == doctest::Approx(1.0f));
    CHECK(L[3] == doctest::Approx(3.0f));
    CHECK(L[4] == doctest::Approx(0.0f));   // ran off the end (pos hit frames-1) -> silence
    CHECK(!v.active());
}

TEST_CASE("SampleVoice rate 2 reads every other sample (interpolation at 0.5)") {
    AudioClip c = rampClip(9);
    SampleVoice v; v.trigger(c, false);
    float L[4] = {0}, R[4] = {0};
    v.render(c, 2.0, 1.0f, 1.0f, L, R, 4);
    CHECK(L[0] == doctest::Approx(0.0f));
    CHECK(L[1] == doctest::Approx(2.0f));
    CHECK(L[2] == doctest::Approx(4.0f));
}

TEST_CASE("SampleVoice reverse starts at the end and plays backward") {
    AudioClip c = rampClip(5);              // 0..4
    SampleVoice v; v.trigger(c, true);      // pos = 4
    float L[3] = {0}, R[3] = {0};
    v.render(c, -1.0, 1.0f, 1.0f, L, R, 3);
    CHECK(L[0] == doctest::Approx(4.0f));
    CHECK(L[1] == doctest::Approx(3.0f));
    CHECK(L[2] == doctest::Approx(2.0f));
}

TEST_CASE("SampleVoice render adds (mixes) and applies per-channel gains") {
    AudioClip c = rampClip(4);              // 0,1,2,3
    SampleVoice v; v.trigger(c, false);
    float L[3] = {10.0f, 10.0f, 10.0f}, R[3] = {0};
    v.render(c, 1.0, 0.5f, 0.25f, L, R, 3);
    CHECK(L[1] == doctest::Approx(10.0f + 1.0f * 0.5f));   // added, not overwritten
    CHECK(R[1] == doctest::Approx(1.0f * 0.25f));
}

TEST_CASE("SampleVoice retrigger restarts; rate 0 and empty clip are no-ops") {
    AudioClip c = rampClip(4);
    SampleVoice v; v.trigger(c, false);
    float L[2] = {0}, R[2] = {0};
    v.render(c, 1.0, 1.0f, 1.0f, L, R, 2);  // consumes 0,1
    v.trigger(c, false);                    // restart at 0
    float L2[2] = {0}, R2[2] = {0};
    v.render(c, 1.0, 1.0f, 1.0f, L2, R2, 2);
    CHECK(L2[0] == doctest::Approx(0.0f));
    // rate 0 -> no-op (no advance, no infinite hold expectation), empty clip -> inactive
    AudioClip empty; SampleVoice e; e.trigger(empty, false); CHECK(!e.active());
}
```

- [ ] **Step 2: Run to verify it fails** — `cmake --build build -j` → expected: `audio/SampleVoice.h` not found.

- [ ] **Step 3: Implement** — create `src/audio/SampleVoice.h`:

```cpp
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
        for (int i = 0; i < n; ++i) {
            if (pos_ < 0.0 || pos_ >= (double)(f - 1)) { active_ = false; break; }
            int   i0 = (int)pos_;
            float fr = (float)(pos_ - (double)i0);
            float s  = monoAt(clip, i0) * (1.0f - fr) + monoAt(clip, i0 + 1) * fr;
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
```

- [ ] **Step 4: Run to verify it passes** — `cmake --build build -j && ./build/core_tests -tc="*SampleVoice*"` → expected: all 5 cases pass.

- [ ] **Step 5: Commit**

```bash
git add src/audio/SampleVoice.h tests/test_drum_machine.cpp
git commit -m "feat(audio): add SampleVoice one-shot monophonic sample voice"
```

---

### Task 3: `core/Node.h` — generic tri-state grid hook

**Files:**
- Modify: `src/core/Node.h` (add 5 virtuals after the button-bank block, ~line 66 after `onButtonPressed`)

- [ ] **Step 1: Implement** — in `src/core/Node.h`, immediately after the line `virtual void onButtonPressed(int /*i*/) {}` (the last button-bank virtual), add:

```cpp
    // Optional tri-state grid, rendered by the node editor under the button bank (GL-free:
    // ints/strings only). Cell values are 0/1/2 (e.g. off/on/accent). Default = none. The
    // editor calls onGridCellPressed() on a click (the node decides how to cycle the value).
    virtual int         gridRows() const { return 0; }              // 0 = no grid
    virtual int         gridCols() const { return 0; }
    virtual int         gridCell(int /*r*/, int /*c*/) const { return 0; }   // 0/1/2
    virtual void        onGridCellPressed(int /*r*/, int /*c*/) {}
    virtual std::string gridRowLabel(int /*r*/) const { return std::string(); }
```

- [ ] **Step 2: Verify it compiles** — `cmake --build build -j` → expected: builds clean (pure interface addition; defaults mean no existing node changes behavior).

- [ ] **Step 3: Commit**

```bash
git add src/core/Node.h
git commit -m "feat(core): add a generic tri-state grid hook to Node"
```

---

### Task 4: `modules/DrumMachineNode.h` — the node (ports, hooks, loading, sequencing, mixing)

**Files:**
- Create: `src/modules/DrumMachineNode.h` (header-only)
- Modify: `tests/gl_smoke.cpp` (add a node integration scenario; links FFmpeg so the node's `decodeAudioFile` reference resolves)

Depends on Tasks 1–3.

- [ ] **Step 1: Write the node** — create `src/modules/DrumMachineNode.h`:

```cpp
#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>
#include "core/Node.h"
#include "core/Value.h"
#include "core/DrumPattern.h"
#include "core/StepSync.h"
#include "core/AudioPan.h"
#include "core/AsyncLoader.h"
#include "audio/AudioFile.h"
#include "audio/AudioBlock.h"
#include "audio/SampleVoice.h"

namespace oss {

// Sample-based drum machine: 4 sample voices sequenced on a 4x16 tri-state grid (8 pattern slots),
// mixed to a stereo left/right output. Per sample: file path, volume, signed rate (pitch/reverse),
// pan. Clock mirrors Step Seq (transport-synced or free). A step's on/accent cells (re)trigger that
// row's voice; accent (cell==2) plays louder. GL-free + header-only.
//
// Port layout: per voice v (0..3) a block at 4v: file(4v), vol(4v+1), rate(4v+2), pan(4v+3).
// Then: tempo(16), sync(17), rate sync(18, choice), pattern(19, 1..8). Outputs: left(0), right(1).
class DrumMachineNode : public Node {
public:
    static constexpr int kVoices = 4;
    static constexpr int kSteps  = DrumPatterns::kCols;   // 16
    static constexpr int kTempoIdx    = 4 * kVoices + 0;  // 16
    static constexpr int kSyncIdx     = 4 * kVoices + 1;  // 17
    static constexpr int kRateSyncIdx = 4 * kVoices + 2;  // 18
    static constexpr int kPatternIdx  = 4 * kVoices + 3;  // 19

    DrumMachineNode() : Node("Drum Machine") {
        for (int v = 0; v < kVoices; ++v) {
            std::string s = std::to_string(v + 1);
            addInput("file " + s, PortType::String, std::string(""));
            addInput("vol "  + s, PortType::Float, 0.8f,  0.0f, 1.0f);
            addInput("rate " + s, PortType::Float, 1.0f, -4.0f, 4.0f);  // signed: <0 reverses
            addInput("pan "  + s, PortType::Float, 0.0f, -1.0f, 1.0f);
        }
        addInput("tempo", PortType::Float, 120.0f, 40.0f, 240.0f);      // free-mode BPM
        addInput("sync",  PortType::Bool, false);                       // lock to transport
        addChoiceInput("rate sync", stepDivisionLabels(), kStepDivisionDefault);  // step length
        addInput("pattern", PortType::Float, 1.0f, 1.0f, 8.0f);         // active slot (automatable)
        addOutput("left",  PortType::Audio);
        addOutput("right", PortType::Audio);
        outL_.assign(kAudioMaxBlock, 0.0f);
        outR_.assign(kAudioMaxBlock, 0.0f);
        for (int c = 0; c < kSteps; c += 4) patterns_.setCell(0, 0, c, 1);  // default kick four-on-the-floor
    }

    // --- tri-state grid hook (rows = voices, cols = steps; edits the active pattern) ---
    int gridRows() const override { return DrumPatterns::kRows; }
    int gridCols() const override { return DrumPatterns::kCols; }
    int gridCell(int r, int c) const override { return patterns_.cell(r, c); }
    void onGridCellPressed(int r, int c) override { patterns_.cycleCell(r, c); }
    std::string gridRowLabel(int r) const override { return std::to_string(r + 1); }

    // --- 8 pattern-slot buttons ---
    int buttonCount() const override { return DrumPatterns::kSlots; }
    std::string buttonLabel(int i) const override { return std::to_string(i + 1); }
    int buttonActive() const override { return patterns_.active(); }
    void onButtonPressed(int i) override { patterns_.setActive(i); }

    void evaluate(EvalContext& ctx) override {
        // (1) pattern selection: buttons set active directly; the pattern port is edge-detected so a
        // constant default never overrides a click, while an automated input drives the slot live.
        float patPort = ctx.in<float>(kPatternIdx);
        if (patPort != lastPatternPort_) {
            patterns_.setActive((int)std::lround(patPort) - 1);   // 1-based port -> 0-based slot
            lastPatternPort_ = patPort;
        }

        // (2) per-slot sample loading, mirroring AudioPlayerNode (worker-thread decode on path change).
        loaded_ = 0;
        for (int v = 0; v < kVoices; ++v) {
            const std::string& path = ctx.in<std::string>((std::size_t)(4 * v));
            if (loaders_[v].request(path, [path] { return decodeAudioFile(path); })) {
                haveClip_[v] = false; clips_[v] = AudioClip{};
            }
            AudioClip done;
            if (loaders_[v].poll(done)) { clips_[v] = std::move(done); haveClip_[v] = clips_[v].ok; }
            if (haveClip_[v]) ++loaded_;
        }

        // (3) derive the step that fires this frame (-1 = none).
        bool sync = ctx.in<bool>(kSyncIdx);
        int  fire = -1;
        if (sync) {
            int div = std::clamp((int)std::lround(ctx.in<float>(kRateSyncIdx)), 0, 7);
            double barsPerStep = stepDivisionBars(div);
            if (ctx.transport && ctx.transport->playing && barsPerStep > 0.0) {
                long long stepAbs = (long long)std::floor(ctx.transport->bars() / barsPerStep);
                if (!primed_ || stepAbs != lastStepAbs_) {
                    fire = (int)(((stepAbs % kSteps) + kSteps) % kSteps);
                    lastStepAbs_ = stepAbs; primed_ = true;
                }
            } else {
                primed_ = false;   // stopped: re-prime so resume fires cleanly
            }
        } else {
            float tempo = ctx.in<float>(kTempoIdx);
            if (tempo < 1.0f) tempo = 1.0f;
            const double period = 15.0 / tempo;          // seconds per 16th note (4 per beat)
            clock_ += ctx.dt;
            while (clock_ >= nextStep_) {                 // last crossed step wins (1/frame at normal dt)
                fire = freeStep_;
                freeStep_ = (freeStep_ + 1) % kSteps;
                nextStep_ += period;
            }
        }

        // (4) on a step boundary, (re)trigger the on/accent rows of that column.
        if (fire >= 0) {
            for (int v = 0; v < kVoices; ++v) {
                int st = patterns_.cell(v, fire);
                if (st != 0 && haveClip_[v]) {
                    double rate = (double)ctx.in<float>((std::size_t)(4 * v + 2));
                    voices_[v].trigger(clips_[v], rate < 0.0);
                    accent_[v] = (st == 2);
                }
            }
        }

        // (5) render + mix the voices into the stereo block.
        int n = audioBlockFrames(48000, ctx.dt);
        std::fill(outL_.begin(), outL_.begin() + n, 0.0f);
        std::fill(outR_.begin(), outR_.begin() + n, 0.0f);
        for (int v = 0; v < kVoices; ++v) {
            if (!haveClip_[v] || !voices_[v].active()) continue;
            float vol  = std::clamp(ctx.in<float>((std::size_t)(4 * v + 1)), 0.0f, 1.0f);
            float rate = ctx.in<float>((std::size_t)(4 * v + 2));
            float pan  = std::clamp(ctx.in<float>((std::size_t)(4 * v + 3)), -1.0f, 1.0f);
            float g    = vol * (accent_[v] ? 1.0f : 0.75f);   // accent flag is per-hit (set at trigger)
            PanGains pg = panGains(pan);
            voices_[v].render(clips_[v], (double)rate, g * pg.l, g * pg.r,
                              outL_.data(), outR_.data(), n);
        }

        lastN_ = n;
        ctx.out<AudioRef>(0, AudioRef{outL_.data(), (std::size_t)n, 48000});
        ctx.out<AudioRef>(1, AudioRef{outR_.data(), (std::size_t)n, 48000});
    }

    std::string statusLine() const override {
        return "P" + std::to_string(patterns_.active() + 1) + " \xE2\x80\xA2 "
             + std::to_string(loaded_) + "/4 loaded";
    }

    // Persist only the 8 grids + active index; paths/vol/rate/pan persist as control defaults.
    std::string saveState() const override { return patterns_.encode(); }
    void        loadState(const std::string& s) override { patterns_.decode(s); }

    // --- test seams (GL-free) ---
    void     injectClip(int v, AudioClip clip) {
        if (v < 0 || v >= kVoices) return;
        clips_[v] = std::move(clip); haveClip_[v] = clips_[v].ok;
    }
    DrumPatterns& patterns() { return patterns_; }
    AudioRef leftOut()  const { return AudioRef{outL_.data(), (std::size_t)lastN_, 48000}; }
    AudioRef rightOut() const { return AudioRef{outR_.data(), (std::size_t)lastN_, 48000}; }

private:
    AsyncLoader<AudioClip> loaders_[kVoices];
    AudioClip    clips_[kVoices];
    bool         haveClip_[kVoices] = {false, false, false, false};
    SampleVoice  voices_[kVoices];
    bool         accent_[kVoices]   = {false, false, false, false};
    DrumPatterns patterns_;
    std::vector<float> outL_, outR_;
    int    lastN_   = 0;
    int    loaded_  = 0;
    float  lastPatternPort_ = 1.0f;   // matches the `pattern` port default so load+first-frame don't override active
    // sync stepping
    long long lastStepAbs_ = 0;
    bool      primed_      = false;
    // free stepping
    double clock_ = 0.0, nextStep_ = 0.0;
    int    freeStep_ = 0;
};

} // namespace oss
```

- [ ] **Step 2: Write the integration test** — in `tests/gl_smoke.cpp`, add `#include "modules/DrumMachineNode.h"` near the other `modules/` includes (~line 34), then add this scenario just before the final success print / `glfwTerminate()` (model it on the existing Trail→Deform scenario placement). It is GL-free (no GL calls); it lives here only because this target links FFmpeg:

```cpp
    // --- Scenario: Drum Machine triggers a sample on a step + applies accent / pan (GL-free) ---
    {
        auto ramp = [](int frames) {
            AudioClip c; c.ok = true; c.sampleRate = 48000; c.channels = 2;
            c.samples.assign((std::size_t)frames * 2, 1.0f);   // constant 1.0 L/R
            return c;
        };
        // 20 input Values in port order; voice 0 vol=1 rate=1 pan=`pan`, sync off (free clock fires step 0).
        auto inputs = [](float pan) {
            std::vector<Value> in;
            for (int v = 0; v < 4; ++v) {
                in.push_back(Value(std::string("")));                       // file
                in.push_back(Value(1.0f));                                  // vol
                in.push_back(Value(1.0f));                                  // rate
                in.push_back(Value(v == 0 ? pan : 0.0f));                   // pan
            }
            in.push_back(Value(120.0f));   // tempo
            in.push_back(Value(false));    // sync (free)
            in.push_back(Value(4.0f));     // rate sync (1/16)
            in.push_back(Value(1.0f));     // pattern
            return in;
        };
        auto peak = [](const AudioRef& a) {
            float m = 0.0f; for (std::size_t i = 0; i < a.count; ++i) m = std::max(m, std::fabs(a.samples[i])); return m;
        };

        // (a) an ON cell triggers voice 0 -> non-zero output on both channels (center pan).
        DrumMachineNode dm;
        dm.injectClip(0, ramp(64));
        dm.patterns().setCell(0, 0, 0, 1);
        std::vector<Value> in = inputs(0.0f), outs(2);
        EvalContext ctx{in, outs, 0.02f, nullptr, nullptr};
        dm.evaluate(ctx);
        float onPeak = peak(dm.leftOut());
        if (onPeak <= 0.05f) { glfwTerminate(); return fail("Drum Machine: ON step produced no audio"); }
        if (peak(dm.rightOut()) <= 0.05f) { glfwTerminate(); return fail("Drum Machine: center pan gave no right channel"); }

        // (b) an ACCENT cell is louder than an ON cell.
        DrumMachineNode dmA;
        dmA.injectClip(0, ramp(64));
        dmA.patterns().setCell(0, 0, 0, 2);   // accent
        std::vector<Value> inA = inputs(0.0f), outsA(2);
        EvalContext ctxA{inA, outsA, 0.02f, nullptr, nullptr};
        dmA.evaluate(ctxA);
        if (peak(dmA.leftOut()) <= onPeak + 0.01f) { glfwTerminate(); return fail("Drum Machine: accent not louder than on"); }

        // (c) hard-left pan routes to left only.
        DrumMachineNode dmP;
        dmP.injectClip(0, ramp(64));
        dmP.patterns().setCell(0, 0, 0, 1);
        std::vector<Value> inP = inputs(-1.0f), outsP(2);
        EvalContext ctxP{inP, outsP, 0.02f, nullptr, nullptr};
        dmP.evaluate(ctxP);
        if (peak(dmP.leftOut()) <= 0.05f || peak(dmP.rightOut()) >= 0.02f) {
            glfwTerminate(); return fail("Drum Machine: hard-left pan did not route to left only");
        }
        std::fprintf(stderr, "gl_smoke OK: Drum Machine triggers a step, accent louder, pan routes\n");
    }
```

- [ ] **Step 3: Build + run** — `cmake --build build -j && ctest --test-dir build -R gl_smoke --output-on-failure` → expected: pass, with `gl_smoke OK: Drum Machine triggers a step, accent louder, pan routes`. (`core_tests` still green.)

- [ ] **Step 4: Commit**

```bash
git add src/modules/DrumMachineNode.h tests/gl_smoke.cpp
git commit -m "feat(modules): add the Drum Machine node (4-voice sampler + tri-state sequencer)"
```

---

### Task 5: `ui/NodeEditorPanel.cpp` — render the tri-state grid

**Files:**
- Modify: `src/ui/NodeEditorPanel.cpp` (after the button-bank block, ~line 92, before the input-port loop `for (std::size_t i = 0; i < n.inputs().size(); ++i)`)

- [ ] **Step 1: Implement** — in `src/ui/NodeEditorPanel.cpp`, immediately after the closing `}` of the `if (nbtn > 0) { ... }` button-bank block and before the input-pin `for` loop, add:

```cpp
        int grows = n.gridRows(), gcols = n.gridCols();
        if (grows > 0 && gcols > 0) {
            for (int r = 0; r < grows; ++r) {
                std::string rl = n.gridRowLabel(r);
                if (!rl.empty()) { ImGui::TextUnformatted(rl.c_str()); ImGui::SameLine(); }
                for (int c = 0; c < gcols; ++c) {
                    if (c) ImGui::SameLine();
                    int st = n.gridCell(r, c);
                    ImU32 col = st == 2 ? IM_COL32(250, 210,  70, 255)    // accent: brightest
                              : st == 1 ? IM_COL32(120, 150, 200, 255)    // on
                                        : IM_COL32( 45,  48,  56, 255);   // off: dark
                    ImGui::PushStyleColor(ImGuiCol_Button, col);
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, col);
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive, col);
                    std::string id = "##g" + std::to_string(r) + "_" + std::to_string(c);
                    if (ImGui::Button(id.c_str(), ImVec2(14, 14))) n.onGridCellPressed(r, c);
                    ImGui::PopStyleColor(3);
                }
            }
        }
```

- [ ] **Step 2: Build** — `cmake --build build -j` → expected: builds clean. (No automated UI test; rendering needs an ImGui frame.)

- [ ] **Step 3: Manual verification** — run `./build/shader_streamer`, add a **Drum Machine** node (after Task 6 registers it), confirm the 4×16 grid renders, clicking a cell cycles dark → blue → bright-yellow → dark, and the 8 slot buttons switch patterns. (If doing Task 6 after this, defer the manual check until then.)

- [ ] **Step 4: Commit**

```bash
git add src/ui/NodeEditorPanel.cpp
git commit -m "feat(ui): render the tri-state step grid in the node editor"
```

---

### Task 6: `app/Application.cpp` — register the node

**Files:**
- Modify: `src/app/Application.cpp` (include ~line 34; `makeNode()` ~line 50; `nodeCategories()` Audio list ~line 87)

- [ ] **Step 1: Add the include** — in `src/app/Application.cpp`, with the other `modules/` includes (alphabetical-ish, near `#include "modules/AudioPlayerNode.h"`), add:

```cpp
#include "modules/DrumMachineNode.h"
```

- [ ] **Step 2: Register in `makeNode()`** — add this line alongside the other `if (type == ...)` returns (e.g. after the `"Audio File"` line):

```cpp
    if (type == "Drum Machine") return std::make_unique<DrumMachineNode>();
```

- [ ] **Step 3: Add to the Audio category menu** — in `nodeCategories()`, append `"Drum Machine"` to the `"Audio"` list (before `"Audio Out"`):

```cpp
        { "Audio",   { "Sine", "Acid Bass", "Audio File", "Audio In", "Audio Mix", "Mono to Stereo", "Stereo to Mono", "Spectrograph", "Oscilloscope", "Drum Machine", "Audio Out" } },
```

- [ ] **Step 4: Build + verify registration** — `cmake --build build -j` → expected: builds clean. Then:

```bash
grep -n "Drum Machine" src/app/Application.cpp
```
Expected: 2 hits (makeNode + category).

- [ ] **Step 5: Manual smoke** — run `./build/shader_streamer`; right-click → **Audio → Drum Machine** adds the node; the grid + 8 buttons render; wire `left`/`right` into **Audio Out**; with a sample path in `file 1`, pressing Play triggers the kit. Confirm Save/Load preserves the grid (the `state` line) and the sample paths.

- [ ] **Step 6: Commit**

```bash
git add src/app/Application.cpp
git commit -m "feat(app): register the Drum Machine node in the Audio category"
```

---

### Task 7: Documentation

**Files:**
- Modify: `CLAUDE.md` (add an architecture bullet, near the other sequencer/audio bullets)
- Modify: `README.md` (add a Modules-table row, near `Step Seq` / the audio nodes)

- [ ] **Step 1: CLAUDE.md** — add this bullet after the "Transport-synced sequencers" / Acid Bass area (a place describing audio/sequencer nodes):

```markdown
- **Drum Machine** — `DrumMachineNode` (`src/modules/DrumMachineNode.h`, header-only) is a
  sample-based drum machine: 4 voices, each a file (async-decoded via `AsyncLoader<AudioClip>` like
  the Audio Player) with `vol`/`rate`/`pan` input ports, sequenced on a 4×16 tri-state step grid
  (off/on/**accent**). Eight pattern slots (the GL-free `core/DrumPattern.h` store + text codec) hold
  independent grids; switch with 8 buttons or the automatable `pattern` port (edge-detected). The
  clock mirrors **Step Seq** (`core/StepSync.h`): transport-synced or free. A step's on/accent cells
  (re)trigger that row's GL-free `audio/SampleVoice.h` (one-shot, retrigger, mono-downmix + linear
  interp); accent plays louder; voices mix through `core/AudioPan.h` `panGains` into a `left`/`right`
  output. A new generic tri-state **grid** `Node` hook (`gridRows/gridCols/gridCell/onGridCellPressed`)
  is rendered by `NodeEditorPanel`. The grids persist via `saveState`; paths/vol/rate/pan persist as
  control defaults. `DrumPattern` + `SampleVoice` are unit-tested in `core_tests`; the node trigger/
  accent/pan path is `gl_smoke`-checked.
```

- [ ] **Step 2: README.md** — add this row to the Modules table (after the `Step Seq` row or near the audio rows):

```markdown
| **Drum Machine** | sample-based drum machine → stereo `left`/`right` audio: 4 sample voices (each a `file` + `vol`/`rate`/`pan`) sequenced on a 4×16 tri-state grid (off / on / **accent**), with 8 pattern slots (8 buttons or an automatable `pattern` input). `sync` locks the 16 steps to the project transport (a `rate sync` division) or free-runs at `tempo`. Wire `left`/`right` into **Audio Out** |
```

- [ ] **Step 3: Verify + commit** — `cmake --build build -j && ctest --test-dir build --output-on-failure` (sanity; docs don't affect the build) then:

```bash
git add CLAUDE.md README.md
git commit -m "docs: document the Drum Machine node"
```

---

## Final verification (after all tasks)

```bash
cmake --build build -j
ctest --test-dir build --output-on-failure     # core_tests + gl_smoke both green
grep -n "Drum Machine" src/app/Application.cpp  # 2 hits
```

Then dispatch a final whole-branch code review before finishing the branch.
