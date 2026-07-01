# Dockable Editor Panels Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the in-editor ImGui panels (Node Graph, Automation, Assets, Preferences) dockable via the ImGui docking branch, with a crafted default layout, `imgui.ini` persistence, and a View → Reset Layout menu item. The Output stays a separate OS window.

**Architecture:** Bump Dear ImGui to its `docking` branch tag, enable `ImGuiConfigFlags_DockingEnable`, and submit a full-viewport host dockspace each frame before the panels. A new GL-free-of-node-logic `ui/DockLayout.{h,cpp}` confines the DockBuilder usage (host + crafted default). The existing panel `Begin("…")` windows dock in by title; nothing about the Output window / dual-context loop changes.

**Tech Stack:** C++17, Dear ImGui docking branch (`v1.91.5-docking`) + imgui-node-editor, GLFW, OpenGL, CMake FetchContent.

**Spec:** `docs/superpowers/specs/2026-07-01-dockable-panels-design.md`

**No unit tests here** — this is ImGui UI glue (DockBuilder needs a live ImGui+docking context). Every task is verified by build + full `ctest` (regression), and the docking behavior by `--screenshot` + a manual interactive smoke. `.gitignore` already contains `imgui.ini`, so no gitignore change is needed.

---

### Task 1: Bump Dear ImGui to the docking branch

De-risk the dependency change first: switch the tag, re-fetch, and confirm the app + backends + imgui-node-editor still build and all tests pass. No behavior changes yet (docking stays off until Task 3 sets the flag).

**Files:**
- Modify: `CMakeLists.txt:37`

- [ ] **Step 1: Change the ImGui FetchContent tag**

In `CMakeLists.txt`, line 37 currently reads:

```cmake
FetchContent_Declare(imgui GIT_REPOSITORY https://github.com/ocornut/imgui.git GIT_TAG v1.91.5)
```

Change it to the docking-branch release tag:

```cmake
FetchContent_Declare(imgui GIT_REPOSITORY https://github.com/ocornut/imgui.git GIT_TAG v1.91.5-docking)
```

- [ ] **Step 2: Reconfigure (re-fetch ImGui) and build**

Run:
```bash
cmake -S . -B build && cmake --build build -j
```
Expected: CMake re-fetches ImGui at `v1.91.5-docking`; the `ui_thirdparty` lib (imgui core + `imgui_impl_glfw`/`imgui_impl_opengl3` backends + imgui-node-editor) and `shader_streamer` all compile and link with no errors.

- [ ] **Step 3: Run the full test suite (regression)**

Run:
```bash
ctest --test-dir build --output-on-failure
```
Expected: `100% tests passed` (`core_tests` + `gl_smoke`). The tag bump alone changes no runtime behavior.

- [ ] **Step 4: Commit**

```bash
git add CMakeLists.txt
git commit -m "build: pin Dear ImGui to the docking branch (v1.91.5-docking)"
```

---

### Task 2: `ui/DockLayout.{h,cpp}` — host dockspace + default layout

The new unit that confines the DockBuilder (`imgui_internal.h`) usage. Compiled but not yet called, so no behavior change — this task just proves it builds against the docking branch.

**Files:**
- Create: `src/ui/DockLayout.h`
- Create: `src/ui/DockLayout.cpp`
- Modify: `CMakeLists.txt` (add `src/ui/DockLayout.cpp` to `APP_SOURCES`, after `src/ui/AssetsPanel.cpp`)

- [ ] **Step 1: Create `src/ui/DockLayout.h`**

```cpp
#pragma once
#include <imgui.h>

namespace oss {

// Submit the invisible full-viewport host dockspace (sits below the main menu bar and
// behind all panels) and return its dockspace id. Call once per frame, after the menu
// bar and before the dockable panels' Begin() calls.
ImGuiID beginDockHost();

// (Re)build the crafted default layout for `dockspaceId`, docking the panels by their
// window titles: Node Graph in the centre, Automation across the bottom, Assets +
// Preferences tabbed on the right. Overrides any existing layout for this id.
void buildDefaultDockLayout(ImGuiID dockspaceId);

} // namespace oss
```

- [ ] **Step 2: Create `src/ui/DockLayout.cpp`**

