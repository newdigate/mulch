# Grouped Automation Channels Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Split automation into two structurally-distinct channel kinds — stream channels (an `AutomationNode`'s 4 outputs, wired with edges) and ui channels (bound directly to a node's Float control, no edge) — grouped in collapsible containers in the Automation editor.

**Architecture:** Factor the breakpoint curve into a GL-free `core/AutoCurve`. `Graph` gains a GL-free `AutomationStore` that owns the ui channels + one global song length, and applies them each frame by writing the sampled value into the target control's input default before evaluation. The Automation window becomes collapsible groups over one shared, global-length time axis. A node's right-click menu creates ui channels. The per-channel category combo/enum is removed (kind is now structural).

**Tech Stack:** C++17, Dear ImGui + imgui-node-editor, doctest. GL-free core in `src/core/`.

**Spec:** `docs/superpowers/specs/2026-06-17-grouped-automation-channels-design.md`

---

## File Structure

| File | Responsibility | Change |
|---|---|---|
| `src/core/AutoCurve.h` | `AutoPoint` + piecewise-linear `AutoCurve::sample` | **create** (header-only) |
| `src/core/AutomationStore.{h,cpp}` | owns ui channels + global length; `add`/`remove`/`removeNode`/`apply` | **create** |
| `src/core/Graph.{h,cpp}` | own an `AutomationStore`; apply it in `evaluate`; clean up in `removeNode` | modify |
| `src/modules/AutomationNode.h` | 4 stream channels as `AutoCurve`; drop category + per-node length | modify |
| `src/ui/NodeEditorPanel.cpp` | node context menu → automate a Float parameter | modify |
| `src/ui/AutomationPanel.{h,cpp}` | collapsible grouped grid over a shared time axis | rewrite |
| `src/main.cpp` | screenshot demo seeds a ui channel too | modify |
| `tests/test_auto_curve.cpp` | curve sampling | **create** |
| `tests/test_automation_store.cpp` | store add/apply/remove + Graph integration | **create** |
| `tests/test_automation.cpp` | keep the `Graph::evaluate` stream test only | modify |
| `CMakeLists.txt` | wire the new `.cpp` + test files | modify |
| `README.md`, `CLAUDE.md` | document the new model | modify |

Each task ends green (builds + `ctest` passes), so tasks can land independently.

---

### Task 1: `AutoCurve` — factor the curve out of `AutomationNode`

**Files:**
- Create: `src/core/AutoCurve.h`
- Test: `tests/test_auto_curve.cpp`
- Modify: `CMakeLists.txt` (add the test to `core_tests`)

- [ ] **Step 1: Write the failing test**

Create `tests/test_auto_curve.cpp`:

```cpp
#include <doctest/doctest.h>
#include "core/AutoCurve.h"

using namespace oss;

TEST_CASE("an empty curve samples to zero") {
    AutoCurve c;
    CHECK(c.sample(0.0f) == doctest::Approx(0.0f));
    CHECK(c.sample(3.5f) == doctest::Approx(0.0f));
}

TEST_CASE("a curve interpolates and holds at the ends") {
    AutoCurve c;
    c.points = { {0.0f, 0.0f}, {2.0f, 1.0f}, {4.0f, 0.0f} };   // a triangle
    CHECK(c.sample(0.0f) == doctest::Approx(0.0f));
    CHECK(c.sample(1.0f) == doctest::Approx(0.5f));    // halfway up
    CHECK(c.sample(2.0f) == doctest::Approx(1.0f));    // peak
    CHECK(c.sample(3.0f) == doctest::Approx(0.5f));    // halfway down
    CHECK(c.sample(4.0f) == doctest::Approx(0.0f));
    CHECK(c.sample(5.0f) == doctest::Approx(0.0f));    // past the end -> hold last
    CHECK(c.sample(-1.0f) == doctest::Approx(0.0f));   // before start -> hold first
}
```

- [ ] **Step 2: Wire the test into CMake**

In `CMakeLists.txt`, in the `add_executable(core_tests ...)` list, add this line after `  tests/test_world_transform.cpp`:

```cmake
  tests/test_auto_curve.cpp
```

- [ ] **Step 3: Run the test to verify it fails**

Run: `cmake --build build -j 2>&1 | tail -5`
Expected: compile error — `core/AutoCurve.h` does not exist.

- [ ] **Step 4: Create the header**

Create `src/core/AutoCurve.h`:

```cpp
#pragma once
#include <cstddef>
#include <vector>

namespace oss {

// One automation breakpoint: a normalised value in [0,1] at a song position (bars).
struct AutoPoint { float bar = 0.0f; float value = 0.0f; };

// A piecewise-linear breakpoint curve over song bars, sampled to [0,1]. Points are
// kept sorted by bar (callers maintain the ordering); sample() holds the first and
// last value beyond the ends.
struct AutoCurve {
    std::vector<AutoPoint> points;

    float sample(float bar) const {
        if (points.empty()) return 0.0f;
        if (bar <= points.front().bar) return points.front().value;
        if (bar >= points.back().bar)  return points.back().value;
        for (std::size_t i = 0; i + 1 < points.size(); ++i) {
            if (bar >= points[i].bar && bar <= points[i + 1].bar) {
                float span = points[i + 1].bar - points[i].bar;
                float t = span > 0.0f ? (bar - points[i].bar) / span : 0.0f;
                return points[i].value + t * (points[i + 1].value - points[i].value);
            }
        }
        return points.back().value;
    }
};

} // namespace oss
```

- [ ] **Step 5: Run the test to verify it passes**

Run: `cmake --build build -j && ./build/core_tests --test-case="*curve*"`
Expected: PASS (2 test cases).

- [ ] **Step 6: Commit**

```bash
git add src/core/AutoCurve.h tests/test_auto_curve.cpp CMakeLists.txt
git commit -m "feat(core): add AutoCurve breakpoint curve + tests"
```

---

### Task 2: `AutomationStore` — owns ui channels + global length

**Files:**
- Create: `src/core/AutomationStore.h`, `src/core/AutomationStore.cpp`
- Test: `tests/test_automation_store.cpp`
- Modify: `CMakeLists.txt`

The store is standalone here (its methods take `Graph&` explicitly) — `Graph` does not own it until Task 3, so the tests construct a store alongside a graph.

- [ ] **Step 1: Write the failing test**

Create `tests/test_automation_store.cpp`:

```cpp
#include <doctest/doctest.h>
#include "core/AutomationStore.h"
#include "core/Graph.h"
#include "core/Node.h"
#include "core/Value.h"
#include <memory>
#include <variant>

using namespace oss;

namespace {
// A node with one Float input control (default 15, range [10,20]).
struct Knob : Node {
    Knob() : Node("Knob") { addInput("amt", PortType::Float, 15.0f, 10.0f, 20.0f); }
    void evaluate(EvalContext&) override {}
};
// A node whose only input is non-Float.
struct Switch : Node {
    Switch() : Node("Switch") { addInput("on", PortType::Bool, false); }
    void evaluate(EvalContext&) override {}
};
// A node with a Float output, to drive an edge into a Knob.
struct Src : Node {
    Src() : Node("Src") { addOutput("v", PortType::Float); }
    void evaluate(EvalContext& ctx) override { ctx.out<float>(0, 7.0f); }
};
} // namespace

TEST_CASE("add seeds the range and a bar-0 point at the control's current value") {
    Graph g; int id = g.addNode(std::make_unique<Knob>());
    AutomationStore store;
    auto* ch = store.add(g, id, 0);
    REQUIRE(ch);
    CHECK(ch->outMin == doctest::Approx(10.0f));
    CHECK(ch->outMax == doctest::Approx(20.0f));
    REQUIRE(ch->curve.points.size() == 1);
    CHECK(ch->curve.points[0].bar == doctest::Approx(0.0f));
    CHECK(ch->curve.points[0].value == doctest::Approx(0.5f));   // (15-10)/(20-10)
}

TEST_CASE("add is idempotent for the same node+port") {
    Graph g; int id = g.addNode(std::make_unique<Knob>());
    AutomationStore store;
    auto* a = store.add(g, id, 0);
    auto* b = store.add(g, id, 0);
    CHECK(a == b);
    CHECK(store.channels().size() == 1);
}

TEST_CASE("add rejects missing nodes, bad ports, and non-Float ports") {
    Graph g;
    int knob = g.addNode(std::make_unique<Knob>());
    int sw   = g.addNode(std::make_unique<Switch>());
    AutomationStore store;
    CHECK(store.add(g, 9999, 0) == nullptr);   // no such node
    CHECK(store.add(g, knob, 5) == nullptr);   // no such port
    CHECK(store.add(g, sw, 0)   == nullptr);   // Bool, not Float
    CHECK(store.channels().empty());
}

TEST_CASE("apply writes the scaled sampled value into the control") {
    Graph g; int id = g.addNode(std::make_unique<Knob>());
    AutomationStore store;
    auto* ch = store.add(g, id, 0);
    ch->curve.points = { {0.0f, 0.0f}, {4.0f, 1.0f} };
    ch->outMin = 100.0f; ch->outMax = 200.0f;
    g.transport().bpm = 120.0; g.transport().seconds = 4.0; g.transport().pause();  // bar 2
    store.apply(g, g.transport());
    CHECK(std::get<float>(g.findNode(id)->inputDefault(0)) == doctest::Approx(150.0f));
}

TEST_CASE("apply does not overwrite a connected input") {
    Graph g;
    int id  = g.addNode(std::make_unique<Knob>());
    int src = g.addNode(std::make_unique<Src>());
    REQUIRE(g.connect(src, 0, id, 0));
    AutomationStore store;
    auto* ch = store.add(g, id, 0);
    ch->curve.points = { {0.0f, 1.0f} };
    ch->outMin = 100.0f; ch->outMax = 200.0f;
    float before = std::get<float>(g.findNode(id)->inputDefault(0));
    store.apply(g, g.transport());
    CHECK(std::get<float>(g.findNode(id)->inputDefault(0)) == doctest::Approx(before));
}

TEST_CASE("remove and removeNode drop channels") {
    Graph g; int id = g.addNode(std::make_unique<Knob>());
    AutomationStore store;
    store.add(g, id, 0);
    store.remove(id, 0);
    CHECK(store.channels().empty());
    store.add(g, id, 0);
    store.removeNode(id);
    CHECK(store.channels().empty());
}

TEST_CASE("setLengthBars never goes below one bar") {
    AutomationStore store;
    store.setLengthBars(0.0f);
    CHECK(store.lengthBars() == doctest::Approx(1.0f));
    store.setLengthBars(16.0f);
    CHECK(store.lengthBars() == doctest::Approx(16.0f));
}
```

- [ ] **Step 2: Wire the new source + test into CMake**

In `CMakeLists.txt`:

In `set(APP_SOURCES ...)`, add after `  src/core/Graph.cpp`:
```cmake
  src/core/AutomationStore.cpp
```

In `add_executable(core_tests ...)`, add after the `tests/test_auto_curve.cpp` line:
```cmake
  tests/test_automation_store.cpp
```
and add after the core_tests `  src/core/Graph.cpp` line:
```cmake
  src/core/AutomationStore.cpp
```

In `add_executable(gl_smoke ...)`, add after its `  src/core/Graph.cpp` line:
```cmake
  src/core/AutomationStore.cpp
```

- [ ] **Step 3: Run the test to verify it fails**

Run: `cmake --build build -j 2>&1 | tail -5`
Expected: compile error — `core/AutomationStore.h` does not exist.

- [ ] **Step 4: Create the header**

Create `src/core/AutomationStore.h`:

```cpp
#pragma once
#include <vector>
#include "core/AutoCurve.h"

namespace oss {

class Graph;
struct Transport;

// A UI-automation channel: bound directly to one Float input control of a node
// (no edge). Its curve is sampled at the transport position and the scaled value
// is written straight into that control's input default by apply().
struct UiAutomationChannel {
    int       nodeId = 0;     // target node id
    int       port   = 0;     // target Float input port index
    AutoCurve curve;
    float     outMin = 0.0f;  // scales sample() [0,1] -> [outMin, outMax]
    float     outMax = 1.0f;
};

// Owns the graph's UI-automation channels plus one global song length (bars) shared
// by every lane's time axis in the Automation editor. GL-free.
class AutomationStore {
public:
    float lengthBars() const { return lengthBars_; }
    void  setLengthBars(float L) { lengthBars_ = L < 1.0f ? 1.0f : L; }

    // First existing channel for node+port, or nullptr.
    UiAutomationChannel* find(int nodeId, int port);

    // Create a channel for (nodeId, port). Idempotent: returns the existing channel
    // if one already targets that node+port. Seeds outMin/outMax from the port's
    // slider range and inserts one breakpoint at bar 0 equal to the control's
    // current normalised value (so creation never changes the live value). Returns
    // nullptr if the node/port is missing or the port is not a Float.
    UiAutomationChannel* add(Graph& graph, int nodeId, int port);

    void remove(int nodeId, int port);   // drop the channel for node+port (if any)
    void removeNode(int nodeId);          // drop all channels targeting a node

    const std::vector<UiAutomationChannel>& channels() const { return channels_; }
    std::vector<UiAutomationChannel>&        channels()       { return channels_; }

    // Sample each channel at transport.bars() and write the scaled value into the
    // target node's input default -- but only when that input is unconnected (an
    // edge wins). Skips channels whose target node/port is gone or not a Float.
    void apply(Graph& graph, const Transport& transport);

private:
    std::vector<UiAutomationChannel> channels_;
    float lengthBars_ = 8.0f;
};

} // namespace oss
```

