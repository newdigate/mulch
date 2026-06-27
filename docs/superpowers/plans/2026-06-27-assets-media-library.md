# Assets / Media Library Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A per-project **Assets** window — a media library (Audio / Video / MIDI / 3D tabs) where files can be added, edited (label + path), and removed, persisted with the project.

**Architecture:** A GL-free `core/AssetLibrary` data model (each asset = stable unique id + type + label + path) owned by `Graph` and persisted through `ProjectFile`, exactly like `AutomationStore`. An ImGui `ui/AssetsPanel` renders four tabbed tables; a `ui/FileDialog` wraps NFD for the Browse button. A toolbar button toggles the window. This is **Phase 1**; rewiring node `file` controls into asset dropdowns is a separate Phase-2 plan.

**Tech Stack:** C++17, Dear ImGui, `nativefiledialog-extended` (NFD) via CMake FetchContent, doctest (`core_tests`).

**Spec:** `docs/superpowers/specs/2026-06-27-assets-media-library-design.md`

---

## File Structure

| File | Responsibility |
|---|---|
| `src/core/AssetLibrary.h` / `.cpp` | **new** — `Asset{id,type,label,path}` + `AssetLibrary` (add/remove/find/setLabel/setPath/byType/all/clear/load). GL-free. |
| `tests/test_assets.cpp` | **new** — `core_tests`: library ops, Graph ownership, ProjectFile round-trip. |
| `src/core/Graph.h` / `.cpp` | **modify** — own an `AssetLibrary`; `assets()` accessors; `clear()` empties it. |
| `src/core/ProjectFile.h` / `.cpp` | **modify** — `ProjectDoc::assets`; serialize/parse `asset`/`alabel`/`apath`; capture/restore. |
| `src/ui/FileDialog.h` / `.cpp` | **new** — `openFileDialog(title, filters)` → path; NFD confined to the `.cpp`. |
| `src/ui/AssetsPanel.h` / `.cpp` | **new** — the "Assets" window: 4 tabs, a table per tab. |
| `src/ui/TransportBar.h` / `.cpp` | **modify** — `ProjectBarIO::showAssets` + an "Assets" toolbar button. |
| `src/app/Application.h` / `.cpp` | **modify** — own `AssetsPanel`; draw it; `showAssets_` toggle. |
| `CMakeLists.txt` | **modify** — NFD fetch; new sources into the right targets. |
| `CLAUDE.md`, `README.md` | **modify** — docs. |

---

## Task 1: `core/AssetLibrary` data model + unit tests

**Files:**
- Create: `src/core/AssetLibrary.h`, `src/core/AssetLibrary.cpp`
- Create: `tests/test_assets.cpp`
- Modify: `CMakeLists.txt` (add both to `core_tests`)

- [ ] **Step 1: Write the failing tests**

Create `tests/test_assets.cpp`:

```cpp
#include <doctest/doctest.h>
#include "core/AssetLibrary.h"

using namespace oss;

TEST_CASE("AssetLibrary add returns unique increasing ids") {
    AssetLibrary lib;
    int a = lib.add(AssetType::Audio, "kick", "k.wav");
    int b = lib.add(AssetType::Audio, "snare", "s.wav");
    CHECK(a == 1);
    CHECK(b == 2);
    CHECK(b > a);
}

TEST_CASE("AssetLibrary byType filters by type and preserves insertion order") {
    AssetLibrary lib;
    int aud0 = lib.add(AssetType::Audio, "a0", "a0");
    lib.add(AssetType::Mesh, "m0", "m0");
    int aud1 = lib.add(AssetType::Audio, "a1", "a1");
    auto audio = lib.byType(AssetType::Audio);
    REQUIRE(audio.size() == 2);
    CHECK(audio[0]->id == aud0);
    CHECK(audio[1]->id == aud1);
    CHECK(lib.byType(AssetType::Video).empty());
}

TEST_CASE("AssetLibrary find/setLabel/setPath mutate the right asset; no-op on a bad id") {
    AssetLibrary lib;
    int id = lib.add(AssetType::Midi, "old", "old.mid");
    lib.setLabel(id, "new");
    lib.setPath(id, "new.mid");
    REQUIRE(lib.find(id) != nullptr);
    CHECK(lib.find(id)->label == "new");
    CHECK(lib.find(id)->path == "new.mid");
    lib.setLabel(999, "x");          // absent id: must be a harmless no-op
    CHECK(lib.find(999) == nullptr);
}

TEST_CASE("AssetLibrary remove deletes only its id and never reuses ids") {
    AssetLibrary lib;
    int a = lib.add(AssetType::Audio, "a", "a");
    int b = lib.add(AssetType::Audio, "b", "b");
    lib.remove(a);
    CHECK(lib.find(a) == nullptr);
    CHECK(lib.find(b) != nullptr);
    int c = lib.add(AssetType::Audio, "c", "c");
    CHECK(c > b);                     // monotonic; the removed id is not reused
    CHECK(c != a);
}

TEST_CASE("AssetLibrary clear empties the library") {
    AssetLibrary lib;
    lib.add(AssetType::Audio, "a", "a");
    lib.clear();
    CHECK(lib.all().empty());
}

TEST_CASE("AssetLibrary load adopts ids verbatim and advances nextId past the max") {
    AssetLibrary lib;
    std::vector<Asset> in = {
        Asset{5, AssetType::Audio, "five", "5.wav"},
        Asset{9, AssetType::Mesh,  "nine", "9.obj"},
    };
    lib.load(in);
    REQUIRE(lib.find(5) != nullptr);
    CHECK(lib.find(5)->label == "five");
    CHECK(lib.find(9)->type == AssetType::Mesh);
    int next = lib.add(AssetType::Video, "v", "v.mp4");
    CHECK(next == 10);                // max(id)+1; never collides with a loaded id
}
```

- [ ] **Step 2: Register the test + source in CMake**

In `CMakeLists.txt`, add to the `core_tests` `add_executable(...)` list — put the test line after `tests/test_drum_machine.cpp` and the source line after `src/core/ProjectFile.cpp`:

```cmake
  tests/test_assets.cpp
```
```cmake
  src/core/AssetLibrary.cpp
```

- [ ] **Step 3: Run the tests to verify they fail to build**

Run: `cmake --build build -j --target core_tests`
Expected: FAIL — `fatal error: 'core/AssetLibrary.h' file not found`.

- [ ] **Step 4: Write `AssetLibrary.h`**

Create `src/core/AssetLibrary.h`:

```cpp
#pragma once
#include <string>
#include <vector>

namespace oss {

// The four Assets-window tabs, in tab order. Persisted as the int 0..3.
enum class AssetType { Audio, Video, Midi, Mesh };

struct Asset {
    int         id = 0;                 // unique within the library; monotonic; never reused
    AssetType   type = AssetType::Audio;
    std::string label;                  // human name (editable; may be empty / duplicated)
    std::string path;                   // file path (editable; may be empty / duplicated)
};

// A per-project media library: media files grouped by type, each with a stable unique id
// plus an editable label and path. GL-free. Owned by Graph and persisted via ProjectFile;
// Phase-2 node controls will reference an asset by id, so ids must be stable across edits
// and preserved verbatim across a save/load.
class AssetLibrary {
public:
    int  add(AssetType type, std::string label, std::string path);   // returns the fresh id
    void remove(int id);                                             // no-op if id is absent
    Asset*       find(int id);                                        // nullptr if absent
    const Asset* find(int id) const;
    void setLabel(int id, std::string label);                        // no-op if id is absent
    void setPath(int id, std::string path);                          // no-op if id is absent

    // All assets of one type in insertion order (drives a tab now, Phase-2 dropdowns later).
    std::vector<const Asset*> byType(AssetType type) const;

    const std::vector<Asset>& all() const { return assets_; }
    void clear();                                                    // empty; nextId_ -> 1
    void load(std::vector<Asset> assets);                            // adopt; nextId_ = max(id)+1

private:
    std::vector<Asset> assets_;
    int nextId_ = 1;                                                 // monotonic; never reused
};

} // namespace oss
```

- [ ] **Step 5: Write `AssetLibrary.cpp`**

Create `src/core/AssetLibrary.cpp`:

```cpp
#include "core/AssetLibrary.h"
#include <algorithm>
#include <utility>

namespace oss {

int AssetLibrary::add(AssetType type, std::string label, std::string path) {
    int id = nextId_++;
    assets_.push_back(Asset{id, type, std::move(label), std::move(path)});
    return id;
}

void AssetLibrary::remove(int id) {
    assets_.erase(std::remove_if(assets_.begin(), assets_.end(),
                                 [id](const Asset& a) { return a.id == id; }),
                  assets_.end());
}

Asset* AssetLibrary::find(int id) {
    for (Asset& a : assets_) if (a.id == id) return &a;
    return nullptr;
}
const Asset* AssetLibrary::find(int id) const {
    for (const Asset& a : assets_) if (a.id == id) return &a;
    return nullptr;
}

void AssetLibrary::setLabel(int id, std::string label) {
    if (Asset* a = find(id)) a->label = std::move(label);
}
void AssetLibrary::setPath(int id, std::string path) {
    if (Asset* a = find(id)) a->path = std::move(path);
}

std::vector<const Asset*> AssetLibrary::byType(AssetType type) const {
    std::vector<const Asset*> out;
    for (const Asset& a : assets_) if (a.type == type) out.push_back(&a);
    return out;
}

void AssetLibrary::clear() {
    assets_.clear();
    nextId_ = 1;
}

void AssetLibrary::load(std::vector<Asset> assets) {
    assets_ = std::move(assets);
    int maxId = 0;
    for (const Asset& a : assets_) maxId = std::max(maxId, a.id);
    nextId_ = maxId + 1;
}

} // namespace oss
```

- [ ] **Step 6: Run the tests to verify they pass**

Run: `cmake --build build -j --target core_tests && ./build/core_tests -tc="AssetLibrary*"`
Expected: PASS (6 AssetLibrary test cases, 0 failures).

- [ ] **Step 7: Commit**

```bash
git add src/core/AssetLibrary.h src/core/AssetLibrary.cpp tests/test_assets.cpp CMakeLists.txt
git commit -m "feat(core): AssetLibrary media model (id/type/label/path) + tests"
```

---

## Task 2: `Graph` owns the `AssetLibrary`

**Files:**
- Modify: `src/core/Graph.h` (include, member, accessors)
- Modify: `src/core/Graph.cpp` (`clear()` empties the library)
- Modify: `CMakeLists.txt` (add `AssetLibrary.cpp` to the app + `gl_smoke` targets — both compile `Graph.cpp`, which now references the library)
- Test: `tests/test_assets.cpp` (append a Graph-ownership case)

- [ ] **Step 1: Write the failing test**

Append to `tests/test_assets.cpp` (and add `#include "core/Graph.h"` under the existing include at the top):

```cpp
TEST_CASE("Graph owns an AssetLibrary and clear() empties it") {
    Graph g;
    g.assets().add(AssetType::Audio, "k", "k.wav");
    CHECK(g.assets().all().size() == 1);
    g.clear();
    CHECK(g.assets().all().empty());
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build build -j --target core_tests`
Expected: FAIL — `'class oss::Graph' has no member named 'assets'`.

- [ ] **Step 3: Add the member + accessors to `Graph.h`**

In `src/core/Graph.h`, add the include beside the others near the top:

```cpp
#include "core/AssetLibrary.h"
```

Add the accessors right after the `automation()` accessors (after line ~25):

```cpp
    // Per-project media library (the Assets window). Saved/loaded with the project.
    AssetLibrary&       assets()       { return assets_; }
    const AssetLibrary& assets() const { return assets_; }
```

Add the member beside `AutomationStore automation_;` in the private section:

```cpp
    AssetLibrary assets_;
```

- [ ] **Step 4: Empty the library in `Graph::clear()`**

In `src/core/Graph.cpp`, inside `void Graph::clear()`, add `assets_.clear();` (right after `automation_.clear();`):

```cpp
void Graph::clear() {
    connections_.clear();
    nodes_.clear();
    outputs_.clear();
    automation_.clear();
    assets_.clear();
    markDirty();
    // nextId_ is intentionally NOT reset.
}
```

- [ ] **Step 5: Add `AssetLibrary.cpp` to the app + gl_smoke targets**

