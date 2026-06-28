# Native Save/Load Project Dialogs — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the toolbar's editable filename field + Save/Load with native OS dialogs — **Load** (open), **Save As** (save), and **Save** (writes the current file, or prompts when untitled).

**Architecture:** A GL-free `core/PathUtil.h` (basename + `.oss`-extension helpers, unit-tested) holds the only real logic. `ui/FileDialog` gains `saveFileDialog` (NFD_SaveDialog) and a filter display-name on both functions. `Application` replaces its `filename_` buffer with `currentPath_` (empty = untitled) and three callbacks; `TransportBar` drops the text field and draws Save / Save As / Load. The `.oss` format and `ProjectFile` are unchanged.

**Tech Stack:** C++17, Dear ImGui, `nativefiledialog-extended` (NFD), doctest (`core_tests`).

**Spec:** `docs/superpowers/specs/2026-06-28-native-project-dialogs-design.md`

---

## File Structure

| File | Responsibility |
|---|---|
| `src/core/PathUtil.h` | **new** — `fileBaseName(path)` + `ensureExtension(path, ext)`; header-only, GL-free, unit-tested. |
| `tests/test_path_util.cpp` | **new** — `core_tests` for the two helpers. |
| `src/ui/FileDialog.{h,cpp}` | **modify** — add `saveFileDialog(...)`; add a `filterName` parameter to both functions. |
| `src/ui/AssetsPanel.cpp` | **modify** — the one `openFileDialog` caller passes the new `filterName` (`"Media"`). |
| `src/app/Application.{h,cpp}` | **modify** — `filename_[256]` → `std::string currentPath_`; add `saveProjectAs`/`saveCurrentOrPrompt`/`loadProjectDialog`; rewire `frame()`. |
| `src/ui/TransportBar.{h,cpp}` | **modify** — `ProjectBarIO` drops `filename`/`filenameLen`, gains `onSaveAs`; draw Save / Save As / Load. |
| `CMakeLists.txt` | **modify** — add `tests/test_path_util.cpp` to `core_tests`. |
| `CLAUDE.md`, `README.md` | **modify** — docs. |

---

## Task 1: `core/PathUtil.h` + unit tests

**Files:**
- Create: `src/core/PathUtil.h`, `tests/test_path_util.cpp`
- Modify: `CMakeLists.txt` (add the test to `core_tests`)

- [ ] **Step 1: Write the failing tests**

Create `tests/test_path_util.cpp`:

```cpp
#include <doctest/doctest.h>
#include "core/PathUtil.h"

using namespace oss;

TEST_CASE("fileBaseName strips directories") {
    CHECK(fileBaseName("/a/b/c.oss") == "c.oss");
    CHECK(fileBaseName("c.oss") == "c.oss");
    CHECK(fileBaseName("a\\b\\c.oss") == "c.oss");   // backslash separator
    CHECK(fileBaseName("") == "");
    CHECK(fileBaseName("/a/b/") == "");              // trailing slash -> empty basename
}

TEST_CASE("ensureExtension appends only when missing (case-insensitive)") {
    CHECK(ensureExtension("foo", "oss") == "foo.oss");
    CHECK(ensureExtension("foo.oss", "oss") == "foo.oss");
    CHECK(ensureExtension("foo.OSS", "oss") == "foo.OSS");          // already present (any case) -> unchanged
    CHECK(ensureExtension("path/to/proj", "oss") == "path/to/proj.oss");
    CHECK(ensureExtension("", "oss") == "");                         // empty stays empty
    CHECK(ensureExtension("name.bak", "oss") == "name.bak.oss");     // different ext -> appended
}
```

- [ ] **Step 2: Register the test in CMake**

In `CMakeLists.txt`, add to the `add_executable(core_tests ...)` test list (e.g. after `tests/test_assets.cpp`):

```cmake
  tests/test_path_util.cpp
```

(No source line — `PathUtil.h` is header-only; `core_tests` already has `target_include_directories(... PRIVATE src)`.)

- [ ] **Step 3: Run the tests to verify they fail**

Run: `cmake --build build -j --target core_tests`
Expected: FAIL — `fatal error: 'core/PathUtil.h' file not found`.

- [ ] **Step 4: Write `PathUtil.h`**

Create `src/core/PathUtil.h`:

```cpp
#pragma once
#include <string>
#include <cctype>

namespace oss {

// The filename portion of a path (after the last '/' or '\\'); the whole string if no
// separator, and "" for a trailing-slash path. GL-free, header-only.
inline std::string fileBaseName(const std::string& path) {
    std::size_t slash = path.find_last_of("/\\");
    return (slash == std::string::npos) ? path : path.substr(slash + 1);
}

// Append "." + ext to `path` unless it already ends in ".<ext>" (case-insensitive).
// `ext` is given without a dot (e.g. "oss"). Empty path or ext returns `path` unchanged.
inline std::string ensureExtension(const std::string& path, const std::string& ext) {
    if (path.empty() || ext.empty()) return path;
    const std::string dotExt = "." + ext;
    if (path.size() >= dotExt.size()) {
        bool match = true;
        std::size_t off = path.size() - dotExt.size();
        for (std::size_t i = 0; i < dotExt.size(); ++i) {
            if (std::tolower((unsigned char)path[off + i]) !=
                std::tolower((unsigned char)dotExt[i])) { match = false; break; }
        }
        if (match) return path;
    }
    return path + dotExt;
}

} // namespace oss
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `cmake --build build -j --target core_tests && ./build/core_tests -tc="fileBaseName*,ensureExtension*"`
Expected: PASS (2 cases). Then `ctest --test-dir build --output-on-failure -R core_tests` — all green.

- [ ] **Step 6: Commit**

```bash
git add src/core/PathUtil.h tests/test_path_util.cpp CMakeLists.txt
git commit -m "feat(core): PathUtil fileBaseName + ensureExtension helpers + tests"
```

---

## Task 2: `FileDialog` — `saveFileDialog` + filter name

**Files:**
- Modify: `src/ui/FileDialog.h`, `src/ui/FileDialog.cpp`, `src/ui/AssetsPanel.cpp`

No automated test (NFD dialogs can't run headlessly). Verified by a clean app build.

- [ ] **Step 1: Update `FileDialog.h`**

Replace the body of `src/ui/FileDialog.h` (keep the `#pragma once` + includes) with:

```cpp
#pragma once
#include <string>
#include <vector>

namespace oss {

// Native open-file dialog. `filterName` labels the filter shown in the dialog (e.g. "Project");
// `filters` are bare extensions ("oss"); an empty list allows any file. Returns the chosen
// path, or "" if the user cancels or on error. The native dialog library is confined to the .cpp.
std::string openFileDialog(const char* title, const char* filterName,
                           const std::vector<std::string>& filters);

// Native save-file dialog. `defaultName` seeds the filename field (e.g. "project.oss").
// Same return contract as openFileDialog.
std::string saveFileDialog(const char* title, const char* filterName,
                           const std::vector<std::string>& filters,
                           const std::string& defaultName);

} // namespace oss
```

- [ ] **Step 2: Update `FileDialog.cpp`**

Replace the body of `src/ui/FileDialog.cpp` with:

```cpp
#include "ui/FileDialog.h"
#include <nfd.h>
#include <cstddef>
#include <string>

namespace oss {

namespace {
// Join bare extensions into NFD's comma-separated spec, e.g. {"wav","mp3"} -> "wav,mp3".
std::string joinSpec(const std::vector<std::string>& filters) {
    std::string spec;
    for (std::size_t i = 0; i < filters.size(); ++i) { if (i) spec += ','; spec += filters[i]; }
    return spec;
}
} // namespace

std::string openFileDialog(const char* /*title*/, const char* filterName,
                           const std::vector<std::string>& filters) {
    // NFD-extended has no title parameter (native dialogs use the OS default); `title`
    // stays in the signature for clarity.
    if (NFD_Init() != NFD_OKAY) return std::string();
    std::string spec = joinSpec(filters);
    nfdfilteritem_t item{ filterName, spec.c_str() };
    const nfdfilteritem_t* list = spec.empty() ? nullptr : &item;
    nfdfiltersize_t count = spec.empty() ? 0 : 1;

    nfdchar_t*  outPath = nullptr;
    nfdresult_t r = NFD_OpenDialog(&outPath, list, count, nullptr);
    std::string result;
    if (r == NFD_OKAY && outPath) { result = outPath; NFD_FreePath(outPath); }
    NFD_Quit();
    return result;
}

std::string saveFileDialog(const char* /*title*/, const char* filterName,
                           const std::vector<std::string>& filters,
                           const std::string& defaultName) {
    if (NFD_Init() != NFD_OKAY) return std::string();
    std::string spec = joinSpec(filters);
    nfdfilteritem_t item{ filterName, spec.c_str() };
    const nfdfilteritem_t* list = spec.empty() ? nullptr : &item;
    nfdfiltersize_t count = spec.empty() ? 0 : 1;

    nfdchar_t*  outPath = nullptr;
    // NFD_SaveDialog(outPath, filterList, count, defaultPath, defaultName)
    nfdresult_t r = NFD_SaveDialog(&outPath, list, count, nullptr, defaultName.c_str());
    std::string result;
    if (r == NFD_OKAY && outPath) { result = outPath; NFD_FreePath(outPath); }
    NFD_Quit();
    return result;
}

} // namespace oss
```

- [ ] **Step 3: Update the AssetsPanel caller**

In `src/ui/AssetsPanel.cpp`, the line (around line 54):

