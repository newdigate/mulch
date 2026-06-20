# Shader Edge + Vertex Deform Node — Design

**Date:** 2026-06-20
**Status:** Approved (brainstorming) — ready for implementation plan

## Goal

A small **shader sub-system**: a new `Shader` edge type that carries a GLSL vertex shader,
a new **Shader** node category, a **Vertex Shader** node that emits a preset vertex shader,
and a **Deform** node that runs that shader over an input vertex buffer via GPU transform
feedback — outputting the transformed vertices as a new colored vertex buffer to wire into
a renderer (Wireframe / Shaded Render).

## Architecture

Transform feedback is the OpenGL mechanism for running a vertex shader and capturing the
transformed vertices into a buffer (no rasterization). The Deform node compiles the
incoming shader as a transform-feedback program (capturing `vPosition`/`vColor`), draws the
input VBO as points with rasterizer-discard, and captures into its own output VBO.

### Unit 1 — the `Shader` value type (`src/core/Value.h`, GL-free)

- `enum class PortType { …, Transform, Shader };` — add `Shader`.
- `struct ShaderRef { std::string vertexSrc; };` — a GLSL vertex-shader source (a string,
  so `core/` stays GL-free). Add it to the `Value` variant (after `Transform`).
- `typeOf`: the catch-all `else` currently returns `Transform`; add an **explicit**
  `Transform` branch and make `ShaderRef` the new `else` (→ `PortType::Shader`).
- `portTypeName`: add `case PortType::Shader: return "Shader";`.
- No `Graph` change — `connect` already rejects mismatched `PortType`, so `Shader→Shader`
  joins and e.g. `Float→Shader` is rejected. A `Shader` *input* shows no inline editor (it
  falls through the same `default` as Texture/Vertex in `PortWidgets` — set only by a wire).

### Unit 2 — `src/core/VertexShaders.h` (GL-free, unit-tested)

```cpp
inline const std::vector<std::string>& vertexShaderLabels();   // {"Identity","Twist","Wave","Bulge"}
inline std::string vertexShaderSource(int preset);             // full GLSL VS (clamped index)
```

Each preset is a **complete** GLSL 410 vertex shader following one convention so the Deform
node can compile any of them identically:

```glsl
#version 410 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aColor;
uniform float uPos;       // the Deform node's `position` scalar
uniform vec4  uColour;    // the Deform node's `colour`
out vec3 vPosition;       // captured by transform feedback
out vec3 vColor;
void main() {
    <preset transform of aPos -> vPosition>
    vColor = aColor + uColour.rgb;            // colourless input (aColor=0) -> tinted by uColour
    gl_Position = vec4(vPosition, 1.0);       // unused under rasterizer discard; harmless
}
```

Preset transforms:
- **Identity:** `vPosition = aPos;`
- **Twist:** rotate about Y by `uPos*aPos.y` — `float a=uPos*aPos.y, c=cos(a), s=sin(a); vPosition = vec3(aPos.x*c - aPos.z*s, aPos.y, aPos.x*s + aPos.z*c);`
- **Wave:** `vPosition = aPos + vec3(0.0, uPos*sin(aPos.x*6.2831853), 0.0);`
- **Bulge:** `vPosition = aPos * (1.0 + uPos*length(aPos));`

### Unit 3 — `gfx/GLUtil` transform-feedback program helper (GL)

```cpp
GLuint linkFeedbackProgram(const std::string& vertSrc, const std::vector<const char*>& varyings);
```
Compiles `vertSrc` as a vertex shader + a trivial fragment shader (`void main(){}`), calls
`glTransformFeedbackVaryings(prog, n, varyings, GL_INTERLEAVED_ATTRIBS)` **before** linking,
links, and returns the program (0 on compile/link failure, logged). Added to
`src/gfx/GLUtil.{h,cpp}` (already linked into the app + `gl_smoke`).

### Unit 4 — `src/modules/VertexShaderNode.h` (Shader category, GL-free, header-only)

A `preset` choice input (`vertexShaderLabels()`); output `shader` (Shader). `evaluate`:
`ctx.out<ShaderRef>(0, ShaderRef{ vertexShaderSource(clamp(round(in<float>(0)), 0, n-1)) })`.
`statusLine()` shows the preset name. (No GL — it just emits a source string.)

### Unit 5 — `src/modules/DeformNode.h` (Shader category, header-only, GL)

Inputs: 0 `geometry` (Vertex, `VertexRef{}`), 1 `position` (Float, 0.5, −2…2), 2 `colour`
(Colour, white), 3 `shader` (Shader, `ShaderRef{}`). Output 0 `geometry` (Vertex). Owns a
cached program + VAO + output VBO.

`evaluate`:
- If `in<ShaderRef>(3).vertexSrc != cachedSrc_`: delete the old program, recompile via
  `linkFeedbackProgram(src, {"vPosition","vColor"})` (or 0 if the source is empty), cache the
  source, set `status_` (preset ok / "compile error" / "no shader").
- If `program_ == 0` or the input VBO is empty → **pass the input `VertexRef` through**
  unchanged.
