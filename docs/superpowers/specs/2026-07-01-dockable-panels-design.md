# Dockable editor panels (ImGui docking branch) — design

**Date:** 2026-07-01
**Status:** Approved (brainstorm)
**Branch:** `feat/dockable-panels` (off `develop`)

## Goal

Make the in-editor ImGui panels — **Node Graph**, **Automation**, **Assets**, **Preferences** —
dockable (split / tabbed / rearrangeable) with a crafted default layout, layout persistence, and a
**View → Reset Layout** menu item. The **Output stays a dedicated separate OS window**, unchanged.

## Decisions (from brainstorm)

- **Editor panels only.** The Output remains its own GLFW window with its own GL context (the
  hand-rolled dual-window loop in `src/main.cpp`), preserving the fullscreen-on-a-projector
  workflow and the "two windows, one shared GL context" hard rule. Docking applies only to the
  ImGui panels living inside the editor window.
- **Crafted default layout + persistence.** On first run (no saved layout) a DockBuilder default
  is built: Node Graph in the center, Automation docked across the bottom, Assets/Preferences
  pre-docked as a right-side tab group. The user's rearrangements persist to `imgui.ini`; a
  **Reset Layout** menu item restores the default.
- **Multi-viewport is out of scope** (a separate future decision).

## Architecture / components

The docking branch adds `ImGuiConfigFlags_DockingEnable`, `DockSpaceOverViewport()`, and the
`DockBuilder*` API (in `imgui_internal.h`). The panels are already plain `ImGui::Begin("…")`
windows drawn each frame, so they become dockable with no per-panel change — only a host dockspace
and a default-layout builder are added.

| File | Change |
|---|---|
| `CMakeLists.txt` | Bump `ocornut/imgui` `GIT_TAG v1.91.5` → **`v1.91.5-docking`**. |
| `src/ui/DockLayout.{h,cpp}` | **New.** `beginDockHost()` submits the full-viewport host dockspace each frame and returns its id; `buildDefaultDockLayout(id)` crafts the default split via DockBuilder. Confines the `imgui_internal.h` usage. |
| `src/app/Application.{h,cpp}` | `frame()` submits the dock host after the menu bar and before the panels; builds the default on first run or on reset; holds `wantResetLayout_`; wires `io.onResetLayout`. |
| `src/ui/TransportBar.{h,cpp}` | `ProjectBarIO` gains `std::function<void()> onResetLayout`; a **View → Reset Layout** item. |
| `src/main.cpp` | Set `ImGuiConfigFlags_DockingEnable` in both init paths; in the screenshot path disable the ini file and drop the manual `SetWindowPos`. |
| `.gitignore` | Add `imgui.ini`. |
| `CLAUDE.md`, `README.md` | Docs. |

## Panel window titles (DockBuilder targets)

Confirmed exact `ImGui::Begin` titles the default layout docks by name:

- `Node Graph` — `src/ui/NodeEditorPanel.cpp` (always shown; hosts imgui-node-editor).
- `Automation` — `src/ui/AutomationPanel.cpp` (always shown).
- `Assets` — `src/ui/AssetsPanel.cpp` (has an `open` bool → `showAssets_`).
- `Preferences` — `src/ui/PreferencesPanel.cpp` (has an `open` bool → `showPreferences_`).

## `src/ui/DockLayout.{h,cpp}`

```cpp
// DockLayout.h
#pragma once
#include <imgui.h>
namespace oss {
// Submit the invisible full-viewport host dockspace (below the main menu bar) and return its id.
ImGuiID beginDockHost();
// Build the crafted default layout, docking the panels by their window titles. Overrides any
// existing layout for this dockspace id (used on first run and on Reset Layout).
void buildDefaultDockLayout(ImGuiID dockspaceId);
}
```

```cpp
// DockLayout.cpp
#include "ui/DockLayout.h"
#include <imgui_internal.h>   // DockBuilder*

namespace oss {

ImGuiID beginDockHost() {
    // PassthruCentralNode keeps the central node transparent; the host has no visible chrome.
    return ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport(),
                                        ImGuiDockNodeFlags_PassthruCentralNode);
}

void buildDefaultDockLayout(ImGuiID id) {
    ImGui::DockBuilderRemoveNode(id);
    ImGui::DockBuilderAddNode(id, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(id, ImGui::GetMainViewport()->WorkSize);

    ImGuiID mainId = id, bottomId = 0, rightId = 0;
    bottomId = ImGui::DockBuilderSplitNode(mainId, ImGuiDir_Down,  0.30f, nullptr, &mainId);
    rightId  = ImGui::DockBuilderSplitNode(mainId, ImGuiDir_Right, 0.25f, nullptr, &mainId);

    ImGui::DockBuilderDockWindow("Node Graph",  mainId);
    ImGui::DockBuilderDockWindow("Automation",  bottomId);
    ImGui::DockBuilderDockWindow("Assets",      rightId);
    ImGui::DockBuilderDockWindow("Preferences", rightId);   // tabbed with Assets
    ImGui::DockBuilderFinish(id);
}

} // namespace oss
```

