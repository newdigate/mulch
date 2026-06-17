# LFO Module Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A GL-free LFO node that outputs a control-rate `Float` modulation signal — selectable waveform, free (Hz) or BPM-synced (bar divisions) rate, mapped into a `[min,max]` output range — with every control as an input port so LFOs chain.

**Architecture:** `LfoNode` (header-only, `src/modules/`) reads the transport + `dt` and writes one `Float`/frame. A small reusable "choice" inline widget (`Port::choices` + `Node::addChoiceInput` + a combo branch in `PortWidgets.cpp`) renders the waveform and sync-rate dropdowns, so the node stays purely port-based with no LFO-specific UI.

**Tech Stack:** C++17, Dear ImGui, doctest. GL-free core/modules.

**Spec:** `docs/superpowers/specs/2026-06-17-lfo-module-design.md`

---

## File Structure

| File | Responsibility | Change |
|---|---|---|
| `src/core/Port.h` | add `choices` label list to `Port` | modify |
| `src/core/Node.h` | `addChoiceInput()` helper | modify |
| `src/ui/PortWidgets.cpp` | render a combo for a Float port with choices | modify |
| `src/modules/LfoNode.h` | the LFO node + `lfoSample` + division table | **create** (header-only) |
| `src/app/Application.cpp` | register `"LFO"` (factory + `Control` category) | modify |
| `src/main.cpp` | add an LFO to the screenshot demo | modify |
| `tests/test_choice_port.cpp` | `addChoiceInput` builds the right port | **create** |
| `tests/test_lfo.cpp` | waveforms, free/sync phase, range, S&H, choice ports | **create** |
| `CMakeLists.txt` | add the two test files to `core_tests` | modify |
| `README.md`, `CLAUDE.md` | document the LFO + choice widget | modify |

Each task ends green (build + `ctest`), so tasks can land independently.

---

### Task 1: Choice port — `Port::choices` + `Node::addChoiceInput`

A Float input that renders as a dropdown of labels; its value is the selected index.
Reusable by any node (the LFO uses it for waveform + sync-rate).

**Files:**
- Modify: `src/core/Port.h`, `src/core/Node.h`
- Test: `tests/test_choice_port.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Write the failing test**

Create `tests/test_choice_port.cpp`:

```cpp
#include <doctest/doctest.h>
#include "core/Node.h"
#include "core/Value.h"
#include <variant>

using namespace oss;

namespace {
// A throwaway node that declares one choice input.
struct Choosy : Node {
    Choosy() : Node("Choosy") { addChoiceInput("pick", {"a", "b", "c"}, 1); }
    void evaluate(EvalContext&) override {}
};
} // namespace

TEST_CASE("addChoiceInput builds a Float port carrying its choice labels") {
    Choosy n;
    REQUIRE(n.inputs().size() == 1);
    const Port& p = n.inputs()[0];
    CHECK(p.type == PortType::Float);
    REQUIRE(p.choices.size() == 3);
    CHECK(p.choices[0] == "a");
    CHECK(p.choices[2] == "c");
    CHECK(std::get<float>(p.defaultValue) == doctest::Approx(1.0f));   // default index
    CHECK(p.minVal == doctest::Approx(0.0f));
    CHECK(p.maxVal == doctest::Approx(2.0f));                          // size - 1
}
```

- [ ] **Step 2: Wire the test into CMake**

In `CMakeLists.txt`, in the `add_executable(core_tests ...)` list, add after
`  tests/test_world_transform.cpp`:

```cmake
  tests/test_choice_port.cpp
```

- [ ] **Step 3: Run to verify it fails**

Run: `cmake --build build -j 2>&1 | tail -5`
Expected: compile error — `Node` has no member `addChoiceInput` (and `Port` has no `choices`).

- [ ] **Step 4: Add `choices` to `Port`**

In `src/core/Port.h`, add `#include <vector>` after `#include <string>`, and add the
`choices` member as the last field of `Port`:

```cpp
#pragma once
#include <string>
#include <vector>
#include "core/Value.h"

namespace oss {

enum class Direction { Input, Output };

struct Port {
    std::string name;
    Direction  direction;
    PortType   type;
    Value      defaultValue;   // used for an unconnected input; drives inline widgets
    float      minVal = 0.0f;  // range for an inline Float slider (ignored otherwise)
    float      maxVal = 1.0f;
    // Optional dropdown labels: a Float input with a non-empty list renders as a
    // combo whose value is the selected index (used for enum-like parameters).
    std::vector<std::string> choices;
};

} // namespace oss
```

- [ ] **Step 5: Add the `addChoiceInput` helper to `Node`**

In `src/core/Node.h`, in the `protected:` section, after the existing `addOutput`
helper, add:

```cpp
    // A Float input rendered as a dropdown of `labels`; its value is the selected
    // index (0-based). Used for enum-like parameters (e.g. an LFO waveform).
    void addChoiceInput(std::string n, std::vector<std::string> labels, int def) {
        Port p;
        p.name = std::move(n);
        p.direction = Direction::Input;
        p.type = PortType::Float;
        p.defaultValue = Value((float)def);
        p.minVal = 0.0f;
        p.maxVal = labels.empty() ? 0.0f : (float)(labels.size() - 1);
        p.choices = std::move(labels);
        inputs_.push_back(std::move(p));
    }
```

(`Node.h` already includes `<string>`, `<vector>`, and `core/Port.h`, so no new
includes are needed.)

- [ ] **Step 6: Run the test to verify it passes**

Run: `cmake --build build -j && ./build/core_tests --test-case="addChoiceInput builds a Float port carrying its choice labels"`
Expected: PASS.

- [ ] **Step 7: Full suite**

Run: `ctest --test-dir build --output-on-failure`
Expected: all pass (the new `choices` field is backward-compatible — existing
`addInput` brace-inits leave it default-empty).

- [ ] **Step 8: Commit**

```bash
git add src/core/Port.h src/core/Node.h tests/test_choice_port.cpp CMakeLists.txt
git commit -m "feat(core): add choice (dropdown) input ports"
```

---

### Task 2: Render a combo for choice ports

**Files:**
- Modify: `src/ui/PortWidgets.cpp`

No unit test (this is ImGui draw code); verified by a clean build + the suite staying
green, and exercised visually in Task 4's screenshot.

- [ ] **Step 1: Add the combo branch**

In `src/ui/PortWidgets.cpp`, add these includes after the existing includes (below
`#include <cstdio>`):

```cpp
#include <algorithm>
#include <cmath>
```

Then replace the `PortType::Float` case:

```cpp
        case PortType::Float: {
            ImGui::SliderFloat("##f", &std::get<float>(v), port.minVal, port.maxVal);
            break;
        }
```

with:

```cpp
        case PortType::Float: {
            if (!port.choices.empty()) {
                // A choice port: a dropdown whose value is the selected index.
                int idx = (int)std::lround(std::get<float>(v));
                idx = std::clamp(idx, 0, (int)port.choices.size() - 1);
                if (ImGui::BeginCombo("##choice", port.choices[idx].c_str())) {
                    for (int k = 0; k < (int)port.choices.size(); ++k) {
                        bool sel = (k == idx);
                        if (ImGui::Selectable(port.choices[k].c_str(), sel)) v = Value((float)k);
                        if (sel) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
            } else {
                ImGui::SliderFloat("##f", &std::get<float>(v), port.minVal, port.maxVal);
            }
            break;
        }
```

- [ ] **Step 2: Build**

Run: `cmake --build build -j 2>&1 | tail -5`
Expected: builds clean.

- [ ] **Step 3: Suite stays green**

Run: `ctest --test-dir build --output-on-failure`
Expected: all pass.

- [ ] **Step 4: Commit**

```bash
git add src/ui/PortWidgets.cpp
git commit -m "feat(ui): render choice input ports as a dropdown"
```

---

### Task 3: `LfoNode`

