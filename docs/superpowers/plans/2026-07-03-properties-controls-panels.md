# Properties + Controls panels — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Two dockable panels — **Properties** (fields + toggle buttons + connections) and **Controls** (sliders + grids) — that edit the currently selected node, while the graph canvas shows nodes as header + ports only.

**Architecture:** A GL-free `inputSlot` classifier routes each input to a panel; a GL-free `nodeConnectionSummary` feeds the connections list. Two new ImGui window units render the selected node's widgets with standard in-window popups. `NodeEditorPanel` exposes the selected node id and (last) drops all node-body UI + the deferred popup machinery; `PortWidgets` is removed.

**Tech Stack:** C++17, Dear ImGui (docking) + imgui-node-editor, doctest (`core_tests`). UI panels are app-only (no headless test), like Assets/Preferences.

**Reference spec:** `docs/superpowers/specs/2026-07-03-properties-controls-panels-design.md`

**Conventions (CLAUDE.md):**
- `src/core/` stays GL-free (`PanelSlot.h`, `nodeConnectionSummary` are pure logic). Panels live in `src/ui/`.
- Conventional Commits. Branch `feat/properties-controls-panels` (already created, off `develop`).
- Never `git add -A`/`git add .` — stage only the files each step names. Leave `build.sh`, `examples/`, `preferences.oss`, `project.oss`, `imgui.ini` untracked.
- Build: `cmake --build build --target <t> -j`; tests: `ctest --test-dir build --output-on-failure`. Re-run `cmake -S . -B build` after changing the source list in `CMakeLists.txt`.

**Context:** `Connection` is `{int srcNode, srcPort, dstNode, dstPort}` (`core/Connection.h`). `Graph` has `connections()`, `findNode(id)`, `nodes()`, `assets()`. `Port` (`core/Port.h`) has `type`, `minVal`, `maxVal`, `choices`, `integer`, `assetBacked`, `folderPicker`, `assetType`. `Value` is a `std::variant` (`std::get<float/bool/glm::vec4/std::string>`). `Node` has `inputs()`, `inputDefault(i)`, `name()`, `statusLine()`, and the button/grid hooks (`buttonCount/buttonLabel/buttonActive/buttonPending/onButtonPressed`, `gridRows/gridCols/gridCell/onGridCellPressed/gridRowLabel`). The editor uses `ed::GetSelectedNodes(ed::NodeId*, count)`; a `NodeId`'s `.Get()` is the node id (set to `n.id()`). Panels are ImGui windows drawn in `Application::frame` and docked by title in `ui/DockLayout.cpp`; the **View** menu is in `ui/TransportBar.cpp` driven by `ProjectBarIO` `bool*` fields.

The task order keeps every build green **and** the app editable throughout: the panels are added and wired *before* the inline node UI is removed.

---

### Task 1: `inputSlot` classifier + test

**Files:**
- Create: `src/core/PanelSlot.h`
- Create: `tests/test_panel_slot.cpp`
- Modify: `CMakeLists.txt` (register the test)

- [ ] **Step 1: Create the header**

Create `src/core/PanelSlot.h`:

```cpp
#pragma once
#include "core/Port.h"

namespace oss {

// Which panel edits a given input's widget.
enum class PanelSlot { Controls, Properties, None };

// Continuous Float sliders -> Controls; integer/choice Floats, Bool, Colour, String -> Properties;
// non-control ports (Texture/Audio/Midi/Vertex/Transform/Shader) have no widget -> None (they show
// only in the Properties connections list). GL-free.
inline PanelSlot inputSlot(const Port& p) {
    switch (p.type) {
        case PortType::Float:
            if (!p.choices.empty() || p.integer) return PanelSlot::Properties;  // dropdown / int field
            return PanelSlot::Controls;                                          // continuous slider
        case PortType::Bool:
        case PortType::Colour:
        case PortType::String:
            return PanelSlot::Properties;
        default:
            return PanelSlot::None;
    }
}

} // namespace oss
```

- [ ] **Step 2: Write the failing test**

Create `tests/test_panel_slot.cpp`:

