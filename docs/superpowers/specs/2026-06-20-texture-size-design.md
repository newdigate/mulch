# Streaming Texture Resolution Preference — Design

**Date:** 2026-06-20
**Status:** Approved (brainstorming) — ready for implementation plan

## Goal

A **Video** tab in Preferences that sets the resolution of the streaming textures (the
render-to-texture FBOs), from 320×240 up to 1920×1080. The change applies **live** — every
render-to-texture node recreates its framebuffer at the new size. Persisted in
`preferences.oss`; falls back to the current 1280×720 when no preferences are present.

## Architecture

The streaming textures are the FBOs created at the global `kCanvasW×kCanvasH` constant
(`gfx/Canvas.h`, 1280×720) in three places: `ShaderNode` (all shader texture nodes),
`WireframeNode`, and `ShadedRenderNode`. The resolution becomes a `Preferences` field flowed
to nodes via `EvalContext::prefs` (like the device settings). Each node recreates its FBO when
the preferred size changes; `Framebuffer::create` is made re-creation-safe.

### Unit 1 — `core/Preferences` gains a texture size

Add to `struct Preferences` (`core/Preferences.h`):
```cpp
    int textureWidth  = 1280;    // streaming-texture (render FBO) resolution
    int textureHeight = 720;
```
- `serializePreferences` writes one line `texture-size <w> <h>` (always; it is always meaningful).
- `parsePreferences` reads `texture-size <w> <h>` and clamps via a new free helper
  `void clampTextureSize(int& w, int& h);` to `[320,1920] × [240,1080]` (so a hand-edited or
  corrupt file can't make a degenerate FBO). A missing line leaves the 1280×720 default.

### Unit 2 — `gfx/Framebuffer::create` is re-creation-safe

At the top of `Framebuffer::create(w, h, depth)`, delete any existing GL objects before
regenerating (today a second call leaks `tex_`/`fbo_`/`depth_`):
```cpp
void Framebuffer::create(int w, int h, bool depth) {
    if (tex_)   { glDeleteTextures(1, &tex_);       tex_   = 0; }
    if (depth_) { glDeleteRenderbuffers(1, &depth_); depth_ = 0; }
    if (fbo_)   { glDeleteFramebuffers(1, &fbo_);    fbo_   = 0; }
    // ... existing creation body ...
}
```
No signature change; the destructor is unaffected.

### Unit 3 — Render-to-texture nodes resize live

Each node reads the wanted size from `ctx.prefs` (fallback to `kCanvasW/kCanvasH` when
`ctx.prefs == nullptr`) and recreates its FBO when it differs from the current one, before
binding/rendering. The published `TexRef` then carries the new `w/h` automatically.

- **`gfx/ShaderNode::render(ctx)`** — at the top, before `fbo_.bind()`:
  ```cpp
  int w = ctx.prefs ? ctx.prefs->textureWidth  : kCanvasW;
  int h = ctx.prefs ? ctx.prefs->textureHeight : kCanvasH;
  if (fbo_.width() != w || fbo_.height() != h) fbo_.create(w, h);   // no depth
  ```
  (covers Colour, Mix, Compositor, Spectrograph, Skybox, and every other `ShaderNode`).
- **`modules/WireframeNode::evaluate(ctx)`** — same check at the top, `fbo_.create(w, h)` (no depth).
- **`modules/ShadedRenderNode::evaluate(ctx)`** — same check, `fbo_.create(w, h, /*depth=*/true)`.

`initGL` still creates the FBO at `kCanvasW/kCanvasH`; the first `render`/`evaluate` recreates it
to the preferred size if different. `ShaderNode::render` needs `gfx/Canvas.h` + `core/Preferences.h`;
the two module `.cpp`s already include `gfx/Canvas.h` and add `core/Preferences.h`.

### Unit 4 — PreferencesPanel "Video" tab

Add a fourth tab **Video** with a resolution combo. A small static preset list (in
`PreferencesPanel.cpp`):
```cpp
struct Res { int w, h; const char* label; };
static const Res kResolutions[] = {
    {320, 240, "320 x 240"}, {640, 480, "640 x 480"},
    {1280, 720, "1280 x 720"}, {1920, 1080, "1920 x 1080"},
};
```
The combo's current label is the entry matching `prefs.textureWidth/Height` (or
`"<w> x <h>"` if it's a non-preset custom size). Selecting an entry sets
`prefs.textureWidth/textureHeight` and calls `onChange()` (which persists).

`Application` already defaults `prefs_` (1280×720 via the struct) and persists on change — no new
app wiring beyond what Preferences already has.

## Data flow

```
Preferences panel (Video tab) → prefs_.textureWidth/Height → (save preferences.oss)
                              → EvalContext.prefs → each render node: recreate FBO on size change
                              → new-size TexRef → downstream (Mix/Compositor/Output/Recorder)
No prefs (gl_smoke / Application-less graph) → ctx.prefs == nullptr → kCanvasW×kCanvasH (1280×720)
```

## Edge cases

- **`ctx.prefs == nullptr`** (headless `gl_smoke`, tests) → 1280×720, identical to today → no
  pixel-readback regressions.
- **Corrupt/hand-edited size** → clamped to `[320,1920]×[240,1080]` on parse; a 0 or huge value
  can't create a broken FBO.
- **Resize while recording** (Recorder) → the encoder was opened at the size recording started;
  the Recorder's existing `vin.w == encW_ && vin.h == encH_` guard skips mismatched frames until
  you stop/restart. (Don't change resolution mid-record.)
- **Per-frame cost** → an int compare per render node; `Framebuffer::create` runs only on an
  actual size change.
- **All shader FBOs share one size** → multi-input blends (Mix/Compositor) stay aligned.
- **Video Player / data textures** are NOT canvas-sized (native video size / FFT bins) → unchanged.

## Testing

- **`tests/test_preferences.cpp`** (extend, `core_tests`):
  - Round-trip including `texture-size` (e.g. set 640×480 → serialize → parse → 640×480).
  - `clampTextureSize`: `(0,0)` → `(320,240)`; `(5000,5000)` → `(1920,1080)`; `(640,480)` unchanged.
  - A parsed `texture-size 5000 5000` line comes back clamped to 1920×1080; a file with no
    `texture-size` line leaves the 1280×720 default.
- **Build + manual:** the live FBO resize needs a GL context (verified by a clean build + a
  manual run: pick a resolution, the output texture changes size). `gl_smoke`'s fallback to
  1280×720 (prefs null) confirms no regression; the Video tab renders in the `--screenshot` check.

## Docs

- **README.md** — extend the Preferences note: a **Video** tab sets the streaming-texture
  resolution (320×240 … 1920×1080), applied live.
- **CLAUDE.md** — extend the Preferences bullet: `Preferences` also carries the streaming-texture
  resolution (`textureWidth/Height`); `ShaderNode`/`Wireframe`/`ShadedRender` recreate their FBO
  when it changes (fallback `kCanvasW×kCanvasH` when `prefs` is null); `Framebuffer::create` is
  re-creation-safe.
</content>