**Files:**
- Create: `src/modules/LfoNode.h`
- Test: `tests/test_lfo.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Write the failing tests**

Create `tests/test_lfo.cpp`:

```cpp
#include <doctest/doctest.h>
#include "modules/LfoNode.h"
#include "core/Node.h"
#include "core/Transport.h"
#include "core/Value.h"
#include <algorithm>
#include <variant>
#include <vector>

using namespace oss;

// The 6 input Values in port order: waveform, sync, rate Hz, rate sync, min, max.
static std::vector<Value> lfoInputs(float waveform, bool sync, float rateHz,
                                    float rateSync, float lo, float hi) {
    return { Value(waveform), Value(sync), Value(rateHz),
             Value(rateSync), Value(lo), Value(hi) };
}

TEST_CASE("lfoSample produces the expected deterministic shapes") {
    CHECK(lfoSample(0, 0.0)  == doctest::Approx(0.5));   // Sine
    CHECK(lfoSample(0, 0.25) == doctest::Approx(1.0));
    CHECK(lfoSample(0, 0.5)  == doctest::Approx(0.5));
    CHECK(lfoSample(0, 0.75) == doctest::Approx(0.0));
    CHECK(lfoSample(1, 0.0)  == doctest::Approx(0.0));   // Triangle
    CHECK(lfoSample(1, 0.25) == doctest::Approx(0.5));
    CHECK(lfoSample(1, 0.5)  == doctest::Approx(1.0));
    CHECK(lfoSample(2, 0.25) == doctest::Approx(1.0));   // Square
    CHECK(lfoSample(2, 0.75) == doctest::Approx(0.0));
    CHECK(lfoSample(3, 0.3)  == doctest::Approx(0.3));   // Ramp Up
    CHECK(lfoSample(4, 0.3)  == doctest::Approx(0.7));   // Ramp Down
}

TEST_CASE("free-run LFO advances phase by rate*dt") {
    LfoNode lfo;
    std::vector<Value> out(1);
    std::vector<Value> in = lfoInputs(0.0f, false, 1.0f, 8.0f, 0.0f, 1.0f);
    EvalContext c0{in, out, 0.0f};   lfo.evaluate(c0);   // phase 0 -> sine 0.5
    CHECK(std::get<float>(out[0]) == doctest::Approx(0.5f));
    EvalContext c1{in, out, 0.25f};  lfo.evaluate(c1);   // +0.25 -> sine peak 1.0
    CHECK(std::get<float>(out[0]) == doctest::Approx(1.0f));
}

TEST_CASE("output range maps the normalised waveform into [min,max]") {
    LfoNode lfo;
    std::vector<Value> out(1);
    std::vector<Value> in = lfoInputs(0.0f, false, 1.0f, 8.0f, 0.0f, 2.0f);
    EvalContext c0{in, out, 0.0f};   lfo.evaluate(c0);   // mid 0.5 -> 1.0
    CHECK(std::get<float>(out[0]) == doctest::Approx(1.0f));
    EvalContext c1{in, out, 0.25f};  lfo.evaluate(c1);   // peak 1.0 -> 2.0
    CHECK(std::get<float>(out[0]) == doctest::Approx(2.0f));
}

TEST_CASE("synced LFO derives phase from the transport bar position") {
    LfoNode lfo;
    Transport t; t.bpm = 120.0; t.seconds = 0.5;   // 2 s/bar -> bars = 0.25
    std::vector<Value> in = lfoInputs(0.0f, true, 1.0f, 8.0f, 0.0f, 1.0f);  // "1 bar"
    std::vector<Value> out(1);
    EvalContext ctx{in, out, 0.0f, &t};
    lfo.evaluate(ctx);
    CHECK(std::get<float>(out[0]) == doctest::Approx(1.0f));   // sine(0.25) = 1.0
}