```cpp
#include <doctest/doctest.h>
#include "core/PanelSlot.h"

using namespace oss;

static Port mk(PortType t) { Port p; p.type = t; return p; }

TEST_CASE("inputSlot routes control inputs to Controls, fields to Properties") {
    CHECK(inputSlot(mk(PortType::Float)) == PanelSlot::Controls);           // plain slider

    Port choice = mk(PortType::Float); choice.choices = {"a", "b"};
    CHECK(inputSlot(choice) == PanelSlot::Properties);                       // dropdown
    Port intp = mk(PortType::Float); intp.integer = true;
    CHECK(inputSlot(intp) == PanelSlot::Properties);                         // integer field

    CHECK(inputSlot(mk(PortType::Bool))   == PanelSlot::Properties);
    CHECK(inputSlot(mk(PortType::Colour)) == PanelSlot::Properties);
    CHECK(inputSlot(mk(PortType::String)) == PanelSlot::Properties);

    CHECK(inputSlot(mk(PortType::Texture)) == PanelSlot::None);
    CHECK(inputSlot(mk(PortType::Audio))   == PanelSlot::None);
}
```

- [ ] **Step 3: Register the test in CMake**

In `CMakeLists.txt`, in the `core_tests` source list, after `tests/test_image_sequence.cpp` add:

```cmake
  tests/test_panel_slot.cpp
```

- [ ] **Step 4: Run — fail then pass**

Run: `cmake -S . -B build && cmake --build build --target core_tests -j && ctest --test-dir build -R core_tests --output-on-failure`
Expected: compiles and PASSES. (If you write the test before the header, it fails to compile first; here the header is trivial and lands with it — a characterization test.)

- [ ] **Step 5: Commit**

```bash
git add src/core/PanelSlot.h tests/test_panel_slot.cpp CMakeLists.txt
git commit -m "feat(core): inputSlot panel classifier"
```

---

### Task 2: `nodeConnectionSummary` (Graph helper) + test

**Files:**
- Modify: `src/core/Graph.h`, `src/core/Graph.cpp`
- Test: `tests/test_graph.cpp`

- [ ] **Step 1: Declare the helper**

In `src/core/Graph.h`, **after** the `class Graph { ... };` definition and still **inside** `namespace oss` (so `Graph` is already complete — no forward-decl needed), add:

```cpp
// The selected node's connections, for the Properties panel. `inputs` lists this node's connected
// input ports (port <- otherNode.otherPort); `outputs` lists its outgoing links (port -> otherNode.
// otherPort). GL-free. Defined in Graph.cpp.
struct NodeConnections {
    struct Link { int port; int otherNode; int otherPort; };
    std::vector<Link> inputs;
    std::vector<Link> outputs;
};
NodeConnections nodeConnectionSummary(const Graph& g, int nodeId);
```

(`<vector>` is already included by Graph.h via its existing members.)

- [ ] **Step 2: Write the failing test**

Append to the **end** of `tests/test_graph.cpp` (the file already `#include`s `core/Graph.h` at line ~45 and defines file-local `ConstFloat` — a Float `out` at port 0 — and `AddFloats` — Float inputs `a`(0)/`b`(1), Float output `sum`(0)):

```cpp
TEST_CASE("nodeConnectionSummary lists a node's input sources and output destinations") {
    Graph g;
    int a = g.addNode(std::make_unique<ConstFloat>(1.0f));   // out 0
    int b = g.addNode(std::make_unique<AddFloats>());        // in 0, out 0
    int c = g.addNode(std::make_unique<AddFloats>());        // in 0
    REQUIRE(g.connect(a, 0, b, 0));   // a.out0 -> b.in0
    REQUIRE(g.connect(b, 0, c, 0));   // b.out0 -> c.in0

    NodeConnections nb = nodeConnectionSummary(g, b);
    REQUIRE(nb.inputs.size() == 1);
    CHECK(nb.inputs[0].port == 0);
    CHECK(nb.inputs[0].otherNode == a);
    CHECK(nb.inputs[0].otherPort == 0);
    REQUIRE(nb.outputs.size() == 1);
    CHECK(nb.outputs[0].port == 0);
    CHECK(nb.outputs[0].otherNode == c);
    CHECK(nb.outputs[0].otherPort == 0);

    CHECK(nodeConnectionSummary(g, a).inputs.empty());   // a (ConstFloat) has no inputs
}
```

