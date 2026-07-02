# Image assets + Image Streamer + Kaleidoscope Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add an **Image** asset type (fifth Assets tab), an **Image Streamer** node that loads a still image as a texture, and a **Kaleidoscope** node that folds an input texture into a mirrored pattern.

**Architecture:** `AssetType::Image` is appended to the enum (value 4) for file-format compatibility. A new GL-free `gfx/ImageLoader` wraps `stb_image` (already a dependency). `ImageStreamerNode` is a header-only texture *source* (asset path → GL texture → `TexRef`); `KaleidoscopeNode` is a header-only `ShaderNode` texture *transform*, mirroring `CompositorNode`.

**Tech Stack:** C++17, OpenGL 4.1, `stb_image.h` / `stb_image_write.h` (from the existing `stb` FetchContent dep), Dear ImGui, doctest (`core_tests`), headless GL (`gl_smoke`).

**Reference spec:** `docs/superpowers/specs/2026-07-02-image-nodes-design.md`

**Conventions to honor (from CLAUDE.md):**
- `src/core/` and `src/audio/` stay GL-free. `gfx/ImageLoader` is GL-free but lives in `gfx/` (like `VideoDecoder`); GL texture code lives in the node header.
- Commit messages: Conventional Commits. Work on branch `feat/image-nodes` (already created, off `develop`).
- Never `git add -A` / `git add .` — stage only the files each step names. Leave untracked `build.sh`, `examples/`, `preferences.oss`, `project.oss`, `imgui.ini` alone.
- Build/run: `cmake --build build -j`; tests: `ctest --test-dir build --output-on-failure`.

---

### Task 1: `AssetType::Image` + codec round-trip test

Append the new asset type; prove it persists through the asset-block codec unchanged.

**Files:**
- Modify: `src/core/AssetLibrary.h:10-11`
- Test: `tests/test_asset_library_file.cpp` (append a `TEST_CASE`)

- [ ] **Step 1: Write the failing test**

Append this to `tests/test_asset_library_file.cpp` (after the existing cases):

```cpp
TEST_CASE("AssetType::Image is the fifth type and round-trips the codec") {
    CHECK(kAssetTypeCount == 5);
    CHECK((int)AssetType::Image == 4);

    AssetLibrary lib;
    int i = lib.add(AssetType::Image, "Logo", "/m/img/logo.png");

    std::string text = serializeLibrary(lib);
    AssetLibrary out;
    REQUIRE(parseLibrary(text, out));
    const Asset* a = out.find(i);
    REQUIRE(a != nullptr);
    CHECK(a->type == AssetType::Image);
    CHECK(a->label == "Logo");
    CHECK(a->path == "/m/img/logo.png");
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build build -j core_tests && ctest --test-dir build -R core_tests --output-on-failure`
Expected: FAIL to **compile** — `AssetType` has no member `Image`, and `kAssetTypeCount` is 4.

- [ ] **Step 3: Make the change**

In `src/core/AssetLibrary.h`, change lines 10-11 from:

```cpp
enum class AssetType { Audio, Video, Midi, Mesh };
constexpr int kAssetTypeCount = 4;   // number of AssetType values (and Assets-window tabs)
```

to:

```cpp
enum class AssetType { Audio, Video, Midi, Mesh, Image };
constexpr int kAssetTypeCount = 5;   // number of AssetType values (and Assets-window tabs)
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `cmake --build build -j core_tests && ctest --test-dir build -R core_tests --output-on-failure`
Expected: PASS (all `core_tests`, including the new case).

- [ ] **Step 5: Commit**

```bash
git add src/core/AssetLibrary.h tests/test_asset_library_file.cpp
git commit -m "feat(assets): add AssetType::Image (fifth type)"
```

---

### Task 2: Assets panel **Image** tab + node-editor label

Give the new type a UI tab and a picker label. UI-only (no unit test); verify by building.

**Files:**
- Modify: `src/ui/AssetsPanel.cpp:315-319` (add the Image tab after the Video tab)
- Modify: `src/ui/NodeEditorPanel.cpp:30-38` (add the `Image` case to `assetTypeName`)

- [ ] **Step 1: Add the Image tab**

In `src/ui/AssetsPanel.cpp`, immediately **after** the Video tab block (which ends at the line `}` following `ImGui::EndTabItem();` for Video, around line 319) and before the MIDI tab, insert:

```cpp
        if (ImGui::BeginTabItem("Image")) {
            drawTab(lib, AssetType::Image, "image file",
                    {"png", "jpg", "jpeg", "bmp", "tga", "gif", "hdr", "psd"});
            ImGui::EndTabItem();
        }
