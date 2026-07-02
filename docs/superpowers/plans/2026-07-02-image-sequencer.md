# Image Sequencer Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add an **Image Sequencer** node (Texture category) that plays a folder of images in sequence, advancing one image every `duration` (seconds free-running, or beats when `sync` is on), with the folder chosen from a dropdown of the Assets library's image-containing folders.

**Architecture:** New GL-free helpers (`parentDir`, `uniqueAssetFolders`, `syncedImageIndex`, `listImagesInDir`) do the pure logic. A new `Port::folderPicker` flag + `Node::addImageFolderInput` reuse the existing asset-picker inline widget; the `NodeEditorPanel` popup lists folders instead of assets. The header-only `ImageSequencerNode` scans the folder on disk and decodes one image at a time (mirrors `ImageStreamerNode`).

**Tech Stack:** C++17 (`std::filesystem`), OpenGL 4.1, `stb_image` (via `gfx/ImageLoader`, already built), Dear ImGui, doctest (`core_tests`), headless GL (`gl_smoke`).

**Reference spec:** `docs/superpowers/specs/2026-07-02-image-sequencer-design.md`

**Conventions (from CLAUDE.md):**
- `src/core/` and `src/audio/` stay GL-free. `std::filesystem` scanning lives in `gfx/ImageLoader.cpp` (GL-free but in `gfx/`, like `VideoDecoder`). `core/ImageSequence.h` is pure math.
- Conventional Commits. Branch `feat/image-sequencer` (already created, off `develop`).
- Never `git add -A` / `git add .` — stage only the files each step names. Leave `build.sh`, `examples/`, `preferences.oss`, `project.oss`, `imgui.ini` untracked.
- Build: `cmake --build build --target <t> -j`; tests: `ctest --test-dir build --output-on-failure`. Re-run `cmake -S . -B build` after adding a source to `CMakeLists.txt`.

**Context — `core_tests` already links `src/gfx/ImageLoader.cpp` and has the `${stb_SOURCE_DIR}` include** (added by the prior Image-nodes feature), and `gl_smoke` already links `ImageLoader.cpp` and defines `STB_IMAGE_WRITE_IMPLEMENTATION`. So `listImagesInDir` (added to `ImageLoader.cpp` here) is available to both test targets with no new CMake wiring beyond registering the new test file.

---

### Task 1: `parentDir` helper + test

**Files:**
- Modify: `src/core/PathUtil.h`
- Test: `tests/test_path_util.cpp`

- [ ] **Step 1: Write the failing test**

Append to `tests/test_path_util.cpp` (after the last `TEST_CASE`):

```cpp
TEST_CASE("parentDir returns the directory portion") {
    CHECK(parentDir("a/b/c.png") == "a/b");
    CHECK(parentDir("c.png") == "");                 // no separator -> empty
    CHECK(parentDir("a\\b\\c.png") == "a\\b");        // backslash separator
    CHECK(parentDir("/a/b/") == "/a/b");              // trailing slash -> dir before it
    CHECK(parentDir("") == "");
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build build --target core_tests -j`
Expected: FAIL to compile — `parentDir` is not declared.

- [ ] **Step 3: Implement `parentDir`**

In `src/core/PathUtil.h`, add after the `fileBaseName` function (before `ensureExtension`):

```cpp
// The directory portion of a path (before the last '/' or '\\'); "" if there is no
// separator (a bare filename). Complements fileBaseName (same split point).
inline std::string parentDir(const std::string& path) {
    std::size_t slash = path.find_last_of("/\\");
    return (slash == std::string::npos) ? std::string() : path.substr(0, slash);
}
```

- [ ] **Step 4: Run to verify it passes**

