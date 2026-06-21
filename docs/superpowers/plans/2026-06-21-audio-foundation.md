# Audio Foundation Implementation Plan (Phase A of the audio-decouple roadmap)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Stop the intermittent audio under-runs by making the per-frame audio block track `dt` (removing the small per-node caps), adding a configurable output-buffer length in Preferences, and raising the MIDI timing thread's priority.

**Architecture:** A new GL-free `audio/AudioBlock.h` holds the shared block-size + ring-sizing math (unit-tested); every audio source node uses it. `AudioOutputNode`'s ring becomes a `unique_ptr` sized from a new `Preferences::audioBufferMs`, recreated when the device or buffer changes. A best-effort `app/ThreadPriority.h` raises the `MidiSyncEngine` sender thread. Audio stays on the main thread (the worker-thread decouple is Phase C).

**Tech Stack:** C++17, libsoundio, Dear ImGui, pthread QoS (macOS), doctest, CMake. Design: `docs/superpowers/specs/2026-06-21-audio-foundation-design.md`.

**Notes:** `AudioBlock.h` and `ThreadPriority.h` follow the repo's GL-free/layering rules (`audio/` is GL-free; `app/ThreadPriority.cpp` is platform glue → `APP_SOURCES`). If any existing audio test changes output because it fed a `dt` larger than a node's old cap, STOP and report rather than editing the test's expectation.

---

### Task 1: `audio/AudioBlock.h` (shared block-size + ring-sizing math)

**Files:** Create `src/audio/AudioBlock.h`, `tests/test_audio_block.cpp`; Modify `CMakeLists.txt`.

- [ ] **Step 1: Write the failing test `tests/test_audio_block.cpp`**

```cpp
#include <doctest/doctest.h>
#include "audio/AudioBlock.h"

using namespace oss;

TEST_CASE("audioBlockFrames tracks dt, bounded by the audio dt clamp") {
    CHECK(audioBlockFrames(48000.0, 1.0 / 60.0) == 800);
    CHECK(audioBlockFrames(48000.0, 0.025) == 1200);          // a slow 25ms frame: NOT clamped to 1024
    CHECK(audioBlockFrames(48000.0, 0.0) == 1);               // floor
    CHECK(audioBlockFrames(48000.0, -1.0) == 1);              // negative -> floor
    CHECK(audioBlockFrames(48000.0, 1.0) == 4080);            // a 1s dt clamps via kMaxAudioDt (48000*0.085)
    CHECK(audioBlockFrames(48000.0, 100.0) <= kAudioMaxBlock);
}

TEST_CASE("audioRingFloats maps ms -> interleaved-stereo float capacity") {
    CHECK(audioRingFloats(150, 48000) == 14400);              // 7200 stereo frames * 2
    CHECK(audioRingFloats(20, 48000) == 1920);                // 960 * 2
    CHECK(audioRingFloats(150, 48000) > audioRingFloats(100, 48000));   // monotonic in ms
}
```

- [ ] **Step 2: Wire the test into the build (`CMakeLists.txt`)**

In the `core_tests` test-source list, add after `tests/test_midi_timecode.cpp`:  `tests/test_audio_block.cpp`
(`AudioBlock.h` is header-only — no `APP_SOURCES`/`core_tests` `.cpp` entry needed.)

- [ ] **Step 3: Run to verify it fails**

Run: `cmake -S . -B build && cmake --build build -j --target core_tests`
Expected: FAIL — `'audio/AudioBlock.h' file not found`.

- [ ] **Step 4: Create `src/audio/AudioBlock.h`**