```

- [ ] **Step 2: Add the picker label**

In `src/ui/NodeEditorPanel.cpp`, in `assetTypeName` (lines 30-38), add the `Image` case so the switch reads:

```cpp
static const char* assetTypeName(AssetType t) {
    switch (t) {
        case AssetType::Audio: return "Audio";
        case AssetType::Video: return "Video";
        case AssetType::Midi:  return "MIDI";
        case AssetType::Mesh:  return "3D";
        case AssetType::Image: return "Image";
    }
    return "media";
}
```

- [ ] **Step 3: Build to verify it compiles**

Run: `cmake --build build -j shader_streamer`
Expected: builds clean. (The per-tab state arrays `filter_`/`selected_`/`anchor_` in `AssetsPanel.h` are sized by `kAssetTypeCount`, now 5, so they grow automatically — no other change needed.)

- [ ] **Step 4: Commit**

```bash
git add src/ui/AssetsPanel.cpp src/ui/NodeEditorPanel.cpp
git commit -m "feat(assets): Image tab in the Assets panel + node picker label"
```

---

### Task 3: `gfx/ImageLoader` (GL-free `stb_image` wrapper) + core test

Decode a file to a top-down… *bottom-up* RGBA buffer (matching the app's texture orientation). Unit-test the decode + vertical flip with a fixture authored at runtime.

**Files:**
- Create: `src/gfx/ImageLoader.h`
- Create: `src/gfx/ImageLoader.cpp`
- Create: `tests/test_image_loader.cpp`
- Modify: `CMakeLists.txt` (APP_SOURCES, core_tests sources + include dir, gl_smoke sources)

- [ ] **Step 1: Write the header**

Create `src/gfx/ImageLoader.h`:

```cpp
#pragma once
#include <string>
#include <vector>

namespace oss {

// Decoded image pixels. GL-free (the caller uploads to a texture). Rows are stored
// bottom-up to match the app's existing texture convention (VideoDecoder frames are
// bottom-up and display upright through the same shaders / Output blit).
struct ImageData {
    std::vector<unsigned char> rgba;   // width*height*4, row-major, bottom-up
    int width  = 0;
    int height = 0;
    bool ok() const { return width > 0 && height > 0 && !rgba.empty(); }
};

// Decode `path` to RGBA8. On failure returns an ImageData with ok()==false and fills
// `err` with a reason. An empty path is a (silent) failure with err = "empty path".
ImageData loadImage(const std::string& path, std::string& err);

} // namespace oss
```

- [ ] **Step 2: Write the implementation**

Create `src/gfx/ImageLoader.cpp`:

```cpp
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#include "gfx/ImageLoader.h"

namespace oss {

ImageData loadImage(const std::string& path, std::string& err) {
    ImageData out;
    if (path.empty()) { err = "empty path"; return out; }

    stbi_set_flip_vertically_on_load(1);   // -> bottom-up rows, matching VideoDecoder
    int w = 0, h = 0, comps = 0;
    unsigned char* data = stbi_load(path.c_str(), &w, &h, &comps, 4);   // force RGBA
    if (!data) {
        const char* why = stbi_failure_reason();
        err = why ? why : "decode failed";
        return out;
    }
    out.width  = w;
    out.height = h;
    out.rgba.assign(data, data + (std::size_t)w * h * 4);
    stbi_image_free(data);
    return out;
}

} // namespace oss
```

- [ ] **Step 3: Wire it into CMake**

In `CMakeLists.txt`:

(a) In `APP_SOURCES`, after `src/gfx/TextGeometry.cpp` (line 238) add:

```cmake
  src/gfx/ImageLoader.cpp