Run: `cmake --build build --target core_tests -j && ctest --test-dir build -R core_tests --output-on-failure`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/core/PathUtil.h tests/test_path_util.cpp
git commit -m "feat(core): parentDir path helper"
```

---

### Task 2: `uniqueAssetFolders` helper + test

**Files:**
- Modify: `src/core/AssetTree.h`
- Test: `tests/test_asset_tree.cpp`

- [ ] **Step 1: Write the failing test**

Append to `tests/test_asset_tree.cpp` (the file already defines a `static ... ptrs(...)` helper and `using namespace oss;`):

```cpp
TEST_CASE("uniqueAssetFolders lists distinct parent dirs, sorted, bare files dropped") {
    std::vector<Asset> v = {
        {1, AssetType::Image, "a", "media/fire/a.png", {}},
        {2, AssetType::Image, "b", "media/fire/b.png", {}},
        {3, AssetType::Image, "c", "media/rain/c.png", {}},
        {4, AssetType::Image, "x", "other/x.png",      {}},
        {5, AssetType::Image, "y", "y.png",            {}},   // no dir -> dropped
    };
    std::vector<std::string> f = uniqueAssetFolders(ptrs(v));
    REQUIRE(f.size() == 3);
    CHECK(f[0] == "media/fire");
    CHECK(f[1] == "media/rain");
    CHECK(f[2] == "other");
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build build --target core_tests -j`
Expected: FAIL to compile — `uniqueAssetFolders` not declared.

- [ ] **Step 3: Implement `uniqueAssetFolders`**

In `src/core/AssetTree.h`, add the include `#include "core/PathUtil.h"` at the top (with the other includes), then add this function just before the closing `} // namespace oss`:

```cpp
// The distinct parent directories of `rows` (each asset's parentDir), dropping assets with
// no directory part, sorted ascending and de-duplicated. Drives the Image Sequencer's folder
// picker (the "image-containing folders").
inline std::vector<std::string> uniqueAssetFolders(const std::vector<const Asset*>& rows) {
    std::vector<std::string> out;
    for (const Asset* a : rows) {
        if (!a) continue;
        std::string dir = parentDir(a->path);
        if (!dir.empty()) out.push_back(std::move(dir));
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}
```

- [ ] **Step 4: Run to verify it passes**

Run: `cmake --build build --target core_tests -j && ctest --test-dir build -R core_tests --output-on-failure`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/core/AssetTree.h tests/test_asset_tree.cpp
git commit -m "feat(core): uniqueAssetFolders (distinct asset parent dirs)"
```

---

### Task 3: `syncedImageIndex` + `listImagesInDir` + tests

**Files:**
- Create: `src/core/ImageSequence.h`
- Modify: `src/gfx/ImageLoader.h`, `src/gfx/ImageLoader.cpp`
- Create: `tests/test_image_sequence.cpp`
- Modify: `CMakeLists.txt` (register the new test in `core_tests`)

- [ ] **Step 1: Write the GL-free sync-index header**

Create `src/core/ImageSequence.h`:

```cpp
#pragma once
#include <cmath>

namespace oss {

// The transport-synced image index: which of `count` images shows at song position `beats`,
// given `durationBeats` beats per image. Loops; handles negative beats. count<=0 -> 0.
inline int syncedImageIndex(double beats, float durationBeats, int count) {
    if (count <= 0) return 0;
    double d = (double)(durationBeats > 1e-4f ? durationBeats : 1e-4f);
    long step = (long)std::floor(beats / d);
    long idx  = ((step % count) + count) % count;   // positive modulo
    return (int)idx;
}

} // namespace oss
```

- [ ] **Step 2: Declare `listImagesInDir`**

In `src/gfx/ImageLoader.h`, add before the closing `} // namespace oss`:

```cpp
// List the image files directly in `dir` (non-recursive), sorted ascending by path.
// Filters to the extensions loadImage decodes. Returns empty on a missing/unreadable dir
// (never throws).
std::vector<std::string> listImagesInDir(const std::string& dir);
```

- [ ] **Step 3: Implement `listImagesInDir`**

In `src/gfx/ImageLoader.cpp`, add these includes under the existing ones (after `#include "gfx/ImageLoader.h"`):

```cpp
#include <algorithm>
#include <cctype>
#include <filesystem>
```

Then add before the closing `} // namespace oss`:

```cpp
static bool isImageExt(std::string ext) {
    for (char& c : ext) c = (char)std::tolower((unsigned char)c);
    return ext == "png" || ext == "jpg" || ext == "jpeg" || ext == "bmp" ||
           ext == "tga" || ext == "gif" || ext == "hdr" || ext == "psd";
}

std::vector<std::string> listImagesInDir(const std::string& dir) {
    std::vector<std::string> out;
    if (dir.empty()) return out;
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::directory_iterator it(dir, ec), end;
    if (ec) return out;                                  // missing / unreadable
    for (; it != end; it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec) || ec) continue;
        std::string ext = it->path().extension().string();   // e.g. ".PNG"
        if (!ext.empty() && ext[0] == '.') ext.erase(0, 1);
        if (isImageExt(ext)) out.push_back(it->path().string());
    }
    std::sort(out.begin(), out.end());   // same dir -> path sort == filename sort
    return out;
}
```

- [ ] **Step 4: Write the failing tests**

Create `tests/test_image_sequence.cpp`:

```cpp
#include <doctest/doctest.h>
#include "core/ImageSequence.h"
#include "gfx/ImageLoader.h"
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace oss;

TEST_CASE("syncedImageIndex wraps over the beat position") {
    CHECK(syncedImageIndex(0.0, 1.0f, 3) == 0);
    CHECK(syncedImageIndex(0.9, 1.0f, 3) == 0);
    CHECK(syncedImageIndex(1.0, 1.0f, 3) == 1);
    CHECK(syncedImageIndex(2.0, 1.0f, 3) == 2);
    CHECK(syncedImageIndex(3.0, 1.0f, 3) == 0);   // wrap
    CHECK(syncedImageIndex(5.5, 1.0f, 3) == 2);   // floor(5.5)=5 -> 5 % 3 = 2
    CHECK(syncedImageIndex(10.0, 1.0f, 0) == 0);  // no images
    CHECK(syncedImageIndex(-1.0, 1.0f, 3) == 2);  // negative wraps into range
}

TEST_CASE("listImagesInDir returns sorted image files, ignoring non-images") {
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() / "oss_imgseq_test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    auto touch = [&](const char* n){ std::ofstream(dir / n) << "x"; };
    touch("b.png"); touch("a.png"); touch("c.jpg"); touch("note.txt"); touch("d.PNG");

    std::vector<std::string> got = listImagesInDir(dir.string());
    fs::remove_all(dir);

    REQUIRE(got.size() == 4);   // a.png, b.png, c.jpg, d.PNG  (note.txt excluded)
    CHECK(fs::path(got[0]).filename() == "a.png");
    CHECK(fs::path(got[1]).filename() == "b.png");
    CHECK(fs::path(got[2]).filename() == "c.jpg");
    CHECK(fs::path(got[3]).filename() == "d.PNG");
}

TEST_CASE("listImagesInDir on a missing or empty directory is empty, not an error") {
    CHECK(listImagesInDir("/no/such/dir/oss_zzz").empty());
    CHECK(listImagesInDir("").empty());
}
```

- [ ] **Step 5: Register the test in CMake**

In `CMakeLists.txt`, in the `core_tests` source list, after `tests/test_image_loader.cpp` add:

```cmake
  tests/test_image_sequence.cpp
```

- [ ] **Step 6: Run to verify it passes**

Run: `cmake -S . -B build && cmake --build build --target core_tests -j && ctest --test-dir build -R core_tests --output-on-failure`
Expected: PASS (reconfigure needed — a source was added).

- [ ] **Step 7: Commit**

```bash
git add src/core/ImageSequence.h src/gfx/ImageLoader.h src/gfx/ImageLoader.cpp tests/test_image_sequence.cpp CMakeLists.txt
git commit -m "feat(gfx): listImagesInDir + syncedImageIndex (image-sequence logic)"
```

---

### Task 4: folder-picker port + editor popup branch

**Files:**
- Modify: `src/core/Port.h`
- Modify: `src/core/Node.h`
- Modify: `src/ui/NodeEditorPanel.cpp`

- [ ] **Step 1: Add the `folderPicker` port flag**

In `src/core/Port.h`, add after the `assetType` member (inside `struct Port`, after the `assetBacked`/`assetType` lines):

```cpp
    // A folder-picker String (also assetBacked): its ▾ lists the distinct folders that
    // contain assets of `assetType`, and picking one copies the folder path here (not an
    // asset path). Set via Node::addImageFolderInput. Defaults off.
    bool      folderPicker = false;
```