```cpp
#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>

namespace oss {

constexpr int    kAudioMaxBlock = 8192;   // max audio frames produced per evaluate (safety bound)
constexpr double kMaxAudioDt    = 0.085;  // clamp dt for AUDIO block sizing (~4080 frames @48k)

// Frames of audio a source should produce this frame: round(sampleRate * min(dt, kMaxAudioDt)),
// clamped to [1, kAudioMaxBlock]. The dt clamp is local to audio (the transport keeps its own dt),
// so a multi-second stall yields a bounded ~85 ms catch-up block, not a runaway allocation.
inline int audioBlockFrames(double sampleRate, double dt) {
    double d = dt < 0.0 ? 0.0 : (dt > kMaxAudioDt ? kMaxAudioDt : dt);
    long n = std::lround(sampleRate * d);
    if (n < 1) n = 1;
    if (n > kAudioMaxBlock) n = kAudioMaxBlock;
    return (int)n;
}

// Output-ring capacity in interleaved-stereo floats for a buffer length in milliseconds.
// (SpscRingBuffer rounds this up to a power of two internally.)
inline std::size_t audioRingFloats(int bufferMs, int sampleRate) {
    if (bufferMs < 1) bufferMs = 1;
    long frames = (long)((long long)bufferMs * sampleRate / 1000);
    if (frames < 1) frames = 1;
    return (std::size_t)frames * 2;   // L + R interleaved
}

} // namespace oss
```

- [ ] **Step 5: Run to verify it passes**

Run: `cmake --build build -j --target core_tests && ./build/core_tests`
Expected: PASS — both new cases plus every existing test.

- [ ] **Step 6: Full build**

Run: `cmake --build build -j && ctest --test-dir build --output-on-failure`
Expected: clean build (shader_streamer, core_tests, gl_smoke); all pass.

- [ ] **Step 7: Commit**

```bash
git add src/audio/AudioBlock.h tests/test_audio_block.cpp CMakeLists.txt
git commit -m "$(cat <<'EOF'
feat(audio): add AudioBlock.h (dt-tracked block size + ring sizing)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
)"
```

---

### Task 2: Audio nodes use the shared block helper

**Files:** Modify `src/modules/SineWaveNode.h`, `src/modules/AcidNode.h`, `src/modules/AudioPlayerNode.{h,cpp}`, `src/modules/VideoPlayerNode.{h,cpp}`, `src/modules/AudioMixerNode.h`, `src/modules/MonoToStereoNode.h`, `src/modules/StereoToMonoNode.h`.

Each edit: include `audio/AudioBlock.h`, size owned buffers to `kAudioMaxBlock`, and replace the local cap. This is mechanical — match each exactly.

- [ ] **Step 1: `SineWaveNode.h`**

Add `#include "audio/AudioBlock.h"` after `#include "core/Value.h"`. Change the ctor buffer to `buffer_(kAudioMaxBlock, 0.0f)`. Replace the `const int n = std::clamp(...)` block with:
```cpp
        const int n = audioBlockFrames(sampleRate_, ctx.dt);
```
Delete the `static constexpr int kMaxBlock = 1024;` line.

- [ ] **Step 2: `AcidNode.h`**

Add `#include "audio/AudioBlock.h"` (next to its other includes). Change the ctor `buffer_(kMaxBlock, 0.0f)` to `buffer_(kAudioMaxBlock, 0.0f)`. Replace:
```cpp
        int n = std::clamp((int)std::lround(sampleRate_ * (double)ctx.dt), 1, kMaxBlock);
```
with:
```cpp
        int n = audioBlockFrames(sampleRate_, ctx.dt);
```
Delete the `static constexpr int kMaxBlock = 2048;` line.

- [ ] **Step 3: `AudioPlayerNode.h`**

Add `#include "audio/AudioBlock.h"` (next to its includes). Delete the `static constexpr int kMaxBlock = 4096;` line. Update the `outL_, outR_` member comment to `// mono left / right (kAudioMaxBlock each)`.

- [ ] **Step 4: `AudioPlayerNode.cpp`**

Add `#include "audio/AudioBlock.h"` near the top includes. In the constructor, change `outL_.assign(kMaxBlock, 0.0f);` / `outR_.assign(kMaxBlock, 0.0f);` to `kAudioMaxBlock`. In `emitAudio`, replace:
```cpp
    int n = std::clamp((int)std::lround(sampleRate_ * (double)ctx.dt), 1, kMaxBlock);
```
with:
```cpp
    int n = audioBlockFrames(sampleRate_, ctx.dt);
```

- [ ] **Step 5: `VideoPlayerNode.h`**

Add `#include "audio/AudioBlock.h"` (next to its includes). Delete the `static constexpr int kMaxBlock  = 4096;` line (keep `kMaxFrames`).