- [ ] **Step 5: Create the implementation**

Create `src/core/AutomationStore.cpp`:

```cpp
#include "core/AutomationStore.h"
#include "core/Graph.h"
#include "core/Node.h"
#include "core/Transport.h"
#include "core/Value.h"
#include <algorithm>
#include <variant>

namespace oss {

UiAutomationChannel* AutomationStore::find(int nodeId, int port) {
    for (auto& c : channels_)
        if (c.nodeId == nodeId && c.port == port) return &c;
    return nullptr;
}

UiAutomationChannel* AutomationStore::add(Graph& graph, int nodeId, int port) {
    if (auto* ex = find(nodeId, port)) return ex;
    Node* n = graph.findNode(nodeId);
    if (!n) return nullptr;
    if (port < 0 || port >= (int)n->inputs().size()) return nullptr;
    const Port& p = n->inputs()[port];
    if (p.type != PortType::Float) return nullptr;

    float cur   = std::get<float>(p.defaultValue);
    float lo    = p.minVal, hi = p.maxVal;
    float denom = hi - lo;
    float norm  = denom != 0.0f ? (cur - lo) / denom : 0.0f;

    UiAutomationChannel ch;
    ch.nodeId = nodeId; ch.port = port; ch.outMin = lo; ch.outMax = hi;
    ch.curve.points.push_back({0.0f, norm});
    channels_.push_back(ch);
    return &channels_.back();
}

void AutomationStore::remove(int nodeId, int port) {
    channels_.erase(std::remove_if(channels_.begin(), channels_.end(),
        [&](const UiAutomationChannel& c){ return c.nodeId == nodeId && c.port == port; }),
        channels_.end());
}

void AutomationStore::removeNode(int nodeId) {
    channels_.erase(std::remove_if(channels_.begin(), channels_.end(),
        [&](const UiAutomationChannel& c){ return c.nodeId == nodeId; }),
        channels_.end());
}

void AutomationStore::apply(Graph& graph, const Transport& transport) {
    float bar = (float)transport.bars();
    for (auto& ch : channels_) {
        Node* n = graph.findNode(ch.nodeId);
        if (!n) continue;
        if (ch.port < 0 || ch.port >= (int)n->inputs().size()) continue;
        if (n->inputs()[ch.port].type != PortType::Float) continue;
        if (graph.isInputConnected(ch.nodeId, ch.port)) continue;
        float v = ch.curve.sample(bar);
        n->inputDefault(ch.port) = Value(ch.outMin + v * (ch.outMax - ch.outMin));
    }
}

} // namespace oss
```

- [ ] **Step 6: Run the test to verify it passes**

Run: `cmake --build build -j && ./build/core_tests --test-case="*add*,*apply*,*remove*,*setLengthBars*"`
Expected: PASS (7 test cases from this file).

- [ ] **Step 7: Run the full suite**

Run: `ctest --test-dir build --output-on-failure`
Expected: all tests pass (`core_tests`, `gl_smoke`).

- [ ] **Step 8: Commit**

```bash
git add src/core/AutomationStore.h src/core/AutomationStore.cpp tests/test_automation_store.cpp CMakeLists.txt
git commit -m "feat(core): add AutomationStore for ui-automation channels"
```

---

### Task 3: `Graph` owns the store — apply each frame, clean up on removeNode

**Files:**
- Modify: `src/core/Graph.h`, `src/core/Graph.cpp`
- Test: `tests/test_automation_store.cpp` (append two cases)

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_automation_store.cpp`:

```cpp
TEST_CASE("Graph owns the store and applies it during evaluate") {
    Graph g; int id = g.addNode(std::make_unique<Knob>());
    auto* ch = g.automation().add(g, id, 0);
    REQUIRE(ch);
    ch->curve.points = { {0.0f, 0.0f}, {4.0f, 1.0f} };
    ch->outMin = 100.0f; ch->outMax = 200.0f;
    g.transport().bpm = 120.0; g.transport().seconds = 4.0; g.transport().pause();  // bar 2
    g.evaluate(0.0f);
    CHECK(std::get<float>(g.findNode(id)->inputDefault(0)) == doctest::Approx(150.0f));
}