- Else: grow the output VBO to `count*6` floats if needed; bind the input VBO with `aPos`@0
  (stride 12 for Pos3, else 24) and, only when the input is `Pos3Color3`, `aColor`@1 (else
  `glDisableVertexAttribArray(1)` **and** `glVertexAttrib4f(1, 0,0,0,1)` to pin the generic
  attribute — it's context state, not VAO state, so `aColor` reads a deterministic 0); set
  `uPos`/`uColour`;
  `glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, outVbo_)`; `glEnable(GL_RASTERIZER_DISCARD)`;
  `glBeginTransformFeedback(GL_POINTS)`; `glDrawArrays(GL_POINTS, 0, count)`;
  `glEndTransformFeedback()`; `glDisable(GL_RASTERIZER_DISCARD)`. Publish
  `VertexRef{outVbo_, count, in.primitive, VertexFormat::Pos3Color3}` (topology preserved;
  the 1:1 point transform keeps vertex order, so the original primitive still draws correctly).
- `statusLine()` reports the preset / error.

### New "Shader" category + registration

- `src/app/Application.cpp`: `#include` both node headers; `makeNode`:
  `if (type == "Vertex Shader") return std::make_unique<VertexShaderNode>();` and
  `if (type == "Deform") return std::make_unique<DeformNode>();`; add a new category
  `{ "Shader", { "Vertex Shader", "Deform" } }` to `nodeCategories()`.
- `src/main.cpp`: add a `Vertex Shader` and a `Deform` node to the `--screenshot` demo.
- `CMakeLists.txt`: add `tests/test_vertex_shaders.cpp` to `core_tests`. (`VertexShaders.h`
  and both node headers are header-only; `GLUtil.cpp` is already linked everywhere — only the
  new function is added.)

## Ports

| Node | # | Port | Type | Default |
|------|---|------|------|---------|
| **Vertex Shader** | 0 | `preset` | Float (choice) | 0 (Identity) |
| | out 0 | `shader` | Shader | — |
| **Deform** | 0 | `geometry` | Vertex | `VertexRef{}` |
| | 1 | `position` | Float | 0.5 (−2…2) |
| | 2 | `colour` | Colour | white |
| | 3 | `shader` | Shader | `ShaderRef{}` |
| | out 0 | `geometry` | Vertex (Pos3Color3) | — |

## Data flow

```
Vertex Shader (preset -> ShaderRef) ──┐
geometry (Pos3 / Pos3Color3) ─────────┤
position (float), colour (vec4) ──────┴─► Deform
        compile feedback program(vertexSrc) [cached by source]
        draw input as GL_POINTS, rasterizer-discard,
        capture vPosition+vColor -> output VBO (Pos3Color3)
   └─► VertexRef{outVbo, count, in.primitive, Pos3Color3} ─► Wireframe / Shaded Render
```

## Edge cases

- **No shader connected** (`ShaderRef{}` empty source) → program 0 → pass the input through.
- **Compile failure** → program 0 → pass-through + `statusLine()` "compile error".
- **Pos3 input (no colour)** → `aColor`@1 disabled → reads `(0,0,0)`; presets do
  `vColor = aColor + uColour.rgb`, so the colour comes from the `colour` input.
- **Pos3Normal3 input** → treated as position-only (stride 24, `aPos`@0; normal ignored,
  `aColor` disabled).
- **Output VBO sizing** → grown to `count*6` floats on demand (`GL_DYNAMIC_COPY`).
- **Topology** → output primitive = input primitive; the transform is per-vertex 1:1, so a
  line strip / triangle list redraws correctly.
- **`PortType` exhaustiveness** → `typeOf` gets an explicit `ShaderRef`/`Transform` split;
  `portTypeName` gets a `Shader` case (the build flags any other exhaustive switch).

## Testing

- **`tests/test_vertex_shaders.cpp`** (`core_tests`, GL-free):
  - `vertexShaderLabels().size() == 4`; each `vertexShaderSource(i)` is non-empty and
    contains `vPosition`, `vColor`, `uPos`, `uColour`, `void main` (the compile-convention).
  - **Shader type:** `typeOf(Value{ShaderRef{}}) == PortType::Shader`;
    `portTypeName(PortType::Shader)` is `"Shader"`.
  - **VertexShaderNode** (GL-free): drive it with `preset = 1` and assert its output
    `ShaderRef.vertexSrc == vertexShaderSource(1)`.
- **`gl_smoke`**: build a `DeformNode`, set `geometry` to a hand-built 1-vertex input VBO
  (Pos3, position `(0.2, 0.3, 0.4)`), `position`/`colour` defaults overridden to known values,
  and `shader` to `ShaderRef{ vertexShaderSource(Identity) }`; evaluate, then
  `glGetBufferSubData` the 6-float output and assert it equals the input position +
  `colour.rgb`. Repeat with a displacing preset (Wave) and assert `y` shifted by
  `position*sin(x*2π)`. Also build `VertexShaderNode → Deform` and confirm the `Shader` edge
  connects (proving the new edge type wires end to end).

## Docs

- **README.md** — a **Shader** section / rows: **Vertex Shader** (pick a preset transform →
  a shader edge) and **Deform** (run the shader over a vertex buffer via transform feedback →
  a colored vertex buffer; `position` + `colour` drive it; wire into Wireframe).
- **CLAUDE.md** — an Architecture bullet on the new `Shader` edge (`ShaderRef` carries GLSL
  vertex source) and the Deform node (transform-feedback applier; presets in the GL-free
  `core/VertexShaders.h`; `gfx/GLUtil` `linkFeedbackProgram`), plus the new Shader category.
</content>
