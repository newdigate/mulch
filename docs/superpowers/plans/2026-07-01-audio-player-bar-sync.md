# Audio File Player Bar-Sync Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add `sync` (Bool) + `length` (Int bars) inputs to the Audio File player so that, when synced, the clip is time-warped to span exactly `length` bars, bar-locked to the global transport.

**Architecture:** A new GL-free, header-only helper (`audio/BarSync.h`) holds the one piece of testable math — `barSyncPlayhead(bars, lengthBars, durationSec)` → source-seconds position — and is unit-tested in `core_tests`. `AudioPlayerNode` appends the two ports and, when sync is active, derives its playhead from `transport.bars()` via the helper instead of integrating `rate·dt`; everything else (block emit, seam-silence rule) is reused unchanged.

**Tech Stack:** C++17, doctest (`core_tests`), CMake FetchContent. No GL, no persistence/codec change, no editor change.

**Spec:** `docs/superpowers/specs/2026-07-01-audio-player-bar-sync-design.md`

---

### Task 1: `barSyncPlayhead` helper + unit tests

The GL-free pure function that maps a transport bar position to a source-time playhead for a clip warped to `length` bars. Header-only so `core_tests` can test it without pulling in the FFmpeg decode path.

**Files:**
- Create: `src/audio/BarSync.h`
- Create: `tests/test_bar_sync.cpp`
- Modify: `CMakeLists.txt` (add the test source to `core_tests`, after the existing line `  tests/test_crossover_filter.cpp`)

- [ ] **Step 1: Write the failing test**

Create `tests/test_bar_sync.cpp`:

```cpp
#include <doctest/doctest.h>
#include "audio/BarSync.h"

using namespace oss;

TEST_CASE("barSyncPlayhead: aligns to bar 1 and repeats every length bars") {
    double dur = 8.0;
    CHECK(barSyncPlayhead(0.0, 4, dur) == doctest::Approx(0.0));       // start of window
    CHECK(barSyncPlayhead(4.0, 4, dur) == doctest::Approx(0.0));       // exactly one window -> wrapped to 0
    CHECK(barSyncPlayhead(2.0, 4, dur) == doctest::Approx(4.0));       // halfway -> half the clip
}

TEST_CASE("barSyncPlayhead: linear stretch so a full pass spans length bars") {
    double dur = 8.0;
    // length = 4: slope is dur/4 = 2.0 s per bar within the window
    CHECK(barSyncPlayhead(1.0, 4, dur) == doctest::Approx(2.0));       // dur/4
    CHECK(barSyncPlayhead(3.0, 4, dur) == doctest::Approx(6.0));       // 3*dur/4
    // length = 1: the whole clip fits in one bar
    CHECK(barSyncPlayhead(0.25, 1, dur) == doctest::Approx(2.0));      // dur/4
    CHECK(barSyncPlayhead(0.5,  1, dur) == doctest::Approx(4.0));      // dur/2
}

TEST_CASE("barSyncPlayhead: repeats across windows") {
    double dur = 8.0;
    CHECK(barSyncPlayhead(5.0, 4, dur) == doctest::Approx(barSyncPlayhead(1.0, 4, dur)));
    CHECK(barSyncPlayhead(9.0, 4, dur) == doctest::Approx(barSyncPlayhead(1.0, 4, dur)));
}

TEST_CASE("barSyncPlayhead: guards non-positive length and duration") {
    CHECK(barSyncPlayhead(2.5, 0,  8.0) == doctest::Approx(0.0));      // length < 1
    CHECK(barSyncPlayhead(2.5, -3, 8.0) == doctest::Approx(0.0));      // length < 1
    CHECK(barSyncPlayhead(2.5, 4,  0.0) == doctest::Approx(0.0));      // duration <= 0
    CHECK(barSyncPlayhead(2.5, 4, -1.0) == doctest::Approx(0.0));      // duration <= 0
}
```

- [ ] **Step 2: Register the test and verify it fails**

In `CMakeLists.txt`, add this line inside `add_executable(core_tests ...)` immediately after the existing `  tests/test_crossover_filter.cpp` line:

```cmake
  tests/test_bar_sync.cpp
```

