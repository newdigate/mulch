# 3-Band Crossover Filter node — design

**Date:** 2026-07-01
**Status:** Approved (brainstorm)
**Branch:** `feat/crossover-filter` (off `develop`)

## Goal

A new **Crossover Filter** audio node: one mono audio input split into **bass / mid /
treble** mono outputs by two cascaded resonant crossovers. Each crossover exposes its
cutoff frequency and resonance as Float input ports.

## Decisions (from brainstorm)

- **Topology — cascaded crossover.** A state-variable filter at the *low cutoff* splits
  the input into `bass` (its lowpass) and a remainder (its highpass); a second
  state-variable filter at the *high cutoff* splits that remainder into `mid` (lowpass)
  and `treble` (highpass). The three bands cover the spectrum continuously (no gap), and
  at low resonance they sum to *approximately* the input.
- **Resonance per crossover.** Two resonance inputs — one for the low crossover, one for
  the high — so each split's emphasis is independent.
- **Musical, not mastering-grade.** This is a creative filter. Resonance intentionally
  peaks each crossover frequency. We do **not** claim sample-exact / phase-flat
  reconstruction (a plain 2-pole crossover has a mild dip/ring at the crossover, growing
  with resonance) — that is expected and desired here.

## Architecture

Two new header-only, GL-free files. No change to `Value`, `Graph`, the editor, or
persistence — this is an ordinary stateless-*ports* audio node (its only state is the
live filter memory, which is not saved).

| File | Change |
|---|---|
| `src/audio/StateVariableFilter.h` | **New, header-only, GL-free.** A TPT state-variable filter yielding lowpass/bandpass/highpass simultaneously from a cutoff + resonance. |
| `src/modules/CrossoverFilterNode.h` | **New, header-only, GL-free.** The node: two `StateVariableFilter` members wired as a cascade + three output buffers. |
| `src/app/Application.cpp` | Register `"Crossover Filter"` in `makeNode()` and in the `"Audio"` category of `nodeCategories()`; add the `#include`. |
| `tests/test_state_variable_filter.cpp` | `core_tests`: filter frequency-response + stability. |
| `tests/test_crossover_filter.cpp` | `core_tests`: node band-split energy + I/O shape. |
| `tests/CMakeLists.txt` | Register the two new test sources in `core_tests`. |
| `CLAUDE.md`, `README.md` | Docs. |

## The filter primitive — `src/audio/StateVariableFilter.h`

The classic 2-pole TPT (topology-preserving-transform) state-variable filter — the
Zavalishin / Cytomic form. It is unconditionally stable at any resonance (no `tanh`
clamp needed, unlike the ladder), and its whole point is that it produces the lowpass,
bandpass **and** highpass taps from one shared cutoff + resonance — exactly what a
crossover needs (the existing `LadderFilter` in `audio/AcidVoice.h` is lowpass-only and
so cannot form the highpass side of a split).

```cpp
#pragma once
#include <cmath>

namespace oss {

// One evaluation of the filter yields all three band taps from the same state.
struct SvfOut { float low, band, high; };

// 2-pole topology-preserving-transform state-variable filter (Zavalishin/Cytomic).
// Coefficients are set per control-block from cutoff (Hz) + resonance (0..1); process()
// runs per audio sample. Integrator state persists across calls (and thus across frames).
// GL-free. Unconditionally stable for all cutoff/resonance in range.
struct StateVariableFilter {
    float ic1 = 0.0f, ic2 = 0.0f;                          // integrator memory
    float g = 0.0f, k = 2.0f, a1 = 0.0f, a2 = 0.0f, a3 = 0.0f;   // derived coefficients

    void reset() { ic1 = ic2 = 0.0f; }

    // fc in Hz (clamped to [20, 0.45*sr]); res in [0,1] (clamped). res maps to the
    // damping k = 1/Q: res 0 -> k=2 (Q=0.5, gentle), res 1 -> k~=0.02 (Q~=50, sharp peak).
    void setCoefficients(float fc, float res, int sr) {
        float ny = 0.45f * (float)sr;
        fc  = fc  < 20.0f ? 20.0f : (fc  > ny  ? ny  : fc);
        res = res < 0.0f  ? 0.0f  : (res > 1.0f ? 1.0f : res);
        g  = std::tan(3.14159265f * fc / (float)sr);
        k  = 2.0f - 1.98f * res;
        a1 = 1.0f / (1.0f + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
    }

    SvfOut process(float in) {
        float v3 = in - ic2;
        float v1 = a1 * ic1 + a2 * v3;
        float v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2.0f * v1 - ic1;
        ic2 = 2.0f * v2 - ic2;
        return { v2, v1, in - k * v1 - v2 };   // low, band, high
    }
};

} // namespace oss
```

## The node — `src/modules/CrossoverFilterNode.h`

