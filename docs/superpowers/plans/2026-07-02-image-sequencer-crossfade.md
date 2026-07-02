# Image Sequencer — cross-fade — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a `fade duration` input (seconds) to the Image Sequencer that cross-dissolves from the outgoing image to the incoming one over that time at each image change (0 = instant).

**Architecture:** A GL-free `crossfadeMix` computes the blend factor (capped at the image interval). The node gains its own `Framebuffer` + `FullscreenPass` + a `shaders/crossfade.frag` program (the `ShaderNode`/`Compositor` pattern, inline) and renders `mix(from, to, m)` as its output; a small fade state machine drives `m` over the async double-buffer it already keeps.

**Tech Stack:** C++17, OpenGL 4.1, `gfx/{Framebuffer,FullscreenPass,GLUtil}` (already linked into `shader_streamer` + `gl_smoke`), doctest, headless GL.

**Reference spec:** `docs/superpowers/specs/2026-07-02-image-sequencer-crossfade-design.md`

**Conventions (CLAUDE.md):**
- `src/core/` stays GL-free (`crossfadeMix` is pure math). GL lives in the node/shader.
- Conventional Commits. Branch `feat/image-sequencer-crossfade` (already created, off `develop`).
- Never `git add -A`/`git add .` — stage only the files each step names. Leave `build.sh`, `examples/`, `preferences.oss`, `project.oss`, `imgui.ini` untracked.
- Build: `cmake --build build --target <t> -j`; tests: `ctest --test-dir build --output-on-failure`.

**Context:** `src/modules/ImageSequencerNode.h` currently has ports `folder`(0)/`duration`(1)/`beat length`(2)/`sync`(3), an async `std::future<ImageData>` prefetch into two textures (`texShown_`/`texNext_`), a `failedIndex_` churn-guard, and outputs `texShown_` directly. This plan appends `fade duration` at **port 4** (additive/backward-compatible) and makes the node render its output through an FBO so it can cross-dissolve. `Framebuffer::create` is recreation-safe; `FullscreenPass` draws a fullscreen triangle; `linkProgram(vs,fs)`/`readFile(path)` are in `gfx/GLUtil.h`; `kCanvasW/kCanvasH` in `gfx/Canvas.h`; `ctx.prefs->textureWidth/Height` from `core/Preferences.h`; `ctx.transport->secondsPerBeat()` from `core/Transport.h`.

---

### Task 1: `crossfadeMix` helper + test

**Files:**
- Modify: `src/core/ImageSequence.h`
- Test: `tests/test_image_sequence.cpp`

- [ ] **Step 1: Write the failing test**

Append to `tests/test_image_sequence.cpp`:

```cpp
TEST_CASE("crossfadeMix ramps 0->1, clamps, and caps at the interval") {
    CHECK(crossfadeMix(0.0f,  1.0f, 2.0f) == doctest::Approx(0.0f));
    CHECK(crossfadeMix(0.5f,  1.0f, 2.0f) == doctest::Approx(0.5f));
    CHECK(crossfadeMix(1.0f,  1.0f, 2.0f) == doctest::Approx(1.0f));
    CHECK(crossfadeMix(2.0f,  1.0f, 2.0f) == doctest::Approx(1.0f));   // clamp past the end
    CHECK(crossfadeMix(1.5f,  3.0f, 2.0f) == doctest::Approx(0.75f));  // cap fade to interval (2)
    CHECK(crossfadeMix(0.5f,  0.0f, 2.0f) == doctest::Approx(1.0f));   // no fade -> instant (fully "to")
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build build --target core_tests -j`
Expected: FAIL to compile — `crossfadeMix` not declared.

- [ ] **Step 3: Implement `crossfadeMix`**

In `src/core/ImageSequence.h`, add before the closing `} // namespace oss`:

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

- [ ] **Step 4: Run to verify it passes**

Run: `cmake --build build --target core_tests -j && ctest --test-dir build -R core_tests --output-on-failure`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/core/ImageSequence.h tests/test_image_sequence.cpp
git commit -m "feat(core): crossfadeMix blend factor (capped at interval)"
```

---

### Task 2: crossfade shader + node render + gl_smoke

**Files:**
- Create: `shaders/crossfade.frag`
- Rewrite: `src/modules/ImageSequencerNode.h`
- Modify: `tests/gl_smoke.cpp`

- [ ] **Step 1: Create the crossfade fragment shader**

Create `shaders/crossfade.frag`:

```glsl
#version 410 core
in vec2 vUV;
out vec4 FragColor;

