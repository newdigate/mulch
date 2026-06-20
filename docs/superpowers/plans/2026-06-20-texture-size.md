# Streaming Texture Resolution Preference Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A Preferences **Video** tab that sets the streaming-texture (render FBO) resolution (320×240 … 1920×1080), applied live.

**Architecture:** `Preferences` gains `textureWidth/Height` (flowed to nodes via `EvalContext::prefs`, already plumbed). `ShaderNode`/`WireframeNode`/`ShadedRenderNode` recreate their FBO when the preferred size changes, falling back to `kCanvasW×kCanvasH` (1280×720) when `prefs == nullptr`. `Framebuffer::create` is made re-creation-safe.

**Tech Stack:** C++17, OpenGL 4.1, Dear ImGui, libsoundio/rtmidi (panel enumeration, unchanged), doctest, CMake. Design: `docs/superpowers/specs/2026-06-20-texture-size-design.md`.

**Note:** No new files and NO CMakeLists changes — every file touched is already built. `tests/test_preferences.cpp` is already in `core_tests`.

---

### Task 1: `Preferences` texture size + clamp

**Files:** Modify `src/core/Preferences.h`, `src/core/Preferences.cpp`, `tests/test_preferences.cpp`.

- [ ] **Step 1: Write the failing test (append to `tests/test_preferences.cpp`)**

```cpp
TEST_CASE("texture-size round-trips, defaults, and clamps on parse") {
    Preferences p; p.textureWidth = 640; p.textureHeight = 480;
    Preferences r;
    REQUIRE(parsePreferences(serializePreferences(p), r));
    CHECK(r.textureWidth == 640);
    CHECK(r.textureHeight == 480);

    Preferences d;                                   // no texture-size line -> 1280x720 default
    REQUIRE(parsePreferences("oss-prefs 1\n", d));
    CHECK(d.textureWidth == 1280);
    CHECK(d.textureHeight == 720);

    Preferences big;                                 // clamp high
    REQUIRE(parsePreferences("oss-prefs 1\ntexture-size 5000 5000\n", big));
    CHECK(big.textureWidth == 1920);
    CHECK(big.textureHeight == 1080);

    Preferences small;                               // clamp low
    REQUIRE(parsePreferences("oss-prefs 1\ntexture-size 0 0\n", small));
    CHECK(small.textureWidth == 320);
    CHECK(small.textureHeight == 240);
}

TEST_CASE("clampTextureSize bounds") {
    int w = 0, h = 0; clampTextureSize(w, h); CHECK(w == 320); CHECK(h == 240);
    w = 5000; h = 5000; clampTextureSize(w, h); CHECK(w == 1920); CHECK(h == 1080);
    w = 640;  h = 480;  clampTextureSize(w, h); CHECK(w == 640);  CHECK(h == 480);
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake -S . -B build && cmake --build build -j --target core_tests`
Expected: FAIL — `textureWidth` / `clampTextureSize` undeclared.

- [ ] **Step 3: Add the fields + helper to `src/core/Preferences.h`**

In `struct Preferences`, add after `enabledMidiOutputs;`:
```cpp
    int textureWidth  = 1280;    // streaming-texture (render FBO) resolution
    int textureHeight = 720;
```
And add a free-function declaration (after the `serialize/parsePreferences` declarations, before the closing `}` of `namespace oss`):
```cpp
// Clamp a requested streaming-texture size to sane bounds: [320,1920] x [240,1080].
void clampTextureSize(int& w, int& h);
```

- [ ] **Step 4: Implement in `src/core/Preferences.cpp`**

Add the helper (inside `namespace oss`, e.g. after the anonymous-namespace `setListed` block):
```cpp
void clampTextureSize(int& w, int& h) {
    if (w < 320)  w = 320;
    if (w > 1920) w = 1920;
    if (h < 240)  h = 240;
    if (h > 1080) h = 1080;
}
```
In `serializePreferences`, before `return out;`, add:
```cpp
    out += "texture-size " + std::to_string(p.textureWidth) + " " + std::to_string(p.textureHeight) + "\n";
```
In `parsePreferences`, add a keyword branch alongside the others (the `rest` string already holds the rest-of-line):
```cpp
        else if (kw == "texture-size") {
            std::istringstream rs(rest);
            int w = 0, h = 0;
            rs >> w >> h;
            if (!rs.fail()) { clampTextureSize(w, h); out.textureWidth = w; out.textureHeight = h; }
        }
```

- [ ] **Step 5: Run to verify it passes**