```

(b) In the `core_tests` `add_executable(core_tests ...)` source list, after `tests/test_bar_sync.cpp` (line 325) add the loader implementation and its test:

```cmake
  src/gfx/ImageLoader.cpp
  tests/test_image_loader.cpp
```

(c) Give `core_tests` the `stb` include dir. Change `target_include_directories(core_tests PRIVATE src)` (line 343) to:

```cmake
target_include_directories(core_tests PRIVATE src ${stb_SOURCE_DIR})
```

(d) In the `gl_smoke` `add_executable(gl_smoke ...)` source list, after `src/gfx/VideoDecoder.cpp` (line 365) add:

```cmake
  src/gfx/ImageLoader.cpp
```

- [ ] **Step 4: Write the failing test**

Create `tests/test_image_loader.cpp`. It authors a 4×2 PNG (top row red, bottom row green) with `stb_image_write`, loads it back, and checks dims plus the **vertical flip** (loaded row 0 = file's bottom row = green):

```cpp
#include <doctest/doctest.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#include "gfx/ImageLoader.h"
#include <cstdio>
#include <string>
#include <vector>

using namespace oss;

TEST_CASE("loadImage decodes RGBA and flips to bottom-up rows") {
    // Author a 4x2 image: top row red, bottom row green (as written, top-down).
    const int W = 4, H = 2;
    std::vector<unsigned char> src((std::size_t)W * H * 4);
    for (int x = 0; x < W; ++x) {
        std::size_t top = (std::size_t)(0 * W + x) * 4;   // row 0 = top
        std::size_t bot = (std::size_t)(1 * W + x) * 4;   // row 1 = bottom
        src[top+0]=255; src[top+1]=0;   src[top+2]=0; src[top+3]=255;   // red
        src[bot+0]=0;   src[bot+1]=255; src[bot+2]=0; src[bot+3]=255;   // green
    }
    std::string path = "test_image_loader_fixture.png";
    REQUIRE(stbi_write_png(path.c_str(), W, H, 4, src.data(), W * 4) != 0);

    std::string err;
    ImageData img = loadImage(path, err);
    std::remove(path.c_str());

    REQUIRE(img.ok());
    CHECK(img.width  == W);
    CHECK(img.height == H);
    // Vertical flip: returned row 0 is the file's BOTTOM row -> green.
    CHECK(img.rgba[0] == 0);    CHECK(img.rgba[1] == 255); CHECK(img.rgba[2] == 0);
    // Returned row 1 is the file's TOP row -> red.
    std::size_t r1 = (std::size_t)(1 * W + 0) * 4;
    CHECK(img.rgba[r1+0] == 255); CHECK(img.rgba[r1+1] == 0); CHECK(img.rgba[r1+2] == 0);
}

