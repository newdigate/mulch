# Grouped Automation Channels — Design

**Date:** 2026-06-17
**Status:** Approved (brainstorming) — ready for implementation plan

## Goal

Split automation into two structurally-distinct channel kinds, each with its own
origin and grouping in the Automation editor:

- **Stream channels** — come from `AutomationNode` modules. Adding an Automation
  node contributes a collapsible group of its 4 channels. They reach their target
  through an **edge** (wire the node's output Float into a Float input). Unchanged
  delivery mechanism.
- **UI channels** — bound directly to a single module **control** (a Float input
  parameter). Created on demand by right-clicking a node and picking the parameter.
  They have **no edge**; they write the sampled value straight into the control.
  They are grouped per module in a collapsible container.

A channel's kind is **structural** (which container holds it), so the user cannot
switch a channel between stream and ui. The old per-channel category combo and the
`AutoCategory` enum are removed.

## Architecture

`Graph` gains a GL-free `AutomationStore` that owns the UI channels plus a single
global song length (bars). Each frame, `Graph::evaluate` advances the transport,
then `AutomationStore::apply` samples every UI channel at the transport position
and writes the scaled value into its target node's input default — before input
resolution, so the automated value flows into evaluation. Stream channels are
untouched: `AutomationNode` keeps its output ports and is wired with edges.

The breakpoint curve is factored out of `AutomationNode` into a shared core type
(`AutoCurve`), used by both channel kinds. This also fixes the current layering
wart (the curve + sampling lived in `src/modules/` but is now needed by
`src/core/Graph`).

The Automation window becomes a list of collapsible **groups** over one shared,
horizontally-scrollable bar axis:

- one **stream group** per `AutomationNode` (its 4 channel lanes),
- one **ui group** per module that has UI channels (one lane per channel, labelled
  by the parameter name).

### Rejected alternatives

- **UI channel as a hidden node wired implicitly** — rejected: UI channels are not
  graph-wired; modelling them as nodes clutters the canvas and topology, and "write
  directly into a control" is not an edge.
- **One unified store for both kinds** — rejected: stream channels must stay a
  module with output ports/edges, so they cannot be edge-less store entries.

## Components

### `src/core/AutoCurve.h` (new, GL-free, header-only)

```cpp
struct AutoPoint { float bar = 0.0f; float value = 0.0f; };

// A piecewise-linear breakpoint curve over song bars, sampled to [0, 1].
// Points are kept sorted by bar; sample() holds at the first/last point.
struct AutoCurve {
    std::vector<AutoPoint> points;
    float sample(float bar) const;   // the existing interpolate-and-hold logic
};
```

`sample()` is the body currently in `AutomationNode::sample()`:

- empty → `0.0f`
- `bar <= points.front().bar` → `points.front().value`
- `bar >= points.back().bar` → `points.back().value`
- otherwise linear-interpolate the bracketing segment.

### `src/core/AutomationStore.{h,cpp}` (new, GL-free)

```cpp
struct UiAutomationChannel {
    int       nodeId;          // target node
    int       port;            // target Float input port index
    AutoCurve curve;
    float     outMin = 0.0f;   // scales sample() [0,1] -> [outMin, outMax]
    float     outMax = 1.0f;
};

class AutomationStore {
public:
    float lengthBars() const { return lengthBars_; }
    void  setLengthBars(float L) { lengthBars_ = std::max(1.0f, L); }

    // Create a channel for (nodeId, port). Idempotent: returns the existing
    // channel if one already targets that node+port. Seeds outMin/outMax from
    // the port's slider range and inserts one breakpoint at bar 0 equal to the
    // control's current (normalised) value, so creation never changes the value.
    // Returns nullptr if the node/port is missing or the port is not a Float.
    UiAutomationChannel* add(Graph& graph, int nodeId, int port);

    void remove(int nodeId, int port);   // drop the channel for node+port (if any)
    void removeNode(int nodeId);          // drop all channels targeting a node

    const std::vector<UiAutomationChannel>& channels() const { return channels_; }
    std::vector<UiAutomationChannel>&        channels()       { return channels_; }
    UiAutomationChannel* find(int nodeId, int port);

    // Sample each channel at transport.bars() and write the scaled value into the
    // target node's input default -- but only when that input is unconnected (an
    // edge wins). Skips channels whose target node/port no longer exists.
    void apply(Graph& graph, const Transport& transport);

private:
    std::vector<UiAutomationChannel> channels_;
    float lengthBars_ = 8.0f;
};
```

Layering note: `AutomationStore.h` declares the class with members only; the
`.cpp` includes `Graph.h`/`Node.h` for `add`/`apply`. `Graph.h` includes
`AutomationStore.h` (needs the type's size to own it by value). No include cycle.

### `src/core/Graph.{h,cpp}` (modify)

- Own `AutomationStore automation_;` with accessor `AutomationStore& automation()`
  / `const AutomationStore& automation() const`.
- `evaluate(dt)`: after `transport_.advance(dt)`, call
  `automation_.apply(*this, transport_)`, then run the existing topological eval
  loop. (Apply mutates input defaults; resolution reads them, so order matters.)
- `removeNode(nodeId)`: call `automation_.removeNode(nodeId)` so a deleted module
  drops its UI channels.

### `src/modules/AutomationNode.h` (modify)

- Each channel becomes an `AutoCurve` (drop the local `AutoPoint`/`sample`).
- Remove `AutoCategory`, `category()`, `setCategory()`, `cat_[]`.
- Remove `lengthBars_`/`setLengthBars`/`lengthBars`/`currentBar_`/`currentBar`
  (length is now global on the store; the editor reads the playhead from the
  transport).
- `evaluate`: sample each channel at `ctx.transport->bars()` (no clamp — `sample`
  holds at the ends), scale by `[outMin, outMax]`, write the output.
- Keep: `kChannels = 4`, `channel(c)` (now returns `AutoCurve&`), `outMin/outMax`,
  `setOutRange`.

### `src/ui/NodeEditorPanel.cpp` (modify)

Inside the existing `ed::Suspend()/Resume()` block, add a node context menu:

```cpp
static ed::NodeId ctxNode;
if (ed::ShowNodeContextMenu(&ctxNode)) ImGui::OpenPopup("NodeMenu");
if (ImGui::BeginPopup("NodeMenu")) {
    int nid = (int)ctxNode.Get();
    Node* n = graph.findNode(nid);
    if (n) {
        ImGui::TextDisabled("Automate (UI)");
        bool any = false;
        for (std::size_t i = 0; i < n->inputs().size(); ++i) {
            if (n->inputs()[i].type != PortType::Float) continue;
            any = true;
            bool on = graph.automation().find(nid, (int)i) != nullptr;
            if (ImGui::MenuItem(n->inputs()[i].name.c_str(), nullptr, on)) {
                if (on) graph.automation().remove(nid, (int)i);
                else    graph.automation().add(graph, nid, (int)i);
            }
        }
        if (!any) ImGui::TextDisabled("No automatable parameters");
    }
    ImGui::EndPopup();
}
```

`ctxNode` is panel state. No new plumbing: `draw` already receives `Graph& graph`.

### `src/ui/AutomationPanel.{h,cpp}` (rewrite)

Render a flat list of rows built each frame from the live graph + store:

1. **Toolbar** — `Length (bars)` edits `graph.automation().setLengthBars(...)`.
2. **Ruler row** (replaces the old reserved row) — bar numbers across the shared
   axis; carries the playhead tick.
3. **Stream groups** — for each `AutomationNode`: a collapsible header
   `Automation #<id>`; when expanded, 4 channel lanes (cyan/orange/green/pink),
   each with a left header (channel name, min/max `DragFloat`, `clr`).
4. **UI groups** — for each target node in `store.channels()` (grouped by
   `nodeId`): a collapsible header `<node name> #<id>`; when expanded, one lane per
   channel, left header = parameter name + min/max + `clr` + a delete (`x`) button
   that calls `store.remove(nodeId, port)`.

Shared rendering: a `LaneView { AutoCurve* curve; float outMin, outMax; ImU32 col;
}` drives the draw-list polyline + breakpoints + the mouse add/drag/delete editing
(generalised from the current per-channel code to operate on an `AutoCurve&`). The
red playhead is drawn at `clamp(transport.bars(), 0, lengthBars)` across all lanes.

Collapse state persists in the panel keyed by group identity. Stream and ui groups
are both keyed by node id; to avoid collision the key encodes the kind (e.g.
`std::map<long,bool>` with `key = nodeId*2 + isUi`).

Drag state stays panel-resident but is generalised: instead of
`dragChannel_/dragPoint_` (channel index), track the curve being edited and the
point index — e.g. `AutoCurve* dragCurve_ = nullptr; int dragPoint_ = -1;`. The
edited `AutoCurve*` is stable within a frame (it points into a node or the store);
re-resolve or clear it when the mouse is released.

### `src/main.cpp` (modify — screenshot demo)

Drop the `setCategory` call. After adding the Automation node, also create a UI
channel: add a node with a Float input (e.g. a `Wireframe`, whose `spin` input is
Float with range [0, 2]) and call `app.graph().automation().add(app.graph(),
nodeId, port)` for that input, then push a couple of breakpoints onto the returned
channel's curve — so the captured grid shows both a stream group and a ui group.

### `CMakeLists.txt` (modify)

Add `src/core/AutomationStore.cpp` to `APP_SOURCES`, `gl_smoke`, and `core_tests`.
Add the new `tests/test_auto_curve.cpp` / `tests/test_automation_store.cpp` to
`core_tests`.

## Data flow

```
toolbar Length ─────────────► AutomationStore.lengthBars  (editor x-axis width)

right-click node ► submenu ► AutomationStore.add(graph, nodeId, port)
                                    │ seeds range from port, point at bar 0 = current value
                                    ▼
Graph::evaluate(dt):
  transport.advance(dt)
  AutomationStore.apply(graph, transport):
      for ch in channels:
        if input(ch.nodeId, ch.port) unconnected:
          v = ch.curve.sample(transport.bars())          // [0,1]
          node.inputDefault(ch.port) = outMin + v*(outMax-outMin)
  topological eval (reads the now-automated defaults)

AutomationNode::evaluate(ctx):                            // stream, unchanged path
  for c in 0..4: out(c, outMin[c] + curve[c].sample(transport.bars())*(...))
```

## Lifetime / edge cases

- Removing a node drops its UI channels (`Graph::removeNode` → `removeNode`).
- Removing an `AutomationNode` removes its stream group implicitly (the panel
  iterates current nodes).
- `add` on an already-automated (node, port) returns the existing channel (no
  duplicate lane).
- A connected input is skipped by `apply` (the edge value wins); the lane is still
  shown and editable — it simply has no effect until the edge is removed.
- Only Float inputs are automatable (curves are scalar). Non-Float controls do not
  appear in the submenu. (Bool/Colour are out of scope for this change.)
- Creating a channel never changes the live value: the single bar-0 breakpoint
  equals the control's current normalised value.

## Testing

GL-free doctest unit tests (run under `core_tests`):

- **`tests/test_auto_curve.cpp`** — empty → 0; interpolate mid-segment; hold before
  first / after last point.
- **`tests/test_automation_store.cpp`**
  - `add` seeds `outMin/outMax` from the port range and inserts a bar-0 point equal
    to the control's current value; sampling at bar 0 returns that value.
  - `add` is idempotent (second call returns the same channel; count stays 1).
  - `add` returns nullptr for a missing node, missing port, or non-Float port.
  - `apply` writes the scaled sampled value into the target node's input default at
    the transport position (e.g. ramp 0→1 over 4 bars, range [100,200], transport
    at bar 2 → default == 150).
  - `apply` does **not** overwrite a connected input.
  - `removeNode` drops all channels for that node; `remove` drops one.
- **`tests/test_automation.cpp`** (update) — keep curve-sampling + the
  `Graph::evaluate` stream test; drop the category test.

The grouped grid is UI-only; verify visually with
`./build/shader_streamer --screenshot ui.png` (demo now seeds both group kinds).

## Docs

- **README.md** — update the Automation paragraph + the Automation row in the
  module table to describe stream groups (per Automation node) and ui channels
  (right-click a node → automate a parameter), and that length is global.
- **CLAUDE.md** — update the Automation bullet: curve factored into
  `core/AutoCurve.h`; `Graph` owns `AutomationStore` (UI channels + global length)
  and applies them each frame into node input defaults; stream vs ui is structural;
  category enum removed.