```cpp
#pragma once
#include <algorithm>
#include <cstddef>
#include <vector>
#include "core/Node.h"
#include "core/Value.h"
#include "audio/AudioBlock.h"
#include "audio/StateVariableFilter.h"

namespace oss {

// 3-band crossover filter: one mono input split into bass / mid / treble mono outputs
// by two cascaded state-variable crossovers. Float inputs: low cutoff + its resonance
// (bass | rest split), high cutoff + its resonance (mid | treble split). GL-free;
// filter state persists across per-frame blocks like a real-time filter.
class CrossoverFilterNode : public Node {
public:
    CrossoverFilterNode()
        : Node("Crossover Filter"),
          bass_(kAudioMaxBlock, 0.0f), mid_(kAudioMaxBlock, 0.0f), treble_(kAudioMaxBlock, 0.0f) {
        addInput("mono",           PortType::Audio, AudioRef{});
        addInput("low cutoff",     PortType::Float, 200.0f,  20.0f, 20000.0f);
        addInput("low resonance",  PortType::Float, 0.2f,    0.0f,  1.0f);
        addInput("high cutoff",    PortType::Float, 2000.0f, 20.0f, 20000.0f);
        addInput("high resonance", PortType::Float, 0.2f,    0.0f,  1.0f);
        addOutput("bass",   PortType::Audio);
        addOutput("mid",    PortType::Audio);
        addOutput("treble", PortType::Audio);
    }

    void evaluate(EvalContext& ctx) override {
        AudioRef m = ctx.in<AudioRef>(0);
        std::size_t n = m.samples ? std::min(m.count, (std::size_t)kAudioMaxBlock) : 0;
        int sr = (m.samples && m.sampleRate > 0) ? m.sampleRate : 48000;

        low_.setCoefficients (ctx.in<float>(1), ctx.in<float>(2), sr);   // low cutoff / low res
        high_.setCoefficients(ctx.in<float>(3), ctx.in<float>(4), sr);   // high cutoff / high res

        for (std::size_t i = 0; i < n; ++i) {
            SvfOut a = low_.process(m.samples[i]);   // split at the low cutoff
            bass_[i] = a.low;                         // below low cutoff
            SvfOut b = high_.process(a.high);         // split the remainder at the high cutoff
            mid_[i]    = b.low;                        // between the two cutoffs
            treble_[i] = b.high;                       // above high cutoff
        }

        ctx.out<AudioRef>(0, AudioRef{bass_.data(),   n, sr});
        ctx.out<AudioRef>(1, AudioRef{mid_.data(),    n, sr});
        ctx.out<AudioRef>(2, AudioRef{treble_.data(), n, sr});
    }

private:
    StateVariableFilter low_, high_;
    std::vector<float> bass_, mid_, treble_;
};

} // namespace oss
```

**Port order** keeps each control by its meaning:
`mono · low cutoff · low resonance · high cutoff · high resonance` →
`bass · mid · treble`.

## Data flow

```
mono in ─► [SVF @ low cutoff, low res] ─ low ─────────────────────────► bass
                                        └ high ─► [SVF @ high cutoff, high res] ─ low  ─► mid
                                                                                └ high ─► treble
```

Both SVFs' integrator state lives in the node's two members and persists across frames,
so the filters run as continuous real-time filters even though `evaluate()` is called
once per (variable-length) audio block. Coefficients are recomputed once per block from
the current control-rate cutoff/resonance values.

## Error / edge handling

- **Cutoff clamping:** each cutoff is clamped to `[20 Hz, 0.45·sr]` inside
  `setCoefficients` so `tan()` stays well-conditioned below Nyquist.
- **Cutoff ordering is not forced.** The two cutoffs are independent; if `low > high`
  the mid band simply narrows (or empties) — a valid creative outcome, not an error.
- **No input connected:** `m.samples == nullptr` → `n = 0`; three empty `AudioRef`s are
  emitted (`count == 0`), matching `MonoToStereoNode`. Filter state is untouched.
- **Sample rate:** taken from the input `AudioRef` (fallback 48000). Coefficients are
  recomputed every block, so a sample-rate change is absorbed automatically.
- **Stability:** the TPT SVF is unconditionally stable across the full cutoff/resonance
  range, so no output clamp is applied (the bands are meant to sum back toward the input;
  clamping would distort that). Downstream mixers already clamp when summing to output.

## Testing

- **`tests/test_state_variable_filter.cpp` (`core_tests`, GL-free):**
  - *DC:* feed a constant `1.0` for enough samples to settle; `low` → ≈ `1.0`,
    `high` → ≈ `0.0`.
  - *Below cutoff:* a sine well below cutoff → `low` RMS ≈ input RMS, `high` RMS ≪ input.
  - *Above cutoff:* a sine well above cutoff → `high` RMS ≈ input RMS, `low` RMS ≪ input.
  - *Stability:* with `res = 1.0`, drive an impulse then white-ish noise for many samples
    and assert every `low/band/high` sample is finite and bounded (e.g. `|x| < 100`).
- **`tests/test_crossover_filter.cpp` (`core_tests`, GL-free):** drive the node with the
  `EvalContext{in, out, dt}` pattern from `tests/test_audio_bridges.cpp`:
  - A **low-frequency** sine (e.g. 80 Hz) → `bass` carries most energy, `treble` almost
    none (compare summed squared samples over a settled block).
  - A **high-frequency** sine (e.g. 8 kHz) → `treble` carries most energy, `bass` almost
    none.
  - **I/O shape:** all three outputs are `AudioRef`s with `count == input.count` and the
    input's `sampleRate`.
  - **Unconnected input:** an `AudioRef{}` (null) input → all three outputs `count == 0`.
- **No GL / no `gl_smoke`** needed — the node is header-only and GL-free.

## Out of scope (YAGNI — flag to pull in)

Filter-slope selection (12/24 dB per octave, Linkwitz-Riley); per-band gain / mute /
solo; a fourth band; stereo processing (stays mono-in/mono-out per the app's mono-edge
convention — use two nodes for a stereo pair); an in-editor frequency-response curve;
persisting anything (no non-port state to save).

## Decided defaults (flag to change)

- Two SVF crossovers cascaded (bass = LP@low; mid = LP@high of HP@low; treble = HP@high
  of HP@low).
- Defaults: low cutoff 200 Hz, high cutoff 2000 Hz, both resonances 0.2.
- Resonance→damping map `k = 2 - 1.98·res` (Q from ≈0.5 to ≈50).
- Cutoff range `[20, 20000]` at the port, clamped to `[20, 0.45·sr]` in the filter.
- One new GL-free primitive (`core`-adjacent, in `audio/`) + one header-only node; no
  persistence / `.oss` / editor change.
