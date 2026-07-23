# Spirograph Synth — Design

**Date:** 2026-07-23
**Status:** Approved

## Goal

A new audio module, **Spirograph Synth**, that generates stereo audio directly from
the parametric equations of Spirograph curves (hypotrochoid / epitrochoid). θ advances
at audio rate; the curve's **x coordinate → left channel** and **y coordinate → right
channel**. No visualization — audio comes straight from the math.

## Scope (v1)

A **single stereo voice with full control**. The following spec extensions are
explicitly **deferred** to future versions (the DSP class + node are the extension
points): the multi-spiral engine (2–3 mixed voices), feedback, and a built-in morphing
LFO. In this codebase the morph is achieved idiomatically by wiring the existing **LFO**
node into the `ratio` port, so no internal LFO is built.

## The math (audio-friendly, provably bounded)

Working in units of the inner-wheel radius (which cancels in normalization), with
`ratio = R/r` and `pen = d` as a fraction in `[0,1]`:

**Hypotrochoid** (`k = ratio − 1`):
```
x = (ratio − 1)·cos θ + pen·cos(k·θ)
y = (ratio − 1)·sin θ − pen·sin(k·θ)
```

**Epitrochoid** (`k = ratio + 1`):
```
x = (ratio + 1)·cos θ − pen·cos(k·θ)
y = (ratio + 1)·sin θ − pen·sin(k·θ)
```

Let `bigArm = |ratio ∓ 1|` (the `cos θ`/`sin θ` coefficient for the chosen curve). The
raw outputs are **normalized** by `(bigArm + pen)`:
```
outX = x / (bigArm + pen)
outY = y / (bigArm + pen)
```
By the triangle inequality `|outX|, |outY| ≤ 1` for all parameters — the output is
provably bounded, so **no clamp is needed** (same spirit as `StateVariableFilter`).
Finally both channels are scaled by `level`.

**Why it sounds good / earns the stereo:**
- `pen = 0` → pure sine, L = cos and R = sin (90° apart) → a perfect circle, wide clean
  stereo.
- Rising `pen` fades in the `k`-th partial → richer timbre + a Lissajous L/R figure.
- Rising `ratio` raises `k` → brighter partial.
- Non-integer ratios give evolving / inharmonic spectra — a feature for audio (the curve
  need not visually close).
- Hypo vs. epi place the partial on different harmonics (`k = ratio∓1`), so the curve-type
  choice is audibly distinct.

**Frequency / phase:**
- `freq` (Hz) sets how fast θ advances: `inc = 2π·freq / sampleRate` per sample. θ is a
  persisted accumulator (wrapped to `[0, 2π)`) so blocks join without clicks (like
  `SineWaveNode`).
- `phase` (turns, `[0,1]`) is a global θ offset applied as `θ_eff = θ + 2π·phase` — a
  starting-position control (audible mainly when modulated).

## Ports

| # | Input      | Type          | Range          | Default |
|---|------------|---------------|----------------|---------|
| 0 | curve type | choice        | Hypotrochoid / Epitrochoid | Hypotrochoid (0) |
| 1 | freq       | Float (Hz)    | 1.0 … 1000.0   | 110.0   |
| 2 | ratio (R/r)| Float         | 2.0 … 12.0     | 3.0     |
| 3 | pen (d)    | Float         | 0.0 … 1.0      | 0.5     |
| 4 | phase      | Float (turns) | 0.0 … 1.0      | 0.0     |
| 5 | level      | Float         | 0.0 … 1.0      | 0.8     |

**Outputs:** `0 = left` (Audio, x-waveform), `1 = right` (Audio, y-waveform).

Every control is a Float/choice input port, so each **is** a CV input — wire an LFO,
Automation, or any Float source into it. "Morphing cog ratio" = LFO node → `ratio`.

## Structure

- **`src/audio/Spirograph.h`** — GL-free DSP class `Spirograph` (header-only, mirroring
  the `SampleVoice`/`StateVariableFilter` header-only style). Holds params (curveType,
  freq, ratio, pen, phase, level via setters), the persisted θ accumulator, a
  `setSampleRate(int)`, and `process(float* outL, float* outR, int n)` that fills both
  channels with the normalized, bounded curve. This is the unit-tested core.
- **`src/modules/SpirographSynthNode.h`** — header-only node, editor label
  **"Spirograph Synth"** (Audio category). Owns one `Spirograph` + two
  `kAudioMaxBlock`-sized buffers. Each `evaluate`: read the ports → push to the voice →
  generate `audioBlockFrames(sampleRate, ctx.dt)` samples → publish two `AudioRef`s.
  No `saveState` — every parameter is a control-default input port, persisted
  automatically by `ProjectFile` (like `SineWaveNode`). The θ accumulator is transient
  runtime state.
- **Register** the node in **both** `makeNode()` and `nodeCategories()` (Audio category)
  in `src/app/Application.cpp`.

The spec's separate `SpirographSynth` wrapper is folded into the node for v1 (with one
voice, no feedback/LFO/mixing it would be an empty shell — YAGNI). It becomes the home
for the deferred multi-voice/feedback logic later.

## Tests

`core_tests` only — no `gl_smoke` (GL-free audio, like `AcidVoice`). New file
`tests/test_spirograph.cpp` (doctest), registered in `CMakeLists.txt`:

1. **Bounded output** — sweep `ratio` ∈ [2,12], `pen` ∈ [0,1], both curve types; assert
   every produced L/R sample is within `[-1, 1]` (with a tiny epsilon).
2. **pen = 0 ⇒ quadrature sine** — L ≈ cos, R ≈ sin; assert `L² + R²` ≈ constant across a
   block (a circle) and L/R are 90° apart.
3. **Phase continuity** — the sample at the end of block N and the first sample of block
   N+1 differ only by ~one `inc` step (no discontinuity at the block seam).
4. **Frequency correct** — measured zero-crossing period of L matches `sampleRate / freq`
   within tolerance for a couple of frequencies.
5. **Curve type matters** — hypo vs. epi with identical other params produce a
   measurably different waveform.

## Adding-a-node checklist (from CLAUDE.md)

1. Create `src/audio/Spirograph.h` + `src/modules/SpirographSynthNode.h`.
2. Register in `makeNode()` + `nodeCategories()` (Audio) in `Application.cpp`.
3. Add `tests/test_spirograph.cpp`; wire into `CMakeLists.txt` (`core_tests`).
4. Update `CLAUDE.md` (new node bullet) + `README.md`.

## Deferred (future extensions)

- **Multi-spiral engine** — 2–3 independent spirographs with per-voice ratio/pen/phase/
  detune + mix, summed to stereo. This is where a real `SpirographSynth` wrapper class
  earns its place.
- **Feedback** — phase-feedback FM (previous output → θ advance) or radius feedback
  (previous output → `pen`), kept bounded by the existing normalization.
- **Built-in morph LFO** — an internal sweep of `ratio` (range / rate / enable), if the
  external-LFO-node route proves insufficient.