TEST_CASE("loadImage fails cleanly on an empty or missing path") {
    std::string err;
    CHECK_FALSE(loadImage("", err).ok());
    CHECK(err == "empty path");
    err.clear();
    CHECK_FALSE(loadImage("no_such_file_zzz.png", err).ok());
    CHECK_FALSE(err.empty());
}
```

- [ ] **Step 5: Run the test to verify it passes**

Run: `cmake -S . -B build && cmake --build build -j core_tests && ctest --test-dir build -R core_tests --output-on-failure`
Expected: PASS. (Re-run CMake configure because the source list changed.)

Note: `stb_image.h` (implemented in `ImageLoader.cpp`) and `stb_image_write.h` (implemented in the test) are different headers with different implementation macros, so there is no duplicate-symbol collision in the `core_tests` binary.

- [ ] **Step 6: Commit**

```bash
git add src/gfx/ImageLoader.h src/gfx/ImageLoader.cpp tests/test_image_loader.cpp CMakeLists.txt
git commit -m "feat(gfx): GL-free ImageLoader (stb_image) with vertical-flip test"
```

---

### Task 4: `ImageStreamerNode` + registration + gl_smoke

Header-only texture source. Load on path change, republish a stable `TexRef`. Prove it end-to-end with a headless render.

**Files:**
- Create: `src/modules/ImageStreamerNode.h`
- Modify: `src/app/Application.cpp` (include + `makeNode` + `nodeCategories`)
- Modify: `tests/gl_smoke.cpp` (fixture author helper + new scenario)

- [ ] **Step 1: Write the node**

Create `src/modules/ImageStreamerNode.h`:

```cpp
#pragma once
#include <glad/gl.h>
#include <string>
#include "core/Node.h"
#include "gfx/ImageLoader.h"

namespace oss {

// Loads one still image (from an Image asset path) and publishes it as a TexRef.
// The load happens once, only when the path changes; every frame it republishes the
// same texture. Animate it downstream (Kaleidoscope, World Transform, Compositor).
class ImageStreamerNode : public Node {
public:
    ImageStreamerNode() : Node("Image Streamer") {
        addAssetInput("file", AssetType::Image);
        addOutput("image", PortType::Texture);
    }
    ~ImageStreamerNode() override { if (tex_) glDeleteTextures(1, &tex_); }

    void initGL() override {}   // texture is allocated lazily on first load

    void evaluate(EvalContext& ctx) override {
        const std::string& path = ctx.in<std::string>(0);
        if (path != path_) { path_ = path; load(path); }
        ctx.out<TexRef>(0, haveTex_ ? TexRef{ tex_, w_, h_ } : TexRef{});
    }

    std::string statusLine() const override { return status_; }

private:
    void load(const std::string& path) {
        haveTex_ = false;
        if (path.empty()) { status_.clear(); return; }
        std::string err;
        ImageData img = loadImage(path, err);
        if (!img.ok()) { status_ = "load failed: " + err; return; }

        if (!tex_) glGenTextures(1, &tex_);
        glBindTexture(GL_TEXTURE_2D, tex_);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, img.width, img.height, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, img.rgba.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);

        w_ = img.width; h_ = img.height; haveTex_ = true;
        status_ = std::to_string(w_) + "x" + std::to_string(h_);
    }

    GLuint tex_ = 0;
    int    w_ = 0, h_ = 0;
    bool   haveTex_ = false;
    std::string path_;
    std::string status_;
};

} // namespace oss
```

- [ ] **Step 2: Register the node**

In `src/app/Application.cpp`:

(a) Add the include alongside the other module includes (near line 4-11):

```cpp
#include "modules/ImageStreamerNode.h"
```

(b) In `makeNode`, add before `return nullptr;` (the entry can sit next to the other Texture nodes, e.g. after the `"Video"` line):

```cpp
    if (type == "Image Streamer") return std::make_unique<ImageStreamerNode>();
```

(c) In `nodeCategories`, add `"Image Streamer"` to the `"Texture"` category list so it reads:

```cpp
        { "Texture", { "Colour", "Image Streamer", "Video", "Mix", "Compositor", "Kaleidoscope", "Recorder", "Output" } },
```

(Note: `"Kaleidoscope"` is added here too — Task 5 supplies the node; registering both names now keeps this list edited once. The `makeNode` entry for Kaleidoscope is added in Task 5.)

- [ ] **Step 3: Add the gl_smoke fixture helper + scenario**

In `tests/gl_smoke.cpp`:

(a) At the top of the file, after the existing `#include` block (after line ~46), add the image-write implementation and the Image Streamer header (the Kaleidoscope header is added in Task 5, when that file exists):