- [ ] **Step 6: `VideoPlayerNode.cpp`**

Add `#include "audio/AudioBlock.h"` near the top includes. In the constructor, change `outBuf_(kMaxBlock, 0.0f)` to `outBuf_(kAudioMaxBlock, 0.0f)`. In `emitAudio`, replace:
```cpp
    int n = std::clamp((int)std::lround(outRate_ * (double)ctx.dt), 1, kMaxBlock);
```
with:
```cpp
    int n = audioBlockFrames(outRate_, ctx.dt);
```

- [ ] **Step 7: Unify the processing-node caps (`AudioMixerNode.h`, `MonoToStereoNode.h`, `StereoToMonoNode.h`)**

In each of the three files: add `#include "audio/AudioBlock.h"` (next to its includes), delete the local `static constexpr int kMaxBlock = 8192;` line, and replace every remaining `kMaxBlock` token in that file with `kAudioMaxBlock` (the ctor buffer sizes `bufL_(kMaxBlock,...)`/`bufR_(...)`/`buf_(...)` and the `std::min(n, (std::size_t)kMaxBlock)` input clamp). `kAudioMaxBlock` is 8192, so this is behavior-preserving — it just points all three at the one shared bound.

- [ ] **Step 8: Build + full test suite**

Run: `cmake --build build -j && ctest --test-dir build --output-on-failure`
Expected: clean build; ALL tests pass. The existing audio tests feed small `dt`s (well under the old caps), so output frame counts are unchanged; `gl_smoke` audio scenarios are unaffected. If any test's output count changed, STOP and report (do NOT edit the test).

- [ ] **Step 9: Commit**

```bash
git add src/modules/SineWaveNode.h src/modules/AcidNode.h src/modules/AudioPlayerNode.h src/modules/AudioPlayerNode.cpp src/modules/VideoPlayerNode.h src/modules/VideoPlayerNode.cpp src/modules/AudioMixerNode.h src/modules/MonoToStereoNode.h src/modules/StereoToMonoNode.h
git commit -m "$(cat <<'EOF'
fix(audio): size per-frame audio blocks to dt (shared kAudioMaxBlock)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
)"
```

---

### Task 3: `Preferences::audioBufferMs`

**Files:** Modify `src/core/Preferences.h`, `src/core/Preferences.cpp`, `tests/test_preferences.cpp`.

- [ ] **Step 1: Append the failing test to `tests/test_preferences.cpp`**

```cpp
TEST_CASE("audio buffer ms round-trips and clamps") {
    Preferences p; p.audioBufferMs = 200;
    Preferences r;
    REQUIRE(parsePreferences(serializePreferences(p), r));
    CHECK(r.audioBufferMs == 200);
    Preferences c;
    REQUIRE(parsePreferences("oss-prefs 1\naudio-buffer 5\n", c));
    CHECK(c.audioBufferMs == 20);          // clamps up to the floor
    Preferences d;
    REQUIRE(parsePreferences("oss-prefs 1\naudio-buffer 9999\n", d));
    CHECK(d.audioBufferMs == 500);         // clamps down to the ceiling
    Preferences e;
    REQUIRE(parsePreferences("oss-prefs 1\n", e));
    CHECK(e.audioBufferMs == 150);         // default when the line is absent
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build build -j --target core_tests`
Expected: FAIL — `audioBufferMs` undeclared.

- [ ] **Step 3: Add the field (`src/core/Preferences.h`)**

After `int textureHeight = 720;`, add:
```cpp
    int audioBufferMs = 150;     // output ring length (ms); trades latency for under-run headroom [20,500]
```

- [ ] **Step 4: Serialize + parse (`src/core/Preferences.cpp`)**

In `serializePreferences`, after the `texture-size` line, add:
```cpp
    out += "audio-buffer " + std::to_string(p.audioBufferMs) + "\n";
```
In `parsePreferences`, add a branch alongside the others:
```cpp
        else if (kw == "audio-buffer") {
            std::istringstream rs(rest); int ms = 150; rs >> ms;
            if (ms < 20)  ms = 20;
            if (ms > 500) ms = 500;
            out.audioBufferMs = ms;
        }
```