Run: `cmake --build build -j --target core_tests && ./build/core_tests`
Expected: PASS — all green, including the new texture-size + clamp cases.

- [ ] **Step 6: Commit**

```bash
git add src/core/Preferences.h src/core/Preferences.cpp tests/test_preferences.cpp
git commit -m "$(cat <<'EOF'
feat(core): add streaming-texture size to Preferences (clamped)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
)"
```

---

### Task 2: Re-creation-safe Framebuffer + live FBO resize

**Files:** Modify `src/gfx/Framebuffer.cpp`, `src/gfx/ShaderNode.cpp`, `src/modules/WireframeNode.cpp`, `src/modules/ShadedRenderNode.cpp`.

- [ ] **Step 1: Make `Framebuffer::create` re-creation-safe**

In `src/gfx/Framebuffer.cpp`, at the very top of `void Framebuffer::create(int w, int h, bool depth) {` (before `w_ = w; h_ = h;`), insert:
```cpp
    if (tex_)   { glDeleteTextures(1, &tex_);        tex_   = 0; }
    if (depth_) { glDeleteRenderbuffers(1, &depth_); depth_ = 0; }
    if (fbo_)   { glDeleteFramebuffers(1, &fbo_);    fbo_   = 0; }
```
(The rest of the body is unchanged; this lets `create()` be called again to resize without leaking.)

- [ ] **Step 2: `ShaderNode::render` resizes on change**

In `src/gfx/ShaderNode.cpp`, add `#include "core/Preferences.h"` near the top (`gfx/Canvas.h` is already included). Replace the body of `void ShaderNode::render(EvalContext& ctx) {` so it recreates the FBO when the preferred size differs:
```cpp
void ShaderNode::render(EvalContext& ctx) {
    int w = ctx.prefs ? ctx.prefs->textureWidth  : kCanvasW;
    int h = ctx.prefs ? ctx.prefs->textureHeight : kCanvasH;
    if (fbo_.width() != w || fbo_.height() != h) fbo_.create(w, h);
    fbo_.bind();
    glUseProgram(program_);
    setUniforms(ctx);
    fsq_.draw();
    Framebuffer::unbind();
    ctx.out<TexRef>(0, TexRef{ fbo_.texture(), fbo_.width(), fbo_.height() });
}
```

- [ ] **Step 3: `WireframeNode::evaluate` resizes on change**

In `src/modules/WireframeNode.cpp`, add `#include "core/Preferences.h"` near the top (`gfx/Canvas.h` already included). At the very top of `void WireframeNode::evaluate(EvalContext& ctx) {` (before any `fbo_.bind()` / rendering / reading geometry is fine — just put it first), insert:
```cpp
    int texW = ctx.prefs ? ctx.prefs->textureWidth  : kCanvasW;
    int texH = ctx.prefs ? ctx.prefs->textureHeight : kCanvasH;
    if (fbo_.width() != texW || fbo_.height() != texH) fbo_.create(texW, texH);
```

- [ ] **Step 4: `ShadedRenderNode::evaluate` resizes on change (with depth)**

In `src/modules/ShadedRenderNode.cpp`, add `#include "core/Preferences.h"` near the top (`gfx/Canvas.h` already included). At the very top of `void ShadedRenderNode::evaluate(EvalContext& ctx) {`, insert (note the `true` for the depth attachment, matching its `initGL`):
```cpp
    int texW = ctx.prefs ? ctx.prefs->textureWidth  : kCanvasW;
    int texH = ctx.prefs ? ctx.prefs->textureHeight : kCanvasH;
    if (fbo_.width() != texW || fbo_.height() != texH) fbo_.create(texW, texH, /*depth=*/true);
```

- [ ] **Step 5: Build + tests (gl_smoke is the regression guard)**

Run: `cmake --build build -j && ctest --test-dir build --output-on-failure`
Expected: clean build; ALL tests pass — crucially `gl_smoke` (its graphs run with `ctx.prefs == nullptr`, so every render FBO falls back to 1280×720 exactly as before; all the pixel-readback scenarios for Skybox/Compositor/Pitch-Graph/Wireframe/Deform stay valid). If `gl_smoke` fails, the fallback branch is wrong — the `ctx.prefs ? ... : kCanvas*` must yield 1280×720 when `prefs` is null.

Then `./build/shader_streamer --screenshot build/_ui.png` — exit 0 (the demo runs through `Application`, whose `prefs_` defaults to 1280×720, so the visuals are unchanged).

- [ ] **Step 6: Commit**