- [ ] **Step 2: Add `addImageFolderInput`**

In `src/core/Node.h`, add after the `addAssetInput` method:

```cpp
    // A String input rendered like an asset field (editable + ▾), but the picker lists the
    // distinct folders containing assets of `type` (see NodeEditorPanel). Copy-path model:
    // selecting copies the folder path into this input. Used by Image Sequencer.
    void addImageFolderInput(std::string n, AssetType type = AssetType::Image) {
        Port p;
        p.name         = std::move(n);
        p.direction    = Direction::Input;
        p.type         = PortType::String;
        p.defaultValue = Value(std::string());
        p.assetBacked  = true;
        p.folderPicker = true;
        p.assetType    = type;
        inputs_.push_back(std::move(p));
    }
```

- [ ] **Step 3: Branch the editor popup on `folderPicker`**

In `src/ui/NodeEditorPanel.cpp`, add the include near the other `core/` includes at the top:

```cpp
#include "core/AssetTree.h"
```

Then replace the existing asset-picker block. Find:

```cpp
            } else if (port.type == PortType::String && port.assetBacked) {
                // Pick a library asset of this type; copy its path into the field.
                std::vector<const Asset*> assets = graph.assets().byType(port.assetType);
                if (assets.empty()) {
                    ImGui::TextDisabled("No %s assets -- add them in the Assets window",
                                        assetTypeName(port.assetType));
                } else {
                    const std::string cur = std::get<std::string>(v);
                    for (const Asset* a : assets) {
                        ImGui::PushID(a->id);
                        std::string label = a->label.empty() ? a->path : a->label;
                        if (label.empty()) label = "(unnamed)";
                        // Checkmark by path (copy-path model: the field stores a path, not an
                        // id), so a path match -- not asset identity -- marks the current row.
                        if (ImGui::Selectable(label.c_str(), a->path == cur)) {
                            v = Value(a->path);
                            ImGui::CloseCurrentPopup();
                        }
                        ImGui::PopID();
                    }
                }
            } else {   // a Float choice port: a list of its labels
```

Replace it with (adds the `folderPicker` branch, keeps the asset branch unchanged inside an `else`):

```cpp
            } else if (port.type == PortType::String && port.assetBacked && port.folderPicker) {
                // Pick a folder that contains assets of this type; copy the folder path in.
                std::vector<std::string> folders =
                    uniqueAssetFolders(graph.assets().byType(port.assetType));
                if (folders.empty()) {
                    ImGui::TextDisabled("No %s folders -- add images in the Assets window",
                                        assetTypeName(port.assetType));
                } else {
                    const std::string cur = std::get<std::string>(v);
                    for (const std::string& f : folders) {
                        if (ImGui::Selectable(f.c_str(), f == cur)) {
                            v = Value(f);
                            ImGui::CloseCurrentPopup();
                        }
                    }
                }
            } else if (port.type == PortType::String && port.assetBacked) {
                // Pick a library asset of this type; copy its path into the field.
                std::vector<const Asset*> assets = graph.assets().byType(port.assetType);
                if (assets.empty()) {
                    ImGui::TextDisabled("No %s assets -- add them in the Assets window",
                                        assetTypeName(port.assetType));
                } else {
                    const std::string cur = std::get<std::string>(v);
                    for (const Asset* a : assets) {
                        ImGui::PushID(a->id);
                        std::string label = a->label.empty() ? a->path : a->label;
                        if (label.empty()) label = "(unnamed)";
                        // Checkmark by path (copy-path model: the field stores a path, not an
                        // id), so a path match -- not asset identity -- marks the current row.
                        if (ImGui::Selectable(label.c_str(), a->path == cur)) {
                            v = Value(a->path);
                            ImGui::CloseCurrentPopup();
                        }
                        ImGui::PopID();
                    }
                }
            } else {   // a Float choice port: a list of its labels
```

- [ ] **Step 4: Build the app to verify it compiles**