```cpp
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#include "modules/ImageStreamerNode.h"
```

(b) After the `readCentre` helper (line 79), add a UV-sampling readback helper and a fixture author:

```cpp
// Read the texel nearest UV (u,v in [0,1]); vertical orientation doesn't matter for
// horizontal (u) checks. Used by the image scenarios.
static void readAtUV(TexRef tex, float u, float v, int& r, int& g, int& b, int& a) {
    std::vector<unsigned char> px((size_t)tex.w * tex.h * 4);
    glBindTexture(GL_TEXTURE_2D, tex.id);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    int x = (int)(u * (tex.w - 1));
    int y = (int)(v * (tex.h - 1));
    size_t i = ((size_t)y * tex.w + x) * 4;
    r = px[i]; g = px[i+1]; b = px[i+2]; a = px[i+3];
}

// Write a 64x64 PNG: left half red, right half green. Returns the path (or "" on failure).
static std::string writeSplitFixture() {
    const int W = 64, H = 64;
    std::vector<unsigned char> px((size_t)W * H * 4);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            size_t i = ((size_t)y * W + x) * 4;
            bool left = x < W / 2;
            px[i+0] = left ? 255 : 0;
            px[i+1] = left ? 0   : 255;
            px[i+2] = 0;
            px[i+3] = 255;
        }
    std::string path = "gl_smoke_image_fixture.png";
    if (!stbi_write_png(path.c_str(), W, H, 4, px.data(), W * 4)) return std::string();
    return path;
}
```

(c) Add a new scenario block near the other texture scenarios (e.g. after the Colour scenario, before the final `glfwTerminate()`). It loads the fixture through `ImageStreamerNode` → `OutputNode` and asserts the halves survive:

```cpp
    // --- Scenario: Image Streamer loads a split image ---
    {
        std::string fixture = writeSplitFixture();
        if (fixture.empty()) { glfwTerminate(); return fail("write image fixture"); }

        Graph g;
        auto img = std::make_unique<ImageStreamerNode>();
        auto out = std::make_unique<OutputNode>();
        img->initGL(); out->initGL();
        img->inputDefault(0) = Value(fixture);          // set the "file" path
        int iId = g.addNode(std::move(img));
        int oId = g.addNode(std::move(out));
        if (!g.connect(iId, 0, oId, 0)) { std::remove(fixture.c_str()); glfwTerminate(); return fail("connect ImageStreamer->Output"); }

        g.evaluate(1.0f / 60.0f);
        TexRef tex = dynamic_cast<OutputNode*>(g.findNode(oId))->current();
        std::remove(fixture.c_str());
        if (tex.id == 0 || tex.w <= 0 || tex.h <= 0) { glfwTerminate(); return fail("image output texture not produced"); }

        int r, gg, b, a;
        readAtUV(tex, 0.25f, 0.5f, r, gg, b, a);   // left quarter -> red
        if (!(r > 200 && gg < 60)) { glfwTerminate(); return fail("image left half not red"); }
        readAtUV(tex, 0.75f, 0.5f, r, gg, b, a);   // right quarter -> green
        if (!(gg > 200 && r < 60)) { glfwTerminate(); return fail("image right half not green"); }
        std::fprintf(stderr, "gl_smoke OK: Image Streamer loaded a split image\n");
    }
```

- [ ] **Step 4: Build (app + gl_smoke) and run gl_smoke**

Run: `cmake -S . -B build && cmake --build build -j shader_streamer gl_smoke && ctest --test-dir build -R gl_smoke --output-on-failure`
Expected: both targets build (this compiles the `Application.cpp` registration), and `gl_smoke` PASSES with `gl_smoke OK: Image Streamer loaded a split image` printed. (If the environment has no GL context, `gl_smoke` skips — that is acceptable, per CLAUDE.md. Confirm at least that it built.)

- [ ] **Step 5: Commit**