```cpp
                std::string picked = openFileDialog(noun, filters);
```

becomes:

```cpp
                std::string picked = openFileDialog(noun, "Media", filters);
```

- [ ] **Step 4: Build the app**

Run: `cmake --build build -j --target shader_streamer`
Expected: links cleanly (the new `saveFileDialog` resolves against NFD; AssetsPanel compiles with the new signature).

- [ ] **Step 5: Commit**

```bash
git add src/ui/FileDialog.h src/ui/FileDialog.cpp src/ui/AssetsPanel.cpp
git commit -m "feat(ui): add saveFileDialog + a filter display-name to FileDialog"
```

---

## Task 3: `Application` + `TransportBar` — Save / Save As / Load

**Files:**
- Modify: `src/ui/TransportBar.h`, `src/ui/TransportBar.cpp`, `src/app/Application.h`, `src/app/Application.cpp`

No automated test (UI). Verified by build + a manual smoke check (deferred — done by the coordinator/user).

- [ ] **Step 1: Update `ProjectBarIO` (TransportBar.h)**

In `src/ui/TransportBar.h`, replace the `struct ProjectBarIO { ... };` with:

```cpp
// Project Save / Save As / Load controls drawn at the right of the transport bar. When null,
// the bar shows only transport controls. Each callback is invoked when its button is clicked.
struct ProjectBarIO {
    std::function<void()> onSave;       // write the current file (or prompt if untitled)
    std::function<void()> onSaveAs;     // always prompt for a destination
    std::function<void()> onLoad;       // prompt for a file to open
    std::string status;                 // shown after the buttons
    bool*       showPreferences = nullptr;   // toggled by the View > Preferences item (if non-null)
    bool*       showAssets      = nullptr;   // toggled by the View > Assets item (if non-null)
};
```

(This removes `char* filename` + `std::size_t filenameLen` and adds `onSaveAs`. The `#include <cstddef>` at the top may now be unused but is harmless — leave it.)

- [ ] **Step 2: Update the toolbar button block (TransportBar.cpp)**

In `src/ui/TransportBar.cpp`, the `if (io) { ... }` block currently is:

```cpp
    if (io) {
        ImGui::Separator();
        ImGui::SetNextItemWidth(160.0f);
        if (io->filename) ImGui::InputText("##projfile", io->filename, io->filenameLen);
        if (ImGui::Button("Save") && io->onSave) io->onSave();
        ImGui::SameLine();
        if (ImGui::Button("Load") && io->onLoad) io->onLoad();
        // (Preferences/Assets toggles moved to the left-anchored "View" menu above.)
        if (!io->status.empty()) { ImGui::SameLine(); ImGui::TextUnformatted(io->status.c_str()); }
    }
```

Replace it with (the main menu bar lays items horizontally, so no `SameLine` is needed — consistent with the rest of the bar):

```cpp
    if (io) {
        ImGui::Separator();
        if (ImGui::Button("Save")    && io->onSave)   io->onSave();
        if (ImGui::Button("Save As") && io->onSaveAs) io->onSaveAs();
        if (ImGui::Button("Load")    && io->onLoad)   io->onLoad();
        // (Preferences/Assets toggles live in the left-anchored "View" menu above.)
        if (!io->status.empty()) ImGui::TextUnformatted(io->status.c_str());
    }
```

- [ ] **Step 3: Update `Application.h`**

In `src/app/Application.h`, replace the member:

```cpp
    char        filename_[256] = "project.oss";
```

with:

```cpp
    std::string currentPath_;   // path of the loaded/saved project; empty = untitled
```

