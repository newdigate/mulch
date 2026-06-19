# MIDI Pitch Graph Node — Design

**Date:** 2026-06-18
**Status:** Approved (brainstorming) — ready for implementation plan

## Goal

A node that turns incoming MIDI into a scrolling **pitch graph** (pitch vs. time) streamed
as a colored vertex buffer into the **Wireframe** renderer. Each held note is a horizontal
line segment at its pitch; the 12 pitch classes map around the rainbow (hue = note mod 12)
and the brightness is the note's velocity. The graph scrolls right-to-left in real time.

This requires a small extension to the geometry pipeline first: a **colored vertex format**
so per-vertex color can flow through the Wireframe renderer (today it is position-only and
hard-coded green).

## Architecture (two units)

### Unit 1 — colored geometry support (reusable infrastructure)

**`src/core/Value.h`** — add a third vertex format:
```cpp
enum class VertexFormat { Pos3, Pos3Normal3, Pos3Color3 };
//   Pos3Color3 - 6 floats: position + RGB colour (stride 24, colour at offset 12)
```

**`src/modules/WireframeNode.{h,cpp}`** — a second shader program for colored geometry,
selected when the incoming `VertexRef` is `Pos3Color3`. The existing fixed-green `Pos3`
path is unchanged; this mirrors how `ShadedRenderNode` already branches on
`Pos3Normal3`.
- New `program_color_` (compiled in `initGL`, deleted in the dtor):
  ```glsl
  // VS:  layout(location=0) in vec3 aPos; layout(location=1) in vec3 aColor;
  //      uniform mat4 uMVP; out vec3 vColor;
  //      void main(){ vColor=aColor; gl_Position=uMVP*vec4(aPos,1.0); }
  // FS:  in vec3 vColor; out vec4 FragColor; void main(){ FragColor=vec4(vColor,1.0); }
  ```
- In `evaluate`, when `geo.format == VertexFormat::Pos3Color3`: use `program_color_`, set
  its `uMVP`, bind the VBO with stride 24, attrib 0 = pos (offset 0), attrib 1 = colour
  (offset 12), **enable** attrib 1, draw with the mapped primitive. Otherwise the existing
  `Pos3` path (stride 12, attrib 0 only) — and **disable** attrib 1 there so a stale color
  attribute from a previous colored frame can't bleed in (the node has one VAO).

### Unit 2 — the pitch graph

**`src/core/PitchGraph.{h,cpp}` (GL-free, unit-tested)** — the geometry/color math.

```cpp
namespace oss {

// HSV (each in [0,1], hue wraps) -> RGB in [0,1].
glm::vec3 hsvToRgb(float h, float s, float v);

// Rolling MIDI note history -> a scrolling pitch-vs-time line graph.
class PitchGraph {
public:
    // Advance the local clock by dt, ingest this frame's MIDI (note-on opens a record,
    // note-off closes the matching open record), and prune records scrolled off the left.
    void ingest(const MidiRef& midi, float dt, float window);

    // Emit the line-segment vertices as flat Pos3Color3 floats (x,y,z,r,g,b per vertex,
    // 2 vertices per note segment, Primitive::Lines). Returns the vertex count
    // (out.size() == count*6).
    int build(float window, std::vector<float>& out) const;

    int activeCount() const;   // records currently open (held), for the status line

private:
    struct Note { int note; int vel; double startT; double endT; };  // endT < 0 while held
    std::vector<Note> notes_;
    double clock_ = 0.0;
};

} // namespace oss
```

**`ingest(midi, dt, window)`:**
- `clock_ += dt`.
- For each event: **note-on** (`midiIsNoteOn`) → if an open record exists for that note,
  close it (`endT = clock_`) first (retrigger); push `{note, vel=data2, startT=clock_,
  endT=-1}`. **note-off** (`midiIsNoteOff`) → close the most recent open record for that
  note (`endT = clock_`).
- Prune: drop closed records whose `endT < clock_ - window` (scrolled off the left). Held
  records (`endT < 0`) are never pruned. Cap `notes_` at 512 (drop oldest) as a safety
  bound against missed note-offs.