TEST_CASE("waveform input rounds to the nearest index and clamps") {
    LfoNode lfo;
    std::vector<Value> out(1);
    {   // 1.4 -> Triangle (index 1); triangle at phase 0 = 0
        std::vector<Value> in = lfoInputs(1.4f, false, 1.0f, 8.0f, 0.0f, 1.0f);
        EvalContext ctx{in, out, 0.0f};
        lfo.evaluate(ctx);
        CHECK(std::get<float>(out[0]) == doctest::Approx(0.0f));
    }
    {   // 9.0 clamps to Sample & Hold (index 5) -> a value in [0,1], no out-of-bounds
        std::vector<Value> in = lfoInputs(9.0f, false, 1.0f, 8.0f, 0.0f, 1.0f);
        EvalContext ctx{in, out, 0.0f};
        lfo.evaluate(ctx);
        float v = std::get<float>(out[0]);
        CHECK(v >= 0.0f);
        CHECK(v <= 1.0f);
    }
}

TEST_CASE("Sample & Hold holds within a cycle and changes across cycles") {
    LfoNode lfo;
    std::vector<Value> out(1);
    auto evalSH = [&](float dt) {
        std::vector<Value> in = lfoInputs(5.0f, false, 1.0f, 8.0f, 0.0f, 1.0f);
        EvalContext ctx{in, out, dt};
        lfo.evaluate(ctx);
        return std::get<float>(out[0]);
    };
    float a = evalSH(0.0f);    // phase 0, initial held value
    float b = evalSH(0.4f);    // same cycle -> unchanged
    CHECK(a == doctest::Approx(b));
    std::vector<float> seen{a};
    for (int i = 0; i < 8; ++i) {
        float v = evalSH(1.0f);   // wrap a whole cycle -> re-latch
        bool known = std::any_of(seen.begin(), seen.end(),
            [&](float s){ return s == doctest::Approx(v); });
        if (!known) seen.push_back(v);
    }
    CHECK(seen.size() >= 2);   // not stuck on one value
}

TEST_CASE("the LFO exposes choice ports for waveform and sync rate") {
    LfoNode lfo;
    REQUIRE(lfo.inputs().size() == 6);
    CHECK(lfo.inputs()[0].choices.size() == 6);    // waveform
    CHECK(std::get<float>(lfo.inputs()[0].defaultValue) == doctest::Approx(0.0f));
    CHECK(lfo.inputs()[3].choices.size() == 15);   // rate sync
    CHECK(std::get<float>(lfo.inputs()[3].defaultValue) == doctest::Approx(8.0f));
}
```

- [ ] **Step 2: Wire the test into CMake**

In `CMakeLists.txt`, in the `add_executable(core_tests ...)` list, add after
`  tests/test_choice_port.cpp`:

```cmake
  tests/test_lfo.cpp
```

- [ ] **Step 3: Run to verify it fails**

Run: `cmake --build build -j 2>&1 | tail -5`
Expected: compile error — `modules/LfoNode.h` does not exist.

- [ ] **Step 4: Create `LfoNode.h`**

Create `src/modules/LfoNode.h`:

```cpp
#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <random>
#include <string>
#include <vector>
#include "core/Node.h"
#include "core/Value.h"