```cpp
#include "ui/DockLayout.h"
#include <imgui_internal.h>   // DockBuilder* API

namespace oss {

ImGuiID beginDockHost() {
    // PassthruCentralNode keeps the host transparent (no visible chrome behind the panels).
    return ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport(),
                                        ImGuiDockNodeFlags_PassthruCentralNode);
}

void buildDefaultDockLayout(ImGuiID dockspaceId) {
    ImGui::DockBuilderRemoveNode(dockspaceId);
    ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspaceId, ImGui::GetMainViewport()->WorkSize);

    ImGuiID mainId = dockspaceId, bottomId = 0, rightId = 0;
    bottomId = ImGui::DockBuilderSplitNode(mainId, ImGuiDir_Down,  0.30f, nullptr, &mainId);
    rightId  = ImGui::DockBuilderSplitNode(mainId, ImGuiDir_Right, 0.25f, nullptr, &mainId);

    ImGui::DockBuilderDockWindow("Node Graph",  mainId);
    ImGui::DockBuilderDockWindow("Automation",  bottomId);
    ImGui::DockBuilderDockWindow("Assets",      rightId);
    ImGui::DockBuilderDockWindow("Preferences", rightId);   // tabbed with Assets
    ImGui::DockBuilderFinish(dockspaceId);
}

} // namespace oss
```

- [ ] **Step 3: Add the source to CMake**

In `CMakeLists.txt`, the `APP_SOURCES` list has the line `  src/ui/AssetsPanel.cpp`. Add the new source immediately after it:

```cmake
  src/ui/AssetsPanel.cpp
  src/ui/DockLayout.cpp
```

- [ ] **Step 4: Build (confirm it compiles against the docking branch)**

