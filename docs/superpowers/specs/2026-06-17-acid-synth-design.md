# Acid Bass Synth Voice — Design

**Date:** 2026-06-17
**Status:** Approved (brainstorming) — ready for implementation plan

## Goal

A monophonic 303-style "acid bass" synth node, loosely based on the Behringer
TD-3-MO: MIDI in → mono audio out. Saw/square VCO + sub-oscillator → 4-pole
resonant low-pass filter with an envelope (decay, env-mod, accent) → VCA →
distortion. Plus note slide, filter FM (VCA output → cutoff), and filter key
tracking. Every control is an input port so it can be wired/automated.

## Architecture

Split DSP from node, mirroring `SignalGenerator`/`SineWaveNode`:

- **`src/audio/AcidVoice.{h,cpp}`** — GL-free, unit-testable monophonic voice. Owns
  all state; has `setSampleRate`, `noteOn`, `noteOff`, parameter setters, and
  `process(float* out, int n)`. No GL/ImGui. A small public `LadderFilter` struct
  lives in the header so the filter is testable on its own.
- **`src/modules/AcidNode.h`** — header-only node. MIDI-in + 12 control ports + mono
  `Audio` out. Each frame it folds MIDI events into `noteOn/noteOff`, pushes the
  control-port values into the voice, and `process()`es one block into an owned
  buffer published as an `AudioRef` (like `SineWaveNode`).

## Components

### `LadderFilter` (in `AcidVoice.h`, public, testable)

A compact 4-pole (24 dB/oct) resonant low-pass — the classic "simplified Moog"
cascade (Stilson/Smith). Stateful; reset() zeroes it. GL-free.

```cpp
struct LadderFilter {
    float s1=0, s2=0, s3=0, s4=0;   // stage outputs
    float d1=0, d2=0, d3=0, d4=0;   // one-sample delays
    void reset() { s1=s2=s3=s4=d1=d2=d3=d4=0.0f; }

    // One sample. cutoffHz clamped to (0, ~0.45*sr); res in [0,1] (self-oscillates
    // near 1). Returns the 4-pole low-pass output.
    float process(float in, float cutoffHz, float res, int sr) {
        float fc = cutoffHz / (0.5f * (float)sr);
        if (fc < 0.0f) fc = 0.0f;
        if (fc > 0.99f) fc = 0.99f;
        float f  = fc * 1.16f;
        float fb = res * 4.0f * (1.0f - 0.15f * f * f);
        float x  = in - s4 * fb;
        x *= 0.35013f * (f*f) * (f*f);
        s1 = x  + 0.3f*d1 + (1.0f - f)*s1;  d1 = x;
        s2 = s1 + 0.3f*d2 + (1.0f - f)*s2;  d2 = s1;
        s3 = s2 + 0.3f*d3 + (1.0f - f)*s3;  d3 = s2;
        s4 = s3 + 0.3f*d4 + (1.0f - f)*s4;  d4 = s3;
        return s4;
    }
};
```

### `AcidVoice` (in `AcidVoice.{h,cpp}`)

**Public interface:**

```cpp
class AcidVoice {
public:
    void setSampleRate(int sr);                       // default 48000
    void noteOn(int midiNote, int velocity, bool slide);
    void noteOff(int midiNote);

    // Per-block parameters (the node pushes these from its input ports each frame).
    void setWaveform(int w);          // 0 = saw, 1 = square
    void setCutoff(float hz);
    void setResonance(float r);       // 0..1
    void setEnvMod(float a);          // 0..1
    void setDecay(float seconds);
    void setAccent(float a);          // 0..1 (depth; scaled per-note by velocity)
    void setSubLevel(float a);        // 0..1
    void setSlideTime(float seconds);
    void setFilterFM(float a);        // 0..1
    void setKeyTrack(float a);        // 0..1
    void setDistortion(float a);      // 0..1

    void process(float* out, int n);  // fill n mono samples

    // Testing hooks:
    float currentFreq() const;        // oscillator frequency (for glide tests)
    float filtEnv() const;            // filter envelope value (0..1)
};
```

**State:** held-note stack (mono, last-note priority), `curNote_`, `curVel_`,
`gateOn_`, `gliding_`, oscillator `phase_`/`subPhase_`, `curFreq_`/`targetFreq_`,
`filtEnv_`, `ampEnv_`, `LadderFilter filter_`, `lastOut_` (for filter FM), and the
parameter fields. Per-sample coefficients (`decayCoef_`, `glideCoef_`, attack/release
coefs) recomputed when the relevant parameter or sample rate changes.

**`noteOn(note, vel, slide)`:**
- Push `note` onto the held stack (top = sounding); `curNote_ = note`,
  `curVel_ = vel`, `targetFreq_ = 440 * 2^((note-69)/12)`.
- If `slide` **and a note is already sounding**: keep `curFreq_` and glide toward
  `targetFreq_` (`gliding_ = true`); do **not** retrigger `filtEnv_` (legato, like a
  tied 303 note).
- Else: `curFreq_ = targetFreq_` (jump), `gliding_ = false`, `filtEnv_ = 1.0`
  (retrigger).