- [ ] **Step 3: Run to verify it fails**

Run: `cmake --build build --target core_tests -j`
Expected: FAIL to link/compile — `nodeConnectionSummary` undefined.

- [ ] **Step 4: Implement**

In `src/core/Graph.cpp`, add:

```cpp
NodeConnections nodeConnectionSummary(const Graph& g, int nodeId) {
    NodeConnections out;
    for (const Connection& c : g.connections()) {
        if (c.dstNode == nodeId) out.inputs.push_back({ c.dstPort, c.srcNode, c.srcPort });
        if (c.srcNode == nodeId) out.outputs.push_back({ c.srcPort, c.dstNode, c.dstPort });
    }
    return out;
}
```

- [ ] **Step 5: Run to verify it passes**

Run: `cmake --build build --target core_tests -j && ctest --test-dir build -R core_tests --output-on-failure`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add src/core/Graph.h src/core/Graph.cpp tests/test_graph.cpp
git commit -m "feat(core): nodeConnectionSummary helper"
```

---

### Task 3: expose the selected node id from `NodeEditorPanel`

**Files:**
- Modify: `src/ui/NodeEditorPanel.h`, `src/ui/NodeEditorPanel.cpp`

- [ ] **Step 1: Add the accessor to the header**

In `src/ui/NodeEditorPanel.h`, add a public method:

```cpp
    // The primary selected node's id (first of the editor's selection), or -1 if none.
    // Valid after draw(); used by the Properties/Controls panels.
    int selectedNodeId() const;
```

- [ ] **Step 2: Track + return it in the .cpp**

In `src/ui/NodeEditorPanel.cpp`, add an `int selectedNode = -1;` field to the `Impl` struct (next to the other Impl fields). Near the end of `draw`, after the node loop / selection handling (anywhere inside the `ed::Begin("graph") ... ed::End()` block where `ed::` calls are valid), record the primary selection:

```cpp
    // Record the primary selection for the Properties/Controls panels.
    {
        int selCount = ed::GetSelectedObjectCount();
        impl_->selectedNode = -1;
        if (selCount > 0) {
            std::vector<ed::NodeId> sel(selCount);
            int nNodes = ed::GetSelectedNodes(sel.data(), selCount);
            if (nNodes > 0) impl_->selectedNode = (int)sel[0].Get();
        }
    }
```

Then add the accessor definition (outside `draw`):

```cpp
int NodeEditorPanel::selectedNodeId() const { return impl_->selectedNode; }
```

- [ ] **Step 3: Build**

Run: `cmake --build build --target shader_streamer -j`
Expected: builds clean.

- [ ] **Step 4: Commit**

```bash
git add src/ui/NodeEditorPanel.h src/ui/NodeEditorPanel.cpp
git commit -m "feat(ui): NodeEditorPanel exposes the selected node id"
```

---

### Task 4: `ControlsPanel` (sliders + grids)

**Files:**
- Create: `src/ui/ControlsPanel.h`, `src/ui/ControlsPanel.cpp`
- Modify: `CMakeLists.txt` (add the source)

- [ ] **Step 1: Header**

Create `src/ui/ControlsPanel.h`:

```cpp
#pragma once
#include "core/Graph.h"

namespace oss {

// The "Controls" window: continuous sliders + tri-state grids for the selected node.
class ControlsPanel {
public:
    void draw(Graph& graph, int selectedNodeId, bool* open);
};

} // namespace oss
```

- [ ] **Step 2: Implementation**

Create `src/ui/ControlsPanel.cpp`:

```cpp
#include "ui/ControlsPanel.h"
#include "core/PanelSlot.h"
#include <imgui.h>
#include <variant>
#include <string>