And add three private method declarations near `saveProjectToFile`/`loadProjectFromFile` (they're already in the class; add these in the `private:` section, e.g. just above `GLFWwindow* window_;` or beside the other members):

```cpp
    void saveProjectAs();         // prompt for a destination, save, remember it
    void saveCurrentOrPrompt();   // Save: write currentPath_, or Save As when untitled
    void loadProjectDialog();     // prompt for a file, load, remember it
```

(`Application.h` already includes `<string>`.)

- [ ] **Step 4: Update `Application.cpp`**

In `src/app/Application.cpp`, add the includes near the top (with the other `#include`s):

```cpp
#include "ui/FileDialog.h"
#include "core/PathUtil.h"
```

Replace the `frame()` project-IO wiring. The current lines are:

```cpp
    io.filename = filename_;
    io.filenameLen = sizeof(filename_);
    io.onSave = [this]{ projectStatus_ = saveProjectToFile(filename_) ? (std::string("saved ") + filename_) : "save failed"; };
    io.onLoad = [this]{ projectStatus_ = loadProjectFromFile(filename_) ? (std::string("loaded ") + filename_) : "load failed"; };
    io.status = projectStatus_;
```

Replace them with:

```cpp
    io.onSave   = [this]{ saveCurrentOrPrompt(); };
    io.onSaveAs = [this]{ saveProjectAs(); };
    io.onLoad   = [this]{ loadProjectDialog(); };
    io.status = projectStatus_;
```

Then add the three method definitions (anywhere in the `oss` namespace in `Application.cpp`, e.g. just after `frame()`):

```cpp
void Application::saveProjectAs() {
    std::string defName = currentPath_.empty() ? std::string("project.oss")
                                               : fileBaseName(currentPath_);
    std::string path = saveFileDialog("Save Project", "Project", {"oss"}, defName);
    if (path.empty()) return;                       // cancelled
    path = ensureExtension(path, "oss");
    if (saveProjectToFile(path)) { currentPath_ = path; projectStatus_ = "saved " + fileBaseName(path); }
    else                          projectStatus_ = "save failed";
}

void Application::saveCurrentOrPrompt() {
    if (currentPath_.empty()) { saveProjectAs(); return; }   // untitled -> prompt
    if (saveProjectToFile(currentPath_)) projectStatus_ = "saved " + fileBaseName(currentPath_);
    else                                 projectStatus_ = "save failed";
}

void Application::loadProjectDialog() {
    std::string path = openFileDialog("Load Project", "Project", {"oss"});
    if (path.empty()) return;                       // cancelled
    if (loadProjectFromFile(path)) { currentPath_ = path; projectStatus_ = "loaded " + fileBaseName(path); }
    else                            projectStatus_ = "load failed";
}
```

- [ ] **Step 5: Build the app**

Run: `cmake --build build -j --target shader_streamer`
Expected: links cleanly. Then a full `cmake --build build -j` to confirm `core_tests`/`gl_smoke` still build (they don't use the toolbar, but verify no shared header broke).

- [ ] **Step 6: Manual smoke check (DEFERRED — do not launch the GUI as a subagent)**

The interactive check (Save prompts when untitled; Save writes silently after; Save As re-prompts; Load opens a filtered dialog; cancel = no-op) is performed later by the coordinator/user. Your verification is the clean build (Step 5) + re-reading the wiring.

- [ ] **Step 7: Commit**

```bash
git add src/ui/TransportBar.h src/ui/TransportBar.cpp src/app/Application.h src/app/Application.cpp
git commit -m "feat(app): native Save / Save As / Load dialogs; untitled project state"
```

---

## Task 4: Documentation

**Files:**
- Modify: `README.md`, `CLAUDE.md`

- [ ] **Step 1: Update the README Save / Load subsection**

In `README.md`, find the **### Save / Load** subsection. Replace its first sentence (the one describing the filename field + Save/Load buttons) so it describes the native dialogs. The current opening is:

```markdown
The toolbar's **Save** / **Load** buttons (with the filename field, default `project.oss`)
write and read a project file: every node (type, canvas position, and control values),
```

Replace that opening sentence with:

```markdown
The toolbar's **Save** / **Save As** / **Load** buttons use native OS file dialogs. **Load** and
**Save As** open a file picker (filtered to `.oss`); **Save** writes the current file, or prompts
like **Save As** when the project is still untitled. A saved/loaded project file holds every node
(type, canvas position, and control values),
```

(Leave the rest of the paragraph — what the file contains, the playback-position note, the `.oss`-is-text note — unchanged.)

- [ ] **Step 2: Update the CLAUDE.md ProjectFile bullet**

In `CLAUDE.md`, find the **Project save/load** bullet. It ends with a sentence about the toolbar:

```
The toolbar
  (`src/ui/TransportBar.cpp`) drives it via `Application::saveProjectToFile`/`loadProjectFromFile`.
```

Replace that sentence with:

```
The toolbar
  (`src/ui/TransportBar.cpp`) drives it through native NFD file dialogs (`ui/FileDialog`
  `openFileDialog`/`saveFileDialog`): **Load**/**Save As** prompt, **Save** writes
  `Application::currentPath_` (or prompts when untitled). Path basename + `.oss`-extension handling
  live in the GL-free `core/PathUtil.h`.
```

- [ ] **Step 3: Verify only docs changed**

Run: `git diff --stat`
Expected: only `README.md` and `CLAUDE.md` changed.

- [ ] **Step 4: Commit**

```bash
git add README.md CLAUDE.md
git commit -m "docs: native Save / Save As / Load project dialogs"
```

---

## Final verification (after all tasks)

- [ ] `cmake --build build -j && ctest --test-dir build --output-on-failure` — `core_tests` (incl. the new PathUtil cases) and `gl_smoke` pass.
- [ ] Manual: `./build/shader_streamer` — untitled at start; **Save** prompts (suggests `project.oss`); after saving, **Save** writes silently; **Save As** re-prompts; **Load** opens a `.oss`-filtered dialog and restores; cancelling any dialog changes nothing.
