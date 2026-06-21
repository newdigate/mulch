# Bézier Automation Curves — Design

**Date:** 2026-06-21
**Status:** Approved (brainstorming) — ready for implementation plan

## Goal

Upgrade the Automation editor's breakpoint curves from piecewise-**linear** to cubic **Bézier**
with draggable tangent handles. Each breakpoint gets in/out tangent handles that shape the
segments on either side; handles stay colinear (smooth) by default and can be broken for a sharp
asymmetric corner. The new curvature is sampled by both automation samplers and persisted in
projects + node state, all backward-compatible with existing linear curves.

## Architecture

The upgrade is deliberately **localized**. Both samplers — the stream `AutomationNode` and the UI
`AutomationStore` — sample through `AutoCurve::sample()`, and all persistence goes through
`encodeCurve`/`decodeCurve`. Teaching *those* about Bézier means stream channels, UI channels,
project files (`ProjectFile`), and `AutomationNode::saveState` all gain curves for free. Only two
files carry real new logic: the GL-free `core/AutoCurve.h` (data model + sampling math + codec)
and `ui/AutomationPanel.cpp` (editing). No changes to the nodes, the store, or `ProjectFile`.

Backward compatibility is a first-class constraint: a curve with all handles retracted samples,
draws, and serializes identically to today's linear curve (verified algebraically below), and old
`.oss` projects / saved node state load unchanged.

### Unit 1 — `AutoPoint` data model + Bézier sampling (`core/AutoCurve.h`)

`AutoPoint` gains two tangent handles, stored as **(bar, value) offsets** from the point, plus a
break flag:

```cpp
struct AutoPoint {
    float bar = 0.0f, value = 0.0f;
    float outDBar = 0.0f, outDValue = 0.0f;   // handle toward the next point (shapes segment to the right)
    float inDBar  = 0.0f, inDValue  = 0.0f;   // handle toward the previous point (shapes segment to the left)
    bool  broken  = false;                    // false = in/out kept colinear (aligned) by the editor
};
```

All-zero handles ⇒ a retracted, linear segment. Old `AutoPoint`s and old projects deserialize
with zeros, so everything currently linear stays bit-for-bit linear.

**Segment math.** A segment between point `a` (left) and `b` (right) is the cubic Bézier with
control points (in (bar, value) space):

```
B0 = (a.bar, a.value)
B1 = (a.bar + a.outDBar, a.value + a.outDValue)
B2 = (b.bar + b.inDBar,  b.value + b.inDValue)
B3 = (b.bar, b.value)
B(t) = (1-t)^3 B0 + 3(1-t)^2 t B1 + 3(1-t) t^2 B2 + t^3 B3,   t in [0,1]
```

To keep `value` a true single-valued function of `bar` (no time-travel), `sample()` first **clamps
the control x's monotonic** so `B0.bar ≤ B1.bar ≤ B2.bar ≤ B3.bar`:

```
B1.bar = clamp(a.bar + a.outDBar, a.bar, b.bar)
B2.bar = clamp(b.bar + b.inDBar,  B1.bar, b.bar)
```

A cubic Bézier whose x control points are non-decreasing has a non-decreasing `x(t)` (its
derivative is a quadratic Bézier with non-negative coefficients), so `x(t)` is invertible. To
sample at a query `bar`, find `t` by **bisection on `x(t) = bar`** (fixed iteration count, ~24 →
~1e-6 in t), then return `y(t)`. Cost is a handful of cubic evaluations per channel per frame —
negligible for the <~20 channels in play.

**Linear fast-path + exact compatibility.** When both facing handles of a segment are zero
(`a.out* == 0 && b.in* == 0`), the segment is returned by direct linear interpolation. This is not
just an optimization: with retracted handles the Bézier reduces to `x(t) ≡ y(t)` (controls are
`[P0,P0,P1,P1]` on each axis), so `value` is *exactly* linear in `bar` — the fast-path keeps the
result bit-identical to the current `sample()` and the existing `test_auto_curve` assertions.

**Endpoints** are unchanged: hold the first value for `bar ≤ points.front().bar` and the last for
`bar ≥ points.back().bar`. Zero/degenerate spans are guarded (no NaN/Inf).

**Shared clamp helper.** The monotonic-x clamp + control-point construction live in one free
function (e.g. `bezierControls(a, b)` → the four (bar,value) control points) used by both
`sample()` and the editor's drawing, so the drawn curve always matches the sampled curve
("what you see is what you get"). The single-segment evaluation is a free function
(e.g. `bezierSampleSegment(a, b, bar)`) so it is unit-testable in isolation.

### Unit 2 — Versioned, backward-compatible codec (`core/AutoCurve.h`)

`encodeCurve` emits a `b1;`-prefixed form with **7 numbers per point**, comma-joined:
`b1;bar,value,outDBar,outDValue,inDBar,inDValue,broken,...` (`broken` as `0`/`1`). The empty
curve still encodes to `""`.

`decodeCurve` **sniffs the prefix**:
- starts with `b1;` → parse 7 numbers per point into the new fields;
- empty string → empty curve;
- otherwise → the existing **legacy 2-float parser** (`bar,value,...`), filling handles with zeros.

This keeps existing `.oss` projects and previously-saved `AutomationNode` state loading correctly,
and new saves use the `b1;` form. The encoding contains no spaces, `:`, or `|`, so the
`ProjectFile` whitespace-delimited line format and the `AutomationNode::saveState` `|`/`:`-delimited
block format are unaffected (they already assume `encodeCurve` produces none of those).

