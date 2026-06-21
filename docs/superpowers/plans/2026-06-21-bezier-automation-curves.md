# Bézier Automation Curves Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Upgrade the Automation editor's breakpoint curves from piecewise-linear to cubic Bézier with draggable, aligned-but-breakable tangent handles, sampled + persisted backward-compatibly.

**Architecture:** Extend `AutoPoint` with two (bar,value) tangent-handle offsets + a `broken` flag; rewrite `AutoCurve::sample()` to evaluate a per-segment cubic Bézier as a monotonic-time function (clamped control bars + bisection, with an exact linear fast-path for retracted handles); version the `encode/decodeCurve` text codec (`b1;` prefix) keeping the legacy parser. The two samplers (`AutomationNode`, `AutomationStore`) and `ProjectFile` inherit Bézier unchanged. The Automation panel draws segments with `AddBezierCubic` and grows point-selection + handle editing.

**Tech Stack:** C++17, Dear ImGui 1.91.5 (`ImDrawList::AddBezierCubic`), doctest, CMake. Design: `docs/superpowers/specs/2026-06-21-bezier-automation-curves-design.md`.

**Notes:**
- `core/AutoCurve.h` is a GL-free header-only file → no CMake change; it's already compiled into `core_tests` (via `tests/test_auto_curve.cpp`) and the app.
- The extended `AutoPoint` keeps default member initializers, so existing 2-field brace-inits like `{0.0f, 0.2f}` and `{nums[i], nums[i+1]}` still compile (aggregate init fills the rest with defaults). Do **not** change those call sites.
- Tasks 1–2 are TDD (GL-free math/codec). Tasks 3–4 are the interactive ImGui editor — there is no headless mouse, and the panel has no automated test today; they are verified by a clean build, `ctest` staying green, `--screenshot` exiting 0 (the new code compiles and doesn't crash drawing the Automation window), and a manual checklist.

---

### Task 1: `AutoPoint` tangent handles + cubic Bézier sampling

**Files:**
- Modify: `src/core/AutoCurve.h` (the `AutoPoint` struct, add helpers, rewrite `AutoCurve::sample`)
- Test: `tests/test_auto_curve.cpp` (append cases)

- [ ] **Step 1: Append the failing tests to `tests/test_auto_curve.cpp`**

```cpp
TEST_CASE("retracted handles keep a segment exactly linear") {
    AutoCurve c;
    c.points = { {0.0f, 0.0f}, {2.0f, 1.0f} };
    CHECK(c.sample(1.0f) == doctest::Approx(0.5f));      // identical to the old linear curve
    CHECK(c.sample(0.5f) == doctest::Approx(0.25f));
}

TEST_CASE("an out-handle bends a segment into an ease") {
    AutoCurve c;
    AutoPoint a{0.0f, 0.0f}, b{4.0f, 1.0f};
    a.outDBar = 2.0f; a.outDValue = 0.0f;                // flat start handle -> slow start (ease-in)
    c.points = { a, b };
    float mid = c.sample(2.0f);
    CHECK(mid < 0.5f);                                   // ease-in dips below the linear midpoint
    CHECK(mid > 0.0f);
    CHECK(c.sample(0.0f) == doctest::Approx(0.0f));      // endpoints stay exact
    CHECK(c.sample(4.0f) == doctest::Approx(1.0f));
}

TEST_CASE("an extreme handle still samples monotonic, single-valued, no NaN") {
    AutoCurve c;
    AutoPoint a{0.0f, 0.0f}, b{2.0f, 1.0f};
    a.outDBar = 100.0f; a.outDValue = 1.0f;              // way past the next point; clamp keeps x monotonic
    c.points = { a, b };
    float prev = -1.0f;
    for (float bar = 0.0f; bar <= 2.0f + 1e-4f; bar += 0.25f) {
        float v = c.sample(bar);
        CHECK(v == v);                                   // not NaN
        CHECK(v >= prev - 1e-4f);                        // non-decreasing (this curve only rises)
        prev = v;
    }
    CHECK(c.sample(0.0f) == doctest::Approx(0.0f));
    CHECK(c.sample(2.0f) == doctest::Approx(1.0f));
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake -S . -B build && cmake --build build -j --target core_tests`
Expected: FAIL — `AutoPoint` has no member `outDBar`/`outDValue`.

- [ ] **Step 3: Extend `AutoPoint` and add the Bézier helpers (`src/core/AutoCurve.h`)**

Replace the `AutoPoint` struct (the current lines 8–9):

```cpp
// One automation breakpoint: a normalised value in [0,1] at a song position (bars).
// Tangent handles are stored as (bar, value) offsets from the point: out* shapes the
// cubic segment toward the next point, in* toward the previous. All-zero handles => a
// straight (linear) segment. 'broken' lets the editor move the two handles independently;
// otherwise it keeps them colinear. New points default to retracted (linear).
struct AutoPoint {
    float bar = 0.0f, value = 0.0f;
    float outDBar = 0.0f, outDValue = 0.0f;   // handle toward the next point (shapes the segment to the right)
    float inDBar  = 0.0f, inDValue  = 0.0f;   // handle toward the previous point (shapes the segment to the left)
    bool  broken  = false;                    // false = editor keeps in/out colinear (aligned)
};

// One Bézier control point in (bar, value) space.
struct CurvePt { float bar = 0.0f, value = 0.0f; };

// The four cubic-Bézier control points for the segment between a (left) and b (right),
// with the control *bars* clamped monotonic (c[0].bar <= c[1].bar <= c[2].bar <= c[3].bar)
// so x(t) is non-decreasing and the curve is a single-valued function of bar. Shared by
// sampling and the editor's drawing so the drawn curve always matches the sampled one.
inline void bezierControls(const AutoPoint& a, const AutoPoint& b, CurvePt c[4]) {
    float b1 = a.bar + a.outDBar;
    if (b1 < a.bar) b1 = a.bar;
    if (b1 > b.bar) b1 = b.bar;
    float b2 = b.bar + b.inDBar;
    if (b2 < b1)    b2 = b1;
    if (b2 > b.bar) b2 = b.bar;
    c[0] = { a.bar, a.value };
    c[1] = { b1,    a.value + a.outDValue };
    c[2] = { b2,    b.value + b.inDValue };
    c[3] = { b.bar, b.value };
}

// Sample the cubic segment between a and b at song position `bar` (a.bar <= bar <= b.bar).
// Linear fast-path when both facing handles are retracted (bit-exact with the old linear
// curve); otherwise solve x(t)=bar by bisection on the monotonic control polygon, return y(t).
inline float bezierSampleSegment(const AutoPoint& a, const AutoPoint& b, float bar) {
    float span = b.bar - a.bar;
    if (span <= 0.0f) return a.value;
    if (a.outDBar == 0.0f && a.outDValue == 0.0f && b.inDBar == 0.0f && b.inDValue == 0.0f)
        return a.value + (bar - a.bar) / span * (b.value - a.value);
    CurvePt c[4];
    bezierControls(a, b, c);
    auto cubic = [](float p0, float p1, float p2, float p3, float t) {
        float u = 1.0f - t;
        return u*u*u*p0 + 3.0f*u*u*t*p1 + 3.0f*u*t*t*p2 + t*t*t*p3;
    };
    float lo = 0.0f, hi = 1.0f;
    for (int i = 0; i < 24; ++i) {
        float t = 0.5f * (lo + hi);
        float x = cubic(c[0].bar, c[1].bar, c[2].bar, c[3].bar, t);
        if (x < bar) lo = t; else hi = t;
    }
    float t = 0.5f * (lo + hi);
    return cubic(c[0].value, c[1].value, c[2].value, c[3].value, t);
}
```

- [ ] **Step 4: Rewrite `AutoCurve::sample` to use the segment helper (`src/core/AutoCurve.h`)**

In the `AutoCurve` struct, replace the body of the `for` loop in `sample()` (the current lines 21–27) so the bracketing-segment branch calls the helper:

```cpp
    float sample(float bar) const {
        if (points.empty()) return 0.0f;
        if (bar <= points.front().bar) return points.front().value;
        if (bar >= points.back().bar)  return points.back().value;
        for (std::size_t i = 0; i + 1 < points.size(); ++i) {
            if (bar >= points[i].bar && bar <= points[i + 1].bar)
                return bezierSampleSegment(points[i], points[i + 1], bar);
        }
        return points.back().value;
    }
```

Leave `encodeCurve`/`decodeCurve` unchanged in this task — they still compile (they only read `.bar`/`.value` and brace-init two fields) and keep emitting the legacy 2-float form for now; Task 2 upgrades them.

- [ ] **Step 5: Run to verify it passes**

Run: `cmake --build build -j --target core_tests && ./build/core_tests`
Expected: PASS — the three new cases plus the two existing `test_auto_curve` cases (the linear triangle still samples exactly, via the fast-path).

- [ ] **Step 6: Commit**

```bash
git add src/core/AutoCurve.h tests/test_auto_curve.cpp
git commit -m "$(cat <<'EOF'
feat(core): cubic Bezier tangent handles + sampling for AutoCurve

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
)"
```

---

### Task 2: Versioned, backward-compatible curve codec

**Files:**
- Modify: `src/core/AutoCurve.h` (`encodeCurve`, `decodeCurve`)
- Test: `tests/test_auto_curve.cpp` (append), `tests/test_project_enablers.cpp` (extend one case)

- [ ] **Step 1: Append the failing codec tests to `tests/test_auto_curve.cpp`**

```cpp
TEST_CASE("curve codec round-trips handles and the broken flag") {
    AutoCurve c;
    AutoPoint a{0.0f, 0.2f}; a.outDBar = 0.5f; a.outDValue = 0.1f; a.broken = true;
    AutoPoint b{2.0f, 0.8f}; b.inDBar = -0.3f; b.inDValue = -0.05f;
    c.points = { a, b };
    AutoCurve r = decodeCurve(encodeCurve(c));
    REQUIRE(r.points.size() == 2);
    CHECK(r.points[0].outDBar   == doctest::Approx(0.5f));
    CHECK(r.points[0].outDValue == doctest::Approx(0.1f));
    CHECK(r.points[0].broken);
    CHECK(r.points[1].inDBar    == doctest::Approx(-0.3f));
    CHECK(r.points[1].inDValue  == doctest::Approx(-0.05f));
    CHECK_FALSE(r.points[1].broken);
}

TEST_CASE("a legacy bar,value curve still decodes with retracted handles") {
    AutoCurve r = decodeCurve("0.000000,0.250000,2.000000,0.750000");
    REQUIRE(r.points.size() == 2);
    CHECK(r.points[0].bar    == doctest::Approx(0.0f));
    CHECK(r.points[0].value  == doctest::Approx(0.25f));
    CHECK(r.points[1].value  == doctest::Approx(0.75f));
    CHECK(r.points[0].outDBar == doctest::Approx(0.0f));   // handles default to retracted
    CHECK_FALSE(r.points[0].broken);
}

TEST_CASE("the empty curve still round-trips to empty") {
    CHECK(encodeCurve(AutoCurve{}).empty());
    CHECK(decodeCurve(encodeCurve(AutoCurve{})).points.empty());
    CHECK(decodeCurve("").points.empty());
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build build -j --target core_tests`
Expected: FAIL — the round-trip drops the handle fields (legacy encode writes only bar,value), so `r.points[0].outDBar` is 0, not 0.5.

- [ ] **Step 3: Replace `encodeCurve` / `decodeCurve` (`src/core/AutoCurve.h`)**

Replace both functions (the current lines 34–55) with the versioned form:

```cpp
// Inline text codec for an AutoCurve. New form: "b1;" then 7 comma-joined numbers per
// point (bar,value,outDBar,outDValue,inDBar,inDValue,broken). Empty curve -> "". decode
// sniffs the "b1;" prefix and otherwise falls back to the legacy "bar,value,..." parser,
// so old projects + saved node state still load (with retracted handles). The encoding
// contains no spaces / ':' / '|', so the ProjectFile line format and AutomationNode
// saveState block format are unaffected. GL-free.
inline std::string encodeCurve(const AutoCurve& c) {
    if (c.points.empty()) return "";
    std::string s = "b1;";
    for (std::size_t i = 0; i < c.points.size(); ++i) {
        const AutoPoint& p = c.points[i];
        if (i) s += ',';
        s += std::to_string(p.bar)      + ',' + std::to_string(p.value)
           + ',' + std::to_string(p.outDBar) + ',' + std::to_string(p.outDValue)
           + ',' + std::to_string(p.inDBar)  + ',' + std::to_string(p.inDValue)
           + ',' + (p.broken ? "1" : "0");
    }
    return s;
}
inline AutoCurve decodeCurve(const std::string& s) {
    AutoCurve c;
    if (s.empty()) return c;
    bool bezier = s.rfind("b1;", 0) == 0;            // starts with the version tag
    const std::string body = bezier ? s.substr(3) : s;
    std::vector<float> nums;
    std::size_t pos = 0;
    while (pos <= body.size()) {
        std::size_t comma = body.find(',', pos);
        std::string tok = body.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        if (!tok.empty()) { try { nums.push_back(std::stof(tok)); } catch (...) {} }
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    if (bezier) {
        for (std::size_t i = 0; i + 6 < nums.size(); i += 7) {
            AutoPoint p;
            p.bar = nums[i]; p.value = nums[i + 1];
            p.outDBar = nums[i + 2]; p.outDValue = nums[i + 3];
            p.inDBar  = nums[i + 4]; p.inDValue  = nums[i + 5];
            p.broken  = nums[i + 6] != 0.0f;
            c.points.push_back(p);
        }
    } else {
        for (std::size_t i = 0; i + 1 < nums.size(); i += 2)
            c.points.push_back({ nums[i], nums[i + 1] });
    }
    return c;
}
```

- [ ] **Step 4: Extend the project-level round-trip test (`tests/test_project_enablers.cpp`)**

In the existing `TEST_CASE("encodeCurve / decodeCurve round-trip")` (the current lines 9–17), after the existing assertions and before the closing `}` of that case, add a handle-carrying round-trip so the project codec is shown to carry Bézier:

```cpp
    AutoCurve h;
    AutoPoint hp{1.0f, 0.4f}; hp.outDBar = 0.25f; hp.outDValue = -0.1f;
    h.points = { hp, {3.0f, 0.6f} };
    AutoCurve hr = decodeCurve(encodeCurve(h));
    REQUIRE(hr.points.size() == 2);
    CHECK(hr.points[0].outDBar   == doctest::Approx(0.25f));
    CHECK(hr.points[0].outDValue == doctest::Approx(-0.1f));
```

- [ ] **Step 5: Run to verify it passes**

Run: `cmake --build build -j --target core_tests && ./build/core_tests`
Expected: PASS — codec round-trips handles + `broken`; legacy strings decode to retracted-handle points; empty round-trips; the existing `AutomationNode saveState/loadState` and project round-trip cases still pass (same codec, no spaces/`:`/`|`).

- [ ] **Step 6: Full build (samplers/store/ProjectFile inherit the codec)**

Run: `cmake --build build -j && ctest --test-dir build --output-on-failure`
Expected: clean build of all targets; all tests pass — including the `gl_smoke` save/load round-trip (linear curves encode to `b1;…` and decode back identical).

- [ ] **Step 7: Commit**

```bash
git add src/core/AutoCurve.h tests/test_auto_curve.cpp tests/test_project_enablers.cpp
git commit -m "$(cat <<'EOF'
feat(core): versioned AutoCurve codec carrying Bezier handles

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
)"
```

---

### Task 3: Editor — draw segments as Bézier curves

**Files:**
- Modify: `src/ui/AutomationPanel.cpp` (add include; replace the inter-point line drawing)

- [ ] **Step 1: Add the explicit include (`src/ui/AutomationPanel.cpp`)**

After the existing `#include "core/AutomationStore.h"` line (current line 4), add:

```cpp
#include "core/AutoCurve.h"
```

(`bezierControls`/`CurvePt` live there; it's already pulled in transitively, but be explicit.)

- [ ] **Step 2: Replace the straight inter-point segments with Bézier (`src/ui/AutomationPanel.cpp`)**

In the per-lane curve-drawing loop, replace the inner segment loop (the current lines 237–239):

```cpp
        for (std::size_t k = 0; k + 1 < p->size(); ++k)
            dl->AddLine(ImVec2(X((*p)[k].bar), Yv((*p)[k].value)),
                        ImVec2(X((*p)[k + 1].bar), Yv((*p)[k + 1].value)), col, 2.0f);
```

with a clamped-control cubic Bézier per segment (drawn == sampled):

```cpp
        for (std::size_t k = 0; k + 1 < p->size(); ++k) {
            CurvePt cc[4]; bezierControls((*p)[k], (*p)[k + 1], cc);
            dl->AddBezierCubic(ImVec2(X(cc[0].bar), Yv(cc[0].value)), ImVec2(X(cc[1].bar), Yv(cc[1].value)),
                               ImVec2(X(cc[2].bar), Yv(cc[2].value)), ImVec2(X(cc[3].bar), Yv(cc[3].value)),
                               col, 2.0f, 0);
        }
```

Leave the leading/trailing hold lines and the breakpoint circles unchanged. Linear (retracted) segments degenerate to the same straight line as before.

- [ ] **Step 3: Build + verify no regression / crash**

Run: `cmake --build build -j && ctest --test-dir build --output-on-failure`
Expected: clean build; all tests pass (unchanged — this is draw-only).

Then smoke the app launches and draws the Automation window without crashing:

```bash
./build/shader_streamer --screenshot build/_auto.png && echo "exit $?"
```
Expected: exit 0. Open `build/_auto.png` with the Read tool and confirm the window renders (the "Automation" window shows its toolbar/hint text — there are no nodes in a fresh launch, so no curve yet). Report what you saw.

- [ ] **Step 4: Commit**

```bash
git add src/ui/AutomationPanel.cpp
git commit -m "$(cat <<'EOF'
feat(ui): draw automation segments as cubic Bezier curves

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
)"
```

---

### Task 4: Editor — point selection + tangent-handle editing

**Files:**
- Modify: `src/ui/AutomationPanel.h` (add selection + handle-drag state)
- Modify: `src/ui/AutomationPanel.cpp` (add `<cmath>`; lane-geometry + handle-tip lambdas; draw selected handles; selection + handle hit-test + drag; clear-on-delete)

- [ ] **Step 1: Add panel state (`src/ui/AutomationPanel.h`)**

After the existing `int  dragPoint_ = -1;` member (current line 27), add:

```cpp
    long selLane_    = -1;            // stable key of the lane with a selected point (-1 = none)
    int  selPoint_   = -1;            // index of the selected point in that lane
    int  dragHandle_ = 0;             // what the current drag moves: 0 = point, 1 = out-handle, 2 = in-handle
```

- [ ] **Step 2: Add `<cmath>` (`src/ui/AutomationPanel.cpp`)**

After the existing `#include <algorithm>` line (current line 7), add:

```cpp
#include <cmath>
```

- [ ] **Step 3: Add shared lane-geometry + handle-tip lambdas (`src/ui/AutomationPanel.cpp`)**

Immediately after the `auto X = [&](float bar) { return o.x + bar * pxPerBar; };` line (current line 209), insert:

```cpp
    auto laneYv  = [&](std::size_t i, float v) {
        float tp = o.y + y[i] + inset, bp = o.y + y[i] + hgt[i] - inset;
        return bp - v * (bp - tp);
    };
    auto laneVal = [&](std::size_t i, float py) {
        float tp = o.y + y[i] + inset, bp = o.y + y[i] + hgt[i] - inset;
        return (bp - py) / (bp - tp);
    };
    const float stubPx = 26.0f;   // retracted handles draw as a short, grabbable stub
    auto outTipS = [&](std::size_t i, const AutoPoint& sp) {
        return (sp.outDBar != 0.0f || sp.outDValue != 0.0f)
            ? ImVec2(X(sp.bar + sp.outDBar), laneYv(i, sp.value + sp.outDValue))
            : ImVec2(X(sp.bar) + stubPx, laneYv(i, sp.value));
    };
    auto inTipS = [&](std::size_t i, const AutoPoint& sp) {
        return (sp.inDBar != 0.0f || sp.inDValue != 0.0f)
            ? ImVec2(X(sp.bar + sp.inDBar), laneYv(i, sp.value + sp.inDValue))
            : ImVec2(X(sp.bar) - stubPx, laneYv(i, sp.value));
    };
```

- [ ] **Step 4: Draw the selected point's handles (`src/ui/AutomationPanel.cpp`)**

Immediately after the per-lane curve-drawing loop closes (right after the line `for (auto& pt : *p) dl->AddCircleFilled(... 4.0f, col);` and its closing `}` — current line 242) and before the playhead block (current line 243–245), insert:

```cpp
    // Selected point's tangent handles (drawn only for the selected lane/point).
    if (selLane_ >= 0 && selPoint_ >= 0) {
        for (std::size_t i = 0; i < rows.size(); ++i) {
            if (rows[i].kind != Row::Lane || rows[i].key != selLane_) continue;
            auto* p = rows[i].pts;
            if (selPoint_ < (int)p->size()) {
                const AutoPoint& sp = (*p)[selPoint_];
                ImVec2 pc(X(sp.bar), laneYv(i, sp.value));
                const ImU32 hcol = IM_COL32(232, 232, 244, 230);
                if (selPoint_ + 1 < (int)p->size()) {            // out-handle (segment to the right exists)
                    ImVec2 tp = outTipS(i, sp);
                    dl->AddLine(pc, tp, hcol, 1.0f);
                    dl->AddRectFilled(ImVec2(tp.x - 3, tp.y - 3), ImVec2(tp.x + 3, tp.y + 3), hcol);
                }
                if (selPoint_ > 0) {                             // in-handle (segment to the left exists)
                    ImVec2 tp = inTipS(i, sp);
                    dl->AddLine(pc, tp, hcol, 1.0f);
                    dl->AddRectFilled(ImVec2(tp.x - 3, tp.y - 3), ImVec2(tp.x + 3, tp.y + 3), hcol);
                }
                dl->AddCircle(pc, 6.0f, hcol, 0, 1.5f);          // ring marks the selected point
            }
            break;
        }
    }
```

- [ ] **Step 5: Add a handle hit-test helper (`src/ui/AutomationPanel.cpp`)**

In the mouse-editing section, immediately after the `hitPoint` lambda (current lines 254–262), add a sibling that hit-tests the selected point's two handle tips (returns 0 none / 1 out / 2 in):

```cpp
    auto hitHandle = [&](std::size_t i) -> int {
        if (rows[i].key != selLane_ || selPoint_ < 0) return 0;
        auto* p = rows[i].pts;
        if (selPoint_ >= (int)p->size()) return 0;
        const AutoPoint& sp = (*p)[selPoint_];
        if (selPoint_ + 1 < (int)p->size()) {
            ImVec2 t = outTipS(i, sp);
            if ((m.x - t.x) * (m.x - t.x) + (m.y - t.y) * (m.y - t.y) <= 7.0f * 7.0f) return 1;
        }
        if (selPoint_ > 0) {
            ImVec2 t = inTipS(i, sp);
            if ((m.x - t.x) * (m.x - t.x) + (m.y - t.y) * (m.y - t.y) <= 7.0f * 7.0f) return 2;
        }
        return 0;
    };
```

- [ ] **Step 6: Right-click retracts a handle (else deletes a point) (`src/ui/AutomationPanel.cpp`)**

Replace the whole right-click block (current lines 267–277):

```cpp
    if (hovered && hoverRow >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        auto* p = rows[hoverRow].pts; int h = hitPoint(hoverRow);
        if (h >= 0) {
            p->erase(p->begin() + h);
            // keep an in-progress drag on this lane pointing at the right point
            if (dragLane_ == rows[hoverRow].key) {
                if (h < dragPoint_)      --dragPoint_;
                else if (h == dragPoint_) { dragLane_ = -1; dragPoint_ = -1; }
            }
        }
    }
```

with one that first checks the selected point's handles:

```cpp
    if (hovered && hoverRow >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        int hh = hitHandle((std::size_t)hoverRow);
        if (hh != 0) {
            AutoPoint& sp = (*rows[hoverRow].pts)[selPoint_];
            if (hh == 1) { sp.outDBar = 0.0f; sp.outDValue = 0.0f; }   // retract the handle
            else         { sp.inDBar  = 0.0f; sp.inDValue  = 0.0f; }
        } else {
            auto* p = rows[hoverRow].pts; int h = hitPoint(hoverRow);
            if (h >= 0) {
                p->erase(p->begin() + h);
                // keep an in-progress drag on this lane pointing at the right point
                if (dragLane_ == rows[hoverRow].key) {
                    if (h < dragPoint_)      --dragPoint_;
                    else if (h == dragPoint_) { dragLane_ = -1; dragPoint_ = -1; }
                }
                // keep the selection valid after a delete on its lane
                if (selLane_ == rows[hoverRow].key) {
                    if (h < selPoint_)       --selPoint_;
                    else if (h == selPoint_) { selLane_ = -1; selPoint_ = -1; }
                }
            }
        }
    }
```

- [ ] **Step 7: Left-click grabs a handle / selects or adds a point (`src/ui/AutomationPanel.cpp`)**

Replace the whole left-click block (current lines 278–289):

```cpp
    if (hovered && hoverRow >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        auto* p = rows[hoverRow].pts; int h = hitPoint(hoverRow);
        if (h >= 0) { dragLane_ = rows[hoverRow].key; dragPoint_ = h; }
        else {
            AutoPoint np{ std::clamp(toBar(m.x), 0.0f, length),
                          std::clamp(rowVal(hoverRow, m.y), 0.0f, 1.0f) };
            auto it = std::lower_bound(p->begin(), p->end(), np,
                                       [](const AutoPoint& a, const AutoPoint& b) { return a.bar < b.bar; });
            dragLane_ = rows[hoverRow].key; dragPoint_ = (int)(it - p->begin());
            p->insert(it, np);
        }
    }
```

with one that prefers a handle grab, then selects/creates a point:

```cpp
    if (hovered && hoverRow >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        int hh = hitHandle((std::size_t)hoverRow);
        if (hh != 0) {                                   // grab the selected point's handle
            dragLane_ = selLane_; dragPoint_ = selPoint_; dragHandle_ = hh;
        } else {
            auto* p = rows[hoverRow].pts; int h = hitPoint(hoverRow);
            if (h >= 0) {                                // select + start moving an existing point
                dragLane_ = rows[hoverRow].key; dragPoint_ = h; dragHandle_ = 0;
                selLane_  = rows[hoverRow].key; selPoint_  = h;
            } else {                                     // add a new (linear) point + select it
                AutoPoint np{ std::clamp(toBar(m.x), 0.0f, length),
                              std::clamp(rowVal(hoverRow, m.y), 0.0f, 1.0f) };
                auto it = std::lower_bound(p->begin(), p->end(), np,
                                           [](const AutoPoint& a, const AutoPoint& b) { return a.bar < b.bar; });
                int idx = (int)(it - p->begin());
                p->insert(it, np);
                dragLane_ = rows[hoverRow].key; dragPoint_ = idx; dragHandle_ = 0;
                selLane_  = rows[hoverRow].key; selPoint_  = idx;
            }
        }
    } else if (hovered && hoverRow < 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        selLane_ = -1; selPoint_ = -1;                   // clicked empty space -> deselect
    }
```

- [ ] **Step 8: Drag a point or a handle (`src/ui/AutomationPanel.cpp`)**

Replace the in-progress drag block (current lines 293–307) — which currently only moves the point — with one that branches on `dragHandle_`:

```cpp
    if (dragLane_ >= 0 && dragPoint_ >= 0 && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        int dr = -1;
        for (std::size_t i = 0; i < rows.size(); ++i)
            if (rows[i].kind == Row::Lane && rows[i].key == dragLane_) { dr = (int)i; break; }
        if (dr >= 0) {
            auto* p = rows[dr].pts;
            if (dragPoint_ < (int)p->size()) {
                if (dragHandle_ == 0) {
                    // ---- move the point (unchanged, neighbour-clamped) ----
                    float nb = std::clamp(toBar(m.x), 0.0f, length);
                    float nv = std::clamp(rowVal((std::size_t)dr, m.y), 0.0f, 1.0f);
                    if (dragPoint_ > 0)                  nb = std::max(nb, (*p)[dragPoint_ - 1].bar + 1e-4f);
                    if (dragPoint_ + 1 < (int)p->size()) nb = std::min(nb, (*p)[dragPoint_ + 1].bar - 1e-4f);
                    (*p)[dragPoint_].bar = nb; (*p)[dragPoint_].value = nv;
                } else {
                    // ---- drag a tangent handle ----
                    AutoPoint& sp = (*p)[dragPoint_];
                    int n = (int)p->size();
                    bool out = (dragHandle_ == 1);
                    if (ImGui::GetIO().KeyAlt) sp.broken = true;     // Alt-drag breaks alignment (sticky)
                    // primary handle: offset from cursor, clamped into its segment + lane
                    float nv = std::clamp(laneVal((std::size_t)dr, m.y), 0.0f, 1.0f) - sp.value;
                    if (out) {
                        float nb = std::max(0.0f, toBar(m.x) - sp.bar);
                        if (dragPoint_ + 1 < n) nb = std::min(nb, (*p)[dragPoint_ + 1].bar - sp.bar);
                        sp.outDBar = nb; sp.outDValue = nv;
                    } else {
                        float nb = std::min(0.0f, toBar(m.x) - sp.bar);
                        if (dragPoint_ > 0)     nb = std::max(nb, (*p)[dragPoint_ - 1].bar - sp.bar);
                        sp.inDBar = nb; sp.inDValue = nv;
                    }
                    // align the opposite handle in SCREEN space (preserve its length) unless broken
                    bool haveOpp = out ? (dragPoint_ > 0) : (dragPoint_ + 1 < n);
                    if (!sp.broken && haveOpp) {
                        ImVec2 pcS(X(sp.bar), laneYv((std::size_t)dr, sp.value));
                        ImVec2 priS = out ? outTipS((std::size_t)dr, sp) : inTipS((std::size_t)dr, sp);
                        ImVec2 oppS = out ? inTipS((std::size_t)dr, sp)  : outTipS((std::size_t)dr, sp);
                        float dx = priS.x - pcS.x, dy = priS.y - pcS.y;
                        float len = std::sqrt(dx * dx + dy * dy);
                        float odx = oppS.x - pcS.x, ody = oppS.y - pcS.y;
                        float oppLen = std::sqrt(odx * odx + ody * ody);
                        if (len > 1e-3f) {
                            float tx = pcS.x - dx / len * oppLen;
                            float ty = pcS.y - dy / len * oppLen;
                            float ob = toBar(tx) - sp.bar;
                            float ov = std::clamp(laneVal((std::size_t)dr, ty), 0.0f, 1.0f) - sp.value;
                            if (out) {                                  // opposite is the in-handle (backward)
                                ob = std::min(0.0f, ob);
                                if (dragPoint_ > 0) ob = std::max(ob, (*p)[dragPoint_ - 1].bar - sp.bar);
                                sp.inDBar = ob; sp.inDValue = ov;
                            } else {                                    // opposite is the out-handle (forward)
                                ob = std::max(0.0f, ob);
                                if (dragPoint_ + 1 < n) ob = std::min(ob, (*p)[dragPoint_ + 1].bar - sp.bar);
                                sp.outDBar = ob; sp.outDValue = ov;
                            }
                        }
                    }
                }
            }
        }
    }
```

- [ ] **Step 9: Reset the drag-handle discriminator on mouse-up (`src/ui/AutomationPanel.cpp`)**

Replace the drag-release line (current line 308):

```cpp
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) { dragLane_ = -1; dragPoint_ = -1; }
```

with:

```cpp
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) { dragLane_ = -1; dragPoint_ = -1; dragHandle_ = 0; }
```

- [ ] **Step 10: Clear selection when a lane's curve is cleared (`src/ui/AutomationPanel.cpp`)**

In the `clr` button handler (current lines 184–187), after the existing drag reset, also drop a selection on that lane. Replace:

```cpp
                if (ImGui::SmallButton("clr")) {
                    r.pts->clear();
                    if (dragLane_ == r.key) { dragLane_ = -1; dragPoint_ = -1; }
                }
```

with:

```cpp
                if (ImGui::SmallButton("clr")) {
                    r.pts->clear();
                    if (dragLane_ == r.key) { dragLane_ = -1; dragPoint_ = -1; }
                    if (selLane_  == r.key) { selLane_  = -1; selPoint_  = -1; }
                }
```

- [ ] **Step 11: Update the help text (`src/ui/AutomationPanel.cpp`)**

Replace the help line (current lines 314–315):

```cpp
    ImGui::TextDisabled("Click a lane to add a point, drag to move, right-click to delete. "
                        "Click a group header to collapse. Red line = transport playhead.");
```

with one that mentions handles:

```cpp
    ImGui::TextDisabled("Click a lane to add/select a point, drag to move, right-click to delete. "
                        "Select a point to reveal its Bezier handles; drag a handle to curve, "
                        "Alt-drag to break, right-click a handle to reset. Red line = playhead.");
```

- [ ] **Step 12: Build + verify**

Run: `cmake --build build -j && ctest --test-dir build --output-on-failure`
Expected: clean build; all tests pass (unchanged — editor only).

Smoke that the app still launches + draws the Automation window:

```bash
./build/shader_streamer --screenshot build/_auto.png && echo "exit $?"
```
Expected: exit 0. Open `build/_auto.png` with the Read tool, confirm the Automation window renders, report what you saw.

**Manual checklist (report which you verified; the app needs a display):** run `./build/shader_streamer`, add an **Automation** node (right-click the node editor canvas), then in the **Automation** window: (1) click a lane to add a couple of points; (2) click a point — its two handle stubs + a ring appear; (3) drag a handle — the segment curves and the opposite handle mirrors it; (4) Alt-drag a handle — only it moves (broken); (5) right-click a handle — it retracts (segment straightens); (6) clicking another point moves the selection; (7) save + reload the project — the curve shape persists.

- [ ] **Step 13: Commit**

```bash
git add src/ui/AutomationPanel.h src/ui/AutomationPanel.cpp
git commit -m "$(cat <<'EOF'
feat(ui): select breakpoints and edit cubic Bezier tangent handles

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
)"
```

---

### Task 5: Documentation

**Files:**
- Modify: `README.md`, `CLAUDE.md`

- [ ] **Step 1: README.md**

Find the Automation paragraph (search for "Automation"). After the sentence describing editing breakpoints, add:

```markdown
Curve segments are cubic Bézier: select a breakpoint to reveal its tangent handles, drag a
handle to shape the curve (the two stay aligned for a smooth pass-through), Alt-drag to break
the tangent for a sharp corner, and right-click a handle to reset it. Curves with untouched
handles stay straight lines, and older projects load unchanged.
```

- [ ] **Step 2: CLAUDE.md**

In the **Automation** architecture bullet (search for "`AutoCurve`"), update the breakpoint-curve description. Replace the phrase that calls the curve "a GL-free `AutoCurve`" / "breakpoint curve" with a note that it is now Bézier — append this sentence to that bullet:

```markdown
  The breakpoint curve is a GL-free `AutoCurve` whose segments are cubic **Bézier**: each
  `AutoPoint` carries in/out tangent-handle offsets + a `broken` flag, sampled as a
  monotonic-time function (control bars clamped monotonic + bisection, with a linear
  fast-path for retracted handles so untouched curves stay exactly linear). The text codec
  is versioned (`b1;` prefix) and backward-compatible (legacy `bar,value,…` still decodes).
  Only `AutoCurve` + the Automation panel changed — the two samplers, the `AutomationStore`,
  and `ProjectFile` inherit Bézier through `sample()` / `encode`/`decodeCurve`.
```

- [ ] **Step 3: Verify + commit**

Run: `ctest --test-dir build --output-on-failure`
Expected: all pass.

```bash
git add README.md CLAUDE.md
git commit -m "$(cat <<'EOF'
docs: document Bezier automation curves

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
)"
```

---

## Final verification

- [ ] `cmake --build build -j` — clean build (`shader_streamer`, `core_tests`, `gl_smoke`)
- [ ] `ctest --test-dir build --output-on-failure` — all pass (linear curves bit-exact; Bézier sampling; monotonic clamp; codec round-trip incl. handles + legacy + empty; project round-trip; gl_smoke save/load)
- [ ] `./build/shader_streamer --screenshot build/_auto.png` — exit 0; Automation window renders
- [ ] Manual: add an Automation node, edit a curve's handles (drag / Alt-drag break / right-click reset), save + reload — curve shape persists
- [ ] Use superpowers:finishing-a-development-branch
