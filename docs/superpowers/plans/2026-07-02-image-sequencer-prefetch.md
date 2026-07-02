# Image Sequencer — split timing inputs + async prefetch — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give the Image Sequencer two separate timing inputs — `duration` (seconds, free-running) and `beat length` (int ≥ 1, transport-synced) — and decode the upcoming image on a background thread so transitions don't hitch.

**Architecture:** Rewrite `ImageSequencerNode.h` to hold two GL textures + a single in-flight `std::future<ImageData>`; it prefetches image *(i+1)* off-thread while showing *i* and swaps instantly at the boundary (idle-gated so it never blocks the graph thread on a live future). The GL-free `syncedImageIndex` math is unchanged. Verified by an updated `gl_smoke` scenario that polls until each async image lands.

**Tech Stack:** C++17 (`std::future`/`std::async`), OpenGL 4.1, `gfx/ImageLoader` (already built into `core_tests` + `gl_smoke`), doctest, headless GL.

**Reference spec:** `docs/superpowers/specs/2026-07-02-image-sequencer-prefetch-design.md`

**Conventions (CLAUDE.md):**
- `src/core/`/`src/audio/` stay GL-free (not touched here). The worker only runs GL-free `loadImage`; `glTexImage2D` runs on the graph thread.
- Conventional Commits. Branch `feat/image-sequencer-prefetch` (already created, off `develop`).
- Never `git add -A`/`git add .` — stage only the files each step names. Leave `build.sh`, `examples/`, `preferences.oss`, `project.oss`, `imgui.ini` untracked.
- Build: `cmake --build build --target <t> -j`; tests: `ctest --test-dir build --output-on-failure`.

**Context:** The node already exists (`src/modules/ImageSequencerNode.h`) with ports `folder`(0)/`duration`(1)/`sync`(2) and a single-texture decode-on-advance. This plan changes the ports to `folder`(0)/`duration`(1)/`beat length`(2)/`sync`(3) and the loading to async double-buffer. The existing `gl_smoke` "Image Sequencer" scenario uses the old ports, so Task 2 updates the node **and** its scenario together (they must land together to keep the build green).

---

### Task 1: `syncedImageIndex` — beat-length characterization test

`syncedImageIndex(beats, beatLength, count)` is unchanged (its second arg is now "beats per image"). Add a case documenting `beat length > 1`. It should pass immediately (no production change) — a characterization test for the new semantics.

**Files:**
- Test: `tests/test_image_sequence.cpp`

- [ ] **Step 1: Add the test case**

Append inside `tests/test_image_sequence.cpp` (after the existing `syncedImageIndex` `TEST_CASE`):

```cpp
TEST_CASE("syncedImageIndex with beat length > 1 holds each image for N beats") {
    // beatLength = 2 -> each image spans 2 beats.
    CHECK(syncedImageIndex(0.0, 2.0f, 3) == 0);
    CHECK(syncedImageIndex(1.9, 2.0f, 3) == 0);
    CHECK(syncedImageIndex(2.0, 2.0f, 3) == 1);
    CHECK(syncedImageIndex(4.0, 2.0f, 3) == 2);
    CHECK(syncedImageIndex(6.0, 2.0f, 3) == 0);   // wrap after 3 images * 2 beats
}
```

- [ ] **Step 2: Build + run**

Run: `cmake --build build --target core_tests -j && ctest --test-dir build -R core_tests --output-on-failure`
Expected: PASS (the math already supports it; this documents the beats-per-image reading).

- [ ] **Step 3: Commit**

```bash
git add tests/test_image_sequence.cpp
git commit -m "test(core): syncedImageIndex beat-length > 1 case"
```

---

### Task 2: Rewrite `ImageSequencerNode` (split inputs + async prefetch) + update gl_smoke

**Files:**
- Rewrite: `src/modules/ImageSequencerNode.h`
- Modify: `tests/gl_smoke.cpp` (the existing "Image Sequencer" scenario)

- [ ] **Step 1: Rewrite the node**

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
#include "gfx/ImageLoader.h"