**`build(window, out)`** — `now = clock_`; for each record:
- `s = startT`, `e = (endT < 0) ? now : endT`. Skip if `e < now - window`.
- `xs = clamp(2*(s-now)/window + 1, -1, 1)`, `xe = clamp(2*(e-now)/window + 1, -1, 1)`
  (newest at `x=+1`, scrolling left; a held note's right end stays pinned at `+1`).
- `y = clamp(2*(note - kLoNote)/(kHiNote - kLoNote) - 1, -1, 1)` with `kLoNote = 24`,
  `kHiNote = 96` (C1–C7, log-frequency / by-note axis; notes outside clamp to the edges).
- `col = hsvToRgb((note mod 12)/12.0, 1.0, 0.25 + 0.75*(vel/127.0))` (hue = pitch class;
  saturation 1; brightness = velocity with a 0.25 floor so soft notes stay visible).
- Push two vertices `(xs,y,0, col)` and `(xe,y,0, col)`.

**`src/modules/PitchGraphNode.h` (header-only node)** — owns a `PitchGraph` + a VBO.
`evaluate`: `pg_.ingest(ctx.in<MidiRef>(0), ctx.dt, window)`; `int n = pg_.build(window,
verts_)`; upload `verts_` (`GL_DYNAMIC_DRAW`); publish `VertexRef{vbo_, n,
Primitive::Lines, VertexFormat::Pos3Color3}` on output 0. `statusLine()` → e.g.
`"N held"`. `window = clamp(ctx.in<float>(1), 1, 30)`.

### Ports

| # | Port | Type | Default | Notes |
|---|------|------|---------|-------|
| 0 | `midi` | Midi | `MidiRef{}` | note-ons/offs to plot |
| 1 | `window` | Float | 8.0 | seconds of history shown (1–30) |
| out 0 | `geometry` | Vertex | — | line segments → **Wireframe** |

### Registration / CMake

- `src/app/Application.cpp`: `#include "modules/PitchGraphNode.h"`; `makeNode`
  `if (type == "Pitch Graph") return std::make_unique<PitchGraphNode>();`; add
  `"Pitch Graph"` to the **MIDI** `nodeCategories` list.
- `src/main.cpp`: add a `Pitch Graph` node to the `--screenshot` demo.
- `CMakeLists.txt`: add `src/core/PitchGraph.cpp` to `APP_SOURCES`, `core_tests`, and
  `gl_smoke`; add `tests/test_pitch_graph.cpp` to `core_tests`. (`PitchGraphNode.h` is
  header-only.) `WireframeNode.cpp` is already in all three targets.

## Data flow

```
MIDI ─► PitchGraph.ingest (note history + local dt clock, prune off-screen)
        └─► build(window): per held/recent note a colored line segment
              x = time (newest at +1, scrolling left),  y = note (log-freq),
              colour = hsv(pitchClass hue, 1, velocity brightness)
        └─► verts_ (Pos3Color3) ─► VBO ─► VertexRef{Lines, Pos3Color3}
              └─► Wireframe (colored path) ─► texture
```

## Edge cases

- **No MIDI connected** (`MidiRef{}`, count 0) → nothing ingested; the graph scrolls
  empty (count 0). No internal demo generator (YAGNI).
- **Retrigger** (note-on for an already-held note) → the prior record is closed at the
  current time and a new one opened, so the old segment ends cleanly.
- **Missed note-off** (held forever) → never pruned, but the 512-record cap drops the
  oldest so memory stays bounded; its segment simply spans the whole width.
- **Notes outside 24–96** → clamped to the top/bottom edge (still drawn).
- **`window`** clamped to `[1,30]`; a held note longer than the window has `xs` clamped
  to `-1`.
- **Wireframe `spin`** rotates the graph (its default is 0.5 rad/s); set it to 0 for a
  static, face-on pitch graph (documented).

## Testing

- **`tests/test_pitch_graph.cpp`** (`core_tests`, GL-free):
  - `hsvToRgb`: hue 0 → red `(1,0,0)`, 1/3 → green, 2/3 → blue; hue wraps (`hsvToRgb(1,…)
    == hsvToRgb(0,…)`).
  - **Pitch-class hue:** ingest notes 60 and 72 (both pitch class 0) held, build, and
    check their segment colors are equal; notes 60 vs 61 differ.
  - **Segment geometry:** note-on(note 60, vel 100) then advance the clock then note-off;
    build yields one segment (count 2) at `y == 0` (note 60 is the range centre), color
    hue = pitch class 0 (red-ish), brightness ≈ `0.25 + 0.75*100/127`.
  - **Held note extends to now:** note-on, advance clock, build (no note-off) → the
    segment's right `x ≈ +1`.
  - **Scrolling / prune:** after advancing the clock past `window`, a closed note is
    pruned (count 0) and `activeCount()` reflects held notes only.
  - **Layout:** `out.size() == count*6`.
- **`gl_smoke`** — feed note-ons across distinct pitch classes (e.g. 60=red, 64, 67) into
  a `PitchGraph` → `Wireframe` → `Output`; evaluate a few frames; read back the texture
  and assert it contains line pixels of at least two clearly different hues that are
  **not** the renderer's default green — proving per-vertex color flows through the
  extended Wireframe colored path.

## Docs

- **README.md** — add a **Pitch Graph** row to the module table (MIDI → a scrolling
  pitch-vs-time graph as colored geometry: pitch-class rainbow hue, velocity brightness,
  `window` seconds; wire `geometry` into **Wireframe**, set its `spin` to 0 for a static
  view).
- **CLAUDE.md** — an Architecture bullet: `PitchGraphNode` wraps the GL-free
  `core/PitchGraph` (rolling MIDI note history → colored line segments; pitch-class rainbow
  hue + velocity brightness via `hsvToRgb`) and publishes a `Pos3Color3` `VertexRef`; plus
  note the new `VertexFormat::Pos3Color3` and the Wireframe colored draw path it added.
</content>