- [ ] **Step 5: Run to verify it passes**

Run: `cmake --build build -j --target core_tests && ./build/core_tests`
Expected: PASS — the new case plus the existing Preferences round-trip tests (the new `audio-buffer 150` line defaults harmlessly).

- [ ] **Step 6: Full build**

Run: `cmake --build build -j && ctest --test-dir build --output-on-failure`
Expected: clean build; all tests pass.

- [ ] **Step 7: Commit**

```bash
git add src/core/Preferences.h src/core/Preferences.cpp tests/test_preferences.cpp
git commit -m "$(cat <<'EOF'
feat(core): add audioBufferMs preference (output ring length)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
)"
```

---

### Task 4: `AudioOutputNode` sizes its ring from the preference

**Files:** Modify `src/modules/AudioOutputNode.h`, `src/modules/AudioOutputNode.cpp`.

- [ ] **Step 1: Header (`src/modules/AudioOutputNode.h`)**

Add `#include <memory>` after `#include <vector>`. Change the ring member:
```cpp
    SpscRingBuffer<float> ring_{1 << 14};
```
to:
```cpp
    std::unique_ptr<SpscRingBuffer<float>> ring_;   // sized from Preferences::audioBufferMs at open
```
Change the `ensureDevice` declaration to take the buffer ms:
```cpp
    bool ensureDevice(const std::string& wantId, int wantBufferMs);
```
Change `openStream`:
```cpp
    bool openStream(const std::string& wantId, int wantBufferMs);
```
Add a tracker after `std::string currentDeviceId_;`:
```cpp
    int  currentBufferMs_ = -1;          // buffer ms the stream is currently open with
```

- [ ] **Step 2: `evaluate` reads the buffer ms + guards the push (`src/modules/AudioOutputNode.cpp`)**

Add `#include "audio/AudioBlock.h"` near the top includes. In `evaluate`, replace:
```cpp
    std::string want = ctx.prefs ? ctx.prefs->audioOutputDeviceId : std::string();
    if (!ensureDevice(want)) return;     // no device -> silent no-op
```
with:
```cpp
    std::string want = ctx.prefs ? ctx.prefs->audioOutputDeviceId : std::string();
    int wantMs       = ctx.prefs ? ctx.prefs->audioBufferMs : 150;
    if (!ensureDevice(want, wantMs)) return;   // no device -> silent no-op
```
and replace the push line:
```cpp
    ring_.push(stereoScratch_.data(), n * 2);            // overflow dropped, never blocks
```
with:
```cpp
    if (ring_) ring_->push(stereoScratch_.data(), n * 2);   // overflow dropped, never blocks
```

- [ ] **Step 3: `ensureDevice` reopens on device OR buffer change (`src/modules/AudioOutputNode.cpp`)**

Replace the whole `ensureDevice`:
```cpp
bool AudioOutputNode::ensureDevice(const std::string& wantId) {
    if (!soundio_) {
        if (contextFailed_) return false;
        if (!openContext()) { contextFailed_ = true; return false; }
    }
    if (streamOpen_ && currentDeviceId_ == wantId) return true;
    closeStream();
    return openStream(wantId);
}
```
with:
```cpp
bool AudioOutputNode::ensureDevice(const std::string& wantId, int wantBufferMs) {
    if (!soundio_) {
        if (contextFailed_) return false;
        if (!openContext()) { contextFailed_ = true; return false; }
    }
    if (streamOpen_ && currentDeviceId_ == wantId && currentBufferMs_ == wantBufferMs) return true;
    closeStream();
    return openStream(wantId, wantBufferMs);
}
```

- [ ] **Step 4: `openStream` creates the ring at the configured size (`src/modules/AudioOutputNode.cpp`)**

Change the `openStream` signature line to:
```cpp
bool AudioOutputNode::openStream(const std::string& wantId, int wantBufferMs) {
```
After `sampleRate_ = soundio_device_nearest_sample_rate(device_, 48000);` (before `outstream_ = ...`), add:
```cpp
    ring_ = std::make_unique<SpscRingBuffer<float>>(audioRingFloats(wantBufferMs, sampleRate_));
```
At the end of `openStream`, where it sets `currentDeviceId_ = wantId;`, also set the tracker right after it:
```cpp
    currentBufferMs_ = wantBufferMs;
```