Run: `cmake --build build --target shader_streamer -j`
Expected: builds clean. (The inline widget already renders `assetBacked` String inputs with the ▾, so no `PortWidgets` change is needed.)

- [ ] **Step 5: Commit**

```bash
git add src/core/Port.h src/core/Node.h src/ui/NodeEditorPanel.cpp
git commit -m "feat(ui): folder-picker port + editor popup lists asset folders"
```

---

### Task 5: `ImageSequencerNode` + registration + gl_smoke

**Files:**
- Create: `src/modules/ImageSequencerNode.h`
- Modify: `src/app/Application.cpp`
- Modify: `tests/gl_smoke.cpp`

- [ ] **Step 1: Write the node**

Create `src/modules/ImageSequencerNode.h`:

```cpp
#pragma once
#include <glad/gl.h>
#include <string>
#include <vector>
#include "core/Node.h"
#include "core/PathUtil.h"
#include "core/ImageSequence.h"
#include "gfx/ImageLoader.h"

namespace oss {

// Plays a folder of images in sequence, one every `duration` (seconds when free-running,
// beats when synced). The folder is chosen from the Assets library's image folders; the node
// scans it on disk. Holds one GL texture and decodes the next image only when the index
// changes (bounded memory). Mirrors ImageStreamerNode.
class ImageSequencerNode : public Node {
public:
    ImageSequencerNode() : Node("Image Sequencer") {
        addImageFolderInput("folder");
        addInput("duration", PortType::Float, 1.0f, 0.05f, 60.0f);   // seconds or beats
        addInput("sync", PortType::Bool, false);
        addOutput("image", PortType::Texture);
    }
    ~ImageSequencerNode() override { if (tex_) glDeleteTextures(1, &tex_); }

    void initGL() override {}   // texture allocated lazily on first load

    void evaluate(EvalContext& ctx) override {
        const std::string& folder = ctx.in<std::string>(0);
        float duration = ctx.in<float>(1);
        bool  sync     = ctx.in<bool>(2);
        if (duration < 0.01f) duration = 0.01f;

        if (folder != folder_) {
            folder_  = folder;
            files_   = listImagesInDir(folder);
            index_   = -1;          // force a (re)load
            cur_     = 0;
            elapsed_ = 0.0f;
        }

        int n = (int)files_.size();
        if (n == 0) {
            status_ = folder_.empty() ? std::string() : ("no images in " + folder_);
            haveTex_ = false;
            ctx.out<TexRef>(0, TexRef{});
            return;
        }

        int target;
        if (sync) {
            double beats = ctx.transport ? ctx.transport->beats() : 0.0;
            target = syncedImageIndex(beats, duration, n);
        } else {
            elapsed_ += ctx.dt;
            while (elapsed_ >= duration) { elapsed_ -= duration; cur_ = (cur_ + 1) % n; }
            target = cur_;
        }
        if (target >= n) target = n - 1;   // folder shrank between frames

        if (target != index_) load(target);
        ctx.out<TexRef>(0, haveTex_ ? TexRef{ tex_, w_, h_ } : TexRef{});
    }

    std::string statusLine() const override { return status_; }

private:
    void load(int i) {
        index_ = i;
        std::string err;
        ImageData img = loadImage(files_[(std::size_t)i], err);
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
        status_ = std::to_string(i + 1) + "/" + std::to_string(files_.size())
                + "  " + fileBaseName(files_[(std::size_t)i]);
    }

    GLuint tex_ = 0;
    int    w_ = 0, h_ = 0;
    bool   haveTex_ = false;
    std::vector<std::string> files_;
    std::string folder_;
    std::string status_;
    int   index_   = -1;    // currently-loaded image index (-1 = none)
    int   cur_     = 0;     // free-running position
    float elapsed_ = 0.0f;  // free-running seconds accumulator
};

} // namespace oss
```

- [ ] **Step 2: Register the node**

In `src/app/Application.cpp`:

(a) Add the include next to the other Texture-node includes (near `#include "modules/ImageStreamerNode.h"`):

```cpp
#include "modules/ImageSequencerNode.h"
```

(b) In `makeNode`, after the `"Image Streamer"` line:

```cpp
    if (type == "Image Sequencer") return std::make_unique<ImageSequencerNode>();
```