namespace oss {

void ControlsPanel::draw(Graph& graph, int selectedNodeId, bool* open) {
    if (!open || !*open) return;
    if (!ImGui::Begin("Controls", open)) { ImGui::End(); return; }

    Node* n = graph.findNode(selectedNodeId);
    if (!n) { ImGui::TextDisabled("Select a node"); ImGui::End(); return; }

    ImGui::TextUnformatted(n->name().c_str());
    ImGui::Separator();

    bool any = false;
    for (std::size_t i = 0; i < n->inputs().size(); ++i) {
        const Port& p = n->inputs()[i];
        if (inputSlot(p) != PanelSlot::Controls) continue;
        any = true;
        Value& v = n->inputDefault(i);
        ImGui::PushID((int)i);
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::SliderFloat(p.name.c_str(), &std::get<float>(v), p.minVal, p.maxVal);
        ImGui::PopID();
    }

    int grows = n->gridRows(), gcols = n->gridCols();
    if (grows > 0 && gcols > 0) {
        any = true;
        for (int r = 0; r < grows; ++r) {
            std::string rl = n->gridRowLabel(r);
            if (!rl.empty()) { ImGui::TextUnformatted(rl.c_str()); ImGui::SameLine(); }
            for (int c = 0; c < gcols; ++c) {
                if (c) ImGui::SameLine();
                int st = n->gridCell(r, c);
                ImU32 col = st == 2 ? IM_COL32(250, 210,  70, 255)
                          : st == 1 ? IM_COL32(120, 150, 200, 255)
                                    : IM_COL32( 45,  48,  56, 255);
                ImGui::PushStyleColor(ImGuiCol_Button, col);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, col);
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, col);
                std::string id = "##g" + std::to_string(r) + "_" + std::to_string(c);
                if (ImGui::Button(id.c_str(), ImVec2(16, 16))) n->onGridCellPressed(r, c);
                ImGui::PopStyleColor(3);
            }
        }
    }

    if (!any) ImGui::TextDisabled("(no controls)");
    ImGui::End();
}

} // namespace oss
```

- [ ] **Step 3: Add to CMake**

In `CMakeLists.txt`, in `APP_SOURCES`, after `src/ui/DockLayout.cpp` add:

```cmake
  src/ui/ControlsPanel.cpp
```

- [ ] **Step 4: Build**

Run: `cmake -S . -B build && cmake --build build --target shader_streamer -j`
Expected: builds clean (the unit compiles even though nothing draws it yet).

- [ ] **Step 5: Commit**

```bash
git add src/ui/ControlsPanel.h src/ui/ControlsPanel.cpp CMakeLists.txt
git commit -m "feat(ui): Controls panel (sliders + grids)"
```

---

### Task 5: `PropertiesPanel` (fields + toggle buttons + connections)

**Files:**
- Create: `src/ui/PropertiesPanel.h`, `src/ui/PropertiesPanel.cpp`
- Modify: `CMakeLists.txt` (add the source)

- [ ] **Step 1: Header**

Create `src/ui/PropertiesPanel.h`:

```cpp
#pragma once
#include "core/Graph.h"

namespace oss {

// The "Properties" window: field-style inputs (integer/colour/text/dropdown/checkbox), the node's
// toggle/preset buttons, and a read-only connections list, for the selected node.
class PropertiesPanel {
public:
    void draw(Graph& graph, int selectedNodeId, bool* open);
};

} // namespace oss
```

- [ ] **Step 2: Implementation**

Create `src/ui/PropertiesPanel.cpp`:

```cpp
#include "ui/PropertiesPanel.h"
#include "core/PanelSlot.h"
#include "core/AssetTree.h"     // uniqueAssetFolders
#include <imgui.h>
#include <glm/vec4.hpp>
#include <variant>
#include <string>
#include <cstdio>
#include <cmath>
#include <algorithm>