uniform sampler2D uFrom;
uniform sampler2D uTo;
uniform float uMix;   // 0 = from, 1 = to

void main() {
    FragColor = mix(texture(uFrom, vUV), texture(uTo, vUV), uMix);
}
```

- [ ] **Step 2: Rewrite the node**

Replace the entire contents of `src/modules/ImageSequencerNode.h` with:

```cpp
#pragma once
#include <glad/gl.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <future>
#include <string>
#include <utility>
#include <vector>
#include "core/Node.h"
#include "core/PathUtil.h"
#include "core/ImageSequence.h"
#include "core/Preferences.h"
#include "gfx/ImageLoader.h"
#include "gfx/Framebuffer.h"
#include "gfx/FullscreenPass.h"
#include "gfx/GLUtil.h"
#include "gfx/Canvas.h"

namespace oss {

// Plays a folder of images in sequence, one every `duration` seconds (free-running) or `beat
// length` beats (transport-synced), cross-dissolving over `fade duration` seconds at each change
// (0 = instant). Decodes the upcoming image on a worker thread into one of two GL textures and
// renders mix(from, to, m) into its own FBO (the ShaderNode pattern, inline). The worker only runs
// the GL-free loadImage; the graph thread does GL uploads + the blend pass.
class ImageSequencerNode : public Node {
public:
    ImageSequencerNode() : Node("Image Sequencer") {
        addImageFolderInput("folder");
        addInput("duration", PortType::Float, 1.0f, 0.05f, 60.0f);   // seconds (free-running)
        addIntInput("beat length", 1, 1, 16);                        // beats per image (synced)
        addInput("sync", PortType::Bool, false);
        addInput("fade duration", PortType::Float, 0.0f, 0.0f, 10.0f);  // seconds; 0 = instant
        addOutput("image", PortType::Texture);
    }
    ~ImageSequencerNode() override {
        // fbo_/fsq_ free themselves; the fetch_ member dtor joins any in-flight decode. The worker
        // never touches these GL objects, so freeing them in the dtor body first is safe.
        if (fadeProg_) glDeleteProgram(fadeProg_);
        if (texShown_) glDeleteTextures(1, &texShown_);
        if (texNext_)  glDeleteTextures(1, &texNext_);
    }

    void initGL() override {
        fadeProg_ = linkProgram(kFadeVS, readFile("shaders/crossfade.frag"));
        fbo_.create(kCanvasW, kCanvasH);
        fsq_.create();
    }