(c) In `nodeCategories`, add `"Image Sequencer"` to the `"Texture"` list (after `"Image Streamer"`):

```cpp
        { "Texture", { "Colour", "Image Streamer", "Image Sequencer", "Video", "Mix", "Compositor", "Kaleidoscope", "Recorder", "Output" } },
```

- [ ] **Step 3: Add the gl_smoke scenario**

In `tests/gl_smoke.cpp`:

(a) Add the include and `<filesystem>` near the other module includes at the top (next to `#include "modules/ImageStreamerNode.h"`):

```cpp
#include "modules/ImageSequencerNode.h"
#include <filesystem>
```

(b) After the `writeSplitFixture` helper (added by the prior feature), add a solid-colour PNG writer + a temp-folder builder:

```cpp
// Write a 16x16 solid-colour PNG at `path`. Returns true on success.
static bool writeSolidPNG(const std::string& path, unsigned char r, unsigned char g, unsigned char b) {
    const int W = 16, H = 16;
    std::vector<unsigned char> px((size_t)W * H * 4);
    for (size_t i = 0; i < px.size(); i += 4) { px[i]=r; px[i+1]=g; px[i+2]=b; px[i+3]=255; }
    return stbi_write_png(path.c_str(), W, H, 4, px.data(), W * 4) != 0;
}
```

(c) Add this scenario after the Kaleidoscope scenario (before `Scenario 2`):

```cpp
    // --- Scenario: Image Sequencer cycles a folder of images ---
    {
        namespace fs = std::filesystem;
        fs::path dir = fs::temp_directory_path() / "oss_imgseq_smoke";
        fs::remove_all(dir);
        fs::create_directories(dir);
        bool wrote = writeSolidPNG((dir / "0.png").string(), 255, 0, 0)     // red
                  && writeSolidPNG((dir / "1.png").string(), 0, 255, 0)     // green
                  && writeSolidPNG((dir / "2.png").string(), 0, 0, 255);    // blue
        if (!wrote) { fs::remove_all(dir); glfwTerminate(); return fail("write sequencer fixtures"); }

        // Port-flag check (pure CPU; the ctor doesn't touch GL).
        { ImageSequencerNode probe; const Port& p = probe.inputs()[0];
          if (!(p.type == PortType::String && p.assetBacked && p.folderPicker && p.assetType == AssetType::Image))
            { fs::remove_all(dir); glfwTerminate(); return fail("Sequencer.folder not a folder picker"); } }

        Graph g;
        auto seq = std::make_unique<ImageSequencerNode>();
        auto out = std::make_unique<OutputNode>();
        seq->initGL(); out->initGL();
        seq->inputDefault(0) = Value(dir.string());   // folder
        seq->inputDefault(1) = Value(1.0f);           // duration = 1s
        seq->inputDefault(2) = Value(false);          // sync off (free-running)
        int sId = g.addNode(std::move(seq));
        int oId = g.addNode(std::move(out));
        if (!g.connect(sId, 0, oId, 0)) { fs::remove_all(dir); glfwTerminate(); return fail("connect Sequencer->Output"); }

        auto centreIs = [&](int R, int G, int B)->bool {
            TexRef t = dynamic_cast<OutputNode*>(g.findNode(oId))->current();
            if (t.id == 0) return false;
            int r, gg, b, a; readCentre(t, r, gg, b, a);
            return near(r, R) && near(gg, G) && near(b, B);
        };

        g.evaluate(1.0f / 60.0f);                      // shows image 0 (red)
        if (!centreIs(255, 0, 0)) { fs::remove_all(dir); glfwTerminate(); return fail("sequencer frame 0 not red"); }
        g.evaluate(1.1f);                              // +1.1s -> image 1 (green)
        if (!centreIs(0, 255, 0)) { fs::remove_all(dir); glfwTerminate(); return fail("sequencer frame 1 not green"); }
        g.evaluate(1.1f);                              // -> image 2 (blue)
        if (!centreIs(0, 0, 255)) { fs::remove_all(dir); glfwTerminate(); return fail("sequencer frame 2 not blue"); }
        g.evaluate(1.1f);                              // -> wrap to image 0 (red)
        if (!centreIs(255, 0, 0)) { fs::remove_all(dir); glfwTerminate(); return fail("sequencer did not wrap to red"); }

        fs::remove_all(dir);
        std::fprintf(stderr, "gl_smoke OK: Image Sequencer cycled a folder (red->green->blue->wrap)\n");
    }
```