Run:
```bash
cmake --build build -j --target core_tests
```
Expected: FAIL — compile error, `audio/BarSync.h` file not found (the header doesn't exist yet). This compile failure IS the TDD "red" state for C++.

- [ ] **Step 3: Write the implementation**

Create `src/audio/BarSync.h`:

```cpp
#pragma once
#include <cmath>

namespace oss {

// Source-time position (seconds) for a clip time-warped to span `lengthBars` bars,
// bar-locked to the transport's absolute bar position `bars`. The clip aligns to bar 1
// and repeats every `lengthBars` bars: within each length-bar window the position sweeps
// linearly from 0 to `durationSec` (slope durationSec/lengthBars per bar). Returns 0 for a
// non-positive length or duration. GL-free.
inline double barSyncPlayhead(double bars, int lengthBars, double durationSec) {
    if (lengthBars < 1 || durationSec <= 0.0) return 0.0;
    double win  = bars / (double)lengthBars;   // fractional count of length-bar windows
    double frac = win - std::floor(win);        // position within the current window [0,1)
    return frac * durationSec;                   // -> source seconds
}

} // namespace oss
```

- [ ] **Step 4: Build and run the test — verify it passes**

Run:
```bash
cmake --build build -j --target core_tests && ./build/core_tests -tc="barSyncPlayhead*"
```
Expected: PASS — all 4 `barSyncPlayhead: ...` cases green (`[doctest] Status: SUCCESS!`).

- [ ] **Step 5: Commit**

```bash
git add src/audio/BarSync.h tests/test_bar_sync.cpp CMakeLists.txt
git commit -m "feat(audio): barSyncPlayhead helper for bar-locked clip warp"
```

---

### Task 2: Wire `sync` + `length` into `AudioPlayerNode`

Append the two ports, branch `evaluate()` on sync, and update the status line. The node isn't in `core_tests` (FFmpeg decode), so verification is a full build + full `ctest` (nothing regresses) plus a manual smoke check.

**Files:**
- Modify: `src/modules/AudioPlayerNode.h` (class doc comment; `updateStatus` declaration)
- Modify: `src/modules/AudioPlayerNode.cpp` (include; two `addInput`s; `evaluate()` sync branch; `updateStatus` body)

- [ ] **Step 1: Add the two input ports**

In `src/modules/AudioPlayerNode.cpp`, the constructor currently reads:

```cpp
AudioPlayerNode::AudioPlayerNode() : Node("Audio File") {
    addAssetInput("file", AssetType::Audio);
    addInput("rate", PortType::Float,  1.0f, -2.0f, 2.0f);   // signed: negative = reverse
    addInput("play", PortType::Bool,   true);
    addInput("loop", PortType::Bool,   true);
    addOutput("left",  PortType::Audio);
    addOutput("right", PortType::Audio);
    outL_.assign(kAudioMaxBlock, 0.0f);
    outR_.assign(kAudioMaxBlock, 0.0f);
}
```

Insert the two new inputs immediately after the `loop` input (so they land at port indices 4 and 5, and the outputs stay after them):

```cpp
AudioPlayerNode::AudioPlayerNode() : Node("Audio File") {
    addAssetInput("file", AssetType::Audio);
    addInput("rate", PortType::Float,  1.0f, -2.0f, 2.0f);   // signed: negative = reverse
    addInput("play", PortType::Bool,   true);
    addInput("loop", PortType::Bool,   true);
    addInput("sync", PortType::Bool,   false);               // warp the clip to `length` bars
    addIntInput("length", 4, 1, 64);                         // bars to fit the clip into (whole)
    addOutput("left",  PortType::Audio);
    addOutput("right", PortType::Audio);
    outL_.assign(kAudioMaxBlock, 0.0f);
    outR_.assign(kAudioMaxBlock, 0.0f);
}
```

- [ ] **Step 2: Add the `BarSync.h` include**

At the top of `src/modules/AudioPlayerNode.cpp`, the includes currently are:

```cpp
#include "modules/AudioPlayerNode.h"
#include "core/Value.h"
#include "audio/AudioBlock.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
```

Add `#include "audio/BarSync.h"` after the `audio/AudioBlock.h` line:

```cpp
#include "modules/AudioPlayerNode.h"
#include "core/Value.h"
#include "audio/AudioBlock.h"
#include "audio/BarSync.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
```

- [ ] **Step 3: Branch `evaluate()` on sync**

In `src/modules/AudioPlayerNode.cpp`, read the two new inputs. After the existing:

```cpp
    bool  loop = ctx.in<bool>(3);
```

add:

```cpp
    bool  sync       = ctx.in<bool>(4);
    int   lengthBars = std::max(1, (int)std::lround(ctx.in<float>(5)));
```

Then replace the current advance/wrap block. It currently reads:

```cpp
    double prev = playhead_;
    if (play) playhead_ += (double)rate * (double)ctx.dt;

    bool wrapped = false;
    if (duration_ > 0.0) {
        if (loop) {
            if (playhead_ >= duration_ || playhead_ < 0.0) {
                playhead_ = std::fmod(playhead_, duration_);
                if (playhead_ < 0.0) playhead_ += duration_;
                wrapped = true;
            }
        } else {
            playhead_ = std::clamp(playhead_, 0.0, duration_);
        }
    } else if (playhead_ < 0.0) {
        playhead_ = 0.0;
    }

    // A wrap (or pause) makes the source slice discontinuous -> emit silence for
    // that one block rather than a swept glitch.
    double a0 = prev, a1 = playhead_;
    if (wrapped || !play) a1 = a0;
    emitAudio(ctx, a0, a1);
    updateStatus(play, rate);
```

Replace it with (the new `syncActive` branch first; the existing rate path is kept verbatim in the `else`):

```cpp
    double prev = playhead_;
    bool wrapped = false;
    bool syncActive = sync && ctx.transport && duration_ > 0.0;

    if (syncActive) {
        // Bar-locked: derive the playhead from the transport so the clip spans exactly
        // `lengthBars` bars, aligned to bar 1 and repeating every `lengthBars` bars.
        playhead_ = barSyncPlayhead(ctx.transport->bars(), lengthBars, duration_);
        if (playhead_ < prev) wrapped = true;    // crossed a length-bar seam (or a transport seek-back)
    } else {
        if (play) playhead_ += (double)rate * (double)ctx.dt;
        if (duration_ > 0.0) {
            if (loop) {
                if (playhead_ >= duration_ || playhead_ < 0.0) {
                    playhead_ = std::fmod(playhead_, duration_);
                    if (playhead_ < 0.0) playhead_ += duration_;
                    wrapped = true;
                }
            } else {
                playhead_ = std::clamp(playhead_, 0.0, duration_);
            }
        } else if (playhead_ < 0.0) {
            playhead_ = 0.0;
        }
    }

    // A wrap/seam (or pause) makes the source slice discontinuous -> emit silence for
    // that one block rather than a swept glitch.
    double a0 = prev, a1 = playhead_;
    if (wrapped || !play) a1 = a0;
    emitAudio(ctx, a0, a1);
    updateStatus(play, rate, syncActive, lengthBars);
```

- [ ] **Step 4: Update `updateStatus`**

In `src/modules/AudioPlayerNode.h`, the declaration currently reads:

```cpp
    void updateStatus(bool play, float rate);
```

Change it to:

```cpp
    void updateStatus(bool play, float rate, bool synced, int lengthBars);
```

In `src/modules/AudioPlayerNode.cpp`, the definition currently reads:

```cpp
void AudioPlayerNode::updateStatus(bool play, float rate) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%.1f / %.1f s  x%.2f%s",
                  playhead_, duration_, rate, play ? "" : " (paused)");
    status_ = buf;
}
```

Replace it with:

```cpp
void AudioPlayerNode::updateStatus(bool play, float rate, bool synced, int lengthBars) {
    char buf[96];
    if (synced)
        std::snprintf(buf, sizeof(buf), "%.1f / %.1f s  sync %d bar%s%s",
                      playhead_, duration_, lengthBars, lengthBars == 1 ? "" : "s",
                      play ? "" : " (paused)");
    else
        std::snprintf(buf, sizeof(buf), "%.1f / %.1f s  x%.2f%s",
                      playhead_, duration_, rate, play ? "" : " (paused)");
    status_ = buf;
}
```

- [ ] **Step 5: Update the class doc comment**

In `src/modules/AudioPlayerNode.h`, the class doc comment currently ends:

```cpp
// negative, reverse playback (it reads the clip backwards). `play` pauses; `loop`
// wraps at the ends. GL-free.
```

Change it to add a sentence about sync:

```cpp
// negative, reverse playback (it reads the clip backwards). `play` pauses; `loop`
// wraps at the ends. When `sync` is on, the clip is instead time-warped to span exactly
// `length` bars, bar-locked to the transport (via audio/BarSync.h); `rate` and `loop`
// are ignored while synced. GL-free.
```

- [ ] **Step 6: Build the whole app and run the full test suite**

Run:
```bash
cmake --build build -j && ctest --test-dir build --output-on-failure
```
Expected: the app links, and all tests pass (nothing regresses; the `barSyncPlayhead` tests from Task 1 stay green).

- [ ] **Step 7: Commit**

```bash
git add src/modules/AudioPlayerNode.h src/modules/AudioPlayerNode.cpp
git commit -m "feat(audio): Audio File player sync/length (warp to N bars)"
```

---

### Task 3: Documentation

Document the new inputs in `CLAUDE.md` (the relevant hard-rule bullet about `AudioFile`) and `README.md` (the Audio File node-table row).

**Files:**
- Modify: `CLAUDE.md`
- Modify: `README.md`

- [ ] **Step 1: Update the CLAUDE.md `AudioFile` bullet**

In `CLAUDE.md`, find the hard-rule bullet that begins "**`AudioFile` (`src/audio/AudioFile.{h,cpp}`)**". It currently ends:

```markdown
  forward / reverse / variable-rate — the audio analogue of the Video Player.
```

Append a sentence to that bullet:

```markdown
  forward / reverse / variable-rate — the audio analogue of the Video Player. A `sync`
  toggle instead **bar-locks** the clip to the transport, time-warping it to span exactly
  `length` bars (aligned to bar 1, repeating every `length` bars) via the GL-free
  `audio/BarSync.h` `barSyncPlayhead`; `rate`/`loop` are ignored while synced.
```

- [ ] **Step 2: Update the README.md Audio File row**

In `README.md`, find the table row that starts `| **Audio File** |`. It currently reads:

```markdown
| **Audio File** | play an audio file — mp3, wav, flac, m4a, ogg, … (any FFmpeg format), decoded to 48 kHz stereo → `left`/`right` mono outputs; signed `rate` (negative = reverse), variable speed, loop |
```

Replace it with:

```markdown
| **Audio File** | play an audio file — mp3, wav, flac, m4a, ogg, … (any FFmpeg format), decoded to 48 kHz stereo → `left`/`right` mono outputs; signed `rate` (negative = reverse), variable speed, loop. `sync` + `length` bar-lock the clip to the transport, time-warping it to span exactly `length` bars (`rate`/`loop` ignored while synced) |
```

- [ ] **Step 3: Commit**

```bash
git add CLAUDE.md README.md
git commit -m "docs: document Audio File player sync/length"
```

---

## Notes for the implementer

- **`src/audio/BarSync.h` is header-only** — only the `tests/test_bar_sync.cpp` line is added to `CMakeLists.txt`; no new `.cpp` in any source list. It must include only `<cmath>` (GL-free; `src/audio/` invariant).
- **C++ TDD "red" state** is a compile failure (Task 1 Step 2: the test includes a header that doesn't exist yet). Expected — proceed to write the header.
- **Do NOT** add `barSyncPlayhead` unit tests that construct `AudioPlayerNode` — that node pulls in the FFmpeg decode path and is not part of `core_tests`. Test only the helper. The node's integration is verified by the full build + `ctest` (no regressions) and a manual smoke check.
- **Ports are appended** at indices 4 (`sync`) and 5 (`length`); do not reorder the existing `file`/`rate`/`play`/`loop`/outputs — existing `.oss` projects reference ports by index.
- **Do not** `git add -A` / `git add .`. Stage only the files listed in each commit step. Leave the untracked `build.sh`, `preferences.oss`, `project.oss`, `examples/` alone.
- `ctx.transport` is a `const Transport*` (may be null); `transport->bars()` gives the absolute bar position (tempo-aware, wraps with the transport's own loop).