```bash
git add src/modules/ImageStreamerNode.h src/app/Application.cpp tests/gl_smoke.cpp
git commit -m "feat(modules): Image Streamer node (image asset -> texture)"
```

---

### Task 5: `KaleidoscopeNode` + shader + registration + gl_smoke

Header-only `ShaderNode` mirroring `CompositorNode`. Prove the fold with a headless render (N-fold rotational symmetry + a folded sample crossing a colour boundary).

**Files:**
- Create: `shaders/kaleidoscope.frag`
- Create: `src/modules/KaleidoscopeNode.h`
- Modify: `src/app/Application.cpp` (include + `makeNode`; the category was updated in Task 4)
- Modify: `tests/gl_smoke.cpp` (new scenario)

- [ ] **Step 1: Write the fragment shader**

Create `shaders/kaleidoscope.frag`:

```glsl
#version 410 core
in vec2 vUV;
out vec4 FragColor;

uniform sampler2D uImage;
uniform int   uSegments;   // mirror count (>= 1)
uniform float uRotation;   // radians, added after the fold (spin)
uniform float uZoom;       // > 0; scales sampled radius
uniform vec2  uCenter;     // fold centre in UV space

const float TAU = 6.28318530718;

void main() {
    vec2  p   = vUV - uCenter;
    float r   = length(p) / max(uZoom, 0.0001);
    float a   = atan(p.y, p.x);
    float seg = TAU / float(max(uSegments, 1));

    // Fold the angle into one wedge, mirrored so adjacent wedges meet seamlessly.
    // mod() by seg makes value(angle + seg) == value(angle): N-fold rotational symmetry.
    a = mod(a, seg);
    a = abs(a - 0.5 * seg);
    a += uRotation;

    vec2 uv = uCenter + r * vec2(cos(a), sin(a));
    FragColor = texture(uImage, clamp(uv, 0.0, 1.0));
}
```

- [ ] **Step 2: Write the node**

Create `src/modules/KaleidoscopeNode.h`:

```cpp
#pragma once
#include <glad/gl.h>
#include <algorithm>
#include <cmath>
#include "gfx/ShaderNode.h"

namespace oss {

// Folds an input texture into a mirrored kaleidoscopic pattern. A ShaderNode -- the
// polar fold math lives in shaders/kaleidoscope.frag. All params are input ports, so
// `rotation` can be wired to an LFO/Automation to spin. Mirrors CompositorNode.
class KaleidoscopeNode : public ShaderNode {
public:
    KaleidoscopeNode() : ShaderNode("Kaleidoscope", "shaders/kaleidoscope.frag") {
        addInput("image", PortType::Texture, TexRef{});
        addIntInput("segments", 6, 2, 32);
        addInput("rotation", PortType::Float, 0.0f, 0.0f, 1.0f);   // turns
        addInput("zoom",     PortType::Float, 1.0f, 0.1f, 4.0f);
        addInput("center x", PortType::Float, 0.5f, 0.0f, 1.0f);
        addInput("center y", PortType::Float, 0.5f, 0.0f, 1.0f);
        addOutput("out", PortType::Texture);
    }
    void evaluate(EvalContext& ctx) override { render(ctx); }

protected:
    void setUniforms(EvalContext& ctx) override {
        TexRef in = ctx.in<TexRef>(0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, in.id);
        glUniform1i(glGetUniformLocation(program_, "uImage"), 0);

        int segments = std::max(1, (int)std::lround(ctx.in<float>(1)));
        glUniform1i(glGetUniformLocation(program_, "uSegments"), segments);
        glUniform1f(glGetUniformLocation(program_, "uRotation"),
                    ctx.in<float>(2) * 6.28318530718f);            // turns -> radians
        glUniform1f(glGetUniformLocation(program_, "uZoom"), ctx.in<float>(3));
        glUniform2f(glGetUniformLocation(program_, "uCenter"),
                    ctx.in<float>(4), ctx.in<float>(5));
    }
};

} // namespace oss
```

