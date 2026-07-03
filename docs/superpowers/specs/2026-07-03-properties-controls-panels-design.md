# Properties + Controls panels — design

**Date:** 2026-07-03
**Status:** Approved (brainstorm)
**Branch:** `feat/properties-controls-panels` (off `develop`)

## Goal

Two new dockable panels that edit the **currently selected** node. The graph canvas becomes a
clean flow diagram (header + ports only); every editable control moves into a **Properties** panel
(fields + toggles + connections) or a **Controls** panel (sliders + grids).

## Decisions (from brainstorm)

- **Graph nodes render header + ports only.** All inline input widgets, button banks, and grids
  are removed from the node body; the node shows its type name, status line, and input/output pins.
- **Routing by widget type** (for the selected node):
  - **Properties panel** — the "field-style" inputs: **integer** (numeric field), **colour**
    (picker), **text/string** (+ asset ▾ picker), **dropdown/choice** (combo), **checkbox** (Bool);
    the node's **toggle/preset buttons** (existing button-bank hook); and a read-only
    **connections** list.
  - **Controls panel** — the continuous **Float sliders**, and the tri-state **grids** (existing
    grid hook, e.g. the Drum Machine 4×16).
- **No editable node name** (dropped from an earlier draft). The header keeps showing the type name.
- **Integer inputs render as a numeric field** in Properties (not a slider). The **node right-click
  context menu** (add node / delete / bind Automation) stays in the graph.
- **Panels are normal ImGui windows**, so colour/dropdown/asset popups work with standard ImGui —
  removing the canvas-coordinate popup workaround the inline widgets needed.

## Architecture

| File | Change |
|---|---|
| `src/core/PanelSlot.h` | **New, GL-free.** `enum class PanelSlot { Controls, Properties, None }` + `inputSlot(const Port&)` classifier. |
| `src/core/Graph.h` / `.cpp` | **Add** a GL-free `nodeConnectionSummary(const Graph&, int nodeId)` returning the node's input sources + output destinations. |
| `src/ui/ControlsPanel.h` / `.cpp` | **New.** Draws the selected node's sliders + grids. |
| `src/ui/PropertiesPanel.h` / `.cpp` | **New.** Draws the selected node's fields + toggle buttons + connections. |
| `src/ui/NodeEditorPanel.h` / `.cpp` | Add `int selectedNodeId() const`; strip inline widgets, button/grid rendering, and the deferred `NodePopup` machinery — leave header + pins + links + node context menu. |
| `src/ui/PortWidgets.h` / `.cpp` | **Removed** (superseded by the panels' in-window rendering). Drop from `CMakeLists.txt`. |
| `src/app/Application.h` / `.cpp` | Own `ControlsPanel`/`PropertiesPanel` + `showControls_`/`showProperties_`; draw them with the editor's selected node id. |
| `src/ui/DockLayout.cpp` | Add **Properties** + **Controls** to the default layout (right side). |
| `src/ui/TransportBar.cpp`, `ProjectBarIO` | **View** menu toggles for Properties + Controls. |
| `CMakeLists.txt` | Add `ControlsPanel.cpp`/`PropertiesPanel.cpp`, remove `PortWidgets.cpp`; register new tests. |
| `tests/test_panel_slot.cpp` | **New.** `inputSlot` classifier cases. |
| `tests/test_graph.cpp` | Add `nodeConnectionSummary` cases. |
| `CLAUDE.md`, `README.md` | Document the panels. |

## Component detail

### `core/PanelSlot.h` (GL-free classifier)

```cpp
enum class PanelSlot { Controls, Properties, None };

// Which panel edits this input's widget. Continuous Float sliders -> Controls; integer/choice
// Floats, Bool, Colour, String -> Properties; non-control ports (Texture/Audio/Midi/Vertex/
// Transform/Shader) have no widget -> None (they appear only in the connections list).
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
            return PanelSlot::None;   // Texture/Audio/... : connection only
    }
}
```

### `nodeConnectionSummary(graph, nodeId)` (GL-free)

Returns, for the node, a list of `{ inputPort, srcNode, srcPort }` for connected inputs and
`{ outputPort, dstNode, dstPort }` for outgoing links (derived from `graph.connections()`), so the
Properties panel can render "← from <node> · <port>" / "→ to <node> · <port>" without touching GL.
Unit-tested.

### `NodeEditorPanel`

- **`int selectedNodeId() const`** — the primary selected node's id (the first of
  `ed::GetSelectedNodes`), or `-1` if none. Stored during `draw`.