namespace oss {

// LFO period per sync division, slow -> fast (bars per cycle). Index matches the
// "rate sync" choice labels in the LfoNode constructor.
static constexpr double kLfoDivisionBars[15] = {
    32.0, 24.0, 16.0, 12.0, 8.0, 6.0, 4.0, 2.0, 1.0,
    0.5, 0.25, 0.125, 0.0625, 0.03125, 0.015625
};

// One LFO cycle's normalised value in [0,1] at phase p in [0,1), for the
// deterministic waveforms. Sample & Hold (index 5) is stateful and supplied by the
// node, so it returns 0 here.
inline double lfoSample(int wf, double p) {
    constexpr double kTwoPi = 6.283185307179586;
    switch (wf) {
        case 0: return 0.5 + 0.5 * std::sin(kTwoPi * p);   // Sine
        case 1: return p < 0.5 ? 2.0 * p : 2.0 - 2.0 * p;  // Triangle (0 -> 1 -> 0)
        case 2: return p < 0.5 ? 1.0 : 0.0;                // Square (50% duty)
        case 3: return p;                                  // Ramp Up
        case 4: return 1.0 - p;                            // Ramp Down
        default: return 0.0;                               // Sample & Hold: node supplies
    }
}

// Low-frequency oscillator: a control-rate Float modulation source. Pick a waveform
// and run it free (rate in Hz) or synced to the transport (rate in bar divisions);
// the [0,1] waveform is mapped into a [min,max] output range. Every control is an
// input port, so waveform/rate/sync can each be driven by another node -- LFOs
// chain. Free mode integrates rate*dt every frame (runs even while stopped); sync
// mode derives phase from transport.bars() (locked to song position). GL-free.
class LfoNode : public Node {
public:
    LfoNode() : Node("LFO") {
        addChoiceInput("waveform",
            {"Sine", "Triangle", "Square", "Ramp Up", "Ramp Down", "Sample & Hold"}, 0);
        addInput("sync", PortType::Bool, false);
        addInput("rate Hz", PortType::Float, 1.0f, 0.01f, 40.0f);
        addChoiceInput("rate sync",
            {"32 bars", "24 bars", "16 bars", "12 bars", "8 bars", "6 bars", "4 bars",
             "2 bars", "1 bar", "1/2 bar", "1/4 bar", "1/8 bar", "1/16 bar",
             "1/32 bar", "1/64 bar"}, 8);
        addInput("min", PortType::Float, 0.0f, -1.0f, 1.0f);
        addInput("max", PortType::Float, 1.0f, -1.0f, 1.0f);
        addOutput("out", PortType::Float);
        shVal_ = uni_(rng_);   // seed the first Sample & Hold value
    }

    void evaluate(EvalContext& ctx) override {
        int   wf   = std::clamp((int)std::lround(ctx.in<float>(0)), 0, 5);
        bool  sync = ctx.in<bool>(1);
        float lo   = ctx.in<float>(4);
        float hi   = ctx.in<float>(5);
        double phase01 = 0.0;
        bool   newCycle = false;

        if (sync) {
            int div = std::clamp((int)std::lround(ctx.in<float>(3)), 0, 14);
            double periodBars = kLfoDivisionBars[div];
            double bars   = ctx.transport ? ctx.transport->bars() : 0.0;
            double cycles = periodBars > 0.0 ? bars / periodBars : 0.0;
            long long cyc = (long long)std::floor(cycles);
            phase01 = cycles - (double)cyc;
            if (cyc != lastCycle_) { newCycle = true; lastCycle_ = cyc; }
        } else {
            double hz = (double)ctx.in<float>(2);
            phase_ += hz * (double)ctx.dt;
            if (phase_ >= 1.0) newCycle = true;
            phase_ -= std::floor(phase_);   // wrap to [0,1)
            phase01 = phase_;
        }

        if (newCycle) shVal_ = uni_(rng_);   // re-latch Sample & Hold each cycle

        double w01 = (wf == 5) ? shVal_ : lfoSample(wf, phase01);
        ctx.out<float>(0, (float)(lo + w01 * (hi - lo)));
    }

private:
    double    phase_     = 0.0;     // free-run phase in [0,1)
    long long lastCycle_ = 0;       // sync cycle index, for Sample & Hold
    double    shVal_     = 0.0;      // held Sample & Hold value in [0,1]
    std::mt19937 rng_{0x9E3779B9u};  // fixed seed -> deterministic / testable
    std::uniform_real_distribution<double> uni_{0.0, 1.0};
};

} // namespace oss
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `cmake --build build -j && ./build/core_tests --test-case="*LFO*,*lfoSample*,*Sample & Hold*,*choice ports*,*output range maps*,*waveform input rounds*"`
Expected: PASS (all 7 LFO cases).

- [ ] **Step 6: Full suite**

Run: `ctest --test-dir build --output-on-failure`
Expected: all pass.

- [ ] **Step 7: Commit**

```bash
git add src/modules/LfoNode.h tests/test_lfo.cpp CMakeLists.txt
git commit -m "feat(modules): add LFO node (waveform/free-Hz/BPM-sync/range)"
```