    void evaluate(EvalContext& ctx) override {
        const std::string& folder = ctx.in<std::string>(0);
        float duration = ctx.in<float>(1);
        int   beatLen  = std::max(1, (int)std::lround(ctx.in<float>(2)));
        bool  sync     = ctx.in<bool>(3);
        float fadeDur  = ctx.in<float>(4);
        if (duration < 0.01f) duration = 0.01f;

        if (folder != folder_) {
            folder_ = folder;
            files_  = listImagesInDir(folder);
            shownIndex_ = -1; cur_ = 0; elapsed_ = 0.0f;
            nextReady_ = false; nextIndex_ = -1; failedIndex_ = -1;
            fading_ = false; fadeElapsed_ = 0.0f;
            fetch_ = std::future<ImageData>{}; fetchIndex_ = -1;   // (joins any in-flight decode)
            if (!files_.empty()) syncLoadShown(0);                 // immediate first frame
        }

        int n = (int)files_.size();
        if (n == 0) {
            status_ = folder_.empty() ? std::string() : ("no images in " + folder_);
            ctx.out<TexRef>(0, TexRef{});
            return;
        }

        // Poll the background decode; upload its result into the "next" texture.
        if (fetch_.valid() &&
            fetch_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            ImageData img = fetch_.get();
            if (img.ok()) { upload(texNext_, wNext_, hNext_, img); nextIndex_ = fetchIndex_; nextReady_ = true; }
            else          { failedIndex_ = fetchIndex_; }   // don't hammer a bad/missing file every frame
            fetchIndex_ = -1;   // idle
        }

        // Where should we be now, and how long is each image shown (seconds)?
        int target;
        if (sync) {
            double beats = ctx.transport ? ctx.transport->beats() : 0.0;
            target = syncedImageIndex(beats, (float)beatLen, n);
            cur_ = target;   // keep the free-run counter aligned for a later sync->free handoff
        } else {
            elapsed_ += ctx.dt;
            while (elapsed_ >= duration) { elapsed_ -= duration; cur_ = (cur_ + 1) % n; }
            target = cur_;
        }
        if (target >= n) target = n - 1;   // defensive: never index past files_
        float interval = sync
            ? (float)beatLen * (ctx.transport ? (float)ctx.transport->secondsPerBeat() : 0.5f)
            : duration;

        // Fade progression / start.
        float m = 0.0f;
        if (fading_) {
            fadeElapsed_ += ctx.dt;
            m = crossfadeMix(fadeElapsed_, fadeDur, interval);
            if (m >= 1.0f) {   // fade complete -> adopt the incoming image
                std::swap(texShown_, texNext_); std::swap(wShown_, wNext_); std::swap(hShown_, hNext_);
                shownIndex_ = fadeToIndex_; nextReady_ = false; fading_ = false; m = 0.0f;
            }
        } else if (target != shownIndex_ && nextReady_ && nextIndex_ == target) {
            float eff = fadeDur < interval ? fadeDur : interval;
            if (eff > 0.0f) { fading_ = true; fadeElapsed_ = 0.0f; fadeToIndex_ = target; }
            else {   // instant swap (no fade)
                std::swap(texShown_, texNext_); std::swap(wShown_, wNext_); std::swap(hShown_, hNext_);
                shownIndex_ = target; nextReady_ = false;
            }
        }

        // Prefetch (only when not fading -- both textures are in use during a fade).
        if (!fading_) {
            int want = (shownIndex_ == target) ? (target + 1) % n : target;
            if (want != failedIndex_) failedIndex_ = -1;   // moved off the bad index -> allow a retry
            bool haveWant = nextReady_ && nextIndex_ == want;
            if (!haveWant && fetchIndex_ == -1 && want != failedIndex_) {
                std::string p = files_[(std::size_t)want];
                fetch_ = std::async(std::launch::async, [p]() { std::string e; return loadImage(p, e); });
                fetchIndex_ = want;
            }
        }

        if (shownIndex_ >= 0)
            status_ = std::to_string(shownIndex_ + 1) + "/" + std::to_string(n)
                    + "  " + fileBaseName(files_[(std::size_t)shownIndex_]);

        // Render the (possibly blended) frame into the FBO and publish it.
        int fw = ctx.prefs ? ctx.prefs->textureWidth  : kCanvasW;
        int fh = ctx.prefs ? ctx.prefs->textureHeight : kCanvasH;
        if (fbo_.width() != fw || fbo_.height() != fh) fbo_.create(fw, fh);
        if (shownIndex_ >= 0 && texShown_) {
            fbo_.bind();
            glUseProgram(fadeProg_);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, texShown_);
            glUniform1i(glGetUniformLocation(fadeProg_, "uFrom"), 0);
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, fading_ ? texNext_ : texShown_);
            glUniform1i(glGetUniformLocation(fadeProg_, "uTo"), 1);
            glUniform1f(glGetUniformLocation(fadeProg_, "uMix"), fading_ ? m : 0.0f);
            glActiveTexture(GL_TEXTURE0);   // restore default unit
            fsq_.draw();
            Framebuffer::unbind();
            ctx.out<TexRef>(0, TexRef{ fbo_.texture(), fbo_.width(), fbo_.height() });
        } else {
            ctx.out<TexRef>(0, TexRef{});
        }
    }

    std::string statusLine() const override { return status_; }

private:
    // Synchronous decode+upload into the shown texture (the first image on folder load).
    void syncLoadShown(int i) {
        std::string err;
        ImageData img = loadImage(files_[(std::size_t)i], err);
        if (!img.ok()) { status_ = "load failed: " + err; shownIndex_ = -1; return; }
        upload(texShown_, wShown_, hShown_, img);
        shownIndex_ = i; cur_ = i;
    }

    static void upload(GLuint& tex, int& w, int& h, const ImageData& img) {
        if (!tex) glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, img.width, img.height, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, img.rgba.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);
        w = img.width; h = img.height;
    }

    static constexpr const char* kFadeVS = R"(#version 410 core
