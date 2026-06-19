# Acid Bass `level` Output Gain — Design

**Date:** 2026-06-18
**Status:** Approved (brainstorming) — ready for implementation plan

## Goal

Add a `level` (output volume) input to the **Acid Bass** synth — it is loud by default.
A post-distortion gain, range `0.0–1.0`, default **0.7** (so existing patches get
noticeably quieter out of the box; push to `1.0` for the old loudness, `0.0` to mute).

## Architecture (two small changes)

The gain lives in the GL-free `AcidVoice` DSP (so it's unit-tested in `core_tests`); the
node exposes it as one more input port, mirroring every other Acid Bass control.

### `src/audio/AcidVoice.{h,cpp}` — output gain

- Add a member `float level_ = 0.7f;` (alongside the other parameters).
- Add a setter mirroring the existing ones, clamped to `[0,1]`:
  ```cpp
  void setLevel(float a) { level_ = clamp01(a); }
  ```
- In `AcidVoice.cpp`'s `process()`, scale **only the published output sample** by
  `level_`. The current final line is:
  ```cpp
  lastOut_ = std::tanh(s);   // bounded VCA output -> stable filter-FM feedback
  out[i]   = std::tanh(s * (1.0f + distortion_ * 9.0f));
  ```
  becomes:
  ```cpp
  lastOut_ = std::tanh(s);   // bounded VCA output -> stable filter-FM feedback (unscaled)
  out[i]   = level_ * std::tanh(s * (1.0f + distortion_ * 9.0f));
  ```
  `lastOut_` is the filter-FM feedback tap and must stay unscaled, so `level` is a pure
  output trim that does not change the voice's internal dynamics, distortion character,
  or filter-FM behaviour — it only attenuates the final mono output.

### `src/modules/AcidNode.h` — the port

- Add input 13 `level` after `distortion` (input 12):
  ```cpp
  addInput("level", PortType::Float, 0.7f, 0.0f, 1.0f);   // output volume
  ```
- In `evaluate()`, after the other setters: `voice_.setLevel(ctx.in<float>(13));`
- Update the node's input-list doc comment to add `13 = level`.

No new files, no CMake change, no registration change (the node is already registered).

## Edge cases

- `level` outside `[0,1]` → clamped by `setLevel` (consistent with the other params).
- `level = 0` → silence (the block is all zeros).
- `level` does not touch `lastOut_`, so filter-FM and the distortion stage are
  unaffected — turning the level down doesn't clean up or change the tone, only the
  volume.
- Default `0.7` changes the loudness of existing Acid Bass patches (intended).

## Testing

Extend `tests/test_acid_voice.cpp` (GL-free, `core_tests`):

- **Level scales the output linearly.** Drive a voice with a held note; render a block at
  `setLevel(1.0)` capturing peak amplitude `p1`, then (a fresh, identically-driven voice)
  at `setLevel(0.5)` capturing `p2`; assert `p2 ≈ 0.5 * p1` (within a small tolerance).
- **Level 0 is silent.** With `setLevel(0.0)`, every sample in the rendered block is `0`.
- **Default is 0.7.** A freshly constructed `AcidVoice` (no `setLevel` call) renders a
  held note whose peak ≈ `0.7 * p1` (the `level=1.0` peak from the first test) — pinning
  the default gain value, not just that the setter and default agree.

Use a fresh `AcidVoice` per level (the filter/envelope are stateful) and the same note /
parameters / sample count so the only difference is the gain.

## Docs

- **README.md** — the **Acid Bass** module row lists its signal chain and ends "Every
  control is an input port"; add `level` to the listed controls (e.g. "... distortion,
  and output `level`").
- **CLAUDE.md** — the Acid Bass bullet describes the chain generically ("... → VCA → a
  `tanh` distortion stage ..."); append "→ output `level`" to that chain so the bullet
  stays accurate.
</content>