---

### Task 4: Register the LFO + screenshot demo

**Files:**
- Modify: `src/app/Application.cpp`, `src/main.cpp`

- [ ] **Step 1: Register the node type**

In `src/app/Application.cpp`:

Add the include alongside the other module includes (after
`#include "modules/MixNode.h"`):

```cpp
#include "modules/LfoNode.h"
```

In `makeNode()`, add before `if (type == "Automation")`:

```cpp
    if (type == "LFO")         return std::make_unique<LfoNode>();
```

In `nodeCategories()`, change the `Control` line from:

```cpp
        { "Control", { "Automation" } },
```
to:
```cpp
        { "Control", { "Automation", "LFO" } },
```

- [ ] **Step 2: Add an LFO to the screenshot demo**

In `src/main.cpp`, in `runScreenshot`, after the Wireframe/ui-channel seed block
(right after its closing `}` and before `app.graph().transport().bpm = 120.0;`), add:

```cpp
        // An LFO node so the capture shows the choice dropdowns + the new module.
        app.addNodeOfType("LFO", glm::vec2(740.0f, 60.0f));
```

- [ ] **Step 3: Build + run the suite**

Run: `cmake --build build -j && ctest --test-dir build --output-on-failure`
Expected: builds clean; all tests pass.

- [ ] **Step 4: Visual check**

Run: `./build/shader_streamer --screenshot build/_ui.png`
Expected: stderr prints `wrote screenshot build/_ui.png (...)`. Open it and confirm an
`LFO` node appears with a `waveform` dropdown, a `sync` checkbox, a `rate Hz` slider,
a `rate sync` dropdown, and `min`/`max` sliders. (If headless without a GL context,
note that this step was skipped.)

- [ ] **Step 5: Commit**

```bash
git add src/app/Application.cpp src/main.cpp
git commit -m "feat(app): register the LFO node + show it in the screenshot demo"
```

---

### Task 5: Documentation

**Files:**
- Modify: `README.md`, `CLAUDE.md`

- [ ] **Step 1: Add the LFO to the README module table**

In `README.md`, in the module table, add this row immediately after the
`| **Automation** | ... |` row:

```markdown
| **LFO** | low-frequency oscillator → a Float modulation signal: pick a waveform (sine/triangle/square/ramp up/down/sample & hold), run it free (Hz) or BPM-synced (32 bars … 1/64 bar), and map it into a `[min, max]` range. Every control is an input port, so waveform/rate/sync can be driven by another node — chain LFOs. Wire `out` into any Float parameter (e.g. a Sine's `amp`) |
```

- [ ] **Step 2: Note the choice widget + LFO in CLAUDE.md**

In `CLAUDE.md`, in the **Architecture** section, add a new bullet immediately after
the existing **Automation** bullet:

```markdown
- **Choice input ports** — a `Float` input can carry dropdown labels
  (`Port::choices`, built with `Node::addChoiceInput(name, labels, defaultIndex)`);
  the editor renders it as a combo whose value is the selected index
  (`src/ui/PortWidgets.cpp`). The **LFO** (`src/modules/LfoNode.h`, header-only,
  GL-free) uses it for its waveform and BPM-sync-rate menus: a control-rate Float
  modulation source that runs free (Hz, integrating `rate*dt`) or transport-synced
  (phase from `transport.bars()`), mapped into a per-node `[min,max]`. All its
  controls are input ports, so LFOs chain.
```

- [ ] **Step 3: Sanity check + commit**

Run: `ctest --test-dir build --output-on-failure`
Expected: all pass (no code changed).

```bash
git add README.md CLAUDE.md
git commit -m "docs: document the LFO node and choice input ports"
```

---

## Final verification

- [ ] `cmake --build build -j` — clean build
- [ ] `ctest --test-dir build --output-on-failure` — all pass (`core_tests`, `gl_smoke`)
- [ ] `./build/shader_streamer --screenshot build/_ui.png` — the LFO node renders with its dropdowns/sliders
- [ ] Use superpowers:finishing-a-development-branch