- `gateOn_ = true`.

**`noteOff(note):`** remove `note` from the stack. If the stack is empty,
`gateOn_ = false` (amp env releases; `filtEnv_` keeps decaying). Else set `curNote_`
/`targetFreq_` to the new top and jump to it (no retrigger), `gateOn_` stays true.

**`process(out, n)`** — per sample:
```
// glide
if (gliding_) { curFreq_ += (targetFreq_ - curFreq_) * glideCoef_;
                if (|targetFreq_ - curFreq_| < 0.01) { curFreq_ = targetFreq_; gliding_ = false; } }
// oscillators (naive; aliasing acceptable for v1)
phase_    += curFreq_ / sr;        wrap phase_    to [0,1)
subPhase_ += 0.5*curFreq_ / sr;    wrap subPhase_ to [0,1)
main = (waveform_==0) ? (2*phase_ - 1) : (phase_ < 0.5 ? 1 : -1);   // saw / square
sub  = (subPhase_ < 0.5 ? 1.0 : -1.0) * subLevel_;                  // square, 1 oct down
osc  = main + sub;
// envelopes
filtEnv_ *= decayCoef_;                                     // exponential decay to 0
accentAmt = accent_ * (curVel_ / 127.0);                    // velocity-scaled accent
ampTarget = gateOn_ ? (1.0 + 0.5*accentAmt) : 0.0;
ampEnv_  += (ampTarget - ampEnv_) * (ampTarget > ampEnv_ ? attackCoef_ : releaseCoef_);
// filter cutoff: base * key-track * 2^(env + accent + filterFM octaves)
keyF   = pow(2, keyTrack_ * (curNote_ - 60) / 12.0);
modOct = (envMod_ + accentAmt) * filtEnv_ * ENV_OCT + filterFM_ * lastOut_ * FM_OCT;
fcHz   = clamp(cutoff_ * keyF * pow(2, modOct), 20.0, 0.45*sr);
// filter + VCA + distortion (single tanh output stage = baseline saturation + drive)
filtered = filter_.process(osc, fcHz, resonance_, sr);
s        = filtered * ampEnv_;
lastOut_ = s;                                              // feeds filter FM next sample
out[i]   = tanh(s * (1.0 + distortion_ * 9.0));            // bounded to [-1,1]
```
Constants: `ENV_OCT = 4.0` (env opens the filter up to ~4 octaves), `FM_OCT = 2.0`
(filter-FM depth). `decayCoef_ = exp(-1/(decay_*sr))`,
`glideCoef_ = 1 - exp(-1/(slideTime_*sr))`, attack ≈ 3 ms, release ≈ 8 ms coefs.
Guard all params (decay/slideTime ≥ a small epsilon) to avoid div-by-zero.

The output `tanh` gives the "transistor" baseline saturation at `distortion = 0`
(`tanh(s) ≈ s` for small `s`) and progressively harder overdrive as `distortion`
rises — and bounds the output to `[-1, 1]` regardless of resonance/FM, which is the
stability backstop.

### `src/modules/AcidNode.h` (header-only node)

**Ports** (output index is separate, so all inputs are appended in order):

| # | Port | Type | Default | Range/labels |
|---|------|------|---------|--------------|
| 0 | `midi` | Midi | — | note → pitch+gate; velocity → accent |
| 1 | `waveform` | Float (choice) | 0 (Saw) | Saw, Square |
| 2 | `cutoff` | Float | 800 | 20 … 12000 (Hz) |
| 3 | `resonance` | Float | 0.7 | 0 … 1 |
| 4 | `env mod` | Float | 0.6 | 0 … 1 |
| 5 | `decay` | Float | 0.3 | 0.03 … 2.0 (s) |
| 6 | `accent` | Float | 0.4 | 0 … 1 |
| 7 | `sub level` | Float | 0.0 | 0 … 1 |
| 8 | `slide` | Float | 0.0 | 0 … 1 (≥0.5 = glide this note) |
| 9 | `slide time` | Float | 0.08 | 0.005 … 0.5 (s) |
| 10 | `filter FM` | Float | 0.0 | 0 … 1 |
| 11 | `key track` | Float | 0.0 | 0 … 1 |
| 12 | `distortion` | Float | 0.0 | 0 … 1 |
| out 0 | `audio` | Audio | — | mono |

`waveform` uses `addChoiceInput("waveform", {"Saw","Square"}, 0)` (the existing
choice-combo widget). The rest use `addInput(name, PortType::Float, default, lo, hi)`.

**`evaluate(ctx)`:**
- `bool slide = ctx.in<float>(8) >= 0.5f;`
- Fold MIDI: for each event in `ctx.in<MidiRef>(0)`, `midiIsNoteOn` →
  `voice_.noteOn(data1, data2, slide)`; `midiIsNoteOff` → `voice_.noteOff(data1)`.
