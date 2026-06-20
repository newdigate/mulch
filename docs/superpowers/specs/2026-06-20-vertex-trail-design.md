# Vertex Trail Node — Design

**Date:** 2026-06-20
**Status:** Approved (brainstorming) — ready for implementation plan

## Goal

A **Vertex Trail** node: each frame it snapshots the incoming vertex buffer and keeps a
queue of the last *N* snapshots. The combined output is one colored vertex buffer where the
snapshot of age *k* (0 = newest) is offset by *k × z-spacing* in Z and has its hue rotated by
*k × hue-rate* — a trail of the geometry receding in Z and drifting through the colour wheel
over time. Output is a `Pos3Color3` `VertexRef` → wire into **Wireframe**.

## Architecture

The queue + recolour math is GL-free and unit-tested (`core/VertexTrail`), mirroring how
`PitchGraph`/`Oscilloscope` keep their logic GL-free with a thin node owning the VBO. The
node reads the input VBO back to CPU (`glGetBufferSubData`) each frame, pushes the snapshot,
rebuilds the combined buffer, and uploads it.

### Unit 1 — `core/ColorHsv.h` (new, GL-free, header-only)

Extract the existing `hsvToRgb` here (move it out of `PitchGraph.{h,cpp}`) and add `rgbToHsv`
(needed to rotate an existing colour's hue). Both `inline`, components in `[0,1]`, hue wraps.

```cpp
namespace oss {
glm::vec3 hsvToRgb(float h, float s, float v);   // moved verbatim from PitchGraph.cpp
glm::vec3 rgbToHsv(float r, float g, float b);   // new: max/min/chroma -> (h,s,v)
}
```
- `PitchGraph.h` includes `core/ColorHsv.h` and drops its own `hsvToRgb` declaration.
- `PitchGraph.cpp` drops the `hsvToRgb` definition (now inline in the header), keeps using it.
- `tests/test_pitch_graph.cpp` is unaffected (still gets `hsvToRgb` via `PitchGraph.h`).

### Unit 2 — `core/VertexTrail.{h,cpp}` (new, GL-free, unit-tested)

A FIFO queue of geometry snapshots, each normalised to flat `Pos3Color3` floats.

```cpp
class VertexTrail {
public:
    // Capture a snapshot. `verts` is `count` vertices in `fmt` layout (Pos3 = 3 floats,
    // Pos3Normal3 / Pos3Color3 = 6 floats). Normalises to pos+colour: a Pos3Color3 input
    // keeps its colour; a colourless input (Pos3 / Pos3Normal3) gets a base of red
    // (hue 0, full sat/value) so it still rainbows by age. Newest goes to the front.
    void push(const float* verts, int count, VertexFormat fmt);
    void setMaxFrames(int n);          // clamp to >=1 and prune the oldest beyond n
    // Concatenate every snapshot as flat Pos3Color3 floats. For age k (0 = newest):
    //   pos.z += k * zSpacing;  colour hue rotated by k * hueRate (rgbToHsv -> +h -> hsvToRgb).
    // Returns total vertex count (out.size() == count*6).
    int build(float zSpacing, float hueRate, std::vector<float>& out) const;
    int frameCount() const;            // snapshots held
    Primitive primitive() const;       // the newest snapshot's primitive (LineStrip if empty)
private:
    struct Snapshot { std::vector<float> pc; int count; Primitive prim; };  // pc = Pos3Color3 flat
    std::deque<Snapshot> snaps_;       // front = newest (age 0)
    int maxFrames_ = 16;
};
```

Hue rotation of a snapshot whose stored colour is the red base `(1,0,0)` gives a clean
rainbow by age; for a real per-vertex colour it rotates that colour's hue (greys, `s≈0`,
are unaffected — fine).

### Unit 3 — `src/modules/VertexTrailNode.h` (new, GL, header-only)

Owns a `VertexTrail`, a readback buffer, a build buffer, and the output VBO.

| # | Port | Type | Default | Range |
|---|------|------|---------|-------|
| in 0 | `geometry` | Vertex | `VertexRef{}` | Pos3 / Pos3Normal3 / Pos3Color3 |
| in 1 | `max frames` | Float | 16 | 1…240 |
| in 2 | `z spacing` | Float | 0.15 | −2…2 |
| in 3 | `hue rate` | Float | 0.03 | −1…1 |
| out 0 | `geometry` | Vertex (**Pos3Color3**) | — | — |

`evaluate`:
- `setMaxFrames(clamp(round(in<float>(1)), 1, 240))`.
- If `in.vbo != 0 && in.count > 0`: read it back — `floatsPerVert = (format == Pos3) ? 3 : 6`;
  `glBindBuffer(GL_ARRAY_BUFFER, in.vbo)`; `glGetBufferSubData(0, count*floatsPerVert*4, readback)`;
  `trail_.push(readback, count, format)`. (One snapshot per frame the input has geometry; an
  empty/unconnected input just holds the current queue — a history of the last N geometry frames.)