namespace oss {

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

void PropertiesPanel::draw(Graph& graph, int selectedNodeId, bool* open) {
    if (!open || !*open) return;
    if (!ImGui::Begin("Properties", open)) { ImGui::End(); return; }

    Node* n = graph.findNode(selectedNodeId);
    if (!n) { ImGui::TextDisabled("Select a node"); ImGui::End(); return; }

    ImGui::TextUnformatted(n->name().c_str());
    ImGui::Separator();

    for (std::size_t i = 0; i < n->inputs().size(); ++i) {
        const Port& p = n->inputs()[i];
        if (inputSlot(p) != PanelSlot::Properties) continue;
        Value& v = n->inputDefault(i);
        ImGui::PushID((int)i);
        ImGui::SetNextItemWidth(180.0f);

        if (p.type == PortType::Colour) {
            auto& c = std::get<glm::vec4>(v);
            ImGui::ColorEdit4(p.name.c_str(), &c.x, ImGuiColorEditFlags_AlphaBar);
        } else if (p.type == PortType::Bool) {
            ImGui::Checkbox(p.name.c_str(), &std::get<bool>(v));
        } else if (p.type == PortType::Float && !p.choices.empty()) {
            int idx = std::clamp((int)std::lround(std::get<float>(v)), 0, (int)p.choices.size() - 1);
            if (ImGui::BeginCombo(p.name.c_str(), p.choices[idx].c_str())) {
                for (int k = 0; k < (int)p.choices.size(); ++k)
                    if (ImGui::Selectable(p.choices[k].c_str(), k == idx)) std::get<float>(v) = (float)k;
                ImGui::EndCombo();
            }
        } else if (p.type == PortType::Float && p.integer) {
            int iv = (int)std::lround(std::get<float>(v));
            if (ImGui::DragInt(p.name.c_str(), &iv, 1.0f, (int)p.minVal, (int)p.maxVal))
                std::get<float>(v) = (float)iv;
        } else if (p.type == PortType::String) {
            auto& s = std::get<std::string>(v);
            char buf[256];
            std::snprintf(buf, sizeof(buf), "%s", s.c_str());
            if (p.assetBacked) {
                ImGui::SetNextItemWidth(180.0f - ImGui::GetFrameHeight() - 4.0f);
                if (ImGui::InputText("##s", buf, sizeof(buf))) s = buf;
                ImGui::SameLine(0.0f, 2.0f);
                if (ImGui::ArrowButton("##pick", ImGuiDir_Down)) ImGui::OpenPopup("pick");
                ImGui::SameLine(); ImGui::TextUnformatted(p.name.c_str());
                if (ImGui::BeginPopup("pick")) {
                    if (p.folderPicker) {
                        std::vector<std::string> folders = uniqueAssetFolders(graph.assets().byType(p.assetType));
                        if (folders.empty()) ImGui::TextDisabled("No %s folders", assetTypeName(p.assetType));
                        for (const std::string& f : folders)
                            if (ImGui::Selectable(f.c_str(), f == s)) { s = f; ImGui::CloseCurrentPopup(); }
                    } else {
                        std::vector<const Asset*> assets = graph.assets().byType(p.assetType);
                        if (assets.empty()) ImGui::TextDisabled("No %s assets", assetTypeName(p.assetType));
                        for (const Asset* a : assets) {
                            std::string label = a->label.empty() ? a->path : a->label;
                            if (label.empty()) label = "(unnamed)";
                            ImGui::PushID(a->id);
                            if (ImGui::Selectable(label.c_str(), a->path == s)) { s = a->path; ImGui::CloseCurrentPopup(); }
                            ImGui::PopID();
                        }
                    }
                    ImGui::EndPopup();
                }
            } else {
                if (ImGui::InputText(p.name.c_str(), buf, sizeof(buf))) s = buf;
            }
        }
        ImGui::PopID();
    }

    // Toggle / preset buttons (the node's button-bank hook).
    int nbtn = n->buttonCount();
    if (nbtn > 0) {
        ImGui::Separator();
        int bAct = n->buttonActive(), bPend = n->buttonPending();
        for (int b = 0; b < nbtn; ++b) {
            if (b) ImGui::SameLine();
            int pushed = 0;
            if (b == bAct)       { ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(70, 130, 200, 255)); pushed = 1; }
            else if (b == bPend) { ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(70,  90, 110, 255)); pushed = 1; }
            if (ImGui::SmallButton((n->buttonLabel(b) + "##btn" + std::to_string(b)).c_str()))
                n->onButtonPressed(b);
            if (pushed) ImGui::PopStyleColor(pushed);
        }
    }

    // Connections (read-only).
    ImGui::Separator();
    ImGui::TextUnformatted("Connections");
    NodeConnections cc = nodeConnectionSummary(graph, selectedNodeId);
    auto label = [&](int nodeId) -> const char* {
        Node* o = graph.findNode(nodeId);
        return o ? o->name().c_str() : "?";
    };
    if (cc.inputs.empty() && cc.outputs.empty()) {
        ImGui::TextDisabled("(none)");
    } else {
        for (const auto& l : cc.inputs)
            ImGui::BulletText("%s <- %s : out %d", n->inputs()[(std::size_t)l.port].name.c_str(),
                              label(l.otherNode), l.otherPort);
        for (const auto& l : cc.outputs)
            ImGui::BulletText("%s -> %s : in %d", n->outputs()[(std::size_t)l.port].name.c_str(),
                              label(l.otherNode), l.otherPort);
    }

    ImGui::End();
}

} // namespace oss
```

- [ ] **Step 3: Add to CMake**

In `CMakeLists.txt`, in `APP_SOURCES`, after `src/ui/ControlsPanel.cpp` add:

```cmake
  src/ui/PropertiesPanel.cpp