- Push parameters: `voice_.setWaveform(round(in<float>(1)))`,
  `setCutoff(in<float>(2))`, `setResonance(in<float>(3))`, `setEnvMod(in<float>(4))`,
  `setDecay(in<float>(5))`, `setAccent(in<float>(6))`, `setSubLevel(in<float>(7))`,
  `setSlideTime(in<float>(9))`, `setFilterFM(in<float>(10))`,
  `setKeyTrack(in<float>(11))`, `setDistortion(in<float>(12))`.
- `int n = clamp(round(sampleRate_ * ctx.dt), 1, kMaxBlock);` (`kMaxBlock = 2048`),
  `voice_.process(buffer_.data(), n)`, output
  `AudioRef{buffer_.data(), (size_t)n, sampleRate_, 1}`.
- The constructor declares the ports, sets `voice_.setSampleRate(48000)`, and sizes
  the buffer (the node is GL-free — no `initGL` needed). The buffer is owned by the
  node (a `std::vector<float>(kMaxBlock)`), so the `AudioRef` stays valid until the
  next frame, exactly like `SineWaveNode`.

### Registration

- `src/app/Application.cpp`: `#include "modules/AcidNode.h"`; `makeNode`:
  `if (type == "Acid Bass") return std::make_unique<AcidNode>();`; add `"Acid Bass"`
  to the **Audio** `nodeCategories` list.

### CMake

Add `src/audio/AcidVoice.cpp` to `APP_SOURCES` and to the `core_tests` sources; add
`tests/test_acid_voice.cpp` to `core_tests`. (`AcidNode.h` is header-only; no
`gl_smoke` scenario needed — the voice is GL-free and fully covered in `core_tests`.)

## Data flow

```
MIDI note ─► noteOn(note, vel, slide) ─► pitch (glide if slide) + gate + accent(vel)
control ports ─► voice setters each frame
process(n):  VCO(saw/sq)+Sub ─► LadderFilter(cutoff·reso, modulated by env/accent/
             keytrack/filterFM) ─► VCA(amp env) ─► tanh distortion ─► AudioRef out
```

## Edge cases & stability

- Output is always `tanh(...)` → bounded to `[-1,1]`; resonance at max or filter FM at
  max cannot make it diverge. A **stability test** asserts finite, bounded output for
  an extreme-parameter sweep.
- `decay`/`slide time` are clamped to a small positive epsilon before forming
  coefficients (no div-by-zero, no `exp` overflow).
- `cutoff` is clamped to `[20, 0.45*sr]` after modulation, so the ladder's normalized
  `fc` stays in range.
- No held notes → `gateOn_` false → amp env releases to silence; `filtEnv_` keeps
  decaying. A note-off for a note not on the stack is ignored.
- `velocity == 0` note-on is treated as note-off by `midiIsNoteOn` (already false), so
  running-status note-offs work.

## Testing

GL-free doctest in `tests/test_acid_voice.cpp` (run under `core_tests`):

- **LadderFilter low-pass:** an 8 kHz sine through the filter at `cutoff = 200 Hz`
  has far lower RMS than at `cutoff = 18 kHz` (high frequencies attenuated).
- **LadderFilter resonance:** a sine at the cutoff frequency has higher output RMS at
  `res = 0.9` than at `res = 0.1` (resonant boost).
- **LadderFilter stability:** silence in at `res = 1.0` stays finite and bounded
  (`|out| < 10`) over a long run (no blow-up / NaN).
- **Voice envelope brightness:** with `envMod` high, low base `cutoff`, a `noteOn`
  then a held run — the first block's RMS exceeds a later block's RMS (the filter
  closes as `filtEnv` decays). Also `filtEnv()` ≈ `1/e` after `decay` seconds.
- **Voice glide:** `noteOn(48, …, slide=false)` → `currentFreq()` == `freq(48)`
  immediately; then `noteOn(60, …, slide=true)` → `currentFreq()` still near
  `freq(48)`, climbs through the gap while processing, and reaches `freq(60)` (within
  ~1%) after `slide time` of samples.
- **Voice accent:** a velocity-127 note yields higher output RMS than a velocity-40
  note (same params, fresh voices).
- **Voice distortion bound:** for any `distortion` (0 and 0.9) the output stays within
  `[-1, 1]`, and the `0.9` waveshape differs from `0.0`.
- **Stability sweep:** process several blocks at extremes (`cutoff` high, `res` 1,
  `filter FM` 1, `distortion` 1, while a note is held) — every sample finite and
  `|out| <= 1`.
- **Node level (`AcidNode`):** build an `EvalContext` (13 inputs), feed a `MidiRef`
  with one note-on → the `AudioRef` output is non-silent (some `|sample| > 1e-3`);
  feed a note-off and process more → the output decays toward silence.

## Docs

- **README.md** — add an **Acid Bass** row to the module table (303-style mono synth:
  MIDI in → audio; saw/square VCO + sub-osc, 4-pole resonant filter with
  envelope/accent, slide, filter FM, key tracking, distortion; all controls are
  input ports).
- **CLAUDE.md** — a brief bullet: `AcidNode` (`src/modules/`) wraps the GL-free
  `AcidVoice` DSP (`src/audio/AcidVoice.{h,cpp}`), the first MIDI-in → audio-out
  synth voice; output is a `tanh` stage so it's always bounded.