- `int total = trail_.build(in<float>(2), in<float>(3), built_)`; upload `built_` to `outVbo_`
  (`GL_DYNAMIC_DRAW`); emit `VertexRef{outVbo_, total, trail_.primitive(), Pos3Color3}`.
- `initGL` creates `outVbo_`; the destructor frees it. `statusLine()` = `"frames/max"`.

### Registration / CMake

- `src/app/Application.cpp`: include `VertexTrailNode.h`; `makeNode` → `"Vertex Trail"`; add
  `"Vertex Trail"` to the **3D** `nodeCategories` list.
- `src/main.cpp`: add a `Vertex Trail` node to the `--screenshot` demo.
- `CMakeLists.txt`: add `src/core/VertexTrail.cpp` to `APP_SOURCES`, `core_tests`, and
  `gl_smoke` (like `src/core/Oscilloscope.cpp`); add `tests/test_color_hsv.cpp` +
  `tests/test_vertex_trail.cpp` to `core_tests`. `ColorHsv.h` and `VertexTrailNode.h` are
  header-only.

## Data flow

```
geometry (Pos3 / Pos3Normal3 / Pos3Color3)
  └─► Vertex Trail: read back input VBO -> push snapshot (front)
        queue of up to `max frames` snapshots; build:
          age k (0=newest): pos.z += k*zSpacing; hue += k*hueRate
        upload combined Pos3Color3 -> VertexRef{outVbo, total, in.primitive, Pos3Color3}
  └─► Wireframe (colored path)
```

## Edge cases

- **Unconnected / empty input** (`vbo==0` or `count==0`) → no push; the queue holds; output is
  the current trail (or count 0 if never fed). `glBufferData` of an empty build is a no-op draw.
- **Varying input vertex count** between frames → each snapshot stores its own count; the
  output concatenates them (total = sum of snapshot counts).
- **Primitive** → output carries the newest snapshot's primitive (the input's). `Lines` /
  `Triangles` concatenate cleanly; a `LineStrip` input shares one strip, so a faint segment
  connects consecutive copies (wire `Lines` geometry to avoid it).
- **`Pos3Normal3` input** → normals are dropped (output is `Pos3Color3`); positions trail,
  coloured from the red base. (A lit trail is out of scope.)
- **Readback cost** → `glGetBufferSubData` is a per-frame GPU sync; suits modest vertex
  counts (the typical scopes/graphs/meshes here), not huge meshes × hundreds of frames.
- **max frames lowered at runtime** → `setMaxFrames` prunes the oldest snapshots immediately.

## Testing

- **`tests/test_color_hsv.cpp`** (`core_tests`): `hsvToRgb(0,1,1)≈(1,0,0)`, `hsvToRgb(1/3,1,1)≈(0,1,0)`;
  `rgbToHsv(1,0,0)≈(0,1,1)`; an rgb→hsv→rgb roundtrip on several colours is ≈ identity;
  a grey `(0.5,0.5,0.5)` → `s≈0`.
- **`tests/test_vertex_trail.cpp`** (`core_tests`, GL-free):
  - push one `Pos3` vertex `(1,2,3)`; `build(0.5, 0.1, out)` → `{1,2,3, 1,0,0}` (red), count 1.
  - push a second; age 0 (newest) keeps z and its base colour; age 1 has `z += 0.5` and colour
    `hsvToRgb(0.1, 1, 1)`.
  - `setMaxFrames(1)` prunes to the newest only; `frameCount()` reflects it.
  - a `Pos3Color3` push (colour green) is preserved at age 0 and hue-rotated at age 1.
- **`gl_smoke`**: build a `VertexTrailNode`, feed a 1-vertex `Pos3` input VBO `(0.1,0.2,0.3)`
  through 3 evaluates (max frames 8, z spacing 0.5, hue rate 0.1), read the output VBO back with
  `glGetBufferSubData`, and assert 3 vertices with z = 0.3 / 0.8 / 1.3 and colours
  red / `hsvToRgb(0.1,1,1)` / `hsvToRgb(0.2,1,1)`. Verifies readback + queue + z-offset + hue.

## Docs

- **README.md** — a **Vertex Trail** row: snapshots a vertex buffer each frame into a trail
  (queue of `max frames`), each copy offset in Z (`z spacing`) and hue-rotated (`hue rate`) by
  age → a colored vertex buffer; wire into Wireframe.
- **CLAUDE.md** — an Architecture bullet: `VertexTrailNode` keeps a GL-free `core/VertexTrail`
  queue of snapshots (read back from the input VBO each frame), emitting a combined `Pos3Color3`
  buffer with per-age Z-offset + hue rotation; the HSV helpers moved to GL-free `core/ColorHsv.h`
  (`hsvToRgb` + new `rgbToHsv`), now shared with `PitchGraph`. Unit-tested in `core_tests`;
  `gl_smoke`-verified by reading the output buffer back.
</content>