## `Application::frame()` change

Add a `bool wantResetLayout_ = false;` member and an `io.onResetLayout` callback. The dock host is
submitted right after the menu bar and before the panels. Because `DockSpaceOverViewport` creates
the dockspace node when it is submitted, the "needs a default" test is whether that node is empty
(fresh, no `imgui.ini`) rather than absent — a restored-from-ini node has splits/windows and is not
empty. The default is (re)built in the same frame, before the panel `Begin`s, when the node is
empty or a reset was requested:

```cpp
// after drawTransportBar(...):
ImGuiID dockId = beginDockHost();                       // submits DockSpaceOverViewport, returns id
ImGuiDockNode* node = ImGui::DockBuilderGetNode(dockId);
if (wantResetLayout_ || node == nullptr || node->IsEmpty()) {
    buildDefaultDockLayout(dockId);
    wantResetLayout_ = false;
}
// then the existing panel draws (editor_/automation_/preferences_/assets_) ...
```

`io.onResetLayout = [this]{ wantResetLayout_ = true; };` is set alongside the other `ProjectBarIO`
callbacks. (`ImGuiDockNode`, `IsEmpty()`, and `DockBuilderGetNode` are declared in
`imgui_internal.h`; `Application.cpp` includes `ui/DockLayout.h` and `imgui_internal.h`.)

## `TransportBar` change

`ProjectBarIO` gains `std::function<void()> onResetLayout;`. In the **View** menu, after the
Assets item:

```cpp
if (io->onResetLayout) { ImGui::Separator(); if (ImGui::MenuItem("Reset Layout")) io->onResetLayout(); }
```

## `main.cpp` changes

- **Interactive path:** after `ImGui::CreateContext()` / `StyleColorsDark()`, add
  `ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable;`. The dual-window render loop and
  the Output window are otherwise untouched.
- **Screenshot path (`runScreenshot`):** set the same flag; set `ImGui::GetIO().IniFilename =
  nullptr` (deterministic capture — always builds the fresh default, never reads/writes the user's
  `imgui.ini`); and **remove the manual `SetWindowPos`/`SetWindowSize` block** for "Node Graph" /
  "Automation" (the default dock layout positions them now). The capture then shows the real
  default docked UI.

## Persistence

ImGui's default `imgui.ini` in the CWD (next to `preferences.oss`) stores the dock layout — no new
codec, no `.oss`/project change, no per-project layout. Added to `.gitignore` so it doesn't clutter
the tree.

## Error / edge handling

- **Existing `imgui.ini`:** if present, ImGui restores it and the default is not rebuilt (the
  first-frame `DockBuilderGetNode(dockId) != nullptr`). Reset Layout calls `DockBuilderRemoveNode`
  first, so it fully overrides a saved layout.
- **On-demand panels:** Assets/Preferences keep their `showAssets_`/`showPreferences_` visibility
  toggles; being pre-docked only decides *where* they appear when opened (right-side tab group).
- **Node editor when docked:** imgui-node-editor renders inside the docked panel's content region
  (works today when the window is floating; docking only changes the host). Flagged for a manual
  interaction check (pan / zoom / wire a node with Node Graph docked + tabbed).
- **Output window:** unaffected — ImGui manages only the editor window's viewport (multi-viewport
  off), so the separate Output GLFW window and the shared-context blit loop are unchanged.

## Testing

This is ImGui UI glue with no GL-free logic to unit-test (DockBuilder needs a live ImGui + docking
context). Gates:

- **Build + suite:** the project compiles and links on the docking branch (verifying the backends
  and `imgui-node-editor` build against it), and the full `ctest` (`core_tests` + `gl_smoke`) still
  passes — nothing in the tested code paths is touched.
- **Screenshot:** `./build/shader_streamer --screenshot <png>` runs headless and produces the
  docked default layout (Node Graph center, Automation bottom); regenerate and eyeball it.
- **Manual interactive smoke:** dock/undock/tab a panel; quit and relaunch to confirm the layout
  persisted via `imgui.ini`; Reset Layout restores the default; confirm the node editor still
  pans/zooms/wires while docked.

## Out of scope (YAGNI — flag to pull in)

Multi-viewport (dragging panels out into their own OS windows); folding the Output into a dockable
ImGui panel (would lose the dedicated fullscreen output window); per-project layouts stored in
`.oss`; named/saved layout presets; docking the top transport/menu bar.

## Decided defaults (flag to change)

- Editor panels only; Output window unchanged; multi-viewport off.
- Default split: Node Graph center, Automation bottom 30%, Assets/Preferences right 25% (tabbed).
- Layout persists to `imgui.ini` (global, CWD); Reset Layout rebuilds the default.
- imgui pinned at `v1.91.5-docking`; `imgui_internal.h` (DockBuilder) confined to `ui/DockLayout.cpp`
  (plus the one `DockBuilderGetNode` guard in `Application.cpp`).