out vec2 vUV;
void main() {
    vec2 p = vec2(gl_VertexID == 1 ? 3.0 : -1.0, gl_VertexID == 2 ? 3.0 : -1.0);
    vUV = (p + 1.0) * 0.5;
    gl_Position = vec4(p, 0.0, 1.0);
}
)";

    std::vector<std::string> files_;
    std::string folder_, status_;
    GLuint texShown_ = 0, texNext_ = 0;
    int    wShown_ = 0, hShown_ = 0, wNext_ = 0, hNext_ = 0;
    int    shownIndex_ = -1;    // index in texShown_ (-1 = none)
    int    cur_ = 0;            // free-run position
    float  elapsed_ = 0.0f;     // free-run seconds accumulator
    std::future<ImageData> fetch_;
    int    fetchIndex_ = -1;    // index being decoded; -1 = idle
    bool   nextReady_ = false;  // texNext_ holds a decoded image
    int    nextIndex_ = -1;     // its index
    int    failedIndex_ = -1;   // an index whose decode failed; don't re-launch it until we move on
    Framebuffer    fbo_;        // the node renders its (blended) output here
    FullscreenPass fsq_;
    GLuint fadeProg_ = 0;       // crossfade shader program
    bool   fading_ = false;     // a cross-fade is in progress
    float  fadeElapsed_ = 0.0f; // seconds into the current fade
    int    fadeToIndex_ = -1;   // the index we're fading to (held in texNext_)
};

} // namespace oss
```

- [ ] **Step 3: Update the gl_smoke port-flag probe + add the cross-fade scenario**

In `tests/gl_smoke.cpp`:

(a) In the existing Image Sequencer scenario, the port-flag probe currently checks 4 ports. Find:

```cpp
          if (probe.inputs().size() != 4 || !probe.inputs()[2].integer)
            { fs::remove_all(dir); glfwTerminate(); return fail("Sequencer 'beat length' not an int input at port 2"); } }
```

and change the count to 5 (the node now has a `fade duration` input at port 4):

```cpp
          if (probe.inputs().size() != 5 || !probe.inputs()[2].integer)
            { fs::remove_all(dir); glfwTerminate(); return fail("Sequencer 'beat length' not an int input at port 2"); } }
```

(b) Immediately after that whole existing Image Sequencer scenario block (right after its
`std::fprintf(stderr, "gl_smoke OK: Image Sequencer cycled a folder (async prefetch, free-run + sync)\n");`
and the block's closing `}`), add a new cross-fade scenario:

```cpp
    // --- Scenario: Image Sequencer cross-fades between images ---
    {
        namespace fs = std::filesystem;
        fs::path dir = fs::temp_directory_path() / "oss_imgseq_fade";
        fs::remove_all(dir);
        fs::create_directories(dir);
        bool wrote = writeSolidPNG((dir / "0.png").string(), 255, 0, 0)     // red
                  && writeSolidPNG((dir / "1.png").string(), 0, 255, 0);    // green
        if (!wrote) { fs::remove_all(dir); glfwTerminate(); return fail("write fade fixtures"); }

        Graph g;
        auto seq = std::make_unique<ImageSequencerNode>();
        auto out = std::make_unique<OutputNode>();
        seq->initGL(); out->initGL();
        seq->inputDefault(0) = Value(dir.string());   // folder
        seq->inputDefault(1) = Value(1.0f);           // duration = 1s
        seq->inputDefault(2) = Value(1.0f);           // beat length
        seq->inputDefault(3) = Value(false);          // sync off
        seq->inputDefault(4) = Value(0.5f);           // fade duration = 0.5s
        int sId = g.addNode(std::move(seq));
        int oId = g.addNode(std::move(out));
        if (!g.connect(sId, 0, oId, 0)) { fs::remove_all(dir); glfwTerminate(); return fail("connect fade Sequencer->Output"); }

        // Step in 20 ms frames: red is shown, image 1 (green) prefetches, then at ~1 s the fade
        // starts and mix(red, green, m) is on the output for ~0.5 s before it resolves to green.
        bool sawBlend = false, reachedGreen = false;
        for (int f = 0; f < 2000 && !reachedGreen; ++f) {
            g.evaluate(0.02f);
            TexRef t = dynamic_cast<OutputNode*>(g.findNode(oId))->current();
            if (t.id == 0) continue;
            int r, gg, b, a; readCentre(t, r, gg, b, a);
            if (r > 60 && gg > 60 && b < 60) sawBlend = true;   // a red+green blend (mid-fade)
            if (gg > 200 && r < 60)          reachedGreen = true;  // resolved to the incoming image
        }
        fs::remove_all(dir);
        if (!sawBlend)     { glfwTerminate(); return fail("cross-fade produced no red/green blend"); }
        if (!reachedGreen) { glfwTerminate(); return fail("cross-fade did not resolve to green"); }
        std::fprintf(stderr, "gl_smoke OK: Image Sequencer cross-fade blends red->green\n");
    }
