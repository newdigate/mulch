# Image Sequencer — cross-fade — design

**Date:** 2026-07-02
**Status:** Approved (brainstorm)
**Branch:** `feat/image-sequencer-crossfade` (off `develop`)

## Goal

Add a **`fade duration`** input to the Image Sequencer that cross-dissolves from the outgoing
image to the incoming one over that many seconds at each image change (0 = instant, today's
behavior).

## Decisions (from brainstorm)

- **`fade duration` is a Float in seconds**, appended as **port 4** (after `sync`). Default **0**
  → instant swap (unchanged behavior). This is **additive / backward-compatible**: existing
  sequencer projects (4 ports) load with fade 0.
- **Cap the fade at the image interval.** The effective fade is `min(fade duration, interval)`,
  where `interval` = `duration` seconds (free-run) or `beat length × transport.secondsPerBeat()`
  (synced). So a fade always completes before the next change; setting `fade duration ≥ interval`
  gives a **continuous dissolve**. This keeps the node's two-image buffer sufficient.
- **The node becomes a renderer.** Blending two textures into one output requires a shader pass,
  so the node gains its own `Framebuffer` + `FullscreenPass` + a crossfade program (the
  `ShaderNode`/`Compositor` pattern, inline). Its output becomes the canvas-sized FBO texture. It
  renders every frame; when not fading it outputs `mix(shown, shown, 0)` = the current image, so
  the downstream output-texture id is stable.
- **Linear cross-dissolve driven by real `dt`** (looks the same free-run or synced). The blend is
  `mix(from, to, m)` with `m` ramping 0→1.

## Architecture

| File | Change |
|---|---|
| `src/core/ImageSequence.h` | Add GL-free `crossfadeMix(elapsed, fadeDur, interval)` (cap + clamp). |
| `shaders/crossfade.frag` | **New.** `mix(texture(uFrom,uv), texture(uTo,uv), uMix)`. |
| `src/modules/ImageSequencerNode.h` | Add the `fade duration` input; own an FBO + `FullscreenPass` + crossfade program; fade state machine; render the blend as output. |
| `tests/test_image_sequence.cpp` | `crossfadeMix` unit tests. |
| `tests/gl_smoke.cpp` | Update the port-flag probe to 5 ports; add a fade sub-check (mid-fade output is a red↔green blend, then resolves to the incoming colour). |
| `CLAUDE.md`, `README.md` | Document the fade. |

No CMake changes (no new source files — the node is header-only, `crossfade.frag` ships via the
existing `shaders/` directory copy). `Framebuffer`/`FullscreenPass`/`GLUtil` are already linked
into `shader_streamer` and `gl_smoke`.

## Component detail

### `crossfadeMix` (GL-free, `core/ImageSequence.h`)

```cpp
// The cross-fade blend factor (0 = from, 1 = to). Caps the effective fade at `interval` so a
// fade never outlasts the gap between images; fadeDur <= 0 -> 1 (instant). Clamped to [0,1].
inline float crossfadeMix(float elapsed, float fadeDur, float interval) {
    float eff = fadeDur < interval ? fadeDur : interval;   // cap to the image interval
    if (eff <= 0.0f) return 1.0f;
    float m = elapsed / eff;
    return m < 0.0f ? 0.0f : (m > 1.0f ? 1.0f : m);
}
```

### Node changes

New input (before the output port): `addInput("fade duration", PortType::Float, 0.0f, 0.0f, 10.0f)`
→ `fade duration` is port 4. Output `image` stays port 0.

New GL members: `Framebuffer fbo_`, `FullscreenPass fsq_`, `GLuint fadeProg_`. New fade state:
`bool fading_`, `float fadeElapsed_`, `int fadeToIndex_`. `initGL` compiles `fadeProg_` from an
inline fullscreen vertex shader + `readFile("shaders/crossfade.frag")` and creates the FBO + quad;
the dtor deletes `fadeProg_` and the two image textures.