### Unit 3 — Editor curve drawing (`ui/AutomationPanel.cpp`)

Replace the straight `AddLine` between consecutive breakpoints with ImGui's `AddBezierCubic`, fed
the four **clamped** control points (from the shared helper) converted to screen space. Linear
(retracted) segments degenerate to the straight lines drawn today. The leading/trailing flat holds
and the breakpoint circles are unchanged. This unit is drawing only — no editing of handles yet —
so the visual can be verified before interaction is added.

### Unit 4 — Editor selection + handle editing (`ui/AutomationPanel.cpp`)

- **Selection state.** Add `long selLane_ = -1; int selPoint_ = -1;` to the panel (stable lane key
  + index, mirroring the existing `dragLane_`/`dragPoint_`). Left-click on a point selects it (and
  begins a point drag, as today); left-click on empty lane adds a point and selects it. Selection
  clears when clicking empty space or a different lane, and is fixed up when a point is deleted
  (mirroring the existing `dragPoint_` adjustment) or when its lane no longer exists.
- **Handle display.** Only the selected point shows its two handles: a thin line from the point to
  each handle tip plus a small square at the tip. A retracted (zero-length) handle is drawn as a
  short grabbable **stub** (a fixed pixel length along the time axis) so it can always be pulled
  out; the stored offset stays zero (curve stays linear) until the user drags it.
- **Handle drag.** Add a drag-target discriminator (point / out-handle / in-handle). Dragging a
  handle tip sets that handle's `(bar, value)` offset = `(cursorBar − point.bar,
  cursorValue − point.value)`, clamped so the handle's x stays within its segment (time stays
  monotonic). **Alignment is governed by `broken`:** while `broken` is false, dragging one handle
  rotates the other to stay colinear, preserving the other handle's length; once `broken` is true,
  each handle moves independently. **Alt-drag sets `broken = true`** (and moves only the dragged
  handle) — break is sticky. **Right-click a handle tip** retracts it to zero. Handle-grab takes
  priority over point-grab / point-add when the selected point's handle tip is under the cursor.
- Point move (with neighbor-bar clamping), right-click-delete, the `clr` button, and the `x`
  channel-delete are all unchanged; `clr` also clears selection on that lane.

### Unit 5 — Documentation

- **README.md** Automation note: segments are cubic Bézier — select a breakpoint to reveal its
  tangent handles, drag to shape the curve, Alt-drag to break the tangent.
- **CLAUDE.md** Automation/`AutoCurve` bullet: the breakpoint curve is cubic Bézier with per-point
  tangent handles (aligned/breakable), sampled as a monotonic-time function (clamped controls +
  bisection, linear fast-path for retracted handles); the codec is versioned (`b1;`) and
  backward-compatible; only `AutoCurve` + the Automation panel changed (samplers/store/ProjectFile
  inherit it).

## Data flow

```
edit handles in Automation panel ──► AutoPoint{out*, in*, broken}
        │                                   │
        │ (shared clamp helper)             ├─► AutoCurve::sample(bar) ──► AutomationNode output / AutomationStore::apply
        └─► AddBezierCubic (drawn == sampled)
                                            └─► encodeCurve "b1;…" ──► ProjectFile / AutomationNode::saveState
legacy "bar,value,…" ──► decodeCurve (no prefix) ──► zero-handle points (exactly linear)
```

## Edge cases

- **Retracted handles** → exactly linear sample + straight draw + (effectively) the legacy encoding
  shape; old projects are visually and numerically identical to before.
- **Extreme handle** (large `outDBar`) → clamped monotonic at sample/draw time, so still
  single-valued; stored value is left as the user set it (sampler is self-protecting).
- **Zero-span segment** (two points at the same bar) → guarded; returns an endpoint value, no NaN.
- **Selected point deleted / its lane removed** → selection cleared or index adjusted, never a
  dangling deref (same stable-key discipline as the existing drag code).
- **Break then re-align** → Alt-drag sets `broken`; dragging without Alt re-aligns by rotating the
  opposite handle (leaving `broken` as set — alignment is enforced live during a non-Alt drag).
- **Many points on a compact lane** → only the selected point's handles draw, so clutter is bounded
  regardless of zoom.

## Testing

- **`tests/test_auto_curve.cpp`** (extend, `core_tests`, GL-free):
  - The existing linear-curve assertions still pass bit-exact (linear fast-path).
  - A point with a pulled out-handle bends its segment the expected way (e.g. an ease makes the
    mid-bar value differ from the linear midpoint in the expected direction), endpoints exact.
  - An extreme handle still samples monotonic + single-valued across increasing bars, no NaN.
  - `encodeCurve`/`decodeCurve` round-trips handles + `broken`; a legacy `bar,value,…` string
    decodes to zero-handle points; empty round-trips to empty.
- **`tests/test_project_enablers.cpp`** (extend): a curve *with* handles survives
  `decodeCurve(encodeCurve(...))` (so project save/load carries Bézier).
- **Build + manual:** the ImGui editing (selection, handle draw, Alt-drag break, retract) is
  interactive and verified by a clean build + a manual run (and the existing save/load `gl_smoke`
  round-trip still passes, since linear curves round-trip identically). No GL is involved in the
  curve, so no new `gl_smoke` scenario.

## Out of scope (YAGNI)

- Per-segment auto-smooth / Catmull-Rom seeding on point insert (points start linear by decision).
- Numeric handle entry fields, copy/paste of handles, or curve presets.
- Changing the sampled output range, the lane layout, zoom, or any non-curve editor behavior.
- MTC-style additions or any non-automation surface.