```

- [ ] **Step 4: Build (app + gl_smoke) and run gl_smoke**

Run: `cmake --build build --target shader_streamer gl_smoke -j && ctest --test-dir build -R gl_smoke --output-on-failure`
Expected: both build; `gl_smoke` PASSES, printing both `gl_smoke OK: Image Sequencer cycled a folder (async prefetch, free-run + sync)` and `gl_smoke OK: Image Sequencer cross-fade blends red->green`. (No-GL environments skip; confirm it built. You can also `./build/gl_smoke 2>&1 | grep -i "sequencer\|fade"`.)

- [ ] **Step 5: Commit**

```bash
git add shaders/crossfade.frag src/modules/ImageSequencerNode.h tests/gl_smoke.cpp
git commit -m "feat(modules): Image Sequencer cross-fade (fade duration input)"
```

---

### Task 3: Documentation

**Files:**
- Modify: `CLAUDE.md`
- Modify: `README.md`

- [ ] **Step 1: Update CLAUDE.md**

In `CLAUDE.md`, find this text in the Image Sequencer sentence (inside the **Image Streamer / Kaleidoscope** bullet):

```markdown
  of two GL textures and swaps at the boundary, so transitions don't hitch (bounded memory; the
  worker runs the GL-free `loadImage`, the graph thread uploads). Concurrent decodes are race-free
```

Replace it with (adds the cross-fade + FBO render note):

```markdown
  of two GL textures and, at each change, **cross-dissolves** over a `fade duration` (seconds;
  0 = instant, capped at the image interval via GL-free `crossfadeMix`) by rendering
  `mix(from, to, m)` through its own FBO (`shaders/crossfade.frag`, the ShaderNode pattern inline)
  — so transitions don't hitch (bounded memory; the worker runs the GL-free `loadImage`, the graph
  thread uploads + blends). Concurrent decodes are race-free
```

- [ ] **Step 2: Update README.md**

In `README.md`, replace the Image Sequencer table row:

```markdown
| **Image Sequencer** | play a folder of images in sequence: one every `duration` seconds, or every `beat length` beats when `sync` is on; the next image is prefetched on a background thread so transitions stay smooth. Pick the folder from your Image assets' folders |
```

with:

```markdown
| **Image Sequencer** | play a folder of images in sequence: one every `duration` seconds, or every `beat length` beats when `sync` is on, with an optional `fade duration` cross-dissolve between them; the next image is prefetched on a background thread so transitions stay smooth. Pick the folder from your Image assets' folders |
```

- [ ] **Step 3: Commit**

```bash
git add CLAUDE.md README.md
git commit -m "docs: Image Sequencer cross-fade"
```

---

## Final verification (after all tasks)

- [ ] Full build: `cmake --build build -j`
- [ ] All tests: `ctest --test-dir build --output-on-failure` — `core_tests` + `gl_smoke` pass (or gl_smoke cleanly skips where no GL context).
- [ ] Manual (optional, needs a display): run `./build/shader_streamer`, add **Image Sequencer**, pick a folder, wire → Output, set `fade duration` to ~0.5–1 s, and watch images cross-dissolve; set it to 0 for instant cuts.
- [ ] Hand off to `superpowers:finishing-a-development-branch`.

## Notes for the implementer

- **Only stage the files each step names.** Never `git add -A`. Leave `build.sh`, `examples/`, `preferences.oss`, `project.oss`, `imgui.ini` untracked.
- No CMake changes: `crossfade.frag` ships via the existing `shaders/` directory copy, the node is header-only, and `Framebuffer`/`FullscreenPass`/`GLUtil` are already linked into both `shader_streamer` and `gl_smoke`.
- `fade duration` is appended at port 4 — additive, so existing sequencer `.oss` projects load with fade 0 (instant), unchanged.
- The node now includes `core/Preferences.h` and `gfx/Canvas.h` (for `ctx.prefs->textureWidth`/`kCanvasW`) — both are already used by `ShaderNode`.
- gl_smoke fixtures write to the OS temp dir and are `remove_all`'d; a stray `oss_imgseq_fade` temp dir after an interrupted run is harmless.