Run:
```bash
cmake -S . -B build && cmake --build build -j
```
Expected: compiles and links (`imgui_internal.h` resolves via `ui_thirdparty`'s PUBLIC include dirs, which include `${imgui_SOURCE_DIR}`). No behavior change — the functions aren't called yet.

- [ ] **Step 5: Commit**

```bash
git add src/ui/DockLayout.h src/ui/DockLayout.cpp CMakeLists.txt
git commit -m "feat(ui): DockLayout host dockspace + default layout builder"
```

---

### Task 3: Enable docking + wire the dock host + Reset Layout

Turn docking on in the interactive app, submit the host dockspace and build the default (or restore from `imgui.ini`), and add the View → Reset Layout item.

**Files:**
- Modify: `src/main.cpp` (interactive init: enable the docking config flag)
- Modify: `src/ui/TransportBar.h` (add `onResetLayout` to `ProjectBarIO`)
- Modify: `src/ui/TransportBar.cpp` (View → Reset Layout item)
- Modify: `src/app/Application.h` (add `wantResetLayout_` member)
- Modify: `src/app/Application.cpp` (includes; submit dock host in `frame()`; wire `onResetLayout`)

- [ ] **Step 1: Enable the docking config flag (interactive path)**

In `src/main.cpp`, the interactive init currently reads:

```cpp
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(editorWin, true);   // ImGui input/callbacks: editor window only
    ImGui_ImplOpenGL3_Init("#version 410");
```

Insert the docking flag right after `StyleColorsDark();`:

```cpp
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable;   // dockable editor panels
    ImGui_ImplGlfw_InitForOpenGL(editorWin, true);   // ImGui input/callbacks: editor window only
    ImGui_ImplOpenGL3_Init("#version 410");
```

- [ ] **Step 2: Add `onResetLayout` to `ProjectBarIO`**

In `src/ui/TransportBar.h`, the `ProjectBarIO` struct has the line `std::function<void()> onLibRemap;`. Add a new callback after it:

```cpp
    std::function<void()> onLibRemap;   // Asset Library > Remap Directory
    std::function<void()> onResetLayout;  // View > Reset Layout (restore the default dock layout)
```

- [ ] **Step 3: Add the View → Reset Layout menu item**

In `src/ui/TransportBar.cpp`, the View menu currently reads:

```cpp
    if (io && (io->showPreferences || io->showAssets)) {
        if (ImGui::BeginMenu("View")) {
            if (io->showPreferences) ImGui::MenuItem("Preferences", nullptr, io->showPreferences);
            if (io->showAssets)      ImGui::MenuItem("Assets",      nullptr, io->showAssets);
            ImGui::EndMenu();
        }
    }
```

Replace it with (widen the guard to include `onResetLayout`, add the item after a separator):

```cpp
    if (io && (io->showPreferences || io->showAssets || io->onResetLayout)) {
        if (ImGui::BeginMenu("View")) {
            if (io->showPreferences) ImGui::MenuItem("Preferences", nullptr, io->showPreferences);
            if (io->showAssets)      ImGui::MenuItem("Assets",      nullptr, io->showAssets);
            if (io->onResetLayout) {
                ImGui::Separator();
                if (ImGui::MenuItem("Reset Layout")) io->onResetLayout();
            }
            ImGui::EndMenu();
        }
    }
```

- [ ] **Step 4: Add the `wantResetLayout_` member**

In `src/app/Application.h`, the private members currently include:

```cpp
    bool             showPreferences_ = false;
    bool             showAssets_ = false;
```

Add the reset flag after them:

```cpp
    bool             showPreferences_ = false;
    bool             showAssets_ = false;
    bool             wantResetLayout_ = false;   // set by View > Reset Layout; rebuilds the default next frame
```

- [ ] **Step 5: Add includes to `Application.cpp`**

In `src/app/Application.cpp`, the first include is `#include "app/Application.h"`. Add the DockLayout header and imgui_internal right after it:

```cpp
#include "app/Application.h"
#include "ui/DockLayout.h"
#include <imgui_internal.h>   // ImGuiDockNode / DockBuilderGetNode for the first-run/reset check
```

- [ ] **Step 6: Submit the dock host and wire `onResetLayout` in `frame()`**

In `src/app/Application.cpp`, `Application::frame()` currently reads:

```cpp
    io.onLibRemap = [this]{ assets_.openRemap(); };
    io.status = projectStatus_;
    io.showPreferences = &showPreferences_;
    io.showAssets = &showAssets_;
    drawTransportBar(graph_.transport(), &io);   // top toolbar: tempo + play/stop/scrub
    editor_.draw(graph_, [this](const std::string& t, glm::vec2 p){ return addNodeOfType(t, p); });
```

Replace it with (add the `onResetLayout` callback, then submit the dock host + build/restore the layout, before the panels):

```cpp
    io.onLibRemap = [this]{ assets_.openRemap(); };
    io.onResetLayout = [this]{ wantResetLayout_ = true; };
    io.status = projectStatus_;
    io.showPreferences = &showPreferences_;
    io.showAssets = &showAssets_;
    drawTransportBar(graph_.transport(), &io);   // top toolbar: tempo + play/stop/scrub

    // Host dockspace for the editor panels. DockSpaceOverViewport creates the node when
    // submitted, so a fresh run (no imgui.ini) shows an empty node -> build the default;
    // a restored-from-ini node is non-empty and is left alone. Reset forces a rebuild.
    ImGuiID dockId = beginDockHost();
    ImGuiDockNode* dockNode = ImGui::DockBuilderGetNode(dockId);
    if (wantResetLayout_ || dockNode == nullptr || dockNode->IsEmpty()) {
        buildDefaultDockLayout(dockId);
        wantResetLayout_ = false;
    }

    editor_.draw(graph_, [this](const std::string& t, glm::vec2 p){ return addNodeOfType(t, p); });
```

- [ ] **Step 7: Build + run the full suite**

Run:
```bash
cmake --build build -j && ctest --test-dir build --output-on-failure
```
Expected: builds/links; `100% tests passed` (the tested code paths are untouched).

- [ ] **Step 8: Manual interactive smoke**

Run `./build/shader_streamer` from the repo root and confirm:
- On first launch (delete `imgui.ini` first if present: `rm -f imgui.ini`), the panels appear in the default layout — Node Graph centre, Automation across the bottom; open View → Assets / Preferences and they dock as right-side tabs.
- Drag a panel out and re-dock / tab it; the node editor still pans, zooms, and wires nodes while docked.
- Quit and relaunch: the arrangement persists (via `imgui.ini`).
- View → Reset Layout restores the default.

- [ ] **Step 9: Commit**

```bash
git add src/main.cpp src/ui/TransportBar.h src/ui/TransportBar.cpp src/app/Application.h src/app/Application.cpp
git commit -m "feat(ui): dockable editor panels + Reset Layout"
```

---

### Task 4: Screenshot path uses the dock layout

The headless `--screenshot` capture currently positions "Node Graph"/"Automation" manually. With docking on, that capture should instead reflect the real default docked layout, deterministically.

**Files:**
- Modify: `src/main.cpp` (`runScreenshot`: enable docking, disable ini, drop the manual layout)

- [ ] **Step 1: Enable docking + deterministic ini in the screenshot init**

In `src/main.cpp` `runScreenshot`, the init currently reads:

```cpp
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NoMouse;   // no hover artefacts
    ImGui_ImplGlfw_InitForOpenGL(win, false);                 // headless: no input callbacks
    ImGui_ImplOpenGL3_Init("#version 410");
```

Replace the `ConfigFlags` line and add the ini-disable line:

```cpp
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NoMouse | ImGuiConfigFlags_DockingEnable;
    ImGui::GetIO().IniFilename = nullptr;   // deterministic capture: always the fresh default layout
    ImGui_ImplGlfw_InitForOpenGL(win, false);                 // headless: no input callbacks
    ImGui_ImplOpenGL3_Init("#version 410");
```

- [ ] **Step 2: Remove the manual window layout (the default dock layout replaces it)**

In `src/main.cpp` `runScreenshot`, the per-frame loop currently contains a window-size query and a manual layout block:

```cpp
        int winW = 0, winH = 0; glfwGetWindowSize(win, &winW, &winH);
        int fbW = 0, fbH = 0;   glfwGetFramebufferSize(win, &fbW, &fbH);
        for (int f = 0; f < 4; ++f) {            // a few frames so ImGui settles
            glfwPollEvents();
            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();
            app.frame(1.0f / 60.0f);
            // Lay the windows out (logical points) below the transport menu bar.
            const float top = 26.0f, gap = 8.0f;
            const float graphH = (winH - top) * 0.55f;
            ImGui::SetWindowPos("Node Graph", ImVec2(gap, top));
            ImGui::SetWindowSize("Node Graph", ImVec2(winW - gap * 2, graphH));
            ImGui::SetWindowPos("Automation", ImVec2(gap, top + graphH + gap));
            ImGui::SetWindowSize("Automation", ImVec2(winW - gap * 2, (winH - top) * 0.45f - gap * 2));
            ImGui::Render();
```

Replace that span with (drop the unused `winW`/`winH` and the whole manual-layout block; `app.frame()` now submits the dockspace and builds the default so the panels lay themselves out):

```cpp
        int fbW = 0, fbH = 0;   glfwGetFramebufferSize(win, &fbW, &fbH);
        for (int f = 0; f < 4; ++f) {            // a few frames so ImGui settles
            glfwPollEvents();
            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();
            app.frame(1.0f / 60.0f);             // submits the host dockspace + default layout
            ImGui::Render();
```

(The comment two lines above, "ImGui lays out in logical points ... keep the two apart", still describes the retained framebuffer-size query — leave it.)

- [ ] **Step 3: Build and regenerate the screenshot**

Run:
```bash
cmake --build build -j && ./build/shader_streamer --screenshot /tmp/dock_layout.png
```
Expected: prints `wrote screenshot /tmp/dock_layout.png (...)` with no crash. Open the PNG and confirm the docked default layout (Node Graph filling the centre, Automation docked across the bottom) rather than two free-floating windows.

- [ ] **Step 4: Commit**

```bash
git add src/main.cpp
git commit -m "feat(ui): screenshot capture uses the default dock layout"
```

---

### Task 5: Documentation

**Files:**
- Modify: `CLAUDE.md`
- Modify: `README.md`

- [ ] **Step 1: Add a CLAUDE.md note under the two-windows hard rule**

In `CLAUDE.md`, find the hard-rule bullet that begins "**Two windows, one shared GL context**" (it describes the Graph window owning the editor + the Output window blitting). Append this sentence to the end of that bullet:

```markdown
  The Graph window's ImGui panels (Node Graph, Automation, Assets, Preferences) are **dockable**
  (Dear ImGui `docking` branch, pinned `v1.91.5-docking`): `src/main.cpp` sets
  `ImGuiConfigFlags_DockingEnable` and `Application::frame` submits a host dockspace via the GL-free-of-node-logic
  `ui/DockLayout` (`beginDockHost` + a crafted `buildDefaultDockLayout`) before the panels; layout
  persists to `imgui.ini` and **View → Reset Layout** rebuilds the default. The **Output stays a
  separate OS window** (multi-viewport is off), so this rule is unchanged.
```

- [ ] **Step 2: Add a README line**

In `README.md`, find the section that lists the app's windows/UI (search for "Output window" or the two-window description). Add a short sentence noting the dockable panels. Concretely, locate the line describing the Graph/editor window and append:

```markdown
The editor panels (Node Graph, Automation, Assets, Preferences) are dockable — drag to split or tab them; the layout is saved between runs, and **View → Reset Layout** restores the default. The Output stays its own window (put it fullscreen on a second display).
```

If no such windows/UI sentence exists to append to, add the above as its own short paragraph in the "Running" / usage area of the README.

- [ ] **Step 3: Commit**

```bash
git add CLAUDE.md README.md
git commit -m "docs: document dockable editor panels"
```

---

## Notes for the implementer

- **`.gitignore` already contains `imgui.ini`** — do not add it again.
- **`imgui_internal.h` resolves** because `ui_thirdparty` exposes `${imgui_SOURCE_DIR}` as a PUBLIC include dir and `shader_streamer` links it; `DockLayout.cpp` and `Application.cpp` can include it directly.
- **The build trigger is `node->IsEmpty()`, not a null check** — `DockSpaceOverViewport` creates the node when submitted, so after the call the node is non-null; a fresh run leaves it *empty* until `buildDefaultDockLayout` docks windows into it.
- **Do not touch the Output window / dual-context loop** in `src/main.cpp` (lines ~182-217) — multi-viewport stays off and Output stays a separate GLFW window.
- **Do NOT** `git add -A` / `git add .`. Stage only the files listed in each commit step. Leave the untracked `build.sh`, `preferences.oss`, `project.oss`, `examples/`, and the runtime-generated `imgui.ini` alone.
- There are **no unit tests** for this feature (ImGui UI glue). Rely on the build, the full `ctest` (regression), the `--screenshot` artifact, and the manual smoke in Task 3 Step 8.
