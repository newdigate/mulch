# Asset Library files + path preferences — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the Asset Library a standalone, project-referenced `.osslib` document (Open/Save/Save As + Remap Directory in a new menu), with two location preferences that seed the file dialogs.

**Architecture:** `core/` gains a GL-free, file-I/O-free `.osslib` text codec (factored out of `ProjectFile` so the asset-line format can't drift) and a path-prefix remap; `ProjectFile` switches to storing a library *reference* (`assetlib <path>`) instead of embedding assets; the **Application** owns the external `.osslib` read/write and the save/load lifecycle; `ui/` gains the menu, the remap modal, a Locations preferences section, and `defaultPath`-aware dialogs.

**Tech Stack:** C++17, GL-free `core/` + doctest tests, Dear ImGui, NFD (native file dialogs).

---

## Reference — design spec

`docs/superpowers/specs/2026-06-28-asset-library-files-design.md`. Read it. Key decisions: referenced-library model (project stores `assetlib <absolute-path>`, no embedded assets; missing-on-load → unbound + warn); save lifecycle = project save also writes the bound `.osslib`, prompts a library Save-As when the library is unbound and non-empty, legacy embedded projects load unbound; remap = exact prefix swap across all assets; two prefs (`projectsDir`, `assetLibraryDir`) seed project / library+media dialogs respectively.

## Sequencing

Phase C (Tasks 1–3, preferences + dialogs) is independent and lands first. Phase A (Tasks 4–7, library file + reference model) is the core; Task 4 is a behavior-preserving refactor, Task 5 is additive, Task 6 is the atomic pivot (stop embedding + wire the Application). Phase B (Tasks 8–9) is remap. Task 10 is docs. **Every task builds and tests green on its own.**

---

### Task 1: Preferences — `projectsDir` + `assetLibraryDir`

**Files:**
- Modify: `src/core/Preferences.h`, `src/core/Preferences.cpp`
- Modify: `tests/test_preferences.cpp`

- [ ] **Step 1: Write the failing test**

In `tests/test_preferences.cpp`, add:

```cpp
TEST_CASE("Preferences round-trips the projects + asset-library dirs") {
    Preferences p;
    p.projectsDir     = "/Users/me/My Projects";       // spaces survive (rest-of-line)
    p.assetLibraryDir = "/Volumes/media/libs";
    Preferences q;
    REQUIRE(parsePreferences(serializePreferences(p), q));
    CHECK(q.projectsDir     == "/Users/me/My Projects");
    CHECK(q.assetLibraryDir == "/Volumes/media/libs");
}

TEST_CASE("Preferences without the new dir lines parse to empty") {
    Preferences q;
    REQUIRE(parsePreferences("oss-prefs 1\naudio-buffer 200\n", q));
    CHECK(q.projectsDir.empty());
    CHECK(q.assetLibraryDir.empty());
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build build -j --target core_tests`
Expected: FAIL — `'projectsDir' is not a member of 'oss::Preferences'`.

- [ ] **Step 3: Add the fields**

In `src/core/Preferences.h`, add to the `Preferences` struct (after `syncFrameRate`):

```cpp
    std::string projectsDir;       // default dir for project Open/Save dialogs ("" = OS default)
    std::string assetLibraryDir;   // default dir for asset-library + media dialogs ("" = OS default)
```

- [ ] **Step 4: Serialize + parse the fields**

In `src/core/Preferences.cpp`, in `serializePreferences`, before `return out;`:

```cpp
    if (!p.projectsDir.empty())     out += "projectsdir " + p.projectsDir + "\n";
    if (!p.assetLibraryDir.empty()) out += "assetlibdir " + p.assetLibraryDir + "\n";
```

In `parsePreferences`, add two branches to the keyword chain (alongside `audio-out`, which is also a raw rest-of-line string — no escaping, matching the existing fields):

```cpp
        else if (kw == "projectsdir") out.projectsDir     = rest;
        else if (kw == "assetlibdir") out.assetLibraryDir = rest;
```

- [ ] **Step 5: Run to verify pass**

Run: `cmake --build build -j --target core_tests && ctest --test-dir build -R core_tests --output-on-failure`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add src/core/Preferences.h src/core/Preferences.cpp tests/test_preferences.cpp
git commit -m "feat(core): Preferences projectsDir + assetLibraryDir"
```

---

### Task 2: FileDialog — `defaultPath` + `pickFolderDialog`

**Files:**
- Modify: `src/ui/FileDialog.h`, `src/ui/FileDialog.cpp`
- Modify: `src/app/Application.cpp` (existing call sites pass `""` for now)

UI/NFD code — not headlessly testable; verified by a clean build.

- [ ] **Step 1: Extend the header**

Replace the three declarations in `src/ui/FileDialog.h` with these (add a trailing `defaultPath` arg, defaulted to `""`, and a folder picker):

```cpp
// `defaultPath` (a directory) seeds where the dialog opens; "" uses the OS default.
std::string openFileDialog(const char* title, const char* filterName,
                           const std::vector<std::string>& filters,
                           const std::string& defaultPath = "");

std::string saveFileDialog(const char* title, const char* filterName,
                           const std::vector<std::string>& filters,
                           const std::string& defaultName,
                           const std::string& defaultPath = "");

std::vector<std::string> openMultipleFileDialog(const char* title, const char* filterName,
                                                const std::vector<std::string>& filters,
                                                const std::string& defaultPath = "");

// Native "choose a folder" dialog. `defaultPath` seeds the starting directory. Returns the chosen
// directory, or "" on cancel/error.
std::string pickFolderDialog(const char* title, const std::string& defaultPath = "");
```

- [ ] **Step 2: Thread `defaultPath` into the NFD calls + add the picker**

In `src/ui/FileDialog.cpp`, change the three functions to pass `defaultPath` to NFD, and add `pickFolderDialog`. NFD takes a `const nfdchar_t*` default path (null when empty). The full file body:

```cpp
#include "ui/FileDialog.h"
#include <nfd.h>
#include <cstddef>
#include <string>

namespace oss {

namespace {
std::string joinSpec(const std::vector<std::string>& filters) {
    std::string spec;
    for (std::size_t i = 0; i < filters.size(); ++i) { if (i) spec += ','; spec += filters[i]; }
    return spec;
}
// NFD wants null (not "") for "no default path".
const nfdchar_t* defPath(const std::string& p) { return p.empty() ? nullptr : p.c_str(); }
} // namespace

std::string openFileDialog(const char* /*title*/, const char* filterName,
                           const std::vector<std::string>& filters,
                           const std::string& defaultPath) {
    if (NFD_Init() != NFD_OKAY) return std::string();
    std::string spec = joinSpec(filters);
    nfdfilteritem_t item{ filterName, spec.c_str() };
    const nfdfilteritem_t* list = spec.empty() ? nullptr : &item;
    nfdfiltersize_t count = spec.empty() ? 0 : 1;

    nfdchar_t*  outPath = nullptr;
    nfdresult_t r = NFD_OpenDialog(&outPath, list, count, defPath(defaultPath));
    std::string result;
    if (r == NFD_OKAY && outPath) { result = outPath; NFD_FreePath(outPath); }
    NFD_Quit();
    return result;
}

std::string saveFileDialog(const char* /*title*/, const char* filterName,
                           const std::vector<std::string>& filters,
                           const std::string& defaultName,
                           const std::string& defaultPath) {
    if (NFD_Init() != NFD_OKAY) return std::string();
    std::string spec = joinSpec(filters);
    nfdfilteritem_t item{ filterName, spec.c_str() };
    const nfdfilteritem_t* list = spec.empty() ? nullptr : &item;
    nfdfiltersize_t count = spec.empty() ? 0 : 1;

    nfdchar_t*  outPath = nullptr;
    nfdresult_t r = NFD_SaveDialog(&outPath, list, count, defPath(defaultPath), defaultName.c_str());
    std::string result;
    if (r == NFD_OKAY && outPath) { result = outPath; NFD_FreePath(outPath); }
    NFD_Quit();
    return result;
}

std::vector<std::string> openMultipleFileDialog(const char* /*title*/, const char* filterName,
                                                const std::vector<std::string>& filters,
                                                const std::string& defaultPath) {
    std::vector<std::string> result;
    if (NFD_Init() != NFD_OKAY) return result;
    std::string spec = joinSpec(filters);
    nfdfilteritem_t item{ filterName, spec.c_str() };
    const nfdfilteritem_t* list = spec.empty() ? nullptr : &item;
    nfdfiltersize_t count = spec.empty() ? 0 : 1;

    const nfdpathset_t* paths = nullptr;
    if (NFD_OpenDialogMultiple(&paths, list, count, defPath(defaultPath)) == NFD_OKAY && paths) {
        nfdpathsetsize_t n = 0;
        if (NFD_PathSet_GetCount(paths, &n) == NFD_OKAY) {
            for (nfdpathsetsize_t i = 0; i < n; ++i) {
                nfdchar_t* p = nullptr;
                if (NFD_PathSet_GetPath(paths, i, &p) == NFD_OKAY && p) {
                    result.push_back(p);
                    NFD_PathSet_FreePath(p);
                }
            }
        }
        NFD_PathSet_Free(paths);
    }
    NFD_Quit();
    return result;
}

std::string pickFolderDialog(const char* /*title*/, const std::string& defaultPath) {
    if (NFD_Init() != NFD_OKAY) return std::string();
    nfdchar_t*  outPath = nullptr;
    nfdresult_t r = NFD_PickFolder(&outPath, defPath(defaultPath));
    std::string result;
    if (r == NFD_OKAY && outPath) { result = outPath; NFD_FreePath(outPath); }
    NFD_Quit();
    return result;
}

} // namespace oss
```

- [ ] **Step 3: Build (existing call sites still compile via the defaulted arg)**

Run: `cmake --build build -j`
Expected: links cleanly. The existing project dialog calls in `Application.cpp` keep working (the new arg defaults to `""`); they get wired to the real pref dirs in Task 3.

- [ ] **Step 4: Commit**

```bash
git add src/ui/FileDialog.h src/ui/FileDialog.cpp
git commit -m "feat(ui): FileDialog default directory + folder picker"
```

---

### Task 3: Locations preferences UI + wire project dialogs

**Files:**
- Modify: `src/ui/PreferencesPanel.cpp` (a "Locations" tab)
- Modify: `src/app/Application.cpp` (project dialogs default to `prefs_.projectsDir`)

Build + manual smoke (UI).

- [ ] **Step 1: Add a Locations tab to the Preferences panel**

In `src/ui/PreferencesPanel.cpp`, find the `BeginTabBar` block (the panel draws an Audio Output / Audio Input / MIDI / Video / Sync tab set). Add one more tab item alongside them. It edits `prefs.projectsDir` / `prefs.assetLibraryDir` via a folder picker and calls the existing on-change save callback (the panel already takes a `std::function<void()> onChange` it invokes when a setting changes — match how the other tabs call it). Insert:

```cpp
        if (ImGui::BeginTabItem("Locations")) {
            auto folderRow = [&](const char* label, std::string& dir) {
                ImGui::TextUnformatted(label);
                ImGui::SameLine(160.0f);
                ImGui::TextUnformatted(dir.empty() ? "(OS default)" : dir.c_str());
                ImGui::SameLine();
                ImGui::PushID(label);
                if (ImGui::SmallButton("Browse...")) {
                    std::string picked = pickFolderDialog(label, dir);
                    if (!picked.empty()) { dir = picked; if (onChange) onChange(); }
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Clear")) { dir.clear(); if (onChange) onChange(); }
                ImGui::PopID();
            };
            folderRow("Projects folder",      prefs.projectsDir);
            folderRow("Asset library folder", prefs.assetLibraryDir);
            ImGui::EndTabItem();
        }
```

Add `#include "ui/FileDialog.h"` to `PreferencesPanel.cpp` if not already present. (If the panel's draw signature names its arg / callback differently — e.g. `p` instead of `prefs`, or the save callback has another name — match the surrounding code; the logic is: edit the two strings, call the existing save-on-change hook.)

- [ ] **Step 2: Wire project dialogs to `prefs_.projectsDir`**

In `src/app/Application.cpp`:
- In `saveProjectAs`, pass the default dir:
  ```cpp
  std::string path = saveFileDialog("Save Project", "Project", {"oss"}, defName, prefs_.projectsDir);
  ```
- In `loadProjectDialog`:
  ```cpp
  std::string path = openFileDialog("Load Project", "Project", {"oss"}, prefs_.projectsDir);
  ```

- [ ] **Step 3: Build + smoke**

Run: `cmake --build build -j`
Expected: clean build. Manually: Preferences → Locations → set a Projects folder; File → Save As / Load now open there.

- [ ] **Step 4: Commit**

```bash
git add src/ui/PreferencesPanel.cpp src/app/Application.cpp
git commit -m "feat(ui): Locations preferences + project dialogs default to projects dir"
```

---

### Task 4: `core/AssetLibraryFile` codec (refactor, no behavior change)

**Files:**
- Create: `src/core/TextCodec.h`
- Create: `src/core/AssetLibraryFile.h`, `src/core/AssetLibraryFile.cpp`
- Modify: `src/core/ProjectFile.cpp` (use the shared helpers)
- Modify: `CMakeLists.txt` (add `AssetLibraryFile.cpp` to the lib + `core_tests`)
- Create: `tests/test_asset_library_file.cpp`

- [ ] **Step 1: Write the failing test**

Create `tests/test_asset_library_file.cpp`:

```cpp
#include <doctest/doctest.h>
#include "core/AssetLibraryFile.h"
#include "core/AssetLibrary.h"

using namespace oss;

TEST_CASE("serializeLibrary -> parseLibrary round-trips assets, tags, and colors") {
    AssetLibrary lib;
    int a = lib.add(AssetType::Audio, "Kick", "/m/drums/kick.wav");
    lib.addTag(a, "drums");
    lib.addTag(a, "to keep");                 // tag with a space
    int b = lib.add(AssetType::Mesh, "Cube", "/m/meshes/cube.obj");
    lib.setTagColor("drums", glm::vec4(0.1f, 0.2f, 0.3f, 1.0f));

    std::string text = serializeLibrary(lib);
    CHECK(text.rfind("oss-assetlib", 0) == 0);   // header

    AssetLibrary out;
    REQUIRE(parseLibrary(text, out));
    REQUIRE(out.all().size() == 2);
    const Asset* ka = out.find(a);
    REQUIRE(ka != nullptr);
    CHECK(ka->label == "Kick");
    CHECK(ka->path  == "/m/drums/kick.wav");
    REQUIRE(ka->tags.size() == 2);
    CHECK(ka->tags[0] == "drums");
    CHECK(ka->tags[1] == "to keep");
    CHECK(out.find(b)->type == AssetType::Mesh);
    glm::vec4 c = out.tagColor("drums");
    CHECK(c.x == doctest::Approx(0.1f));
    CHECK(c.z == doctest::Approx(0.3f));
}

TEST_CASE("parseLibrary rejects a bad header and leaves the library untouched") {
    AssetLibrary out;
    out.add(AssetType::Audio, "keep", "k.wav");
    CHECK_FALSE(parseLibrary("not-a-lib\nasset 1 0\n", out));
    CHECK(out.all().size() == 1);   // unchanged
}
```

- [ ] **Step 2: Register the test + the new source in CMake**

In `CMakeLists.txt`: add `src/core/AssetLibraryFile.cpp` to **both** the main library/executable source list **and** the `core_tests` source list (wherever `src/core/ProjectFile.cpp` and `src/core/Preferences.cpp` already appear — add the new `.cpp` next to them in each list). Add `tests/test_asset_library_file.cpp` to the `core_tests` source list after `tests/test_asset_tree.cpp`.

- [ ] **Step 3: Build to confirm it fails (header missing)**

Run: `cmake --build build -j --target core_tests`
Expected: FAIL — `'core/AssetLibraryFile.h' file not found`.

- [ ] **Step 4: Create `src/core/TextCodec.h`** (the escape/unescape/restOfLine helpers, moved out of `ProjectFile.cpp` so both codecs share them):

```cpp
#pragma once
#include <sstream>
#include <string>

namespace oss {

// Escape '\\' and '\n' so a free-text field survives the line-based .oss/.osslib codecs.
inline std::string escape(const std::string& s) {
    std::string o;
    for (char ch : s) { if (ch == '\\') o += "\\\\"; else if (ch == '\n') o += "\\n"; else o += ch; }
    return o;
}
inline std::string unescape(const std::string& s) {
    std::string o;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            char n = s[i + 1];
            if (n == 'n') { o += '\n'; ++i; }
            else if (n == '\\') { o += '\\'; ++i; }
            else o += s[i];
        } else o += s[i];
    }
    return o;
}
// The remainder of `ls` after the current token, leading whitespace trimmed (rest-of-line fields).
inline std::string restOfLine(std::istringstream& ls) {
    std::string rest; std::getline(ls >> std::ws, rest); return rest;
}

} // namespace oss
```

- [ ] **Step 5: Create `src/core/AssetLibraryFile.h`:**

```cpp
#pragma once
#include <map>
#include <sstream>
#include <string>
#include <vector>
#include <glm/vec4.hpp>
#include "core/AssetLibrary.h"   // Asset / AssetLibrary (GL-free)

namespace oss {

// Append the asset + tag-color block (asset/alabel/apath/atag/tagcolor lines) for `assets`/`colors`
// to `out`. Shared by ProjectFile (legacy embedded read/write) and the .osslib codec.
void appendAssetBlock(std::string& out, const std::vector<Asset>& assets,
                      const std::map<std::string, glm::vec4>& colors);

// Handle one asset-block keyword line during a line-by-line parse: `ls` is positioned just after
// `kw`. Returns true and consumes `ls` if `kw` is an asset-block keyword (updating assets/colors/
// curAsset, where curAsset tracks the asset the most recent `asset` line opened); false otherwise.
bool parseAssetBlockLine(const std::string& kw, std::istringstream& ls,
                         std::vector<Asset>& assets, std::map<std::string, glm::vec4>& colors,
                         Asset*& curAsset);

// The standalone library file: header `oss-assetlib 1` then the asset block.
std::string serializeLibrary(const AssetLibrary& lib);
bool        parseLibrary(const std::string& text, AssetLibrary& out);   // false on bad header

} // namespace oss
```

- [ ] **Step 6: Create `src/core/AssetLibraryFile.cpp`:**

```cpp
#include "core/AssetLibraryFile.h"
#include "core/TextCodec.h"

namespace oss {

void appendAssetBlock(std::string& out, const std::vector<Asset>& assets,
                      const std::map<std::string, glm::vec4>& colors) {
    for (const Asset& a : assets) {
        out += "asset " + std::to_string(a.id) + " " + std::to_string((int)a.type) + "\n";
        if (!a.label.empty()) out += "alabel " + escape(a.label) + "\n";
        if (!a.path.empty())  out += "apath "  + escape(a.path)  + "\n";
        for (const std::string& t : a.tags) out += "atag " + escape(t) + "\n";
    }
    for (const auto& kv : colors) {
        const glm::vec4& c = kv.second;
        out += "tagcolor " + std::to_string(c.x) + " " + std::to_string(c.y) + " "
             + std::to_string(c.z) + " " + std::to_string(c.w) + " " + escape(kv.first) + "\n";
    }
}

bool parseAssetBlockLine(const std::string& kw, std::istringstream& ls,
                         std::vector<Asset>& assets, std::map<std::string, glm::vec4>& colors,
                         Asset*& curAsset) {
    if (kw == "asset") {
        int id, typeInt; ls >> id >> typeInt;
        if (ls.fail()) return true;                        // malformed asset line -> consumed, skipped
        if (typeInt < 0) typeInt = 0;
        if (typeInt >= kAssetTypeCount) typeInt = kAssetTypeCount - 1;
        assets.push_back(Asset{id, (AssetType)typeInt, "", ""});
        curAsset = &assets.back();
        return true;
    } else if (kw == "alabel") { if (curAsset) curAsset->label = unescape(restOfLine(ls)); return true; }
    else if (kw == "apath")    { if (curAsset) curAsset->path  = unescape(restOfLine(ls)); return true; }
    else if (kw == "atag")     { if (curAsset) curAsset->tags.push_back(unescape(restOfLine(ls))); return true; }
    else if (kw == "tagcolor") {
        float r, g, b, a; ls >> r >> g >> b >> a;
        if (ls.fail()) return true;                        // malformed -> consumed, skipped
        colors[unescape(restOfLine(ls))] = glm::vec4(r, g, b, a);
        return true;
    }
    return false;   // not an asset-block keyword
}

std::string serializeLibrary(const AssetLibrary& lib) {
    std::string out = "oss-assetlib 1\n";
    appendAssetBlock(out, lib.all(), lib.tagColors());
    return out;
}

bool parseLibrary(const std::string& text, AssetLibrary& out) {
    std::istringstream in(text);
    std::string line;
    if (!std::getline(in, line) || line.rfind("oss-assetlib", 0) != 0) return false;
    std::vector<Asset> assets;
    std::map<std::string, glm::vec4> colors;
    Asset* curAsset = nullptr;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        std::istringstream ls(line);
        std::string kw; ls >> kw;
        parseAssetBlockLine(kw, ls, assets, colors, curAsset);   // ignore non-asset keywords
    }
    out.load(std::move(assets));              // adopt: replaces assets, resets nextId_
    out.loadTagColors(std::move(colors));     // adopt: replaces the registry
    return true;
}

} // namespace oss
```

- [ ] **Step 7: Refactor `ProjectFile.cpp` to use the shared helpers (no behavior change)**

In `src/core/ProjectFile.cpp`:
1. Add includes near the top: `#include "core/TextCodec.h"` and `#include "core/AssetLibraryFile.h"`.
2. **Delete** the anonymous-namespace `escape`, `unescape`, and `restOfLine` definitions (now provided by `TextCodec.h` in namespace `oss`; the calls resolve unchanged). Keep `writeInput`.
3. In `serializeProject`, **replace** the asset loop + tagcolor loop (the `for (const Asset& a : d.assets) { … }` and `for (const auto& kv : d.tagColors) { … }` blocks) with a single call:
   ```cpp
   appendAssetBlock(out, d.assets, d.tagColors);
   ```
4. In `parseProject`, **remove** the five `else if` branches for `asset` / `alabel` / `apath` / `atag` / `tagcolor`, and instead dispatch right after `std::string kw; ls >> kw;` (before the `if (kw == "transport")` chain):
   ```cpp
   if (parseAssetBlockLine(kw, ls, out.assets, out.tagColors, curAsset)) continue;
   ```
   `curAsset` is already declared in `parseProject`. `parseAssetBlockLine` returns false without touching `ls` for non-asset keywords, so the existing handlers are unaffected.

- [ ] **Step 8: Build + run the full test suite (refactor must keep everything green)**

Run: `cmake --build build -j && ctest --test-dir build --output-on-failure`
Expected: PASS — the new `test_asset_library_file` cases plus all existing `core_tests` (the `ProjectFile` round-trip still embeds assets via `appendAssetBlock`, byte-identical) and `gl_smoke`.

- [ ] **Step 9: Commit**

```bash
git add src/core/TextCodec.h src/core/AssetLibraryFile.h src/core/AssetLibraryFile.cpp \
        src/core/ProjectFile.cpp CMakeLists.txt tests/test_asset_library_file.cpp
git commit -m "feat(core): AssetLibraryFile .osslib codec, shared with ProjectFile"
```

---

### Task 5: ProjectFile stores an `assetlib` reference (additive)

**Files:**
- Modify: `src/core/ProjectFile.h`, `src/core/ProjectFile.cpp`
- Modify: `tests/test_project_file.cpp`

Additive only — `captureProject` still embeds, so the running app is unaffected; this just teaches the codec the new line.

- [ ] **Step 1: Write the failing test**

In `tests/test_project_file.cpp`, add:

```cpp
TEST_CASE("ProjectDoc serializes + parses an assetlib reference") {
    ProjectDoc d;
    d.assetLibraryPath = "/Users/me/My Lib/sounds.osslib";   // spaces survive
    ProjectDoc out;
    REQUIRE(parseProject(serializeProject(d), out));
    CHECK(out.assetLibraryPath == "/Users/me/My Lib/sounds.osslib");
}

TEST_CASE("parseProject still reads legacy embedded assets") {
    const char* legacy =
        "oss-project 1\n"
        "asset 7 0\n"
        "alabel Kick\n"
        "apath /old/kick.wav\n"
        "atag drums\n";
    ProjectDoc out;
    REQUIRE(parseProject(legacy, out));
    REQUIRE(out.assets.size() == 1);
    CHECK(out.assets[0].id == 7);
    CHECK(out.assets[0].label == "Kick");
    CHECK(out.assets[0].path == "/old/kick.wav");
    REQUIRE(out.assets[0].tags.size() == 1);
    CHECK(out.assets[0].tags[0] == "drums");
    CHECK(out.assetLibraryPath.empty());
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build build -j --target core_tests`
Expected: FAIL — `'assetLibraryPath' is not a member of 'oss::ProjectDoc'`.

- [ ] **Step 3: Add the field**

In `src/core/ProjectFile.h`, add to `ProjectDoc` (after `tagColors`):

```cpp
    std::string assetLibraryPath;   // referenced .osslib (empty = none / legacy embedded)
```

- [ ] **Step 4: Serialize + parse the reference**

In `src/core/ProjectFile.cpp`, in `serializeProject`, add **before** the `appendAssetBlock(...)` call:

```cpp
    if (!d.assetLibraryPath.empty()) out += "assetlib " + escape(d.assetLibraryPath) + "\n";
```

In `parseProject`, add one branch to the keyword chain (e.g. right after the asset-block dispatch `continue;`):

```cpp
        if (kw == "assetlib") { out.assetLibraryPath = unescape(restOfLine(ls)); continue; }
```

- [ ] **Step 5: Run to verify pass**

Run: `cmake --build build -j --target core_tests && ctest --test-dir build -R core_tests --output-on-failure`
Expected: PASS (new cases + all existing).

- [ ] **Step 6: Commit**

```bash
git add src/core/ProjectFile.h src/core/ProjectFile.cpp tests/test_project_file.cpp
git commit -m "feat(core): ProjectFile assetlib reference line"
```

---

### Task 6: Reference model — Application lifecycle + stop embedding

**Files:**
- Modify: `src/core/ProjectFile.cpp` (`captureProject` stops embedding)
- Modify: `src/app/Application.h`, `src/app/Application.cpp`
- Modify: `tests/test_project_file.cpp` (capture no longer embeds), and the `gl_smoke` real-graph round-trip if it asserts assets survive the project file (it should assert via the library file instead — see Step 5)

The atomic pivot: capture stops embedding **and** the Application takes over asset persistence via the external `.osslib`, so no commit loses assets.

- [ ] **Step 1: `captureProject` stops embedding**

In `src/core/ProjectFile.cpp`, in `captureProject`, **remove** these two lines:

```cpp
    d.assets = g.assets().all();
    d.tagColors = g.assets().tagColors();
```

(`restoreProject` keeps loading `d.assets`/`d.tagColors` — now only non-empty for legacy projects.)

- [ ] **Step 2: Update the `captureProject` test expectation**

In `tests/test_project_file.cpp`, find the test(s) asserting that `captureProject` / a project round-trip carries assets, and change them to assert the **reference** model: a fresh `captureProject(graph)` leaves `d.assets` empty (assets travel via the library file now). Keep the legacy-parse test from Task 5. Concretely, if a test builds a graph with assets and round-trips it expecting the assets back, change it to expect `captureProject(g).assets.empty()` and move any asset-persistence assertion to `serializeLibrary`/`parseLibrary` (already covered in Task 4). If no such test exists, add:

```cpp
TEST_CASE("captureProject no longer embeds assets (reference model)") {
    Graph g;
    g.assets().add(AssetType::Audio, "Kick", "/m/kick.wav");
    ProjectDoc d = captureProject(g);
    CHECK(d.assets.empty());
    CHECK(d.tagColors.empty());
}
```

(Include `core/Graph.h` if the test file doesn't already.)

- [ ] **Step 3: Application — library state + helpers**

In `src/app/Application.h`, add to the private members (near `currentPath_`):

```cpp
    std::string currentLibraryPath_;   // bound .osslib (empty = unbound)
```

and declare these methods (near the project ones):

```cpp
    void openLibraryDialog();     // prompt, load a .osslib, bind it
    void saveLibraryAs();         // prompt, write, bind
    bool saveLibraryOrPrompt();   // write currentLibraryPath_, or Save-As when unbound; false if cancelled
    bool saveLibraryToFile(const std::string& path);
    bool loadLibraryFromFile(const std::string& path);
```

- [ ] **Step 4: Application — implement library + new project flow**

In `src/app/Application.cpp`, add `#include "core/AssetLibraryFile.h"` near the other core includes, and implement:

```cpp
bool Application::saveLibraryToFile(const std::string& path) {
    std::ofstream f(path);
    if (!f) return false;
    f << serializeLibrary(graph_.assets());
    return (bool)f;
}

bool Application::loadLibraryFromFile(const std::string& path) {
    std::ifstream f(path);
    if (!f) return false;
    std::stringstream ss; ss << f.rdbuf();
    return parseLibrary(ss.str(), graph_.assets());
}

void Application::saveLibraryAs() {
    std::string defName = currentLibraryPath_.empty() ? std::string("library.osslib")
                                                      : fileBaseName(currentLibraryPath_);
    std::string path = saveFileDialog("Save Asset Library", "Asset Library", {"osslib"},
                                      defName, prefs_.assetLibraryDir);
    if (path.empty()) return;
    path = ensureExtension(path, "osslib");
    if (saveLibraryToFile(path)) { currentLibraryPath_ = path; projectStatus_ = "library saved " + fileBaseName(path); }
    else                          projectStatus_ = "library save failed";
}

bool Application::saveLibraryOrPrompt() {
    if (currentLibraryPath_.empty()) {
        saveLibraryAs();
        return !currentLibraryPath_.empty();           // false if the user cancelled the prompt
    }
    if (saveLibraryToFile(currentLibraryPath_)) { projectStatus_ = "library saved " + fileBaseName(currentLibraryPath_); return true; }
    projectStatus_ = "library save failed";
    return false;
}

void Application::openLibraryDialog() {
    std::string path = openFileDialog("Open Asset Library", "Asset Library", {"osslib"}, prefs_.assetLibraryDir);
    if (path.empty()) return;
    if (loadLibraryFromFile(path)) { currentLibraryPath_ = path; projectStatus_ = "library opened " + fileBaseName(path); }
    else                            projectStatus_ = "library open failed";
}
```

- [ ] **Step 5: Application — project save/load honor the reference + sync the library**

Replace `Application::saveProjectToFile` and `Application::loadProjectFromFile` with direct doc-level calls (so the library path is injected/extracted and the external `.osslib` is written/read), and add the unbound-library prompt to the project save flow:

```cpp
bool Application::saveProjectToFile(const std::string& path) {
    ProjectDoc d = captureProject(graph_);
    d.assetLibraryPath = currentLibraryPath_;
    std::ofstream f(path);
    if (!f) return false;
    f << serializeProject(d);
    if (!f) return false;
    if (!currentLibraryPath_.empty()) saveLibraryToFile(currentLibraryPath_);   // keep the bound library in sync
    return true;
}

bool Application::loadProjectFromFile(const std::string& path) {
    std::ifstream f(path);
    if (!f) return false;
    std::stringstream ss; ss << f.rdbuf();
    ProjectDoc d;
    if (!parseProject(ss.str(), d)) return false;
    restoreProject(d, graph_,
                   [](const std::string& t){ return makeNode(t); },
                   [](Node& n){ n.initGL(); });      // loads legacy embedded assets if present
    if (!d.assetLibraryPath.empty() && loadLibraryFromFile(d.assetLibraryPath)) {
        currentLibraryPath_ = d.assetLibraryPath;
    } else {
        if (!d.assetLibraryPath.empty())
            projectStatus_ = "library not found: " + fileBaseName(d.assetLibraryPath);
        currentLibraryPath_ = "";                    // unbound (missing reference, or legacy/none)
    }
    return true;
}
```

Then make the project Save flow prompt for an unbound, non-empty library **before** writing. In `saveCurrentOrPrompt` and `saveProjectAs`, add a guard at the top of each (before they prompt/write the project):

```cpp
    // A non-empty but unsaved library must become a file first, so the project can reference it.
    if (!graph_.assets().all().empty() && currentLibraryPath_.empty()) {
        if (!saveLibraryOrPrompt()) return;          // user cancelled the library Save-As -> abort
    }
```

(Place it as the first statement of `saveCurrentOrPrompt`; in `saveProjectAs` place it first as well. `saveProjectToFile` itself stays prompt-free.)

- [ ] **Step 6: Build + test**

Run: `cmake --build build -j && ctest --test-dir build --output-on-failure`
Expected: PASS. If `gl_smoke`'s real-graph round-trip (SL5) asserts assets survive a project save/load, update that scenario to round-trip assets through `serializeLibrary`/`parseLibrary` (or to no longer expect assets from the project file), matching the reference model. Manually: add assets → File Save As → you're prompted to save a library → the `.oss` contains an `assetlib` line → reload the project → assets return.

- [ ] **Step 7: Commit**

```bash
git add src/core/ProjectFile.cpp src/app/Application.h src/app/Application.cpp \
        tests/test_project_file.cpp
# include tests/gl_smoke source if you updated SL5
git commit -m "feat: reference-model asset library (project references an external .osslib)"
```

---

### Task 7: "Asset Library" menu

**Files:**
- Modify: `src/ui/TransportBar.h`, `src/ui/TransportBar.cpp`
- Modify: `src/app/Application.cpp` (wire the callbacks)

- [ ] **Step 1: Add the callbacks to `ProjectBarIO`**

In `src/ui/TransportBar.h`, add to `struct ProjectBarIO` (after `onLoad`):

```cpp
    std::function<void()> onLibOpen;     // Asset Library > Open
    std::function<void()> onLibSave;     // Asset Library > Save
    std::function<void()> onLibSaveAs;   // Asset Library > Save As
    std::function<void()> onLibRemap;    // Asset Library > Remap Directory
```

- [ ] **Step 2: Draw the menu**

In `src/ui/TransportBar.cpp`, after the `File` menu block and before the `View` menu block, add:

```cpp
    // Asset Library actions in their own left-anchored menu (between File and View).
    if (io && (io->onLibOpen || io->onLibSave || io->onLibSaveAs || io->onLibRemap)) {
        if (ImGui::BeginMenu("Asset Library")) {
            if (io->onLibOpen   && ImGui::MenuItem("Open Asset Library...")) io->onLibOpen();
            ImGui::Separator();
            if (io->onLibSave   && ImGui::MenuItem("Save"))                  io->onLibSave();
            if (io->onLibSaveAs && ImGui::MenuItem("Save As..."))            io->onLibSaveAs();
            ImGui::Separator();
            if (io->onLibRemap  && ImGui::MenuItem("Remap Directory..."))    io->onLibRemap();
            ImGui::EndMenu();
        }
    }
```

- [ ] **Step 3: Wire the callbacks in Application**

In `src/app/Application.cpp`, where `ProjectBarIO io;` is populated (the `frame` method, alongside `io.onSave`/`io.onLoad`), add:

```cpp
    io.onLibOpen   = [this]{ openLibraryDialog(); };
    io.onLibSave   = [this]{ saveLibraryOrPrompt(); };
    io.onLibSaveAs = [this]{ saveLibraryAs(); };
    io.onLibRemap  = [this]{ assets_.openRemap(); };     // assets_.openRemap() added in Task 9
```

(If Task 9 isn't done yet, temporarily set `io.onLibRemap = nullptr;` so the menu item is hidden until the modal exists — then switch to `assets_.openRemap()` in Task 9. The implementer doing these in order can write it directly.)

- [ ] **Step 4: Build + smoke**

Run: `cmake --build build -j`
Expected: clean build; the new "Asset Library" menu appears between File and View with Open / Save / Save As (Remap arrives in Task 9).

- [ ] **Step 5: Commit**

```bash
git add src/ui/TransportBar.h src/ui/TransportBar.cpp src/app/Application.cpp
git commit -m "feat(ui): Asset Library menu (Open/Save/Save As/Remap)"
```

---

### Task 8: `AssetLibrary::remapPathPrefix` + `commonDirPrefix`

**Files:**
- Modify: `src/core/AssetLibrary.h`, `src/core/AssetLibrary.cpp`
- Create: `src/core/PathPrefix.h` (the `commonDirPrefix` helper, GL-free header-only)
- Modify: `tests/test_assets.cpp`

- [ ] **Step 1: Write the failing tests**

In `tests/test_assets.cpp`, add (`#include "core/PathPrefix.h"` at the top if needed):

```cpp
TEST_CASE("remapPathPrefix swaps a base path across matching assets") {
    AssetLibrary lib;
    int a = lib.add(AssetType::Audio, "", "/old/base/drums/kick.wav");
    int b = lib.add(AssetType::Audio, "", "/old/base/pad.wav");
    int c = lib.add(AssetType::Audio, "", "/elsewhere/x.wav");      // no match
    int n = lib.remapPathPrefix("/old/base", "/new/root");
    CHECK(n == 2);
    CHECK(lib.find(a)->path == "/new/root/drums/kick.wav");
    CHECK(lib.find(b)->path == "/new/root/pad.wav");
    CHECK(lib.find(c)->path == "/elsewhere/x.wav");                 // untouched
    CHECK(lib.remapPathPrefix("", "/whatever") == 0);              // empty from = no-op
}

TEST_CASE("commonDirPrefix returns the longest shared directory") {
    CHECK(commonDirPrefix({"/a/b/c/x.wav", "/a/b/d/y.wav"}) == "/a/b");
    CHECK(commonDirPrefix({"/a/b/x.wav"}) == "/a/b");
    CHECK(commonDirPrefix({"/a/x.wav", "/z/y.wav"}) == "");
    CHECK(commonDirPrefix({}) == "");
    CHECK(commonDirPrefix({"", "bare.wav"}) == "");                 // no directory part
}
```

- [ ] **Step 2: Run to verify fail**

Run: `cmake --build build -j --target core_tests`
Expected: FAIL — `'remapPathPrefix' is not a member` / `'core/PathPrefix.h' file not found`.

- [ ] **Step 3: Create `src/core/PathPrefix.h`:**

```cpp
#pragma once
#include <string>
#include <vector>

namespace oss {

// The longest directory prefix shared by every usable path's directory (split on '/' and '\\'),
// trimmed to a directory boundary and without a trailing separator. "" when there is no shared
// directory (e.g. differing roots, a bare filename, or no usable paths).
inline std::string commonDirPrefix(const std::vector<std::string>& paths) {
    auto dirOf = [](const std::string& p) {
        std::size_t slash = p.find_last_of("/\\");
        return slash == std::string::npos ? std::string() : p.substr(0, slash);
    };
    std::vector<std::string> dirs;
    for (const std::string& p : paths) {
        if (p.empty()) continue;
        std::string d = dirOf(p);
        if (d.empty()) return "";                 // a usable path with no directory -> nothing shared
        dirs.push_back(d);
    }
    if (dirs.empty()) return "";

    std::string cp = dirs[0];                     // longest common *character* prefix of the dirs
    for (std::size_t k = 1; k < dirs.size(); ++k) {
        std::size_t i = 0;
        while (i < cp.size() && i < dirs[k].size() && cp[i] == dirs[k][i]) ++i;
        cp.erase(i);
    }
    // cp is a clean boundary only if, in every dir, it's the whole dir or the next char is a separator.
    bool boundary = true;
    for (const std::string& d : dirs) {
        bool ok = cp.size() == d.size() ||
                  (cp.size() < d.size() && (d[cp.size()] == '/' || d[cp.size()] == '\\'));
        if (!ok) { boundary = false; break; }
    }
    if (!boundary) {                              // cut mid-segment -> back up to the last separator
        std::size_t slash = cp.find_last_of("/\\");
        cp = (slash == std::string::npos) ? std::string() : cp.substr(0, slash);
    }
    while (!cp.empty() && (cp.back() == '/' || cp.back() == '\\')) cp.pop_back();
    return cp;
}

} // namespace oss
```

- [ ] **Step 4: Declare + implement `remapPathPrefix`**

In `src/core/AssetLibrary.h`, add to the public section (near `setPath`):

```cpp
    // Replace the leading `from` of every asset path that starts with it with `to`. Exact, case-
    // sensitive prefix match across all assets; returns the number changed. No-op if `from` is empty.
    int remapPathPrefix(const std::string& from, const std::string& to);
```

In `src/core/AssetLibrary.cpp`, implement:

```cpp
int AssetLibrary::remapPathPrefix(const std::string& from, const std::string& to) {
    if (from.empty()) return 0;
    int changed = 0;
    for (Asset& a : assets_) {
        if (a.path.size() >= from.size() && a.path.compare(0, from.size(), from) == 0) {
            a.path = to + a.path.substr(from.size());
            ++changed;
        }
    }
    return changed;
}
```

- [ ] **Step 5: Run to verify pass**

Run: `cmake --build build -j --target core_tests && ctest --test-dir build -R core_tests --output-on-failure`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add src/core/PathPrefix.h src/core/AssetLibrary.h src/core/AssetLibrary.cpp tests/test_assets.cpp
git commit -m "feat(core): AssetLibrary remapPathPrefix + commonDirPrefix"
```

---

### Task 9: Remap Directory modal

**Files:**
- Modify: `src/ui/AssetsPanel.h`, `src/ui/AssetsPanel.cpp`
- Modify: `src/app/Application.cpp` (pass the media default dir to the panel)

Build + manual smoke (UI).

- [ ] **Step 1: Panel state + entry point**

In `src/ui/AssetsPanel.h`, add `#include "core/PathPrefix.h"` is not needed here; add a public method and private state. In the public section:

```cpp
    void openRemap() { showRemap_ = true; remapPrimed_ = false; }   // called by the Asset Library menu
```

Change the `draw` signature to take the media default dir:

```cpp
    void draw(AssetLibrary& lib, bool* open, const std::string& mediaDir = "");
```

Add private members:

```cpp
    bool showRemap_   = false;
    bool remapPrimed_ = false;   // false until the modal's From field is pre-filled on open
    char remapFrom_[1024] = {0};
    char remapTo_[1024]   = {0};
    std::string remapResult_;    // "remapped N assets" status shown in the modal
```

- [ ] **Step 2: Store the media dir + draw the modal**

In `src/ui/AssetsPanel.cpp`, in `draw`, capture the dir for the per-asset Browse + remap (e.g. a member `mediaDir_` set at the top of `draw`, or thread it through). Simplest: add a private `std::string mediaDir_;` and set `mediaDir_ = mediaDir;` at the top of `draw`, then use `mediaDir_` as the `defaultPath` in the per-asset `openFileDialog`/`openMultipleFileDialog` calls in `drawAssetLeaf`/`drawTab` (Task 3 wired prefs into Application; this passes it to the dialogs):

```cpp
// in drawAssetLeaf, the Browse "..." button:
std::string picked = openFileDialog(noun, "Media", filters, mediaDir_);
// in drawTab, the "Add files..." button:
for (const std::string& path : openMultipleFileDialog(noun, "Media", filters, mediaDir_))
```

Then draw the modal at the end of `draw` (after `ImGui::End();`'s content but inside the function — actually open the popup from the flag and render it; ImGui popups can be created after the window):

```cpp
    if (showRemap_) { ImGui::OpenPopup("Remap Directory"); showRemap_ = false; }
    if (ImGui::BeginPopupModal("Remap Directory", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (!remapPrimed_) {                              // pre-fill From with the common dir prefix
            std::vector<std::string> paths;
            for (const Asset& a : lib.all()) if (!a.path.empty()) paths.push_back(a.path);
            std::string common = commonDirPrefix(paths);
            std::snprintf(remapFrom_, sizeof(remapFrom_), "%s", common.c_str());
            remapTo_[0] = '\0';
            remapResult_.clear();
            remapPrimed_ = true;
        }
        ImGui::TextUnformatted("Replace a base directory across every asset path.");
        ImGui::SetNextItemWidth(420.0f);
        ImGui::InputText("##from", remapFrom_, sizeof(remapFrom_));
        ImGui::SameLine();
        if (ImGui::Button("Browse##from")) {
            std::string p = pickFolderDialog("Remap from", mediaDir_.empty() ? remapFrom_ : mediaDir_);
            if (!p.empty()) std::snprintf(remapFrom_, sizeof(remapFrom_), "%s", p.c_str());
        }
        ImGui::TextUnformatted("From (old base)");
        ImGui::SetNextItemWidth(420.0f);
        ImGui::InputText("##to", remapTo_, sizeof(remapTo_));
        ImGui::SameLine();
        if (ImGui::Button("Browse##to")) {
            std::string p = pickFolderDialog("Remap to", mediaDir_);
            if (!p.empty()) std::snprintf(remapTo_, sizeof(remapTo_), "%s", p.c_str());
        }
        ImGui::TextUnformatted("To (new base)");
        if (!remapResult_.empty()) { ImGui::Separator(); ImGui::TextUnformatted(remapResult_.c_str()); }
        ImGui::Separator();
        if (ImGui::Button("Apply")) {
            int n = lib.remapPathPrefix(remapFrom_, remapTo_);
            remapResult_ = "remapped " + std::to_string(n) + " asset" + (n == 1 ? "" : "s");
        }
        ImGui::SameLine();
        if (ImGui::Button("Close")) { remapPrimed_ = false; ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
```

Add includes to `AssetsPanel.cpp` if missing: `#include "core/PathPrefix.h"`. (`pickFolderDialog`/`openFileDialog` come from `ui/FileDialog.h`, already included.)

- [ ] **Step 3: Pass the media dir from Application**

In `src/app/Application.cpp`, update the `assets_.draw(...)` call:

```cpp
    assets_.draw(graph_.assets(), &showAssets_, prefs_.assetLibraryDir);
```

and ensure `io.onLibRemap = [this]{ assets_.openRemap(); };` (from Task 7).

- [ ] **Step 4: Build + smoke**

Run: `cmake --build build -j`
Expected: clean build. Manually: Asset Library → Remap Directory opens the modal with From pre-filled to the common path; set To; Apply reports "remapped N assets"; the grid paths update (and the tree regroups).

- [ ] **Step 5: Commit**

```bash
git add src/ui/AssetsPanel.h src/ui/AssetsPanel.cpp src/app/Application.cpp
git commit -m "feat(ui): Remap Directory modal + per-asset Browse default dir"
```

---

### Task 10: Documentation

**Files:**
- Modify: `CLAUDE.md`, `README.md`

- [ ] **Step 1: CLAUDE.md**

In `CLAUDE.md`, update the **Assets / media library** bullet and the **Project save/load** and **Preferences** bullets to reflect the referenced model. Append to the Assets bullet:

```
  The library is also a **standalone, project-referenced document**: `core/AssetLibraryFile.h`
  (`serializeLibrary`/`parseLibrary`, header `oss-assetlib 1`) writes/reads a `.osslib` reusing the
  shared `appendAssetBlock`/`parseAssetBlockLine` asset-block codec (factored out of `ProjectFile`,
  with `escape`/`unescape`/`restOfLine` now in `core/TextCodec.h`). A project stores only an
  `assetlib <path>` reference (`captureProject` no longer embeds assets); loading a project loads the
  referenced library, or warns and leaves it unbound when the file is missing (legacy projects with
  embedded asset lines still parse). The **Asset Library** toolbar menu does Open / Save / Save As
  (the Application owns the external file I/O + a `currentLibraryPath_`; saving a project also saves
  the bound library, and prompts a library Save-As when the library is unbound and non-empty) and
  **Remap Directory** (`AssetLibrary::remapPathPrefix` swaps a base-path prefix across all assets, the
  modal pre-filling From with `core/PathPrefix.h` `commonDirPrefix`).
```

Append to the **Preferences** bullet:

```
  Two location prefs (`projectsDir`, `assetLibraryDir`) seed the file dialogs (project dialogs default
  to the former; library + per-asset media + remap dialogs to the latter) via a new `defaultPath` arg
  on `ui/FileDialog` (+ a `pickFolderDialog`); set in the Preferences **Locations** tab.
```

- [ ] **Step 2: README.md**

In `README.md`, in the **### Assets** subsection, append a user-facing paragraph:

```markdown
The asset library is a separate file you manage from the **Asset Library** menu: **Open Asset
Library**, **Save** / **Save As** a portable `.osslib`, and **Remap Directory** to swap a base folder
across every asset path (handy when a library was built on another machine). A project stores a
*reference* to its library and loads it on open. Set a default **Projects** folder and **Asset
library** folder under Preferences → Locations to make the file dialogs open where you keep things.
```

- [ ] **Step 3: Verify only docs changed + commit**

Run: `git diff --stat` (expect only `CLAUDE.md`, `README.md`).

```bash
git add CLAUDE.md README.md
git commit -m "docs: Asset Library files + path preferences"
```

---

## Self-Review (plan vs spec)

**Spec coverage:**
- Library `.osslib` codec + shared-with-ProjectFile → Task 4.
- Referenced project model (`assetlib`, stop embedding, legacy parse) → Tasks 5 + 6.
- Application lifecycle (Open/Save/Save As, auto-save-on-project-save, prompt-if-unbound, missing-on-load) → Task 6.
- Remap (`remapPathPrefix` + `commonDirPrefix` + modal) → Tasks 8 + 9.
- Preferences (`projectsDir`/`assetLibraryDir` + Locations UI) → Tasks 1 + 3.
- FileDialog `defaultPath` + `pickFolderDialog` + dialog-default wiring → Tasks 2, 3, 6, 9.
- Menu → Task 7. Docs → Task 10.

**Placeholder scan:** the only soft spot is `commonDirPrefix`'s trim-to-separator expression — flagged with an explicit plain-implementation fallback + tests that pin the required outputs. Every other step has complete code + an expected command result.

**Type consistency:** `serializeLibrary`/`parseLibrary`, `appendAssetBlock`/`parseAssetBlockLine`, `ProjectDoc.assetLibraryPath`, `currentLibraryPath_`, `remapPathPrefix`, `commonDirPrefix`, `openRemap`, the `ProjectBarIO::onLib*` callbacks, and the `defaultPath` dialog args are named identically across header, `.cpp`, tests, and call sites.
