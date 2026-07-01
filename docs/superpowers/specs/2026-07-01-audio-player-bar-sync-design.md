# Audio File player — bar-sync (time-stretch to N bars) — design

**Date:** 2026-07-01
**Status:** Approved (brainstorm)
**Branch:** `feat/audio-player-bar-sync` (off `develop`)

## Goal

Add two inputs to the **Audio File** player (`AudioPlayerNode`):

- **`sync`** (Bool) — when **off**, the node behaves exactly as it does today. When **on**,
  the clip is time-warped so a full pass spans exactly `length` bars.
- **`length`** (Int, ≥ 1) — how many bars the clip is stretched to fill.

When synced, playback is **bar-locked to the global transport**: the playhead is derived from
the transport's absolute bar position, so the clip aligns to bar 1, repeats every `length`
bars, and stays locked through tempo changes, seeking, and transport loops — matching how
Step Seq / MIDI File / Drum Machine sync in this app. This is a resample/rate-based warp (pitch
shifts with speed, like the existing `rate` control) — **not** pitch-preserving.

## Decisions (from brainstorm)

- **Bar-locked, not free-running.** The synced playhead comes from `transport.bars()`, not
  from integrating `rate·dt`. It follows the transport's play/stop and survives BPM changes,
  seeks, and loops. (The rejected alternative — just changing the advance rate — was simpler
  but not anchored to the grid and would drift.)
- **`rate` and `loop` are ignored while synced.** Speed is fully determined by `length`; the
  length-bar wrap *is* the loop.
- **`play` still mutes** in both modes (unchanged rule: `play=false` → silence).
- **Resample-based** (pitch not preserved), consistent with the existing `rate` behavior.

## Architecture

`AudioPlayerNode` decodes via FFmpeg (`audio/AudioFile`), so it is **not** compiled into the
GL-free `core_tests` binary. To keep the new behavior unit-testable, the one piece of real math
is extracted into a tiny pure helper that the node calls; the helper is unit-tested, the node
stays thin.

| File | Change |
|---|---|
| `src/audio/BarSync.h` | **New, header-only, GL-free** (`<cmath>` only). `barSyncPlayhead(bars, lengthBars, durationSec)` → source-seconds position. |
| `src/modules/AudioPlayerNode.h` | Update the class doc comment (mention sync); no interface change beyond behavior. |
| `src/modules/AudioPlayerNode.cpp` | Two new input ports; sync branch in `evaluate()`; `updateStatus` shows the sync state. |
| `tests/test_bar_sync.cpp` | `core_tests` unit tests for `barSyncPlayhead`. |
| `CMakeLists.txt` | register `tests/test_bar_sync.cpp` in `core_tests`. |
| `CLAUDE.md`, `README.md` | docs. |

No `Value`, `Graph`, editor-rendering, or `.oss`-codec change — the new ports are ordinary
control-type inputs whose defaults persist through the existing project codec.

## Ports (appended — indices 0–3 unchanged)

Today: `0 file · 1 rate · 2 play · 3 loop` → `left`, `right`.

Add at the **end** (so existing `.oss` projects and connections keep their port indices, and
old projects load with `sync=false` = today's behavior):

```
4 sync    Bool,  default false
5 length  Int slider (addIntInput), default 4, range [1, 64]
```

`length` is stored as a float (per `addIntInput`) and rounded by the consumer.

## The helper — `src/audio/BarSync.h`

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
    return frac * durationSec;                   // → source seconds
}

} // namespace oss
```

## The `evaluate()` change — `src/modules/AudioPlayerNode.cpp`

Read the two new inputs after the existing four:

```cpp
bool  sync   = ctx.in<bool>(4);
int   lengthBars = std::max(1, (int)std::lround(ctx.in<float>(5)));
```

Replace the current advance/wrap block. When sync is active
(`sync && ctx.transport && duration_ > 0`), derive the playhead from the transport; otherwise
keep the existing rate-based path verbatim:

```cpp
double prev = playhead_;
bool wrapped = false;
bool syncActive = sync && ctx.transport && duration_ > 0.0;