`evaluate` (extends the async-prefetch flow):
1. Read inputs incl. `fadeDuration = ctx.in<float>(4)`.
2. Folder change: rescan + reset (incl. `fading_ = false`, `fadeElapsed_ = 0`), sync-load image 0.
3. `n == 0` → empty `TexRef`, return (no render).
4. Poll the background decode (unchanged).
5. Compute `target` (free-run accumulate / `syncedImageIndex`), and `interval` seconds
   (`sync ? beatLen * (transport ? secondsPerBeat() : 0.5) : duration`).
6. **Fade progression / start:**
   - If `fading_`: `fadeElapsed_ += dt`; `m = crossfadeMix(fadeElapsed_, fadeDuration, interval)`;
     if `m >= 1` complete → swap `texShown_`↔`texNext_` (+dims), `shownIndex_ = fadeToIndex_`,
     `nextReady_ = false`, `fading_ = false`.
   - Else if `target != shownIndex_ && nextReady_ && nextIndex_ == target`: if
     `min(fadeDuration, interval) > 0` start a fade (`fading_ = true`, `fadeElapsed_ = 0`,
     `fadeToIndex_ = target`, `m = 0`); otherwise **instant swap** (as today).
   - Else `m = 0` (showing current, no transition ready).
7. **Prefetch only when not fading** (both textures are in use during a fade): `want =
   (shownIndex_ == target) ? (target+1)%n : target`; launch when idle + not `failedIndex_`.
8. **Render:** bind `fbo_` (resize to prefs texture size, recreation-safe), `glUseProgram(fadeProg_)`,
   bind `uFrom = texShown_` (unit 0), `uTo = (fading_ ? texNext_ : texShown_)` (unit 1),
   `uMix = fading_ ? m : 0`, `fsq_.draw()`, unbind. Output `TexRef{ fbo_.texture(), fbo_.width(),
   fbo_.height() }`.
9. `statusLine` unchanged (`<i+1>/<N>  basename`).

Because the cap guarantees a fade finishes within `interval`, `target` can't advance again mid-fade,
so `fadeToIndex_` stays valid; prefetch of the following image resumes the moment the fade completes.

## Data flow

Same as before, but the output is now a rendered blend rather than the raw image texture. Downstream
nodes (Kaleidoscope, Compositor, Output) sample it by UV exactly as before — no change for them.

## Error handling

- `fade duration = 0` (default): `crossfadeMix` returns 1 immediately → instant swap; identical to
  the pre-fade behavior (plus a 1:1 FBO blit).
- No images / undecodable image: unchanged (empty `TexRef` / keep current); a fade only starts when
  the incoming image is decoded and ready.
- Resolution change: the FBO recreates from `prefs->textureWidth/Height` (fallback `kCanvasW/H`),
  like every other renderer.
- GL objects (FBO, quad VAO, program, two textures) are the node's, freed in the dtor with the
  editor context current (two-context rule).

## Testing

- **`core_tests`**: `crossfadeMix` — `(0,1,2)=0`, `(0.5,1,2)=0.5`, `(1,1,2)=1`, `(2,1,2)=1` (clamp),
  `(1.5,3,2)=0.75` (cap to interval), `(0.5,0,2)=1` (instant).
- **`gl_smoke`**: update the sequencer scenario's port-flag probe to `inputs().size()==5`. Add a fade
  check: 3 solid PNGs, `duration=1`, `fade duration=0.5`, `sync` off; step `evaluate(0.02f)` frames
  and observe the output centre — assert that at some frame it is a **blend** (both R and G present,
  B low) during the red→green transition, and that it later reaches **pure green**. The existing
  instant-mode assertions (fade defaulting off in the other checks) remain valid.

## Out of scope (YAGNI)

- Non-linear fade curves (ease in/out), wipe/slide/other transitions, per-image fade times,
  fades longer than the interval (triple buffering), fading the very first image in from black.