In `CMakeLists.txt`, the app `APP_SOURCES` list and the `gl_smoke` `add_executable` both compile `src/core/Graph.cpp`, which now references `AssetLibrary`. Add the source to each so they link.

In `APP_SOURCES`, after `src/core/ProjectFile.cpp`:
```cmake
  src/core/AssetLibrary.cpp
```

In the `gl_smoke` `add_executable(...)`, after its `src/core/ProjectFile.cpp` line:
```cmake
  src/core/AssetLibrary.cpp
```

- [ ] **Step 6: Run the test + verify the whole build still links**

Run: `cmake --build build -j && ./build/core_tests -tc="Graph owns*"`
Expected: PASS; `shader_streamer` and `gl_smoke` link without undefined-symbol errors.

- [ ] **Step 7: Commit**

```bash
git add src/core/Graph.h src/core/Graph.cpp tests/test_assets.cpp CMakeLists.txt
git commit -m "feat(core): Graph owns the AssetLibrary; clear() empties it"
```

---

## Task 3: ProjectFile persistence (capture / restore / codec)

**Files:**
- Modify: `src/core/ProjectFile.h` (include + `ProjectDoc::assets`)
- Modify: `src/core/ProjectFile.cpp` (serialize, parse, capture, restore)
- Test: `tests/test_assets.cpp` (codec round-trip + capture/restore + empty-list)

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_assets.cpp`. Add these includes at the top of the file (under the existing ones):

```cpp
#include "core/ProjectFile.h"
#include <memory>
#include <string>
```

Then add the cases:

```cpp
TEST_CASE("ProjectFile serialize/parse round-trips assets (spaces, empty label, backslash)") {
    ProjectDoc d;
    d.assets = {
        Asset{1, AssetType::Audio, "kick drum", "samples/kick drum.wav"},
        Asset{2, AssetType::Mesh,  "",           "models/a b c.obj"},   // empty label omits alabel
        Asset{3, AssetType::Midi,  "back\\slash","x.mid"},               // backslash survives escape
    };
    std::string text = serializeProject(d);
    ProjectDoc out;
    REQUIRE(parseProject(text, out));
    REQUIRE(out.assets.size() == 3);
    CHECK(out.assets[0].id == 1);
    CHECK(out.assets[0].type == AssetType::Audio);
    CHECK(out.assets[0].label == "kick drum");
    CHECK(out.assets[0].path == "samples/kick drum.wav");
    CHECK(out.assets[1].label == "");                 // empty round-trips
    CHECK(out.assets[1].type == AssetType::Mesh);
    CHECK(out.assets[1].path == "models/a b c.obj");
    CHECK(out.assets[2].label == "back\\slash");
}

TEST_CASE("ProjectFile without asset lines loads an empty asset list") {
    ProjectDoc out;
    REQUIRE(parseProject("oss-project 1\ntransport 120 4 0 0 4 8\n", out));
    CHECK(out.assets.empty());
}