TEST_CASE("removeNode drops the node's UI automation channels") {
    Graph g; int id = g.addNode(std::make_unique<Knob>());
    g.automation().add(g, id, 0);
    REQUIRE(g.automation().channels().size() == 1);
    g.removeNode(id);
    CHECK(g.automation().channels().empty());
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build build -j 2>&1 | tail -5`
Expected: compile error — `Graph` has no member `automation`.

- [ ] **Step 3: Add the store to `Graph.h`**

In `src/core/Graph.h`, add the include after `#include "core/Transport.h"`:
```cpp
#include "core/AutomationStore.h"
```

Add the accessor right after the existing `transport()` accessors (after the
`const Transport& transport() const { return transport_; }` line):
```cpp
    // UI-automation channels + the global song length (the Automation window).
    AutomationStore&       automation()       { return automation_; }
    const AutomationStore& automation() const { return automation_; }
```

Add the member in the `private:` section, after `Transport transport_;`:
```cpp
    AutomationStore automation_;
```

- [ ] **Step 4: Apply + clean up in `Graph.cpp`**

In `src/core/Graph.cpp`, in `evaluate`, change the first line of the body from:
```cpp
    transport_.advance(dt);   // advance the global clock once per frame
```
to:
```cpp
    transport_.advance(dt);   // advance the global clock once per frame
    automation_.apply(*this, transport_);   // ui channels drive their controls
```

In `removeNode`, add this line after `outputs_.erase(nodeId);`:
```cpp
    automation_.removeNode(nodeId);   // drop ui-automation channels for this node
```

- [ ] **Step 5: Run the tests**

Run: `cmake --build build -j && ./build/core_tests --test-case="*Graph owns*,*removeNode drops*"`
Expected: PASS (2 cases).

- [ ] **Step 6: Full suite**

Run: `ctest --test-dir build --output-on-failure`
Expected: all pass.

- [ ] **Step 7: Commit**

```bash
git add src/core/Graph.h src/core/Graph.cpp tests/test_automation_store.cpp
git commit -m "feat(core): Graph owns AutomationStore and applies it each frame"
```

---

### Task 4: Node context menu → automate a Float parameter

Adds the create/remove gesture. After this task, ui channels can be created by
right-clicking a node and they immediately drive their control (proven by Task 2/3
`apply` tests). The Automation window does not show them yet (Task 6).

**Files:**
- Modify: `src/ui/NodeEditorPanel.cpp`, `src/ui/NodeEditorPanel.h`

- [ ] **Step 1: Add the context-node field to the Impl**

In `src/ui/NodeEditorPanel.cpp`, in `struct NodeEditorPanel::Impl`, add a field:
```cpp
    int ctxNodeId = 0;   // node whose context menu is open
```
So the struct reads:
```cpp
struct NodeEditorPanel::Impl {
    ed::EditorContext* ctx = nullptr;
    std::set<int> placed;
    int ctxNodeId = 0;   // node whose context menu is open
};
```

- [ ] **Step 2: Add the node context menu**

In `src/ui/NodeEditorPanel.cpp`, inside the existing `ed::Suspend(); ... ed::Resume();`
block, after the closing brace `}` of the `if (ImGui::BeginPopup("BackgroundMenu")) {
... }` block and before the `ed::Resume();` call, add:

```cpp
    // Node context menu: right-click a node to automate one of its Float inputs
    // as a UI-automation channel (created/removed in the graph's AutomationStore;
    // shown grouped under the node in the Automation window).
    ed::NodeId ctxNode;
    if (ed::ShowNodeContextMenu(&ctxNode)) {
        impl_->ctxNodeId = (int)ctxNode.Get();
        ImGui::OpenPopup("NodeMenu");
    }
    if (ImGui::BeginPopup("NodeMenu")) {
        Node* n = graph.findNode(impl_->ctxNodeId);
        if (n) {
            ImGui::TextDisabled("Automate (UI)");
            ImGui::Separator();
            bool any = false;
            for (std::size_t i = 0; i < n->inputs().size(); ++i) {
                if (n->inputs()[i].type != PortType::Float) continue;
                any = true;
                bool on = graph.automation().find(n->id(), (int)i) != nullptr;
                if (ImGui::MenuItem(n->inputs()[i].name.c_str(), nullptr, on)) {
                    if (on) graph.automation().remove(n->id(), (int)i);
                    else    graph.automation().add(graph, n->id(), (int)i);
                }
            }
            if (!any) ImGui::TextDisabled("No automatable parameters");
        }
        ImGui::EndPopup();
    }
```

(`PortType`, `Node`, and `graph.automation()` are already reachable: the file
includes `app/Application.h`, which includes `core/Graph.h`, which includes
`core/Node.h`/`core/Value.h`/`core/AutomationStore.h`.)

- [ ] **Step 3: Build**

Run: `cmake --build build -j 2>&1 | tail -5`
Expected: builds clean.

- [ ] **Step 4: Verify the suite still passes**

Run: `ctest --test-dir build --output-on-failure`
Expected: all pass (this is a UI-only change; behaviour is covered by the store's
`apply` tests).

- [ ] **Step 5: Commit**

```bash
git add src/ui/NodeEditorPanel.cpp
git commit -m "feat(ui): right-click a node to automate a Float parameter (ui channel)"
```

---

### Task 5: Refactor `AutomationNode` to `AutoCurve`; drop category + per-node length

Removes the category enum/combo and per-node length. `channel(c)` keeps returning a
`std::vector<AutoPoint>&` so the existing `AutomationPanel` lane code keeps
compiling; this task makes the minimal panel/`main.cpp`/test edits needed to stay
green. The full grouped panel is Task 6.

**Files:**
- Modify: `src/modules/AutomationNode.h`, `src/ui/AutomationPanel.cpp`, `src/main.cpp`
- Modify: `tests/test_automation.cpp`

- [ ] **Step 1: Update the stream-channel tests**

Replace the entire contents of `tests/test_automation.cpp` with:

```cpp
#include <doctest/doctest.h>
#include "modules/AutomationNode.h"
#include "core/Graph.h"
#include "core/Node.h"
#include <memory>

using namespace oss;

TEST_CASE("Graph::evaluate samples the curve at the transport position and scales it") {
    // A node that records the float it was handed.
    struct Probe : Node {
        Probe() : Node("Probe") { addInput("in", PortType::Float, 0.0f); }
        void evaluate(EvalContext& ctx) override { v = ctx.in<float>(0); }
        float v = -1.0f;
    };

    Graph g;
    auto a = std::make_unique<AutomationNode>();
    AutomationNode* ap = a.get();
    ap->channel(0) = { {0.0f, 0.0f}, {4.0f, 1.0f} };   // ramps 0 -> 1 over 4 bars
    ap->setOutRange(0, 100.0f, 200.0f);                // and scales to [100, 200]

    auto pr = std::make_unique<Probe>();
    Probe* prp = pr.get();
    int aId = g.addNode(std::move(a));
    int pId = g.addNode(std::move(pr));
    REQUIRE(g.connect(aId, 0, pId, 0));

    // 120 BPM -> 2 s/bar. Park the transport at bar 2 (paused so evaluate's
    // advance(dt) is a no-op) and check the probe sees the scaled mid value.
    g.transport().bpm = 120.0;
    g.transport().seconds = 4.0;     // beats = 8, bars = 2
    g.transport().pause();
    g.evaluate(0.5f);

    CHECK(g.transport().bars() == doctest::Approx(2.0));
    CHECK(prp->v == doctest::Approx(150.0f));   // sample 0.5 scaled into [100,200]
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build build -j 2>&1 | tail -8`
Expected: still builds/passes for now (the old `AutomationNode` still has
`channel`/`setOutRange`). This step establishes the target test before the refactor;
proceed to Step 3.

- [ ] **Step 3: Rewrite `AutomationNode.h`**

Replace the entire contents of `src/modules/AutomationNode.h` with:

```cpp
#pragma once
#include <cstddef>
#include <string>
#include "core/Node.h"
#include "core/Value.h"
#include "core/AutoCurve.h"

namespace oss {

// Parameter automation: kChannels float-output channels, each an AutoCurve of
// breakpoints over song time (bars). Every frame each channel is sampled at the
// global transport's bar position and emitted on its output (scaled to a
// per-channel [outMin,outMax]), so wiring a channel into any Float input sequences
// that parameter over time. These are the "stream" automation channels; they reach
// their target through an edge. The curves are mouse-edited in the Automation
// window. The editor's time axis length is global (AutomationStore::lengthBars),
// not per-node; sampling holds at the curve's end points.
class AutomationNode : public Node {
public:
    static constexpr int kChannels = 4;

    AutomationNode() : Node("Automation") {
        for (int c = 0; c < kChannels; ++c) {
            addOutput("ch " + std::to_string(c + 1), PortType::Float);
            outMin_[c] = 0.0f;
            outMax_[c] = 1.0f;
        }
    }

    void evaluate(EvalContext& ctx) override {
        float bar = ctx.transport ? (float)ctx.transport->bars() : 0.0f;
        for (int c = 0; c < kChannels; ++c) {
            float v = curve_[c].sample(bar);                       // normalised [0,1]
            ctx.out<float>((std::size_t)c, outMin_[c] + v * (outMax_[c] - outMin_[c]));
        }
    }

    // --- UI access (the Automation window reads/edits these) ---
    int channelCount() const { return kChannels; }
    std::vector<AutoPoint>&       channel(int c)       { return curve_[c].points; }
    const std::vector<AutoPoint>& channel(int c) const { return curve_[c].points; }
    float outMin(int c) const { return outMin_[c]; }
    float outMax(int c) const { return outMax_[c]; }
    void  setOutRange(int c, float lo, float hi) { outMin_[c] = lo; outMax_[c] = hi; }

private:
    AutoCurve curve_[kChannels];
    float     outMin_[kChannels];
    float     outMax_[kChannels];
};

} // namespace oss
```

- [ ] **Step 4: Minimal `AutomationPanel.cpp` edits to keep it compiling**

`AutomationPanel::draw` no longer has `category`/`lengthBars`/`currentBar` on the
node. Make these four edits in `src/ui/AutomationPanel.cpp` (the file is fully
rewritten in Task 6; these keep it green meanwhile):

4a. Replace the toolbar length block:
```cpp
    ImGui::SetNextItemWidth(90.0f);
    int len = (int)node->lengthBars();
    if (ImGui::InputInt("Length (bars)", &len)) node->setLengthBars((float)len);
```
with:
```cpp
    ImGui::SetNextItemWidth(90.0f);
    int len = (int)graph.automation().lengthBars();
    if (ImGui::InputInt("Length (bars)", &len)) graph.automation().setLengthBars((float)len);
```

4b. Delete the category combo block (the `ImGui::SameLine();` + `int ci = ...` +
`const char* cats[]` + `if (ImGui::Combo(...))` lines). Concretely remove:
```cpp
        ImGui::SameLine();
        ImGui::SetNextItemWidth(74.0f);
        int ci = (int)node->category(c);
        const char* cats[] = { "stream", "ui" };
        if (ImGui::Combo(("##cat" + std::to_string(c)).c_str(), &ci, cats, 2))
            node->setCategory(c, (AutoCategory)ci);
```

4c. Replace both `node->lengthBars()` uses in the lane area:
```cpp
    const float contentW = std::max(1.0f, node->lengthBars() * pxPerBar);
```
becomes:
```cpp
    const float contentW = std::max(1.0f, graph.automation().lengthBars() * pxPerBar);
```
and
```cpp
    for (int b = 0; b <= (int)node->lengthBars(); ++b) {
```
becomes:
```cpp
    for (int b = 0; b <= (int)graph.automation().lengthBars(); ++b) {
```

4d. Replace the playhead/clamp uses of `node->lengthBars()` / `node->currentBar()`:
```cpp
    float phx = X(std::clamp(node->currentBar(), 0.0f, node->lengthBars()));
```
becomes:
```cpp
    float phx = X(std::clamp((float)graph.transport().bars(), 0.0f, graph.automation().lengthBars()));
```
and in the mouse section, the two `node->lengthBars()` clamps:
```cpp
            AutoPoint np{ std::clamp(toBar(m.x), 0.0f, node->lengthBars()),
```
becomes
```cpp
            AutoPoint np{ std::clamp(toBar(m.x), 0.0f, graph.automation().lengthBars()),
```
and
```cpp
            float nb = std::clamp(toBar(m.x), 0.0f, node->lengthBars());
```
becomes
```cpp
            float nb = std::clamp(toBar(m.x), 0.0f, graph.automation().lengthBars());
```

- [ ] **Step 5: Minimal `main.cpp` edits**

In `src/main.cpp`, in `runScreenshot`, replace:
```cpp
            an->setLengthBars(8.0f);
            an->channel(0) = { {0.0f, 0.20f}, {2.0f, 0.85f}, {5.0f, 0.40f}, {8.0f, 0.95f} };
            an->channel(1) = { {0.0f, 0.60f}, {4.0f, 0.10f}, {8.0f, 0.70f} };
            an->setCategory(1, oss::AutoCategory::UiControl);
            an->setOutRange(2, 20.0f, 2000.0f);
```
with:
```cpp
            app.graph().automation().setLengthBars(8.0f);
            an->channel(0) = { {0.0f, 0.20f}, {2.0f, 0.85f}, {5.0f, 0.40f}, {8.0f, 0.95f} };
            an->channel(1) = { {0.0f, 0.60f}, {4.0f, 0.10f}, {8.0f, 0.70f} };
            an->setOutRange(2, 20.0f, 2000.0f);
```

- [ ] **Step 6: Build + run the suite**

Run: `cmake --build build -j && ctest --test-dir build --output-on-failure`
Expected: builds clean; all tests pass (the `Graph::evaluate` automation test still
gives 150.0).

- [ ] **Step 7: Commit**

```bash
git add src/modules/AutomationNode.h src/ui/AutomationPanel.cpp src/main.cpp tests/test_automation.cpp
git commit -m "refactor(automation): AutomationNode uses AutoCurve; drop category + per-node length"
```

---

### Task 6: Rewrite the Automation window as a grouped collapsible grid

Renders one collapsible stream group per `AutomationNode` (4 lanes) and one
collapsible ui group per module that has ui channels (one lane per channel,
labelled by parameter name), all over one shared global-length time axis with a
top bar ruler and the transport playhead. Lanes are edited the same way for both
kinds; ui lanes also get a delete button.

**Files:**
- Rewrite: `src/ui/AutomationPanel.h`, `src/ui/AutomationPanel.cpp`
- Modify: `src/main.cpp` (seed a ui channel so the screenshot shows both kinds)

- [ ] **Step 1: Rewrite the panel header**

Replace the entire contents of `src/ui/AutomationPanel.h` with:

```cpp
#pragma once
#include <map>
#include <vector>
#include "core/AutoCurve.h"

namespace oss {

class Graph;

// Draws the "Automation" window as collapsible groups over one shared,
// horizontally-scrollable time axis (global length from the graph's
// AutomationStore): one stream group per AutomationNode (its 4 channel lanes) and
// one ui group per module that has ui-automation channels (one lane per channel).
// The mouse edits a lane's breakpoint curve; ui lanes can also be deleted. Call
// inside an active ImGui frame. Holds per-group collapse state and the in-progress
// drag, so it's an Application member.
class AutomationPanel {
public:
    void draw(Graph& graph);

private:
    bool isOpen(long key) const {
        auto it = open_.find(key);
        return it == open_.end() ? true : it->second;   // groups default to open
    }

    std::map<long, bool>    open_;             // group collapse state, keyed by group
    std::vector<AutoPoint>* dragPts_ = nullptr; // points vector being edited
    int                     dragPoint_ = -1;    // index of the dragged point
};

} // namespace oss
```

- [ ] **Step 2: Rewrite the panel implementation**

Replace the entire contents of `src/ui/AutomationPanel.cpp` with:

```cpp
#include "ui/AutomationPanel.h"
#include "core/Graph.h"
#include "core/Transport.h"
#include "core/AutomationStore.h"
#include "modules/AutomationNode.h"
#include <imgui.h>
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace oss {

static ImU32 streamColour(int c) {
    static const ImU32 cols[4] = {
        IM_COL32( 90, 200, 255, 255),   // cyan
        IM_COL32(255, 180,  80, 255),   // orange
        IM_COL32(140, 255, 120, 255),   // green
        IM_COL32(255, 120, 200, 255),   // pink
    };
    return cols[c & 3];
}
static ImU32 uiColour(int i) {
    static const ImU32 cols[4] = {
        IM_COL32(179, 157, 219, 255),   // purple
        IM_COL32(128, 203, 196, 255),   // teal
        IM_COL32(255, 171, 145, 255),   // coral
        IM_COL32(197, 225, 165, 255),   // light green
    };
    return cols[i & 3];
}

void AutomationPanel::draw(Graph& graph) {
    ImGui::Begin("Automation");
    AutomationStore& store = graph.automation();

    // --- Toolbar: one global song length, shared by every lane's time axis ---
    ImGui::SetNextItemWidth(90.0f);
    int len = (int)store.lengthBars();
    if (ImGui::InputInt("Length (bars)", &len)) store.setLengthBars((float)len);
    ImGui::SameLine();
    ImGui::TextDisabled("global \xE2\x80\xA2 right-click a node to automate a parameter");

    // --- Build this frame's row layout: a ruler, then group headers + lanes ---
    struct Row {
        enum Kind { Ruler, Header, Lane } kind = Lane;
        long key = 0; std::string htext;                   // Header
        std::vector<AutoPoint>* pts = nullptr;             // Lane points
        float omin = 0.0f, omax = 1.0f; ImU32 col = 0; std::string label;
        AutomationNode* anode = nullptr; int achan = -1;   // stream source
        UiAutomationChannel* uich = nullptr;               // ui source
        int delNode = 0, delPort = 0;                      // ui delete target
    };
    const float rulerH = 24.0f, headerH = 22.0f, laneH = 46.0f;
    const float leftW = 210.0f, pxPerBar = 55.0f, inset = 7.0f;

    std::vector<Row> rows;
    std::vector<float> hgt;
    auto pushRow = [&](Row r, float h){ rows.push_back(std::move(r)); hgt.push_back(h); };

    { Row r; r.kind = Row::Ruler; pushRow(r, rulerH); }

    // Stream groups: one per AutomationNode, its 4 channel lanes.
    for (auto& up : graph.nodes()) {
        auto* an = dynamic_cast<AutomationNode*>(up.get());
        if (!an) continue;
        long key = (long)an->id() * 2 + 0;
        Row h; h.kind = Row::Header; h.key = key;
        h.htext = "Automation #" + std::to_string(an->id());
        pushRow(h, headerH);
        if (!isOpen(key)) continue;
        for (int c = 0; c < an->channelCount(); ++c) {
            Row r; r.kind = Row::Lane;
            r.pts = &an->channel(c); r.omin = an->outMin(c); r.omax = an->outMax(c);
            r.col = streamColour(c); r.label = "ch " + std::to_string(c + 1);
            r.anode = an; r.achan = c;
            pushRow(r, laneH);
        }
    }

    // UI groups: one per target node (first-seen order), one lane per channel.
    std::vector<int> uiNodes;
    for (auto& ch : store.channels())
        if (std::find(uiNodes.begin(), uiNodes.end(), ch.nodeId) == uiNodes.end())
            uiNodes.push_back(ch.nodeId);
    for (int nid : uiNodes) {
        Node* n = graph.findNode(nid);
        std::string nm = n ? n->name() : std::string("node");
        long key = (long)nid * 2 + 1;
        Row h; h.kind = Row::Header; h.key = key;
        h.htext = nm + " #" + std::to_string(nid);
        pushRow(h, headerH);
        if (!isOpen(key)) continue;
        int idx = 0;
        for (auto& ch : store.channels()) {
            if (ch.nodeId != nid) continue;
            std::string pname = (n && ch.port < (int)n->inputs().size())
                                ? n->inputs()[ch.port].name
                                : ("port " + std::to_string(ch.port));
            Row r; r.kind = Row::Lane;
            r.pts = &ch.curve.points; r.omin = ch.outMin; r.omax = ch.outMax;
            r.col = uiColour(idx); r.label = pname;
            r.uich = &ch; r.delNode = nid; r.delPort = ch.port;
            pushRow(r, laneH);
            ++idx;
        }
    }

    if (rows.size() == 1) {   // only the ruler -> nothing to edit yet
        ImGui::TextDisabled("Add an Automation node, or right-click any node to automate a parameter.");
        ImGui::End();
        return;
    }

    // Cumulative y offset per row.
    std::vector<float> y(rows.size() + 1, 0.0f);
    for (std::size_t i = 0; i < rows.size(); ++i) y[i + 1] = y[i] + hgt[i];
    const float totalH = y[rows.size()];
    const float scrollH = ImGui::GetStyle().ScrollbarSize;

    int pendingDelNode = -1, pendingDelPort = -1;   // deferred ui-channel delete

    // --- Left column: per-row labels and controls (no horizontal scroll) ---
    ImGui::BeginChild("autoLabels", ImVec2(leftW, totalH), false);
    for (std::size_t i = 0; i < rows.size(); ++i) {
        Row& r = rows[i];
        if (r.kind == Row::Ruler) {
            ImGui::SetCursorPos(ImVec2(8.0f, y[i] + rulerH * 0.5f - 8.0f));
            ImGui::TextDisabled("bars");
        } else if (r.kind == Row::Header) {
            ImGui::SetCursorPos(ImVec2(2.0f, y[i] + 2.0f));
            bool op = isOpen(r.key);
            std::string lbl = (op ? "v  " : ">  ") + r.htext + "##grp" + std::to_string(r.key);
            if (ImGui::Selectable(lbl.c_str(), false, 0, ImVec2(leftW - 4.0f, headerH - 3.0f)))
                open_[r.key] = !op;
        } else {
            ImGui::PushID((int)i);
            ImGui::SetCursorPos(ImVec2(12.0f, y[i] + 4.0f));
            ImGui::TextColored(ImColor(r.col), "%s", r.label.c_str());
            ImGui::SetCursorPos(ImVec2(12.0f, y[i] + 24.0f));
            float lo = r.omin, hi = r.omax;
            ImGui::SetNextItemWidth(52.0f);
            if (ImGui::DragFloat("##lo", &lo, 0.01f, 0, 0, "%.2f")) {
                if (r.anode)      r.anode->setOutRange(r.achan, lo, hi);
                else if (r.uich)  r.uich->outMin = lo;
            }
            ImGui::SameLine(); ImGui::SetNextItemWidth(52.0f);
            if (ImGui::DragFloat("##hi", &hi, 0.01f, 0, 0, "%.2f")) {
                if (r.anode)      r.anode->setOutRange(r.achan, lo, hi);
                else if (r.uich)  r.uich->outMax = hi;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("clr")) r.pts->clear();
            if (r.uich) {
                ImGui::SameLine();
                if (ImGui::SmallButton("x")) { pendingDelNode = r.delNode; pendingDelPort = r.delPort; }
            }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();

    ImGui::SameLine();

    // --- Lane area: shared time axis, scrolls horizontally ---
    ImGui::BeginChild("autoLanes", ImVec2(0, totalH + scrollH + 4.0f), false,
                      ImGuiWindowFlags_HorizontalScrollbar);
    const float length   = std::max(1.0f, store.lengthBars());
    const float contentW = std::max(1.0f, length * pxPerBar);
    ImGui::InvisibleButton("lanes", ImVec2(contentW, totalH));
    const bool   hovered = ImGui::IsItemHovered();
    const ImVec2 o = ImGui::GetItemRectMin();
    ImDrawList*  dl = ImGui::GetWindowDrawList();
    auto X = [&](float bar) { return o.x + bar * pxPerBar; };

    // Row backgrounds.
    for (std::size_t i = 0; i < rows.size(); ++i) {
        float top = o.y + y[i], bot = o.y + y[i] + hgt[i];
        ImU32 bg;
        if      (rows[i].kind == Row::Ruler)  bg = IM_COL32(32, 35, 44, 255);
        else if (rows[i].kind == Row::Header) bg = IM_COL32(44, 49, 62, 255);
        else                                  bg = (i & 1) ? IM_COL32(29, 32, 38, 255)
                                                           : IM_COL32(24, 26, 32, 255);
        dl->AddRectFilled(ImVec2(o.x, top), ImVec2(o.x + contentW, bot), bg);
    }
    // Bar grid lines + numbers (numbers in the ruler row).
    for (int b = 0; b <= (int)length; ++b) {
        float x = X((float)b);
        dl->AddLine(ImVec2(x, o.y), ImVec2(x, o.y + totalH), IM_COL32(55, 58, 68, 255));
        char t[8]; std::snprintf(t, sizeof(t), "%d", b);
        dl->AddText(ImVec2(x + 3, o.y + rulerH * 0.5f - 7.0f), IM_COL32(120, 128, 140, 255), t);
    }
    // Each lane's curve + breakpoints.
    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (rows[i].kind != Row::Lane) continue;
        auto* p = rows[i].pts; ImU32 col = rows[i].col;
        float t = o.y + y[i] + inset, bt = o.y + y[i] + hgt[i] - inset;
        auto Yv = [&](float v) { return bt - v * (bt - t); };
        if (p->empty()) continue;
        dl->AddLine(ImVec2(o.x, Yv(p->front().value)), ImVec2(X(p->front().bar), Yv(p->front().value)), col, 2.0f);
        for (std::size_t k = 0; k + 1 < p->size(); ++k)
            dl->AddLine(ImVec2(X((*p)[k].bar), Yv((*p)[k].value)),
                        ImVec2(X((*p)[k + 1].bar), Yv((*p)[k + 1].value)), col, 2.0f);
        dl->AddLine(ImVec2(X(p->back().bar), Yv(p->back().value)), ImVec2(o.x + contentW, Yv(p->back().value)), col, 2.0f);
        for (auto& pt : *p) dl->AddCircleFilled(ImVec2(X(pt.bar), Yv(pt.value)), 4.0f, col);
    }
    // Playhead across the ruler + all lanes.
    float phx = X(std::clamp((float)graph.transport().bars(), 0.0f, length));
    dl->AddLine(ImVec2(phx, o.y), ImVec2(phx, o.y + totalH), IM_COL32(255, 80, 80, 220), 2.0f);

    // --- Mouse editing ---
    const ImVec2 m = ImGui::GetIO().MousePos;
    auto toBar  = [&](float px) { return (px - o.x) / pxPerBar; };
    auto rowVal = [&](std::size_t i, float py) {
        float t = o.y + y[i] + inset, bt = o.y + y[i] + hgt[i] - inset;
        return (bt - py) / (bt - t);
    };
    auto hitPoint = [&](std::size_t i) -> int {
        auto* p = rows[i].pts;
        float t = o.y + y[i] + inset, bt = o.y + y[i] + hgt[i] - inset;
        for (int k = 0; k < (int)p->size(); ++k) {
            float dx = m.x - X((*p)[k].bar), dy = m.y - (bt - (*p)[k].value * (bt - t));
            if (dx * dx + dy * dy <= 8.0f * 8.0f) return k;
        }
        return -1;
    };
    int hoverRow = -1;
    for (std::size_t i = 0; i < rows.size(); ++i)
        if (rows[i].kind == Row::Lane && m.y >= o.y + y[i] && m.y < o.y + y[i] + hgt[i]) { hoverRow = (int)i; break; }

    if (hovered && hoverRow >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        auto* p = rows[hoverRow].pts; int h = hitPoint(hoverRow);
        if (h >= 0) p->erase(p->begin() + h);
    }
    if (hovered && hoverRow >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        auto* p = rows[hoverRow].pts; int h = hitPoint(hoverRow);
        if (h >= 0) { dragPts_ = p; dragPoint_ = h; }
        else {
            AutoPoint np{ std::clamp(toBar(m.x), 0.0f, length),
                          std::clamp(rowVal(hoverRow, m.y), 0.0f, 1.0f) };
            auto it = std::lower_bound(p->begin(), p->end(), np,
                                       [](const AutoPoint& a, const AutoPoint& b) { return a.bar < b.bar; });
            dragPts_ = p; dragPoint_ = (int)(it - p->begin());
            p->insert(it, np);
        }
    }
    // Continue an in-progress drag by re-finding its lane (so the drag survives the
    // cursor leaving the lane vertically). Skip if the lane vanished (e.g. collapsed).
    if (dragPts_ && dragPoint_ >= 0 && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        int dr = -1;
        for (std::size_t i = 0; i < rows.size(); ++i)
            if (rows[i].kind == Row::Lane && rows[i].pts == dragPts_) { dr = (int)i; break; }
        if (dr >= 0 && dragPoint_ < (int)dragPts_->size()) {
            auto* p = dragPts_;
            float nb = std::clamp(toBar(m.x), 0.0f, length);
            float nv = std::clamp(rowVal((std::size_t)dr, m.y), 0.0f, 1.0f);
            if (dragPoint_ > 0)                 nb = std::max(nb, (*p)[dragPoint_ - 1].bar + 1e-4f);
            if (dragPoint_ + 1 < (int)p->size()) nb = std::min(nb, (*p)[dragPoint_ + 1].bar - 1e-4f);
            (*p)[dragPoint_].bar = nb; (*p)[dragPoint_].value = nv;
        }
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) { dragPts_ = nullptr; dragPoint_ = -1; }

    ImGui::EndChild();

    if (pendingDelNode >= 0) store.remove(pendingDelNode, pendingDelPort);

    ImGui::TextDisabled("Click a lane to add a point, drag to move, right-click to delete. "
                        "Click a group header to collapse. Red line = transport playhead.");
    ImGui::End();
}

} // namespace oss
```

- [ ] **Step 3: Seed a ui channel in the screenshot demo**

In `src/main.cpp`, after the `if (auto* an = dynamic_cast<...>(...)) { ... }` block
in `runScreenshot` (right after its closing `}` and before
`app.graph().transport().bpm = 120.0;`), add:

```cpp
        // A ui-automation channel on a second module, so the captured grid shows
        // both a stream group and a ui group.
        int wfId = app.addNodeOfType("Wireframe", glm::vec2(420.0f, 240.0f));
        if (auto* ch = app.graph().automation().add(app.graph(), wfId, 1)) {  // Wireframe "spin" (Float)
            ch->curve.points = { {0.0f, 0.30f}, {3.0f, 0.90f}, {8.0f, 0.20f} };
        }
```

(`Wireframe`'s input **1** is `spin`, a Float with range [0, 2]; input 0 is
`geometry` (Vertex). Confirmed in `src/modules/WireframeNode.cpp:23-25`.)

- [ ] **Step 4: Build + run the suite**

Run: `cmake --build build -j && ctest --test-dir build --output-on-failure`
Expected: builds clean; all tests pass.

- [ ] **Step 5: Visual check via the screenshot mode**

Run: `./build/shader_streamer --screenshot build/_ui.png`
Expected: stderr prints `wrote screenshot build/_ui.png (...)`. Open it and confirm:
a bar ruler on top; a collapsible `Automation #N` stream group with 4 coloured
lanes; a collapsible `Wireframe #M` ui group with a `spin` lane; the red playhead
crossing all lanes. (If running headless without a GL context, this step may be
skipped — note it in the task report.)

- [ ] **Step 6: Commit**

```bash
git add src/ui/AutomationPanel.h src/ui/AutomationPanel.cpp src/main.cpp
git commit -m "feat(ui): grouped collapsible automation grid (stream + ui groups)"
```

---

### Task 7: Documentation

**Files:**
- Modify: `README.md`, `CLAUDE.md`

- [ ] **Step 1: Update `README.md`**

Replace the Automation paragraph (the one beginning "The **Automation** window is a
grid —") with:

```markdown
The **Automation** window groups channels over one shared, global-length time axis
(set the song length in its toolbar). There are two structurally-distinct kinds of
channel. **Stream channels** come from an **Automation** node: adding one contributes
a collapsible group of its 4 channels, and you wire each channel's Float output into
any parameter (an edge). **UI channels** are bound directly to a control: right-click
any node and pick one of its Float parameters to create a channel grouped under that
module — it drives the control directly, no wiring. Draw breakpoint curves in a lane
with the mouse (click to add, drag to move, right-click to delete); a channel's left
header carries its output range, a clear button, and (for ui channels) a delete. A
channel's kind is fixed by how it was created.
```

Replace the **Automation** row in the module table with:

```markdown
| **Automation** | 4 stream channels (Float outputs you wire), each a mouse-drawn curve over song time (bars), sampled at the transport position. Plus ui channels created by right-clicking any node's Float parameter — bound directly to that control. Edited in the **Automation** window |
```

- [ ] **Step 2: Update `CLAUDE.md`**

Replace the **Automation** bullet (under "Architecture", beginning "**Automation**
(`src/modules/AutomationNode.h`...)") with:

```markdown
- **Automation** — two structurally-distinct channel kinds over one shared,
  global-length time axis (the `Automation` window, `src/ui/AutomationPanel.cpp`,
  a collapsible grouped grid). The breakpoint curve is a GL-free
  `AutoCurve` (`src/core/AutoCurve.h`). **Stream channels** live in
  `AutomationNode` (`src/modules/AutomationNode.h`, header-only): 4 `AutoCurve`
  channels sampled at the transport's bar position, scaled per-channel, emitted on
  Float outputs you wire with edges. **UI channels** live in a GL-free
  `AutomationStore` (`src/core/AutomationStore.{h,cpp}`) owned by `Graph`: each is
  bound to one node's Float input control and created by right-clicking the node
  (`src/ui/NodeEditorPanel.cpp`); `Graph::evaluate` calls `AutomationStore::apply`
  each frame to write the sampled value straight into the control's input default
  (skipping connected inputs). A channel's kind is structural — there is no switch.
```

- [ ] **Step 3: Build (docs-only, sanity) + commit**

Run: `ctest --test-dir build --output-on-failure`
Expected: all pass (no code changed).

```bash
git add README.md CLAUDE.md
git commit -m "docs: document grouped stream/ui automation channels"
```

---

## Final verification

- [ ] `cmake --build build -j` — clean build
- [ ] `ctest --test-dir build --output-on-failure` — all pass (`core_tests`, `gl_smoke`)
- [ ] `./build/shader_streamer --screenshot build/_ui.png` — grid shows a stream group + a ui group with the playhead
- [ ] Use superpowers:finishing-a-development-branch
