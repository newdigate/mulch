# Acid Bass `level` Output Gain Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a `level` output-volume control to the Acid Bass — a post-distortion gain (range 0–1, default 0.7) so the synth is quieter by default.

**Architecture:** A `level_` gain + `setLevel()` on the GL-free `AcidVoice` DSP, applied as the final scale on the published output sample (unit-tested in `core_tests`); plus a `level` input port on `AcidNode` wired to it.

**Tech Stack:** C++17, doctest, CMake. Design: `docs/superpowers/specs/2026-06-18-acid-bass-level-design.md`.

**Context:** `AcidVoice` (`src/audio/AcidVoice.{h,cpp}`) is the GL-free monophonic 303 DSP; `AcidNode` (`src/modules/AcidNode.h`) wraps it, exposing every control as an input port. Both are unit-tested in `tests/test_acid_voice.cpp` (`core_tests`). No new files, no CMake change.

---

### Task 1: `AcidVoice` output gain + unit tests

**Files:**
- Modify: `tests/test_acid_voice.cpp` (add 3 voice cases)
- Modify: `src/audio/AcidVoice.h` (add `level_` + `setLevel`)
- Modify: `src/audio/AcidVoice.cpp` (scale the output)

- [ ] **Step 1: Write the failing tests**

In `tests/test_acid_voice.cpp`, add this helper and three test cases immediately AFTER the existing `TEST_CASE("voice: an out-of-range MIDI note is clamped (no inf/NaN)")` block (i.e. before the `AcidNode renders audio` test). The helper uses the file's existing `<algorithm>`/`<cmath>` includes:

```cpp
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
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake -S . -B build && cmake --build build -j --target core_tests`
Expected: FAIL — compile error `no member named 'setLevel' in 'oss::AcidVoice'`.

- [ ] **Step 3: Add the gain to the voice header**

In `src/audio/AcidVoice.h`, add the setter right after the `setDistortion` line (`void setDistortion(float a) { distortion_ = clamp01(a); }`):

```cpp
    void setLevel(float a)      { level_ = clamp01(a); }
```

And add the member right after the `distortion_` parameter (`float distortion_ = 0.0f;`):

```cpp
    float level_      = 0.7f;   // output gain (post-distortion volume trim)
```

- [ ] **Step 4: Apply the gain in `process()`**

In `src/audio/AcidVoice.cpp`, find the final two lines of the `process()` loop:

```cpp
        lastOut_ = std::tanh(s);   // bounded VCA output -> stable filter-FM feedback
        out[i] = std::tanh(s * (1.0f + distortion_ * 9.0f));
```

Replace them with (scale only the published sample; `lastOut_` is the filter-FM feedback tap and stays unscaled):

```cpp
        lastOut_ = std::tanh(s);   // bounded VCA output -> stable filter-FM feedback (unscaled)
        out[i] = level_ * std::tanh(s * (1.0f + distortion_ * 9.0f));
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `cmake --build build -j --target core_tests && ./build/core_tests`
Expected: PASS — all `core_tests` green, including the 3 new level cases. (Run the full binary to confirm the other Acid Bass tests still pass — the default-0.7 gain now applies to every voice render, but those tests assert relative/bounded properties, not absolute levels, so they remain valid.)

- [ ] **Step 6: Commit**

```bash
git add src/audio/AcidVoice.h src/audio/AcidVoice.cpp tests/test_acid_voice.cpp
git commit -m "$(cat <<'EOF'
feat(audio): add an output level gain to the Acid Bass voice

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
)"
```

---

### Task 2: `AcidNode` `level` port + docs

**Files:**
- Modify: `src/modules/AcidNode.h` (port + setter call + comment)
- Modify: `tests/test_acid_voice.cpp` (update the existing `AcidNode renders audio` test for the new port)
- Modify: `README.md`, `CLAUDE.md`

- [ ] **Step 1: Update the node test for the new 14th input (write the failing change)**

In `tests/test_acid_voice.cpp`, in the `TEST_CASE("AcidNode renders audio for a MIDI note and decays after note-off")`, change the input vector from 13 to 14 entries and add the `level` value. Replace:

```cpp
        std::vector<Value> in(13);
```
with:
```cpp
        std::vector<Value> in(14);
```
and add this line right after the `in[12] = 0.0f;   // distortion` line:
```cpp
        in[13] = 0.7f;   // level
```

(Without the node change this test still compiles and passes — the new `in[13]` is simply unread — so this step is a no-op behaviorally until Step 2 wires the port. The point is the test must supply 14 inputs so that once the node reads `ctx.in<float>(13)` it is in bounds and non-silent.)

- [ ] **Step 2: Add the port to the node**

In `src/modules/AcidNode.h`, add the input right after the `distortion` line (`addInput("distortion", PortType::Float, 0.0f, 0.0f, 1.0f);`):

```cpp
        addInput("level",      PortType::Float, 0.7f,   0.0f,   1.0f);
```

In `evaluate()`, add the setter call right after `voice_.setDistortion(ctx.in<float>(12));`:

```cpp
        voice_.setLevel(ctx.in<float>(13));
```

Update the node's input-list doc comment (the block comment above the class) — change its last line:

```cpp
//   11 = key track, 12 = distortion.
```
to:
```cpp
//   11 = key track, 12 = distortion, 13 = level.
```

- [ ] **Step 3: Build and run the full suite**

Run: `cmake --build build -j && ctest --test-dir build --output-on-failure`
Expected: clean build of `shader_streamer`, `core_tests`, `gl_smoke`; all tests pass (the `AcidNode renders audio` test now drives the node with 14 inputs including `level = 0.7`, still audible, still decays to silence).

- [ ] **Step 4: Verify the node renders the new control**

Run: `./build/shader_streamer --screenshot build/_ui.png`
Expected: exit 0. Open the PNG with the Read tool and confirm the **Acid Bass** node now shows a `level` slider (value 0.700) as its last input, below `distortion`.

- [ ] **Step 5: Update the docs**

In `README.md`, find the **Acid Bass** module-table row. Its control list ends "... and filter key-tracking. Every control is an input port". Insert `output level` into the listed controls — change "with note slide, filter FM (VCA → cutoff), and filter key-tracking." to "with note slide, filter FM (VCA → cutoff), filter key-tracking, and output `level`."

In `CLAUDE.md`, find the **Acid Bass synth voice** Architecture bullet. Its signal chain reads "... → VCA → a `tanh` distortion stage, plus note slide)." Change it to "... → VCA → a `tanh` distortion stage → output `level`, plus note slide)." so the documented chain stays accurate.

- [ ] **Step 6: Commit**

```bash
git add src/modules/AcidNode.h tests/test_acid_voice.cpp README.md CLAUDE.md
git commit -m "$(cat <<'EOF'
feat(modules): expose the Acid Bass output level as an input port

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
)"
```

---

## Final verification

- [ ] `cmake --build build -j` — clean build (`shader_streamer`, `core_tests`, `gl_smoke`)
- [ ] `ctest --test-dir build --output-on-failure` — all pass (incl. the 3 new level cases)
- [ ] `./build/shader_streamer --screenshot build/_ui.png` — the Acid Bass node shows the `level` slider
- [ ] Use superpowers:finishing-a-development-branch
</content>