- **Strip node-body UI:** remove the `drawInlineInputWidget` loop, the `buttonCount`/grid rendering,
  and the deferred `NodePopup` (choice/colour/asset picker) block. Each node renders: type name,
  `statusLine()`, input pins, output pins. Link create/delete, node delete, background add-node menu,
  and the right-click **Automation** binding stay.

### `ControlsPanel` (selected node)

`draw(Graph& graph, int selectedNodeId, bool* open)`. If no node: a "Select a node" hint. Otherwise,
for each input with `inputSlot(port) == Controls`, render its slider (continuous `SliderFloat`).
Then, if the node has a grid (`gridRows() > 0`), render the tri-state grid (the existing
`gridRows/gridCols/gridCell/onGridCellPressed/gridRowLabel` hooks) — standard ImGui buttons.

### `PropertiesPanel` (selected node)

`draw(Graph& graph, int selectedNodeId, bool* open)`. If no node: a "Select a node" hint. Otherwise:
- The node type name (read-only header).
- For each input with `inputSlot(port) == Properties`, render the field: integer (`DragInt`/
  `InputInt` over `[min,max]`), colour (`ColorEdit4`/picker), Bool (`Checkbox`), choice (`Combo`),
  string (`InputText`, plus a ▾ asset/folder picker for `assetBacked`/`folderPicker` reading
  `graph.assets()`). These use **standard in-window popups** (no Suspend/Resume hack).
- The node's toggle/preset **buttons** (`buttonCount/buttonLabel/buttonActive/buttonPending/
  onButtonPressed`).
- A read-only **Connections** section from `nodeConnectionSummary`.

### Application / layout / menu

`Application::frame` draws the editor, then reads `editor_.selectedNodeId()` and draws
`controls_.draw(graph_, sel, &showControls_)` and `properties_.draw(graph_, sel, &showProperties_)`.
`showControls_`/`showProperties_` default true, toggled by new **View** menu items (via `ProjectBarIO`
`bool*` fields like the existing `showAssets`/`showPreferences`). `DockLayout` docks **Properties**
and **Controls** on the right (Properties above Controls; Assets/Preferences tabbed alongside);
**Reset Layout** places them; open/closed state persists in `imgui.ini`.

## Data flow / error handling

- **Selection:** single node drives the panels; none → hint; multiple → the first selected. A node
  deleted while selected → `findNode` returns null → hint (no crash).
- Editing a widget writes the node's `inputDefault(i)` in place (as the inline widgets did), so
  evaluation and `.oss` persistence are unchanged. Connected inputs still show their widget (editing
  the default is harmless while connected, matching current behavior).
- The panels never hold node pointers across frames (look up by id each frame).

## Testing

- **`core_tests`**: `inputSlot` — Float slider → Controls; Float `integer` → Properties; Float
  `choices` → Properties; Bool/Colour/String → Properties; Texture/Audio → None.
  `nodeConnectionSummary` — a small graph (A.out → B.in) reports B's input source and A's output
  destination correctly; unconnected ports omitted.
- **App-only (no headless test)**: the ImGui panels + the node-body stripping, consistent with the
  Assets/Preferences panels (which have no headless test). Manual verification: select nodes, edit
  in both panels, confirm the graph node shows only header + ports.

## Out of scope (YAGNI)

- Editable node name/label; multi-node (batch) editing; drag-to-reorder controls; per-user panel
  presets; docking the Output window; moving the Automation timeline. Non-control ports get no
  editor beyond the connections list.