- [ ] **Step 3: Register the node**

In `src/app/Application.cpp`:

(a) Add the include near the other module includes:

```cpp
#include "modules/KaleidoscopeNode.h"
```

(b) In `makeNode`, add before `return nullptr;` (next to the Texture nodes, e.g. after `"Compositor"`):

```cpp
    if (type == "Kaleidoscope") return std::make_unique<KaleidoscopeNode>();
```

(The `"Kaleidoscope"` entry in the `nodeCategories` `"Texture"` list was already added in Task 4.)

- [ ] **Step 4: Add the gl_smoke scenario**

In `tests/gl_smoke.cpp`, first add the Kaleidoscope header to the include block at the top (next to the `#include "modules/ImageStreamerNode.h"` added in Task 4):

```cpp
#include "modules/KaleidoscopeNode.h"
```

Then add this scenario after the Image Streamer scenario (reusing `writeSplitFixture` and `readAtUV`):

```cpp
    // --- Scenario: Kaleidoscope folds an image (segments=4) ---
    {
        std::string fixture = writeSplitFixture();   // left red, right green
        if (fixture.empty()) { glfwTerminate(); return fail("write kaleidoscope fixture"); }

        Graph g;
        auto img = std::make_unique<ImageStreamerNode>();
        auto kal = std::make_unique<KaleidoscopeNode>();
        auto out = std::make_unique<OutputNode>();
        img->initGL(); kal->initGL(); out->initGL();
        img->inputDefault(0) = Value(fixture);
        kal->inputDefault(1) = Value(4.0f);   // segments = 4 -> 90-degree rotational symmetry
        int iId = g.addNode(std::move(img));
        int kId = g.addNode(std::move(kal));
        int oId = g.addNode(std::move(out));
        bool wired = g.connect(iId, 0, kId, 0) && g.connect(kId, 0, oId, 0);
        if (!wired) { std::remove(fixture.c_str()); glfwTerminate(); return fail("wire ImageStreamer->Kaleidoscope->Output"); }

        g.evaluate(1.0f / 60.0f);
        TexRef tex = dynamic_cast<OutputNode*>(g.findNode(oId))->current();
        std::remove(fixture.c_str());
        if (tex.id == 0) { glfwTerminate(); return fail("kaleidoscope output not produced"); }

        // (1) 4-fold rotational symmetry: UV (0.7,0.5) [angle 0] == UV (0.5,0.7) [angle 90deg].
        int r0, g0, b0, a0, r1, g1, b1, a1;
        readAtUV(tex, 0.7f, 0.5f, r0, g0, b0, a0);
        readAtUV(tex, 0.5f, 0.7f, r1, g1, b1, a1);
        if (!(near(r0, r1) && near(g0, g1) && near(b0, b1)))
            { glfwTerminate(); return fail("kaleidoscope not 4-fold rotationally symmetric"); }

        // (2) It actually folds: a point on the (red) left half samples the (green) right
        // half after folding. Raw input at UV (0.3,0.5) is red; kaleidoscope output is green.
        int r2, g2, b2, a2;
        readAtUV(tex, 0.3f, 0.5f, r2, g2, b2, a2);
        if (!(g2 > 200 && r2 < 60)) { glfwTerminate(); return fail("kaleidoscope did not fold the left half"); }
        std::fprintf(stderr, "gl_smoke OK: Kaleidoscope folds (symmetric + wedge-folded)\n");
    }
```

- [ ] **Step 5: Build and run gl_smoke**

Run: `cmake -S . -B build && cmake --build build -j gl_smoke && ctest --test-dir build -R gl_smoke --output-on-failure`
Expected: PASS, printing `gl_smoke OK: Kaleidoscope folds (symmetric + wedge-folded)`. (No-GL environments skip; confirm it built.)

- [ ] **Step 6: Build the app to confirm registration compiles**