```bash
git add src/gfx/Framebuffer.cpp src/gfx/ShaderNode.cpp src/modules/WireframeNode.cpp src/modules/ShadedRenderNode.cpp
git commit -m "$(cat <<'EOF'
feat(gfx): render nodes recreate their FBO at the preferred texture size

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
)"
```

---

### Task 3: Preferences "Video" tab + docs

**Files:** Modify `src/ui/PreferencesPanel.cpp`, `README.md`, `CLAUDE.md`.

- [ ] **Step 1: Add the resolution preset list + Video tab**

In `src/ui/PreferencesPanel.cpp`, add a static preset table inside `namespace oss` (e.g. just above `void PreferencesPanel::draw(...)`):
```cpp
namespace {
struct Res { int w, h; const char* label; };
const Res kResolutions[] = {
    {320, 240, "320 x 240"}, {640, 480, "640 x 480"},
    {1280, 720, "1280 x 720"}, {1920, 1080, "1920 x 1080"},
};
}
```
Inside `draw(...)`, in the `BeginTabBar("prefs_tabs")` block, add a fourth tab AFTER the `MIDI` `EndTabItem()` and before `ImGui::EndTabBar();`:
```cpp
        if (ImGui::BeginTabItem("Video")) {
            int curW = prefs.textureWidth, curH = prefs.textureHeight;
            std::string cur = std::to_string(curW) + " x " + std::to_string(curH);
            for (const Res& r : kResolutions) if (r.w == curW && r.h == curH) cur = r.label;
            if (ImGui::BeginCombo("Streaming texture size", cur.c_str())) {
                for (const Res& r : kResolutions) {
                    bool sel = (r.w == curW && r.h == curH);
                    if (ImGui::Selectable(r.label, sel)) { prefs.textureWidth = r.w; prefs.textureHeight = r.h; onChange(); }
                }
                ImGui::EndCombo();
            }
            ImGui::EndTabItem();
        }
```

- [ ] **Step 2: Build + screenshot the Video tab**

Run: `cmake --build build -j && ctest --test-dir build --output-on-failure` — clean build, all pass.

To see the panel (hidden by default), TEMPORARILY set `bool showPreferences_ = true;` in `src/app/Application.h`, rebuild, and:
```bash
cmake --build build -j && ./build/shader_streamer --screenshot build/_ui.png
```
Open `build/_ui.png` with the Read tool: confirm the Preferences window now shows **four** tab headers — Audio Output / Audio Input / MIDI / **Video**. Report what you saw. Then **REVERT** `showPreferences_` to `false`, rebuild, and re-screenshot to confirm exit 0 (panel closed). The committed code MUST have `showPreferences_ = false`.

- [ ] **Step 3: README.md**

Find the **Preferences** subsection (added earlier) and extend it — add a sentence (match the surrounding prose):
```markdown
A **Video** tab sets the streaming-texture resolution (320×240, 640×480, 1280×720, or
1920×1080); the change applies live — every render-to-texture node recreates its framebuffer.
```

- [ ] **Step 4: CLAUDE.md**

Find the **Preferences** Architecture bullet and append to it (a sentence at the end of the bullet):
```markdown
  It also carries the streaming-texture resolution (`textureWidth/Height`): `ShaderNode`,
  `WireframeNode`, and `ShadedRenderNode` recreate their FBO when it changes (fallback
  `kCanvasW×kCanvasH` when `prefs` is null), and `gfx/Framebuffer::create` is re-creation-safe.
```

- [ ] **Step 5: Verify + commit**

Run: `ctest --test-dir build --output-on-failure`
Expected: all pass.

```bash
git add src/ui/PreferencesPanel.cpp README.md CLAUDE.md
git commit -m "$(cat <<'EOF'
feat(ui): Preferences Video tab for streaming-texture resolution

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
)"
```

---

## Final verification

- [ ] `cmake --build build -j` — clean build (`shader_streamer`, `core_tests`, `gl_smoke`)
- [ ] `ctest --test-dir build --output-on-failure` — all pass (Preferences texture-size + clamp; gl_smoke unchanged at 1280×720)
- [ ] `./build/shader_streamer --screenshot build/_ui.png` — exit 0; Preferences has a Video tab (verified via the temporary-open screenshot in Task 3)
- [ ] Manual: open Prefs → Video, pick 320×240 then 1920×1080, confirm the Output texture changes size; relaunch and confirm it persisted in `preferences.oss`
- [ ] Use superpowers:finishing-a-development-branch
</content>