TEST_CASE("captureProject / restoreProject carry assets through a Graph") {
    Graph g;
    g.assets().add(AssetType::Audio, "kick", "k.wav");
    g.assets().add(AssetType::Video, "clip", "c.mp4");
    ProjectDoc d = captureProject(g);
    REQUIRE(d.assets.size() == 2);

    Graph g2;
    auto factory = [](const std::string&) -> std::unique_ptr<Node> { return nullptr; };
    auto init    = [](Node&) {};
    restoreProject(d, g2, factory, init);
    REQUIRE(g2.assets().all().size() == 2);
    CHECK(g2.assets().byType(AssetType::Audio).size() == 1);
    CHECK(g2.assets().byType(AssetType::Video).size() == 1);
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build build -j --target core_tests`
Expected: FAIL — `'struct oss::ProjectDoc' has no member named 'assets'`.

- [ ] **Step 3: Add `assets` to `ProjectDoc`**

In `src/core/ProjectFile.h`, add the include near the top (beside `#include "core/AutoCurve.h"`):

```cpp
#include "core/AssetLibrary.h"
```

Add the field to `struct ProjectDoc` (after `std::vector<DocAuto> autos;`):

```cpp
    std::vector<Asset>      assets;
```

- [ ] **Step 4: Serialize asset lines**

In `src/core/ProjectFile.cpp`, add `#include "core/AssetLibrary.h"` beside the other core includes at the top. Then, in `serializeProject`, just before `return out;`, append:

```cpp
    for (const Asset& a : d.assets) {
        out += "asset " + std::to_string(a.id) + " " + std::to_string((int)a.type) + "\n";
        if (!a.label.empty()) out += "alabel " + escape(a.label) + "\n";
        if (!a.path.empty())  out += "apath "  + escape(a.path)  + "\n";
    }
```

- [ ] **Step 5: Parse asset lines**

In `parseProject`, add a current-asset tracker beside `DocNode* cur = nullptr;`:

```cpp
    Asset* curAsset = nullptr;
```

Add three keyword branches inside the parse loop (e.g. after the `else if (kw == "auto")` block, before the trailing comment). A malformed `asset` line is skipped, not fatal — matching the spec's forgiving codec:

```cpp
        } else if (kw == "asset") {
            int id, typeInt; ls >> id >> typeInt;
            if (ls.fail()) continue;                       // skip malformed; not fatal
            if (typeInt < 0) typeInt = 0;
            if (typeInt > 3) typeInt = 3;
            out.assets.push_back(Asset{id, (AssetType)typeInt, "", ""});
            curAsset = &out.assets.back();
        } else if (kw == "alabel") {
            if (curAsset) curAsset->label = unescape(restOfLine(ls));
        } else if (kw == "apath") {
            if (curAsset) curAsset->path = unescape(restOfLine(ls));
```

> Note: `out.assets.push_back` can reallocate, so `curAsset` is refreshed to `&out.assets.back()` on every `asset` line (never cached across one). `alabel`/`apath` only ever run between two `asset` lines, so the pointer is always current.

- [ ] **Step 6: Capture + restore the library**

In `captureProject`, after the `for (... g.automation().channels())` loop and before `return d;`:

```cpp
    d.assets = g.assets().all();
```

In `restoreProject`, immediately after `g.clear();` at the top:

```cpp
    g.assets().load(d.assets);
```

- [ ] **Step 7: Run the tests to verify they pass**

Run: `cmake --build build -j --target core_tests && ./build/core_tests -tc="ProjectFile*assets*,*carry assets*,*empty asset list*"`
Expected: PASS (3 cases). Then `ctest --test-dir build --output-on-failure -R core_tests` — all `core_tests` still green.

- [ ] **Step 8: Commit**

```bash
git add src/core/ProjectFile.h src/core/ProjectFile.cpp tests/test_assets.cpp
git commit -m "feat(core): persist the AssetLibrary in .oss projects (asset/alabel/apath)"
```

---

## Task 4: `ui/FileDialog` — NFD wrapper + dependency

**Files:**
- Create: `src/ui/FileDialog.h`, `src/ui/FileDialog.cpp`
- Modify: `CMakeLists.txt` (NFD FetchContent; `FileDialog.cpp` into `APP_SOURCES`; link `nfd`)

No unit test — a native dialog can't run headlessly. Verified by a clean app build + link.

- [ ] **Step 1: Add NFD via FetchContent**

In `CMakeLists.txt`, after the `earcut` block (before the `OSS_DEFAULT_FONT` line is fine), add:

```cmake
# --- nativefiledialog-extended (NFD): native open-file dialog for the Assets window ---
FetchContent_Declare(nfd
  GIT_REPOSITORY https://github.com/btzy/nativefiledialog-extended.git
  GIT_TAG v1.2.1)
set(NFD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
# Defensive: relax the policy minimum like the other deps in case its bundled CMake
# declares a minimum below 3.5 (harmless when it doesn't). NFD links the macOS
# AppKit/UniformTypeIdentifiers frameworks as usage requirements of its `nfd` target.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
FetchContent_MakeAvailable(nfd)
unset(CMAKE_POLICY_VERSION_MINIMUM)
```

- [ ] **Step 2: Add `FileDialog.cpp` to the app + link `nfd`**

In `APP_SOURCES`, after the new `src/core/AssetLibrary.cpp`, add the UI source (group with the other `src/ui/` lines):

```cmake
  src/ui/FileDialog.cpp
```

In `target_link_libraries(shader_streamer PRIVATE ...)`, append `nfd`:

```cmake
target_link_libraries(shader_streamer PRIVATE ui_thirdparty glad_gl41 glm::glm libsoundio_static rtmidi
  tinyobjloader meshoptimizer draco_static PkgConfig::FFMPEG nfd)
```

- [ ] **Step 3: Write `FileDialog.h`**

Create `src/ui/FileDialog.h`:

```cpp
#pragma once
#include <string>
#include <vector>

namespace oss {

// Native open-file dialog. `filters` are bare extensions ("wav", "mp3"); an empty list
// allows any file. Returns the chosen absolute path, or "" if the user cancels or on
// error. The native dialog library is confined to FileDialog.cpp.
std::string openFileDialog(const char* title, const std::vector<std::string>& filters);

} // namespace oss
```

- [ ] **Step 4: Write `FileDialog.cpp`**

Create `src/ui/FileDialog.cpp`:

```cpp
#include "ui/FileDialog.h"
#include <nfd.h>
#include <cstddef>
#include <string>

namespace oss {

std::string openFileDialog(const char* /*title*/, const std::vector<std::string>& filters) {
    // NFD-extended has no title parameter (native dialogs use the OS default); `title`
    // stays in the signature for clarity and for a possible future backend.
    if (NFD_Init() != NFD_OKAY) return std::string();

    // One filter item built from the bare extensions, e.g. {"wav","mp3"} -> "wav,mp3".
    std::string spec;
    for (std::size_t i = 0; i < filters.size(); ++i) {
        if (i) spec += ',';
        spec += filters[i];
    }
    nfdfilteritem_t item{ "Media", spec.c_str() };
    const nfdfilteritem_t* list = spec.empty() ? nullptr : &item;
    nfdfiltersize_t count = spec.empty() ? 0 : 1;

    nfdchar_t*  outPath = nullptr;
    nfdresult_t r = NFD_OpenDialog(&outPath, list, count, nullptr);
    std::string result;
    if (r == NFD_OKAY && outPath) { result = outPath; NFD_FreePath(outPath); }
    NFD_Quit();
    return result;   // "" on cancel or error
}

} // namespace oss
```

- [ ] **Step 5: Configure + build the app to verify NFD fetches, compiles, and links**

Run: `cmake -S . -B build && cmake --build build -j --target shader_streamer`
Expected: NFD is fetched and built; `shader_streamer` links cleanly (no undefined `NFD_*` symbols, no missing `nfd.h`).

- [ ] **Step 6: Commit**

```bash
git add src/ui/FileDialog.h src/ui/FileDialog.cpp CMakeLists.txt
git commit -m "feat(ui): FileDialog wrapping NFD (nativefiledialog-extended) for Browse"
```

---

## Task 5: `ui/AssetsPanel` + toolbar + Application integration

**Files:**
- Create: `src/ui/AssetsPanel.h`, `src/ui/AssetsPanel.cpp`
- Modify: `src/ui/TransportBar.h` / `.cpp` (`showAssets` + button)
- Modify: `src/app/Application.h` / `.cpp` (own + draw the panel)
- Modify: `CMakeLists.txt` (`AssetsPanel.cpp` into `APP_SOURCES`)

No unit test — ImGui can't run headlessly. Verified by build + an interactive smoke check.

- [ ] **Step 1: Write `AssetsPanel.h`**

Create `src/ui/AssetsPanel.h`:

```cpp
#pragma once

namespace oss {

class AssetLibrary;

// The "Assets" window: a tab bar (Audio / Video / MIDI / 3D); each tab is a table of that
// type's media files with an editable label + path (+ a native Browse button) and a remove
// button, plus an Add row. Edits mutate the library in place (project state — written to disk
// when the user saves the project, like the rest of the graph).
class AssetsPanel {
public:
    void draw(AssetLibrary& lib, bool* open);
};

} // namespace oss
```

- [ ] **Step 2: Write `AssetsPanel.cpp`**

Create `src/ui/AssetsPanel.cpp`:

```cpp
#include "ui/AssetsPanel.h"
#include "ui/FileDialog.h"
#include "core/AssetLibrary.h"
#include <imgui.h>
#include <cfloat>
#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>

namespace oss {

namespace {

// Filename basename, without directory or extension -> a default label seed.
std::string baseLabel(const std::string& path) {
    std::size_t slash = path.find_last_of("/\\");
    std::string name = (slash == std::string::npos) ? path : path.substr(slash + 1);
    std::size_t dot = name.find_last_of('.');
    if (dot != std::string::npos) name = name.substr(0, dot);
    return name;
}

void drawTab(AssetLibrary& lib, AssetType type, const char* noun,
             const std::vector<std::string>& filters) {
    int toRemove = -1;
    if (ImGui::BeginTable("##assettbl", 3,
            ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerH)) {
        ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 140.0f);
        ImGui::TableSetupColumn("Path");
        ImGui::TableSetupColumn("##act", ImGuiTableColumnFlags_WidthFixed, 30.0f);
        ImGui::TableHeadersRow();

        // byType() pointers stay valid for the loop: editing a label/path mutates a string
        // in place (no vector realloc); add/remove are deferred until after EndTable.
        for (const Asset* a : lib.byType(type)) {
            int id = a->id;
            ImGui::PushID(id);
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            char lbl[512];
            std::snprintf(lbl, sizeof(lbl), "%s", a->label.c_str());
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::InputText("##label", lbl, sizeof(lbl))) lib.setLabel(id, lbl);

            ImGui::TableSetColumnIndex(1);
            char pth[512];
            std::snprintf(pth, sizeof(pth), "%s", a->path.c_str());
            ImGui::SetNextItemWidth(-30.0f);
            if (ImGui::InputText("##path", pth, sizeof(pth))) lib.setPath(id, pth);
            ImGui::SameLine();
            if (ImGui::Button("...")) {
                std::string picked = openFileDialog(noun, filters);
                if (!picked.empty()) {
                    lib.setPath(id, picked);
                    const Asset* cur = lib.find(id);
                    if (cur && cur->label.empty()) lib.setLabel(id, baseLabel(picked));
                }
            }

            ImGui::TableSetColumnIndex(2);
            if (ImGui::Button("x")) toRemove = id;

            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    std::string addLabel = std::string("+ Add ") + noun;
    if (ImGui::Button(addLabel.c_str())) lib.add(type, "", "");
    if (toRemove >= 0) lib.remove(toRemove);
}

} // namespace

void AssetsPanel::draw(AssetLibrary& lib, bool* open) {
    if (!open || !*open) return;
    if (!ImGui::Begin("Assets", open)) { ImGui::End(); return; }

    if (ImGui::BeginTabBar("assets_tabs")) {
        if (ImGui::BeginTabItem("Audio")) {
            drawTab(lib, AssetType::Audio, "audio file",
                    {"mp3", "wav", "flac", "m4a", "ogg", "aac", "aiff"});
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Video")) {
            drawTab(lib, AssetType::Video, "video file",
                    {"mp4", "mov", "mkv", "avi", "webm"});
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("MIDI")) {
            drawTab(lib, AssetType::Midi, "MIDI file", {"mid", "midi"});
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("3D")) {
            drawTab(lib, AssetType::Mesh, "3D model", {"obj", "gltf", "glb"});
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

} // namespace oss
```

- [ ] **Step 3: Add the toolbar `showAssets` flag + button**

In `src/ui/TransportBar.h`, add to `struct ProjectBarIO` (after `bool* showPreferences = nullptr;`):

```cpp
    bool*       showAssets = nullptr;        // toggled by the Assets button (if non-null)
```

In `src/ui/TransportBar.cpp`, after the existing `showPreferences` button block (the `if (io->showPreferences) { ... }`), add:

```cpp
        if (io->showAssets) {
            ImGui::SameLine();
            if (ImGui::Button("Assets")) *io->showAssets = !*io->showAssets;
        }
```

- [ ] **Step 4: Own + draw the panel in `Application`**

In `src/app/Application.h`, add the include beside the other UI panel includes:

```cpp
#include "ui/AssetsPanel.h"
```

Add the members beside `PreferencesPanel preferences_;` / `bool showPreferences_`:

```cpp
    AssetsPanel      assets_;
    bool             showAssets_ = false;
```

In `src/app/Application.cpp`, inside `frame(...)`, set the io flag (after `io.showPreferences = &showPreferences_;`):

```cpp
    io.showAssets = &showAssets_;
```

and draw the panel (after `preferences_.draw(...)`):

```cpp
    assets_.draw(graph_.assets(), &showAssets_);
```

- [ ] **Step 5: Add `AssetsPanel.cpp` to the app sources**

In `CMakeLists.txt` `APP_SOURCES`, after `src/ui/FileDialog.cpp`:

```cmake
  src/ui/AssetsPanel.cpp
```

- [ ] **Step 6: Build the app**

Run: `cmake --build build -j --target shader_streamer`
Expected: links cleanly.

- [ ] **Step 7: Interactive smoke check (manual)**

Run: `./build/shader_streamer`
Verify: the toolbar shows an **Assets** button beside **Prefs**; clicking it opens the **Assets** window with four tabs (Audio / Video / MIDI / 3D); **+ Add audio file** appends a row; the **Label** and **Path** fields are editable; **...** opens a native file dialog and fills the path (seeding the label from the filename when empty); **x** removes the row. Save a project with assets, **Load** it back, and confirm the rows return. Close the app.

- [ ] **Step 8: Commit**

```bash
git add src/ui/AssetsPanel.h src/ui/AssetsPanel.cpp \
        src/ui/TransportBar.h src/ui/TransportBar.cpp \
        src/app/Application.h src/app/Application.cpp CMakeLists.txt
git commit -m "feat(ui): Assets window (4 tabs, add/edit/remove, Browse) + toolbar button"
```

---

## Task 6: Documentation

**Files:**
- Modify: `CLAUDE.md` (architecture bullet)
- Modify: `README.md` (a short "Assets" subsection)

- [ ] **Step 1: Add the CLAUDE.md architecture bullet**

In `CLAUDE.md`, under **## Architecture**, add a bullet (place it after the **Project save/load** bullet, since it ties into persistence):

```markdown
- **Assets / media library** — the GL-free `core/AssetLibrary` is a per-project media
  library: each `Asset` is a stable, unique, never-reused `id` + an `AssetType`
  (Audio/Video/Midi/Mesh, the four tabs) + an editable `label` and file `path`. It is owned
  by `Graph` (`graph.assets()`, cleared by `Graph::clear()`) and persisted through
  `ProjectFile` as `asset <id> <type>` / `alabel` / `apath` lines (the two free-text fields
  get their own lines because the codec's `escape()` guards only `\`/`\n`, not spaces; ids
  are preserved verbatim on load, unlike remapped node ids). The `ui/AssetsPanel` renders a
  tab per type as an editable table (label, path + a native **Browse** button via
  `ui/FileDialog` → NFD, remove) with an Add row; a toolbar **Assets** button toggles it.
  Phase 1 of two — Phase 2 will rewire node `file` controls into asset-id dropdowns. The
  library + codec are unit-tested in `core_tests`; the panel/dialog are app-only (no headless test).
```

- [ ] **Step 2: Add the README subsection**

In `README.md`, after the **### Preferences** subsection (and before **## Test**), add:

```markdown
### Assets

The toolbar **Assets** button opens an Assets window: a per-project media library with
**Audio / Video / MIDI / 3D** tabs. Each tab is a table of media files — add a file, edit its
label and path inline, pick a path with the native **…** Browse dialog, or remove it. Each file
carries a hidden, stable id plus a label, and the whole library is saved and loaded with the
`.oss` project. (A later phase will let node file controls pick from this library directly.)
```

- [ ] **Step 3: Verify the docs build nothing but read correctly**

Run: `git diff --stat`
Expected: only `CLAUDE.md` and `README.md` changed.

- [ ] **Step 4: Commit**

```bash
git add CLAUDE.md README.md
git commit -m "docs: document the Assets / media library window"
```

---

## Final verification (after all tasks)

- [ ] Full build + tests: `cmake -S . -B build && cmake --build build -j && ctest --test-dir build --output-on-failure`
  Expected: `core_tests` (incl. all `test_assets.cpp` cases) and `gl_smoke` pass.
- [ ] Manual: launch `./build/shader_streamer`, exercise the Assets window (add/edit/Browse/remove across all 4 tabs), save + load a project, confirm assets persist.