namespace oss {

// Plays a folder of images in sequence, one every `duration` seconds (free-running) or
// `beat length` beats (transport-synced). Decodes the upcoming image on a background thread and
// uploads it ahead of the switch (double-buffered: two GL textures), so transitions don't hitch.
// Bounded memory (~2 textures + one in-flight CPU buffer). The worker only runs the GL-free
// loadImage; the graph thread does the glTexImage2D upload.
class ImageSequencerNode : public Node {
public:
    ImageSequencerNode() : Node("Image Sequencer") {
        addImageFolderInput("folder");
        addInput("duration", PortType::Float, 1.0f, 0.05f, 60.0f);   // seconds (free-running)
        addIntInput("beat length", 1, 1, 16);                        // beats per image (synced)
        addInput("sync", PortType::Bool, false);
        addOutput("image", PortType::Texture);
    }
    ~ImageSequencerNode() override {
        // fetch_'s destructor joins any in-flight decode before we free the GL objects.
        if (texShown_) glDeleteTextures(1, &texShown_);
        if (texNext_)  glDeleteTextures(1, &texNext_);
    }

    void initGL() override {}   // textures allocated lazily on first load

    void evaluate(EvalContext& ctx) override {
        const std::string& folder = ctx.in<std::string>(0);
        float duration = ctx.in<float>(1);
        int   beatLen  = std::max(1, (int)std::lround(ctx.in<float>(2)));
        bool  sync     = ctx.in<bool>(3);
        if (duration < 0.01f) duration = 0.01f;

        if (folder != folder_) {
            folder_ = folder;
            files_  = listImagesInDir(folder);
            shownIndex_ = -1; cur_ = 0; elapsed_ = 0.0f;
            nextReady_ = false; nextIndex_ = -1;
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
            fetchIndex_ = -1;   // idle
        }

        // Where should we be now?
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

        // Switch instantly if the target was prefetched; else keep showing current (no hitch).
        if (target != shownIndex_ && nextReady_ && nextIndex_ == target) {
            std::swap(texShown_, texNext_);
            std::swap(wShown_, wNext_);
            std::swap(hShown_, hNext_);
            shownIndex_ = target;
            nextReady_ = false;
        }

        // Drive the prefetch: decode the wanted image when the worker is idle.
        int want = (shownIndex_ == target) ? (target + 1) % n : target;
        bool haveWant = nextReady_ && nextIndex_ == want;
        if (!haveWant && fetchIndex_ == -1) {
            std::string p = files_[(std::size_t)want];
            fetch_ = std::async(std::launch::async, [p]() { std::string e; return loadImage(p, e); });
            fetchIndex_ = want;
        }

        if (shownIndex_ >= 0)
            status_ = std::to_string(shownIndex_ + 1) + "/" + std::to_string(n)
                    + "  " + fileBaseName(files_[(std::size_t)shownIndex_]);
        ctx.out<TexRef>(0, (shownIndex_ >= 0 && texShown_) ? TexRef{ texShown_, wShown_, hShown_ }
                                                           : TexRef{});
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
};

} // namespace oss
```

- [ ] **Step 2: Update the gl_smoke scenario**

In `tests/gl_smoke.cpp`, find the existing Image Sequencer scenario (it starts with `// --- Scenario: Image Sequencer cycles a folder of images ---`) and replace **the whole block** (from that comment through its closing `}` that follows the `gl_smoke OK: Image Sequencer ...` line) with the version below. It uses the new port indices (`duration`=1, `beat length`=2, `sync`=3) and **polls** `evaluate()` until each async image lands (the first image loads synchronously, so it's immediate; the rest arrive on the worker):

```cpp
    // --- Scenario: Image Sequencer cycles a folder (async prefetch, split inputs) ---
    {
        namespace fs = std::filesystem;
        fs::path dir = fs::temp_directory_path() / "oss_imgseq_smoke";
        fs::remove_all(dir);
        fs::create_directories(dir);
        bool wrote = writeSolidPNG((dir / "0.png").string(), 255, 0, 0)     // red
                  && writeSolidPNG((dir / "1.png").string(), 0, 255, 0)     // green
                  && writeSolidPNG((dir / "2.png").string(), 0, 0, 255);    // blue
        if (!wrote) { fs::remove_all(dir); glfwTerminate(); return fail("write sequencer fixtures"); }

        // Port-flag check (pure CPU; the ctor doesn't touch GL). folder=0, duration=1, beat length=2, sync=3.
        { ImageSequencerNode probe;
          const Port& pf = probe.inputs()[0];
          if (!(pf.type == PortType::String && pf.assetBacked && pf.folderPicker && pf.assetType == AssetType::Image))
            { fs::remove_all(dir); glfwTerminate(); return fail("Sequencer.folder not a folder picker"); }
          if (probe.inputs().size() != 4 || !probe.inputs()[2].integer)
            { fs::remove_all(dir); glfwTerminate(); return fail("Sequencer 'beat length' not an int input at port 2"); } }

        Graph g;
        auto seq = std::make_unique<ImageSequencerNode>();
        auto out = std::make_unique<OutputNode>();
        seq->initGL(); out->initGL();
        seq->inputDefault(0) = Value(dir.string());   // folder
        seq->inputDefault(1) = Value(1.0f);           // duration = 1s (free-running)
        seq->inputDefault(2) = Value(1.0f);           // beat length = 1
        seq->inputDefault(3) = Value(false);          // sync off
        int sId = g.addNode(std::move(seq));
        int oId = g.addNode(std::move(out));
        if (!g.connect(sId, 0, oId, 0)) { fs::remove_all(dir); glfwTerminate(); return fail("connect Sequencer->Output"); }

        auto centreIs = [&](int R, int G, int B)->bool {
            TexRef t = dynamic_cast<OutputNode*>(g.findNode(oId))->current();
            if (t.id == 0) return false;
            int r, gg, b, a; readCentre(t, r, gg, b, a);
            return near(r, R) && near(gg, G) && near(b, B);
        };
        // Evaluate a big-dt frame to advance the counter, then poll small-dt frames until the
        // async image for the new index is decoded + uploaded (or time out).
        auto advanceUntil = [&](int R, int G, int B)->bool {
            g.evaluate(1.1f);                                  // cross one `duration` boundary
            for (int f = 0; f < 400; ++f) {
                if (centreIs(R, G, B)) return true;
                g.evaluate(0.001f);                            // poll without advancing further
            }
            return false;
        };

        g.evaluate(1.0f / 60.0f);                              // image 0 loads synchronously -> red
        if (!centreIs(255, 0, 0)) { fs::remove_all(dir); glfwTerminate(); return fail("sequencer frame 0 not red"); }
        if (!advanceUntil(0, 255, 0)) { fs::remove_all(dir); glfwTerminate(); return fail("sequencer did not reach green"); }
        if (!advanceUntil(0, 0, 255)) { fs::remove_all(dir); glfwTerminate(); return fail("sequencer did not reach blue"); }
        if (!advanceUntil(255, 0, 0)) { fs::remove_all(dir); glfwTerminate(); return fail("sequencer did not wrap to red"); }

        // Synced mode: index derives from transport beats (120 bpm -> 0.5 s/beat), beat length = 1.
        g.findNode(sId)->inputDefault(3) = Value(true);        // sync on
        g.findNode(sId)->inputDefault(2) = Value(1.0f);        // beat length = 1
        g.transport().seconds = 1.0;                           // beats = 2.0 -> image 2 (blue)
        bool syncedBlue = false;
        for (int f = 0; f < 400 && !syncedBlue; ++f) { g.evaluate(0.001f); syncedBlue = centreIs(0, 0, 255); }
        if (!syncedBlue) { fs::remove_all(dir); glfwTerminate(); return fail("sequencer sync beats=2 not blue"); }

        fs::remove_all(dir);
        std::fprintf(stderr, "gl_smoke OK: Image Sequencer cycled a folder (async prefetch, free-run + sync)\n");
    }
```

- [ ] **Step 3: Build (app + gl_smoke) and run gl_smoke**

Run: `cmake --build build --target shader_streamer gl_smoke -j && ctest --test-dir build -R gl_smoke --output-on-failure`
Expected: both build; `gl_smoke` PASSES, printing `gl_smoke OK: Image Sequencer cycled a folder (async prefetch, free-run + sync)`. (No-GL environments skip; confirm it built. You can also run `./build/gl_smoke 2>&1 | grep -i sequencer`.)

- [ ] **Step 4: Commit**

```bash
git add src/modules/ImageSequencerNode.h tests/gl_smoke.cpp
git commit -m "feat(modules): Image Sequencer split duration/beat-length inputs + async prefetch"
```

---

### Task 3: Documentation

**Files:**
- Modify: `CLAUDE.md`
- Modify: `README.md`

- [ ] **Step 1: Update CLAUDE.md**

In `CLAUDE.md`, find the sentence in the **Image Streamer / Kaleidoscope** bullet that describes the sequencer. It currently reads (find this exact text):

```markdown
  The node scans that folder on disk (`listImagesInDir`,
  `std::filesystem` in `gfx/ImageLoader`) and advances one image every `duration` — seconds when
  free-running, or beats when `sync` is on (`syncedImageIndex`, stateless from `transport.beats()`,
  in GL-free `core/ImageSequence.h`) — decoding one image at a time. `parentDir`/`uniqueAssetFolders`/
  `listImagesInDir`/`syncedImageIndex` are unit-tested; the cycle (free-run + sync) is `gl_smoke`-checked.
```

Replace it with:

```markdown
  The node scans that folder on disk (`listImagesInDir`, `std::filesystem` in `gfx/ImageLoader`)
  and advances one image every `duration` **seconds** (free-running) or every `beat length`
  **beats** (an int ≥ 1, transport-synced via `syncedImageIndex`, stateless from `transport.beats()`
  in GL-free `core/ImageSequence.h`). It **prefetches** the next image on a worker thread
  (`std::future<ImageData>`, idle-gated) into one of two GL textures and swaps at the boundary, so
  transitions don't hitch (bounded memory; the worker runs the GL-free `loadImage`, the graph
  thread uploads). `parentDir`/`uniqueAssetFolders`/`listImagesInDir`/`syncedImageIndex` are
  unit-tested; the async cycle (free-run + sync) is `gl_smoke`-checked.
```

- [ ] **Step 2: Update README.md**

In `README.md`, replace the Image Sequencer table row:

```markdown
| **Image Sequencer** | play a folder of images in sequence, one every `duration` (seconds, or beats when `sync` is on); pick the folder from your Image assets' folders |
```

with:

```markdown
| **Image Sequencer** | play a folder of images in sequence: one every `duration` seconds, or every `beat length` beats when `sync` is on; the next image is prefetched on a background thread so transitions stay smooth. Pick the folder from your Image assets' folders |
```

- [ ] **Step 3: Commit**

```bash
git add CLAUDE.md README.md
git commit -m "docs: Image Sequencer split timing inputs + async prefetch"
```

---

## Final verification (after all tasks)

- [ ] Full build: `cmake --build build -j`
- [ ] All tests: `ctest --test-dir build --output-on-failure` — `core_tests` + `gl_smoke` pass (or gl_smoke cleanly skips where no GL context).
- [ ] Manual (optional, needs a display): run `./build/shader_streamer`, add **Image Sequencer**, pick a folder with several images, wire → Output; confirm it cycles by `duration`; toggle `sync` + press play to advance by `beat length` beats. Large images should switch without a visible stutter.
- [ ] Hand off to `superpowers:finishing-a-development-branch`.

## Notes for the implementer

- **Only stage the files each step names.** Never `git add -A`. Leave `build.sh`, `examples/`, `preferences.oss`, `project.oss`, `imgui.ini` untracked.
- No CMake changes and no new files — `core_tests`/`gl_smoke` already link `ImageLoader.cpp`, and `<future>`/`<chrono>` are standard.
- The port order changed (`beat length` is now port 2, `sync` is port 3). The gl_smoke edit in Task 2 already accounts for this; there are no saved `.oss` projects depending on the old layout.
- The async decode of a 16×16 test PNG completes in microseconds, so the gl_smoke poll loops resolve almost immediately; the 400-frame timeout is a generous safety net.