Run: `cmake --build build -j shader_streamer`
Expected: builds clean; `Image Streamer` and `Kaleidoscope` appear under the Texture add-node menu.

- [ ] **Step 7: Commit**

```bash
git add shaders/kaleidoscope.frag src/modules/KaleidoscopeNode.h src/app/Application.cpp tests/gl_smoke.cpp
git commit -m "feat(modules): Kaleidoscope node (polar fold shader)"
```

---

### Task 6: Documentation

Record the new asset type and two nodes where the project documents its architecture.

**Files:**
- Modify: `CLAUDE.md` (add bullets under the node/architecture list)
- Modify: `README.md` (mention the nodes where other nodes are listed)

- [ ] **Step 1: Update CLAUDE.md**

In `CLAUDE.md`, in the **Assets / media library** bullet, update the tab list from four to five types (Audio/Video/Midi/Mesh → **+ Image**). Then add a new bullet in the node list (near the Compositor / Skybox texture nodes):

```markdown
- **Image Streamer / Kaleidoscope** — `ImageStreamerNode` (`src/modules/ImageStreamerNode.h`,
  header-only) loads a still image (a new **Image** `AssetType`, the fifth Assets tab) via the
  GL-free `gfx/ImageLoader` (an `stb_image` wrapper mirroring `VideoDecoder`, rows flipped
  bottom-up to match) and publishes it as a `TexRef`; it loads once on path change and
  republishes each frame. `KaleidoscopeNode` (`src/modules/KaleidoscopeNode.h`, header-only
  `ShaderNode`) folds an input texture into a mirrored pattern in `shaders/kaleidoscope.frag`
  (polar wedge fold with `segments`/`rotation`/`zoom`/`center` ports — wire `rotation` to an
  LFO to spin). Both live in the **Texture** category. `ImageLoader` is unit-tested in
  `core_tests`; both nodes are `gl_smoke`-checked (image round-trip + fold symmetry).
```

- [ ] **Step 2: Update README.md**

In `README.md`, find where nodes/features are listed (e.g. the Texture nodes or feature bullets) and add a short line:

```markdown
- **Image Streamer** — load a still image (PNG/JPG/…) as a texture, from the new **Image** asset library tab.
- **Kaleidoscope** — fold any texture into a mirrored kaleidoscopic pattern (segments, rotation, zoom, centre).
```

- [ ] **Step 3: Commit**

```bash
git add CLAUDE.md README.md
git commit -m "docs: Image assets + Image Streamer + Kaleidoscope"
```

---

## Final verification (after all tasks)

- [ ] Full reconfigure + build: `cmake -S . -B build && cmake --build build -j`
- [ ] All tests: `ctest --test-dir build --output-on-failure` — `core_tests` passes; `gl_smoke` passes (or cleanly skips where no GL context).
- [ ] Manual (optional, needs a display): run `./build/shader_streamer` from the repo root, open **View → Assets**, confirm the **Image** tab; add **Image Streamer** + **Kaleidoscope** from the Texture menu, point the streamer at a PNG, wire Streamer → Kaleidoscope → Output, and confirm the mirrored image.
- [ ] Then hand off to `superpowers:finishing-a-development-branch`.

## Notes for the implementer

- **Only stage the files each step names.** Never `git add -A`. Leave `build.sh`, `examples/`, `preferences.oss`, `project.oss`, `imgui.ini` untracked.
- **Re-run CMake configure** (`cmake -S . -B build`) whenever a task adds a source file to `CMakeLists.txt` (Tasks 3, and the gl_smoke edits in 4/5 don't add sources but the configure is harmless).
- The Image tab's per-tab UI state (filter/selection/anchor arrays) needs no code change — it is already sized by `kAssetTypeCount`.
- `gl_smoke` writes its fixture PNG into the CWD (repo root) and `std::remove`s it; if a run is interrupted, delete a stray `gl_smoke_image_fixture.png`.