```

- [ ] **Step 4: Build**

Run: `cmake -S . -B build && cmake --build build --target shader_streamer -j`
Expected: builds clean.

- [ ] **Step 5: Commit**

```bash
git add src/ui/PropertiesPanel.h src/ui/PropertiesPanel.cpp CMakeLists.txt
git commit -m "feat(ui): Properties panel (fields + buttons + connections)"
```

---

### Task 6: wire the panels into Application + layout + View menu

**Files:**
- Modify: `src/app/Application.h`, `src/app/Application.cpp`
- Modify: `src/ui/DockLayout.cpp`
- Modify: `src/ui/TransportBar.h` (`ProjectBarIO`), `src/ui/TransportBar.cpp` (View menu)

- [ ] **Step 1: Application members**

In `src/app/Application.h`: add includes next to the other panel includes:

```cpp
#include "ui/PropertiesPanel.h"
#include "ui/ControlsPanel.h"
```

Add members next to `assets_` / the show-flags:

```cpp
    PropertiesPanel  properties_;
    ControlsPanel    controls_;
```

and next to `showAssets_`:

```cpp
    bool             showProperties_ = true;
    bool             showControls_ = true;
```

- [ ] **Step 2: Draw the panels in `frame`**

In `src/app/Application.cpp`, in `frame`, set the two new `ProjectBarIO` fields (next to `io.showAssets = ...`):

```cpp
    io.showProperties = &showProperties_;
    io.showControls = &showControls_;
```

and after `editor_.draw(...)` (which computes the selection) and near the other panel draws, add:

```cpp
    int selNode = editor_.selectedNodeId();
    properties_.draw(graph_, selNode, &showProperties_);
    controls_.draw(graph_, selNode, &showControls_);
```

- [ ] **Step 3: View menu fields + items**

In `src/ui/TransportBar.h`, add to `struct ProjectBarIO` (after `showAssets`):

```cpp
    bool*       showProperties = nullptr;   // View > Properties
    bool*       showControls   = nullptr;   // View > Controls
