# Drum Machine node — design

**Date:** 2026-06-27
**Status:** Approved (brainstorm)
**Branch:** `feat/drum-machine` (off `develop`)

## Goal

A self-contained sample-based drum machine node: pick four audio samples, sequence them
on a 4×16 tri-state step grid, and mix the triggered voices to a stereo audio output.
Eight pattern slots hold independent grids. It mirrors the existing **Step Seq** clock and
the **Chord Player** preset/persistence conventions, and stays GL-free.

## Output & topology

Self-contained audio: the node plays the four samples itself and mixes them to a `left`/`right`
mono pair (the codebase's stereo convention — wire into **Audio Out**, **Audio Mix**, **Recorder**,
etc.). Each sample voice is panned into the stereo field via the existing GL-free
`core/AudioPan.h` `panGains(pan)`. No external trigger output.

## The kit vs. the patterns

- **Kit (module-level, shared across all patterns):** the four samples and their per-sample
  `volume` / `rate` / `pan`. These are input *ports* (so they're automatable) — not stored per
  pattern.
- **Pattern (8 slots):** each pattern holds *only* the 4×16 tri-state step grid (64 cells). The
  active pattern is the playback source of truth; the grid edits it in place. All 8 are saved
  with the project.

## Ports

Per sample `i` (0..3), a 4-port block at base `4*i`:

| Port | Index | Type | Default | Range |
|---|---|---|---|---|
| `file i` | `4i+0` | String | `""` | path; async-decoded |
| `vol i` | `4i+1` | Float | `0.8` | `0..1` |
| `rate i` | `4i+2` | Float | `1.0` | `-4..4` (signed: <0 reverses) |
| `pan i` | `4i+3` | Float | `0.0` | `-1..1` |

Globals after the 16 sample-block ports:

| Port | Index | Type | Default | Notes |
|---|---|---|---|---|
| `tempo` | 16 | Float | `120` | BPM, free mode (`40..240`) |
| `sync` | 17 | Bool | `false` | lock to transport (mirrors Step Seq default) |
| `rate sync` | 18 | Choice | `1/16` (idx 4) | `stepDivisionLabels()` from `core/StepSync.h` |
| `pattern` | 19 | Float | `1` | active slot `1..8` (automatable; mirrors button selection) |

Outputs: `left` (0), `right` (1), both `PortType::Audio`.

## Tri-state grid + 8 patterns

### New generic Node hook (in `core/Node.h`)

Alongside the existing button-bank, a grid hook the editor renders:

```cpp
virtual int         gridRows() const { return 0; }            // 0 = no grid
virtual int         gridCols() const { return 0; }
virtual int         gridCell(int r, int c) const { return 0; } // 0=off, 1=on, 2=accent
virtual void        onGridCellPressed(int r, int c) {}         // cycle off->on->accent->off
virtual std::string gridRowLabel(int r) const { return {}; }   // optional (sample name)
```

### Editor rendering (`ui/NodeEditorPanel.cpp`)

After the button-bank block, if `gridRows()>0` draw a `gridRows()×gridCols()` grid of small
square buttons (one row per sample, optional `gridRowLabel(r)` prefix). Cell colour by state:
**off = dark**, **on = mid**, **accent = brightest**. A click calls `onGridCellPressed(r,c)`.
Use scoped ImGui ids (`##grid_r_c`) like the button-bank to avoid id collisions.

### Pattern store (in the node)

- `struct Pattern { uint8_t cell[4][16]; };` — values 0/1/2. `Pattern patterns_[8]`, `int active_`.
- `gridCell` reads `patterns_[active_]`; `onGridCellPressed` cycles that cell `(+1)%3`.
- Switching: the 8 buttons set `active_` directly (immediate). The `pattern` Float port is
  **edge-detected** — `active_` adopts it only when its value *changes* (vs. the last frame), so a
  constant default never overrides button clicks, while a connected/automated `pattern` input drives
  the slot live. The button hook's `buttonActive()` reflects `active_`. No quantize boundary.
- `saveState`/`loadState`: `"<active>;<p0>;...;<p7>"`, each pattern 64 chars of `0/1/2` row-major
  (4 rows × 16). No spaces/`;`. Paths/vol/rate/pan persist separately as control defaults (`ins`/`inf`).
- Pattern 1 ships with a default kick on row 0 at steps 0/4/8/12 (four-on-the-floor); the rest empty.

## Sequencing + voice DSP

### Clock (reuses Step Seq logic, `core/StepSync.h`)

- **sync on:** the current 16th-step is derived statelessly from `transport.bars()` at
  `stepDivisionBars(rate sync)` — loop/stop-robust, bar-aligned. One step fires per frame at its
  boundary (same as Step Seq); paused/stopped fires nothing.
- **sync off:** a free clock advanced by `ctx.dt` at `tempo` (16th notes), stepping `0..15` and
  wrapping.

### Per-step trigger

On a step boundary, for column `c = step%16`, for each row `r` whose `patterns_[active_].cell[r][c] != 0`,
`(re)trigger` voice `r` from sample 0 with an `accent` flag (`cell == 2`). Each voice is one-shot +
monophonic (retrigger restarts it); up to 4 sound at once.

### `audio/SampleVoice.h` (GL-free, header-only)

One monophonic sample voice over an `AudioClip`:

- State: `double pos` (source frame), `bool active`.
- `trigger(const AudioClip&, bool reverse)`: `active = true`; `pos = reverse ? frames-1 : 0`.
- `render(const AudioClip& clip, double rate, float gL, float gR, float* outL, float* outR, int n)`:
  for each of `n` out frames while `active`: read the clip **downmixed to mono** at `pos` with
  linear interpolation; `outL[i] += s*gL; outR[i] += s*gR`; `pos += rate`; if `pos` leaves
  `[0, frames)` set `active = false` (one-shot, no loop). `rate == 0` or no clip → inactive/no-op.

### Mixing (in the node's `evaluate`)

- `n = audioBlockFrames(48000, ctx.dt)`; clear `outL_`/`outR_` (size `kAudioMaxBlock`).
- For each voice `r`: gain `g = vol_r * (voice_r.accent ? 1.0 : 0.75)` — where `voice_r.accent` is
  the flag **stored on the voice at its last trigger** (so a hit keeps its accent level for its whole
  life), while `vol_r`/`rate_r`/`pan_r` are read live from the ports each frame; `panGains(pan_r) ->
  (pL,pR)`; `voice.render(clip_r, rate_r, g*pL, g*pR, outL_, outR_, n)`.
- Emit `AudioRef{outL_, n, 48000}` and `{outR_, n, 48000}`.

> The accent boost is applied **at trigger** (stored on the voice), so the kit `vol`/`pan` ports
> still modulate live, but accent level is fixed per hit. `vol`/`rate`/`pan` are read each frame.

### Sample loading

Four `AsyncLoader<AudioClip>` (one per slot), keyed on `file i` — identical to `AudioPlayerNode`:
worker-thread `decodeAudioFile`, swap in the clip when ready. An empty path or a not-yet-loaded /
failed clip → that voice produces silence (never triggers). `statusLine()` shows loaded/among-N
state + active pattern (e.g. `P1 • 3/4 loaded`).

## Code structure / files

| File | Responsibility |
|---|---|
| `core/Node.h` | + grid hook virtuals (modify) |
| `core/DrumPattern.h` | **new** — `Pattern` + 8-slot store + tri-state codec (`encode`/`decode`); GL-free, unit-tested |
| `audio/SampleVoice.h` | **new** — one monophonic one-shot sample voice; GL-free, unit-tested |
| `modules/DrumMachineNode.{h,cpp}` | **new** — ports, 4× `AsyncLoader<AudioClip>`, pattern store, 4 voices, sequencing, grid+button hooks, save/load, status |
| `ui/NodeEditorPanel.cpp` | + tri-state grid rendering (modify) |
| `app/Application.cpp` | register `"Drum Machine"` in `makeNode()` + the **Audio** category (modify) |
| `tests/test_drum_machine.cpp` | **new** — `core_tests` |
| `CLAUDE.md`, `README.md` | docs (modify) |

`DrumPattern` and `SampleVoice` are GL-free and independently testable; the node orchestrates them.
The node is `.h`+`.cpp` (the `evaluate` body + loaders are substantial), mirroring `AudioPlayerNode`.

## Data flow (per frame)

1. Read `pattern` port → update `active_` if changed (and reflect button presses).
2. Capture grid edits (the grid hook already wrote into `patterns_[active_]`).
3. Compute the current step (sync from `transport.bars()`, or free from `tempo`+`dt`).
4. On a step boundary: trigger voices for the column's on/accent rows (store per-voice accent gain).
5. Pump each `AsyncLoader`; swap in any newly-decoded clip.
6. Render + mix the 4 voices into `outL_`/`outR_` for `n` frames; emit both `AudioRef`s.

## Error handling

- Missing/empty path or failed decode → voice never triggers (silence); `statusLine` reflects the
  loaded count.
- Clip shorter than the block / playhead past the end → voice deactivates mid-block (one-shot).
- `rate == 0` → no advance, treated as inactive (no infinite hold).
- Out-of-range `pattern`/`vol`/`pan`/`rate` → clamped.
- `loadState` with malformed text → clamp per cell, ignore extra; never throws.

## Testing (`core_tests`)

- **DrumPattern**: set/cycle cells; `encode`→`decode` round-trips all 8 grids + active index;
  malformed decode clamps.
- **SampleVoice**: a known clip triggered renders expected samples; `rate=2` halves duration;
  reverse plays end→0; deactivates at clip end; retrigger restarts.
- **Sequencing/mix** (node-level, GL-free): with a synthetic clip in a slot and a one-cell pattern,
  evaluating across the step boundary produces non-zero output on the expected channel; an accent
  cell is louder than an on cell; pan routes to the expected side.

(No `gl_smoke` scenario — the node is GL-free.)

## Out of scope (YAGNI)

- Per-pattern kits, pattern chaining/song mode, MIDI `select` input, swing, per-step velocity beyond
  the three states, choke groups, sample start/end trim, a file-browser dialog (paths are typed
  strings, as elsewhere), and a MIDI trigger output.

## Decided defaults (flag to change)

- Node label **"Drum Machine"**, **Audio** category.
- Accent = louder: normal hit `0.75 × vol`, accent `1.0 × vol`.
- `sync` defaults **off** (mirrors Step Seq); `rate sync` = 1/16.
- Each voice downmixes its (possibly stereo) sample to mono, then pans — so `pan` is meaningful for
  every sample regardless of its channel count.