- [ ] **Step 5: Guard the RT callback against a not-yet-created ring (`src/modules/AudioOutputNode.cpp`)**

At the very top of `writeCallback`, after `auto* self = static_cast<AudioOutputNode*>(os->userdata);`, add:
```cpp
    if (!self->ring_) return;
```
(The callback only runs while the stream is started — hence a live ring — but this is a cheap belt-and-braces guard.) The pop call stays `self->ring_->pop(...)` — change the existing `self->ring_.pop(` to `self->ring_->pop(`.

- [ ] **Step 6: Build + verify**

Run: `cmake -S . -B build && cmake --build build -j && ctest --test-dir build --output-on-failure`
Expected: clean build; all tests pass (no unit test for the node — it's soundio/RT; the math is covered by Task 1/3). Confirm the app launches + quits cleanly:
`./build/shader_streamer --screenshot build/_audio.png && echo "exit $?"` → exit 0.

- [ ] **Step 7: Commit**

```bash
git add src/modules/AudioOutputNode.h src/modules/AudioOutputNode.cpp
git commit -m "$(cat <<'EOF'
feat(audio): size AudioOutputNode ring from audioBufferMs (reopen on change)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
)"
```

---

### Task 5: Preferences Audio Output tab — buffer slider

**Files:** Modify `src/ui/PreferencesPanel.cpp`.

- [ ] **Step 1: Add the slider (`src/ui/PreferencesPanel.cpp`)**

In the `if (ImGui::BeginTabItem("Audio Output")) {` block, after `deviceCombo("Output device", outDevices_, prefs.audioOutputDeviceId);` and before `ImGui::EndTabItem();`, add:
```cpp
            ImGui::SetNextItemWidth(160.0f);
            if (ImGui::SliderInt("Audio buffer (ms)", &prefs.audioBufferMs, 20, 500)) onChange();
            ImGui::TextDisabled("Higher = more under-run headroom + latency; lower = tighter latency.");
```
(`SliderInt` clamps the value to [20, 500]; `onChange()` persists and, via Task 4's reopen-on-change, applies live.)

- [ ] **Step 2: Build + screenshot**

Run: `cmake --build build -j && ctest --test-dir build --output-on-failure` — clean build, all pass.

To verify the control, TEMPORARILY set `showPreferences_` to `true` in `src/app/Application.h`, rebuild, and:
```bash
cmake --build build -j && ./build/shader_streamer --screenshot build/_ui.png && echo "exit $?"
```
Open `build/_ui.png` with the Read tool: confirm the **Audio Output** tab shows the **Audio buffer (ms)** slider (if it's the active tab); otherwise confirm the app rendered + exit 0. Report what you saw. Then **REVERT** `showPreferences_` to `false`, rebuild, re-screenshot (exit 0). The committed code MUST have `showPreferences_ = false`, and `Application.h` must NOT be in the commit.

- [ ] **Step 3: Commit**

```bash
git add src/ui/PreferencesPanel.cpp
git commit -m "$(cat <<'EOF'
feat(ui): Audio buffer (ms) slider in the Audio Output tab

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
)"
```

---

### Task 6: Elevated MIDI timing thread

**Files:** Create `src/app/ThreadPriority.h`, `src/app/ThreadPriority.cpp`; Modify `CMakeLists.txt`, `src/app/MidiSyncEngine.cpp`.

- [ ] **Step 1: Create `src/app/ThreadPriority.h`**

```cpp
#pragma once

namespace oss {

// Best-effort: raise the calling thread to a time-critical scheduling class so timing/audio work
// preempts ordinary threads. No-op / failure-tolerant where unsupported or not permitted.
// (macOS: QOS_CLASS_USER_INTERACTIVE.) Reused by the audio worker thread in a later phase.
void setThisThreadTimeCritical();

} // namespace oss
```

- [ ] **Step 2: Create `src/app/ThreadPriority.cpp`**

```cpp
#include "app/ThreadPriority.h"

#if defined(__APPLE__)
#include <pthread.h>
#include <sys/qos.h>
#endif

namespace oss {

void setThisThreadTimeCritical() {
#if defined(__APPLE__)
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
    // Other platforms: no-op for now (SCHED_FIFO/RR needs privileges; a later phase can add them).
}

} // namespace oss
```

- [ ] **Step 3: Wire into the build (`CMakeLists.txt`)**

In `APP_SOURCES`, add `src/app/ThreadPriority.cpp` (e.g. right before `src/app/MidiSyncEngine.cpp`). Do NOT add it to `core_tests`/`gl_smoke`.

- [ ] **Step 4: Call it from the sender thread (`src/app/MidiSyncEngine.cpp`)**

Add `#include "app/ThreadPriority.h"` near the top includes. At the very top of `void MidiSyncEngine::senderLoop() {` (before any local declarations), add:
```cpp
    setThisThreadTimeCritical();   // the timing thread preempts ordinary threads
```

- [ ] **Step 5: Build + verify**

Run: `cmake -S . -B build && cmake --build build -j && ctest --test-dir build --output-on-failure`
Expected: clean build (the new `.cpp` compiles + links); all tests pass. Confirm the app launches + quits cleanly (the sender thread still joins on exit):
`./build/shader_streamer --screenshot build/_p.png && echo "exit $?"` → exit 0.

- [ ] **Step 6: Commit**

```bash
git add src/app/ThreadPriority.h src/app/ThreadPriority.cpp CMakeLists.txt src/app/MidiSyncEngine.cpp
git commit -m "$(cat <<'EOF'
feat(app): raise the MIDI sender thread to a time-critical QoS

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
)"
```

---

### Task 7: Documentation

**Files:** Modify `README.md`, `CLAUDE.md`.

- [ ] **Step 1: README.md**

In the **Preferences** subsection, after the Audio Output/Input description, add:
```markdown
The Audio Output tab also has an **Audio buffer (ms)** control (20–500) that sizes the output ring:
higher trades latency for under-run headroom, lower tightens latency. The per-frame audio block now
tracks frame time, so a slow render frame no longer starves the output.
```

- [ ] **Step 2: CLAUDE.md**

Add an Architecture bullet after the Preferences bullet:
```markdown
- **Audio block sizing** — the GL-free `audio/AudioBlock.h` is the one source of truth for the
  per-frame audio block: `audioBlockFrames(sampleRate, dt)` returns `round(sampleRate·min(dt,
  kMaxAudioDt))` clamped to `[1, kAudioMaxBlock]` (the `dt` clamp is audio-local, so a slow/stalled
  frame still produces a full block and can't drain the output ring). Every audio source node
  (Sine, Acid Bass, Audio/Video Player) and the pass-through mixers share it. `AudioOutputNode`'s
  ring is a `unique_ptr<SpscRingBuffer>` sized from `Preferences::audioBufferMs` via
  `audioRingFloats`, recreated when the device or buffer length changes (the RT callback is stopped
  during reopen). `app/ThreadPriority.h`'s `setThisThreadTimeCritical()` (macOS QoS) raises the MIDI
  sender thread. This is **Phase A** of the audio-decouple roadmap (B = audio-subgraph
  compile/snapshot, C = dedicated audio thread, D = render frame-drop).
```

- [ ] **Step 3: Verify + commit**

Run: `ctest --test-dir build --output-on-failure`
Expected: all pass.

```bash
git add README.md CLAUDE.md
git commit -m "$(cat <<'EOF'
docs: document the configurable audio buffer + dt-tracked block (Phase A)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
)"
```

---

## Final verification

- [ ] `cmake --build build -j` — clean build (`shader_streamer`, `core_tests`, `gl_smoke`)
- [ ] `ctest --test-dir build --output-on-failure` — all pass (AudioBlock math; Preferences audio-buffer round-trip/clamp; existing audio tests unchanged; gl_smoke unaffected)
- [ ] `./build/shader_streamer --screenshot build/_ui.png` — exit 0; Audio Output tab has the buffer slider
- [ ] Manual: run under heavy render load (large output window), confirm the cut-outs are gone; raise/lower the Audio buffer slider and confirm it re-opens the stream and changes the cushion
- [ ] Use superpowers:finishing-a-development-branch
```