```

In `src/ui/TransportBar.cpp`, update the View-menu guard and items. Change the guard condition:

```cpp
    if (io && (io->showPreferences || io->showAssets || io->showProperties || io->showControls || io->onResetLayout)) {
```

and inside the `BeginMenu("View")` block, after the `Assets` item add:

```cpp
            if (io->showProperties) ImGui::MenuItem("Properties", nullptr, io->showProperties);
            if (io->showControls)   ImGui::MenuItem("Controls",   nullptr, io->showControls);
```

- [ ] **Step 4: Default dock layout**

In `src/ui/DockLayout.cpp`, in `buildDefaultDockLayout`, split the right region so Properties sits above Controls, and dock the windows. Replace the body from the `rightId` split through `DockBuilderFinish` with:

```cpp
    ImGuiID mainId = dockspaceId, bottomId = 0, rightId = 0, rightBottomId = 0;
    bottomId      = ImGui::DockBuilderSplitNode(mainId,  ImGuiDir_Down,  0.30f, nullptr, &mainId);
    rightId       = ImGui::DockBuilderSplitNode(mainId,  ImGuiDir_Right, 0.28f, nullptr, &mainId);
    rightBottomId = ImGui::DockBuilderSplitNode(rightId, ImGuiDir_Down,  0.55f, nullptr, &rightId);

    ImGui::DockBuilderDockWindow("Node Graph",  mainId);
    ImGui::DockBuilderDockWindow("Automation",  bottomId);
    ImGui::DockBuilderDockWindow("Properties",  rightId);         // upper right
    ImGui::DockBuilderDockWindow("Assets",      rightId);         // tabbed with Properties
    ImGui::DockBuilderDockWindow("Preferences", rightId);         // tabbed too
    ImGui::DockBuilderDockWindow("Controls",    rightBottomId);   // lower right
    ImGui::DockBuilderFinish(dockspaceId);
```

- [ ] **Step 5: Build + run the app briefly**

Run: `cmake -S . -B build && cmake --build build --target shader_streamer -j`
Expected: builds clean. (Manual, optional: run `./build/shader_streamer`, select a node — Properties shows its fields/connections, Controls shows its sliders. Inline node widgets are still present at this point; they're removed in Task 7.)

- [ ] **Step 6: Commit**

```bash
git add src/app/Application.h src/app/Application.cpp src/ui/DockLayout.cpp src/ui/TransportBar.h src/ui/TransportBar.cpp
git commit -m "feat(app): wire Properties + Controls panels (layout + View menu)"
```

---

### Task 7: strip node-body UI from `NodeEditorPanel`; remove `PortWidgets`

**Files:**
- Modify: `src/ui/NodeEditorPanel.cpp`
- Delete: `src/ui/PortWidgets.h`, `src/ui/PortWidgets.cpp`
- Modify: `CMakeLists.txt` (drop `PortWidgets.cpp`)

- [ ] **Step 1: Strip the node body**

In `src/ui/NodeEditorPanel.cpp`, in the per-node render loop (inside `for (auto& up : graph.nodes())`), keep the node name + `statusLine()` and the input/output **pin** loops, but **remove**:
- the button-bank block (`int nbtn = n.buttonCount(); ... ` through its closing `}`),
- the grid block (`int grows = n.gridRows(), gcols = n.gridCols(); ...` through its closing `}`),
- the inline-widget call inside the input-pin loop — remove the `if (!graph.isInputConnected(...)) { ImGui::SameLine(); if (drawInlineInputWidget(n, i)) { ...set pending popup... } }` block so the input loop just draws the pin + `ImGui::Text("-> %s", ...)`.

The input-pin loop should reduce to:

```cpp
        for (std::size_t i = 0; i < n.inputs().size(); ++i) {
            ed::BeginPin(ed::PinId(pinId(n.id(), (int)i, false)), ed::PinKind::Input);
            ImGui::Text("-> %s", n.inputs()[i].name.c_str());
            ed::EndPin();
        }
```

- [ ] **Step 2: Remove the deferred popup machinery**

Still in `src/ui/NodeEditorPanel.cpp`:
- Remove the `NodePopup` block — the `if (impl_->pendingPopupNode >= 0) { ... ImGui::OpenPopup("NodePopup"); }` setup and the whole `if (ImGui::BeginPopup("NodePopup")) { ... }` handler (the colour / asset / folder / choice rendering).
- Remove the now-unused `Impl` fields: `pendingPopupNode`, `pendingPopupPort`, `pendingPopupPos`, `openPopupNode`, `openPopupPort`, `openPopupPos`.
- Remove the now-unused includes `#include "ui/PortWidgets.h"` and `#include "core/AssetTree.h"`, and the file-local `assetTypeName` helper if it is no longer referenced.

Keep: the `ed::` link create/delete handling, node delete (Backspace + selection), the background **add-node** context menu, the right-click **Automation** binding context menu, and the `selectedNode` recording from Task 3.

- [ ] **Step 3: Delete PortWidgets + drop from CMake**

```bash
git rm src/ui/PortWidgets.h src/ui/PortWidgets.cpp
```

In `CMakeLists.txt`, remove the `  src/ui/PortWidgets.cpp` line from `APP_SOURCES`.

