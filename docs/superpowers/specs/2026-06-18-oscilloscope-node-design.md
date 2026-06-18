# Oscilloscope Node — Design

**Date:** 2026-06-18
**Status:** Approved (brainstorming) — ready for implementation plan

## Goal

A node that turns an audio signal into an oscilloscope trace as streamed geometry: you
plug audio in, and it publishes a `VertexRef` (a line strip of vec3 positions) on a
`geometry` output that you wire into the **Wireframe** node (the "view framebuffer"),
which renders it through its 3D camera into a texture.

Two display modes, switchable at runtime:
- **Waveform** — amplitude vs. time, mono (L+R downmixed), with an optional rising
  zero-crossing **trigger** so a steady tone stands still instead of sliding/jittering.
- **X-Y (vectorscope)** — a Lissajous plot of the stereo field: L drives x, R drives y.

## Architecture (two units, mirroring the codebase's DSP/node split)

All the trace math is GL-free and unit-testable; the node only does the rolling audio
history + the VBO upload. This mirrors `SpectrographNode` (`src/modules/`) +
`audio/FFT` (`src/audio/`, GL-free): an audio→geometry visualizer whose math is tested
in `core_tests`.

### `src/core/Oscilloscope.{h,cpp}` — vertex builder (GL-free)

```cpp
namespace oss {

enum class ScopeMode { Waveform, XY };

// Fill `out` with `pointCount` vec3 positions (x,y,z) describing the scope trace as a
// line strip. Inputs are rolling sample histories ordered oldest..newest, length n
// (mono sources pass the same pointer for histL and histR). `windowSamples` is how many
// of the most-recent samples the trace spans; it is clamped to [2, n] internally.
//
//  - Waveform: signal = 0.5*(L+R). The window is the most-recent `windowSamples`. If
//    `trigger`, the window start is shifted back to the latest rising zero-crossing
//    (s[i-1] < 0 <= s[i]) found within a one-window search range, so a steady tone is
//    phase-stable; if none is found it falls back to the free-running window. The window
//    is resampled to `pointCount` points by linear interpolation:
//        x = -1 + 2*j/(pointCount-1),  y = sample*gain,  z = 0.
//  - XY: `trigger` is ignored. The most-recent `windowSamples` (L,R) pairs are resampled
//    to `pointCount` points:  x = L*gain,  y = R*gain,  z = 0.
//
// out is resized to pointCount*3. With n < 2 it is filled with zeros (a flat trace).
void buildScopeVertices(const float* histL, const float* histR, std::size_t n,
                        int windowSamples, int pointCount, ScopeMode mode,
                        bool trigger, float gain, std::vector<float>& out);

} // namespace oss
```

**Trigger algorithm (waveform mode):** let `s` be the downmixed signal, length `n`.
The free-running window starts at `start0 = n - windowSamples`. When `trigger` is on,
search indices `i` in `[max(1, start0 - windowSamples), start0]` (a one-window look-back)
for the **latest** rising zero-crossing `s[i-1] < 0 && s[i] >= 0`, and use that `i` as
the window start. If none is found, use `start0`. This keeps the search bounded and the
window always has `windowSamples` samples ahead of `start`.

**Resampling:** for output point `j` in `[0, pointCount)`, sample position
`p = start + (windowSamples-1) * j/(pointCount-1)`; linearly interpolate between
`s[floor(p)]` and `s[ceil(p)]`. (X-Y interpolates L and R the same way.) `pointCount`
is fixed by the node (512).

### `src/modules/OscilloscopeNode.h` — the node (header-only)

Owns two rolling history buffers (`histL_`, `histR_`, each `kHistory = 16384` floats),
an internal `SignalGenerator` fallback for when `audio` is unconnected (so the node shows
a live trace standalone, like Spectrograph), and one VBO. It does no shading and has no
FBO, so it derives directly from `Node` (not `ShaderNode`), allocates its VBO in
`initGL()`, and frees it in the destructor.

**Per-frame `evaluate(ctx)`:**
- Read `audio` (input 0). Determine `sr` = connected source rate or the generator's
  48000; `ch` = source channels or 1; `frames` = `a.frames()` or 0.
- Advance the rolling history by `adv` samples (`adv = clamp(frames, 1, kHistory)` when
  connected, else `clamp(round(sr*dt), 1, kHistory)`), `std::move`-shifting `histL_`/
  `histR_` left and filling the new tail:
  - connected: for each new frame, `histL_tail = a.samples[f*ch]`,
    `histR_tail = (ch==2) ? a.samples[f*2+1] : a.samples[f*ch]`.
  - unconnected/underrun: generate `adv` mono samples into a scratch buffer and copy to
    both `histL_` and `histR_` tails.
- `windowSamples = clamp(round(ctx.in<float>(3)/1000 * sr), 2, kHistory/2)` (input 3 = `window` ms).
- `mode = (round(ctx.in<float>(1)) == 1) ? ScopeMode::XY : ScopeMode::Waveform;`
- `buildScopeVertices(histL_.data(), histR_.data(), kHistory, windowSamples, kPoints,`
  `mode, ctx.in<bool>(2), ctx.in<float>(4), verts_);`
- Upload `verts_` to the VBO (`glBufferData(..., GL_DYNAMIC_DRAW)`).
- `ctx.out<VertexRef>(0, VertexRef{vbo_, kPoints, Primitive::LineStrip, VertexFormat::Pos3});`