if (syncActive) {
    playhead_ = barSyncPlayhead(ctx.transport->bars(), lengthBars, duration_);
    if (playhead_ < prev) wrapped = true;      // crossed a length-bar seam (or a transport seek-back)
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

double a0 = prev, a1 = playhead_;
if (wrapped || !play) a1 = a0;                 // seam/pause → one silent block (existing rule)
emitAudio(ctx, a0, a1);
updateStatus(play, rate, syncActive, lengthBars);
```

The existing `emitAudio` (slice `[a0,a1]` mapped across the output block with linear interp)
and the seam-silence rule are reused unchanged.

`updateStatus` gains two args and, when synced, shows e.g. `2.1 / 8.0 s  sync 4 bars`:

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

(The `updateStatus` declaration in `AudioPlayerNode.h` changes to
`void updateStatus(bool play, float rate, bool synced, int lengthBars);`.)

## Behavioral consequences (as chosen)

- **Follows the transport.** A paused/stopped transport freezes `bars()`, so `a1 == a0` →
  silence. Tempo changes, seeks, and transport loops all carry through because `bars()`
  already accounts for them.
- **`rate` and `loop` ignored while synced** — speed is set by `length`; the length-bar wrap
  is the loop.
- **`play` still mutes** in both modes.
- **Seam:** one silent block every `length` bars, identical to today's loop seam.
- **Mode-switch transient:** flipping `sync` on when the previous playhead was far from the
  synced position yields one silent seam block, then normal playback.

## Error / edge handling

- **No transport** (e.g. a bare unit test, or before `Graph::evaluate` wires it) or **no clip /
  `duration_ == 0`** → sync inactive; the node falls back to the normal rate path.
- **`length` rounding:** stored as float; `std::lround` then clamp to ≥ 1 (the port's `[1,64]`
  range already bounds the slider; the clamp guards an out-of-range automation/edge value).
- **Backward compatibility:** appended ports mean existing `.oss` projects load unchanged with
  `sync=false`; no codec version bump.

## Testing

- **`tests/test_bar_sync.cpp` (`core_tests`, GL-free):**
  - **Alignment + repeat:** `bars=0` → 0; `bars=length` → 0 (wrapped, start of the next
    window); `bars=length/2` → `duration/2`.
  - **Stretch/linearity:** within one window the result is linear in `bars` with slope
    `duration/length` — verify for `length=4` (e.g. `bars=1` → `duration/4`, `bars=3` →
    `3·duration/4`) and for `length=1` (e.g. `bars=0.25` → `duration/4`).
  - **Repeat across windows:** `bars=length+1` gives the same value as `bars=1`.
  - **Guards:** `lengthBars < 1` → 0; `durationSec <= 0` → 0.
- **Node integration** (ports appear, `sync` warps to `length` bars, wrap→silence seam,
  `rate`/`loop` ignored while synced, transport-follow) requires an FFmpeg-decoded clip and so
  isn't in `core_tests` → manual smoke check in the app: load a clip, toggle `sync`, set
  `length`, confirm a full pass fits `length` bars and locks to bar 1; start/stop the transport
  and confirm it follows.

## Out of scope (YAGNI — flag to pull in)

Pitch-preserving time-stretch; fractional/beat `length`; a start-offset or clip-region while
synced; per-clip warp markers; reverse while synced; applying the same sync to the Video
player.

## Decided defaults (flag to change)

- `sync` default **false** (preserves today's behavior); `length` default **4**, range `[1,64]`.
- Bar-locked (transport-derived) position; `rate`/`loop` ignored while synced; `play` still mutes.
- Ports appended at indices 4–5; no `.oss` codec change.
- One new GL-free header (`audio/BarSync.h`) holding the testable math; the node stays thin.