- [ ] **Step 4: Build**

Run: `cmake -S . -B build && cmake --build build --target shader_streamer -j`
Expected: builds clean. If the compiler flags a leftover reference to `drawInlineInputWidget`, `NodePopup`, or a removed Impl field, remove that reference too (it belongs to the stripped UI).

- [ ] **Step 5: Verify tests still pass**

Run: `ctest --test-dir build --output-on-failure`
Expected: `core_tests` + `gl_smoke` pass (neither exercises the editor UI).

- [ ] **Step 6: Commit**

```bash
git add src/ui/NodeEditorPanel.cpp CMakeLists.txt
git commit -m "refactor(ui): graph nodes render header + ports only (edit in panels)"
```

---

### Task 8: Documentation

**Files:**
- Modify: `CLAUDE.md`
- Modify: `README.md`

- [ ] **Step 1: Update CLAUDE.md**

In `CLAUDE.md`, in the section describing the dockable panels / editor (near the "dockable" note about Node Graph / Automation / Assets / Preferences), add a bullet:

```markdown
- **Properties + Controls panels** — the node editor's canvas shows nodes as **header + ports only**;
  editing a node happens in two dockable panels driven by the editor's current selection
  (`NodeEditorPanel::selectedNodeId`). A GL-free `core/PanelSlot.h` `inputSlot` classifier routes each
  input: continuous Float sliders + tri-state grids → **Controls** (`ui/ControlsPanel`); integer/choice
  fields, colour, text (+ asset/folder ▾ picker), checkboxes, and the node's toggle/preset buttons →
  **Properties** (`ui/PropertiesPanel`), which also shows a read-only connections list from the GL-free
  `nodeConnectionSummary` (`core/Graph`). The panels are normal ImGui windows, so their popups use
  standard ImGui (the old canvas-coordinate `NodePopup`/`PortWidgets` machinery was removed). `inputSlot`
  + `nodeConnectionSummary` are unit-tested; the panels are app-only.
```

Also update the earlier "dockable" sentence that lists the panels (Node Graph, Automation, Assets, Preferences) to include Properties + Controls.

- [ ] **Step 2: Update README.md**

In `README.md`, where the editor/UI is described, add a short line:

```markdown
- **Properties & Controls panels** — select a node to edit it: fields (integer, colour, text, dropdowns), checkboxes, toggle buttons, and its connections in **Properties**; sliders and step grids in **Controls**. The graph canvas stays a clean header-and-ports flow diagram.
```

- [ ] **Step 3: Commit**

```bash
git add CLAUDE.md README.md
git commit -m "docs: Properties + Controls panels"
```

---

## Final verification (after all tasks)

- [ ] Full reconfigure + build: `cmake -S . -B build && cmake --build build -j`
- [ ] All tests: `ctest --test-dir build --output-on-failure` — `core_tests` (incl. `inputSlot` + `nodeConnectionSummary`) + `gl_smoke` pass.
- [ ] Manual (needs a display): run `./build/shader_streamer`. Confirm graph nodes show only header + pins; selecting a node populates Properties (fields, buttons, connections) and Controls (sliders, grids); editing there updates evaluation; a Drum Machine's grid appears in Controls and its pattern buttons in Properties; the View menu toggles both panels; Reset Layout places them.
- [ ] Hand off to `superpowers:finishing-a-development-branch`.

## Notes for the implementer

- **Only stage the files each step names.** Never `git add -A`. Leave `build.sh`, `examples/`, `preferences.oss`, `project.oss`, `imgui.ini` untracked.
- Re-run `cmake -S . -B build` after Tasks 1, 4, 5, 7 (they change the `CMakeLists.txt` source/test lists).
- Tasks 4–5 add panel units that don't render until Task 6 — they must still compile (verify with a `shader_streamer` build).
- Task 7 is the only destructive step; do it last so the app stays editable throughout. If a removed symbol is still referenced, that reference is part of the stripped UI — remove it.
- `-FLT_MIN` as an item width means "stretch to fill" in ImGui; `<cfloat>` is pulled in by `imgui.h`, but if the compiler complains, add `#include <cfloat>` to `ControlsPanel.cpp`.