`statusLine()` returns `"waveform · NN ms"` or `"X-Y"` (the window ms rounded).

`kPoints = 512` (fixed vertex count). `kHistory/2` cap on `windowSamples` guarantees the
trigger look-back stays in bounds.

### Ports

| # | Port | Type | Default | Range | Notes |
|---|------|------|---------|-------|-------|
| 0 | `audio` | Audio | `AudioRef{}` | — | unconnected → internal synth |
| 1 | `mode` | Float (choice) | 0 (`Waveform`) | {Waveform, X-Y} | `addChoiceInput("mode", {"Waveform","X-Y"}, 0)` |
| 2 | `trigger` | Bool | true | — | rising zero-crossing lock (waveform mode only) |
| 3 | `window` | Float | 20 | 1–100 | milliseconds of signal the trace spans |
| 4 | `gain` | Float | 1.0 | 0.1–4.0 | vertical / amplitude scale |
| out 0 | `geometry` | Vertex | — | — | line strip → wire into **Wireframe** |

### Registration / CMake

- `src/app/Application.cpp`: `#include "modules/OscilloscopeNode.h"`; in `makeNode`,
  `if (type == "Oscilloscope") return std::make_unique<OscilloscopeNode>();`; add
  `"Oscilloscope"` to the **Audio** `nodeCategories` list (next to `"Spectrograph"`).
- `src/main.cpp`: add an `Oscilloscope` node to the `--screenshot` demo graph.
- `CMakeLists.txt`: add `src/core/Oscilloscope.cpp` to `APP_SOURCES` and to `core_tests`;
  add `tests/test_oscilloscope.cpp` to `core_tests`. (`OscilloscopeNode.h` is
  header-only.)

## Data flow

```
audio ─► rolling histL_/histR_ (per-frame ingest, kHistory samples)
          └─► buildScopeVertices(windowSamples, pointCount, mode, trigger, gain)
                 waveform: 0.5*(L+R), trigger-aligned window, resampled to 512 pts
                 X-Y:      (L,R) pairs → (x,y), resampled to 512 pts
          └─► verts_ ─► VBO ─► VertexRef{vbo, 512, LineStrip, Pos3}
                 └─► Wireframe (geometry in) ─► texture out
```

## Edge cases

- **No audio connected** (`a.samples == nullptr`) → internal `SignalGenerator` fills the
  history so the trace is live; X-Y of a mono signal (L==R) reads as a diagonal line.
- **Upstream underrun** (`frames < adv`) → top up the tail from the generator (mirrors
  Spectrograph).
- **No trigger found** in the search range → free-running window (no special-casing
  downstream; the trace simply isn't phase-locked that frame).
- **`window` too large for the rate** → `windowSamples` clamped to `kHistory/2`.
- **`n < 2`** in the builder → `out` filled with zeros (flat trace; defensive, the node
  always passes `kHistory`).
- **`transport == nullptr`** is irrelevant — the scope is not transport-synced.
- VBO lifetime: `verts_` is a node member re-uploaded each frame; the published
  `VertexRef` names the node-owned VBO, valid until the node's next `evaluate`, matching
  Spectrograph's geometry output and how `Graph::evaluate` consumes it within the frame.

## Testing

GL-free doctest under `core_tests` — `tests/test_oscilloscope.cpp` drives
`buildScopeVertices` directly (no GL):

- **Free-running waveform:** feed a known mono ramp (`histL == histR`), `trigger=false`,
  assert `out.size() == kPoints*3`, x spans exactly `[-1, 1]` (first/last vertices),
  and y at the endpoints equals the corresponding window samples × gain.
- **Trigger aligns to a rising zero-crossing:** feed a sine; with `trigger=true` assert
  the first vertex's y ≈ 0 and the second vertex's y > 0 (rising). Build the trace twice
  from histories that differ only by a sub-period phase shift of the *newest* sample and
  assert the two output traces match closely (phase stability) — the defining scope
  property.
- **Trigger fallback:** an all-positive signal (no zero-crossing) with `trigger=true`
  yields the same trace as `trigger=false` (free-running window).
- **X-Y mapping:** feed `histL = sine`, `histR = cosine`, mode `XY`; assert sampled
  vertices satisfy `x == L*gain` and `y == R*gain`, and that flipping `trigger` does not
  change the X-Y output (trigger ignored in X-Y).
- **Gain scales y:** doubling `gain` doubles every y (waveform) / every x,y (X-Y).

Optionally a `gl_smoke` scenario building `Oscilloscope → Wireframe`, rendering into the
hidden window and asserting non-background pixels (the trace drew). Not required for
correctness — the geometry math is fully covered above — but a cheap integration check.

## Docs

- **README.md** — add an **Oscilloscope** row to the module table (audio → an
  oscilloscope trace as geometry; Waveform/X-Y modes, rising-edge trigger, `window` ms,
  `gain`; wire `geometry` into **Wireframe**).
- **CLAUDE.md** — a brief Architecture bullet: `OscilloscopeNode`
  (`src/modules/OscilloscopeNode.h`, header-only) wraps a GL-free `buildScopeVertices`
  (`src/core/Oscilloscope.{h,cpp}`) that turns a rolling audio history into a line-strip
  VBO — a triggered (rising zero-crossing) waveform or an X-Y vectorscope — published as
  a `VertexRef` to wire into the Wireframe renderer.
</content>
</invoke>