- [ ] **Step 4: Build (app + gl_smoke) and run gl_smoke**

Run: `cmake --build build --target shader_streamer gl_smoke -j && ctest --test-dir build -R gl_smoke --output-on-failure`
Expected: both build; `gl_smoke` PASSES, printing `gl_smoke OK: Image Sequencer cycled a folder ...`. (No-GL environments skip; confirm it built.)

- [ ] **Step 5: Commit**

```bash
git add src/modules/ImageSequencerNode.h src/app/Application.cpp tests/gl_smoke.cpp
git commit -m "feat(modules): Image Sequencer node (folder of images -> texture)"
```

---

### Task 6: Documentation

**Files:**
- Modify: `CLAUDE.md`
- Modify: `README.md`

- [ ] **Step 1: Update CLAUDE.md**

In `CLAUDE.md`, in the **Image Streamer / Kaleidoscope** bullet (added by the prior feature), append a sentence about the sequencer, and note the folder-picker port. Add this text to the end of that bullet:

```markdown
  A sibling **Image Sequencer** (`src/modules/ImageSequencerNode.h`, header-only) plays a
  *folder* of images in sequence: its `folder` input is a **folder picker** (a new
  `Port::folderPicker` built by `Node::addImageFolderInput`; the editor's `NodePopup` lists
  `uniqueAssetFolders(byType(Image))` — the distinct image-containing folders in the library —
  and copies the chosen folder path in). The node scans that folder on disk (`listImagesInDir`,
  `std::filesystem` in `gfx/ImageLoader`) and advances one image every `duration` — seconds when
  free-running, or beats when `sync` is on (`syncedImageIndex`, stateless from `transport.beats()`,
  in GL-free `core/ImageSequence.h`) — decoding one image at a time. `parentDir`/`uniqueAssetFolders`/
  `listImagesInDir`/`syncedImageIndex` are unit-tested; the cycle is `gl_smoke`-checked.
```

- [ ] **Step 2: Update README.md**

In `README.md`, in the Texture-nodes table, add a row after the **Image Streamer** row:

```markdown
| **Image Sequencer** | play a folder of images in sequence, one every `duration` (seconds, or beats when `sync` is on); pick the folder from your Image assets' folders |
```

- [ ] **Step 3: Commit**

```bash
git add CLAUDE.md README.md
git commit -m "docs: Image Sequencer node"
```

---

## Final verification (after all tasks)

- [ ] Full reconfigure + build: `cmake -S . -B build && cmake --build build -j`
- [ ] All tests: `ctest --test-dir build --output-on-failure` — `core_tests` + `gl_smoke` pass (or gl_smoke cleanly skips where no GL context).
- [ ] Manual (optional, needs a display): run `./build/shader_streamer`, add **Image Sequencer** from the Texture menu, click the `folder` ▾ (shows your Image assets' folders), pick one, wire Sequencer → Output, and watch it cycle; toggle `sync` and press play to lock it to the transport.
- [ ] Hand off to `superpowers:finishing-a-development-branch`.

## Notes for the implementer

- **Only stage the files each step names.** Never `git add -A`. Leave `build.sh`, `examples/`, `preferences.oss`, `project.oss`, `imgui.ini` untracked.
- Re-run `cmake -S . -B build` after Task 3 (adds a test source). Other tasks don't add sources.
- `core_tests` and `gl_smoke` already link `ImageLoader.cpp` and have the `stb` include (prior feature), so `listImagesInDir` is available to both without new wiring.
- `std::filesystem` needs no extra link on the project's toolchains (AppleClang, GCC ≥ 9, MSVC).
- The gl_smoke fixtures write to the OS temp dir and are `remove_all`'d; if a run is interrupted, a stray `oss_imgseq_smoke` temp folder is harmless.
