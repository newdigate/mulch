# Skybox Node — Design

**Date:** 2026-06-18
**Status:** Approved (brainstorming) — ready for implementation plan

## Goal

A **Skybox** 3D node: 6 texture inputs (the cube faces) rendered as a cubemap background
into a texture, viewed through a camera rotated by the shared **World Transform** (now
yaw + pitch) plus a self-spin. Wire its output into **Output**, or composite a Wireframe /
Shaded Render scene over it with **Mix** / **Compositor**.

This also extends the shared `Transform` from yaw-only to **yaw + pitch**, so the World
Transform tilts the whole 3D scene (Wireframe, Shaded Render, and the Skybox) together.

## Architecture

The cubemap is sampled **in the fragment shader from 6 separate `sampler2D`** (no
`GL_TEXTURE_CUBE_MAP` object): per pixel it builds a view ray, rotates it, picks the cube
face from the largest-magnitude axis, and samples the matching face texture. This avoids
rebuilding a cubemap object every frame from the 6 changing inputs.

### Unit 1 — shared `Transform` gains pitch (touches existing code)

- **`src/core/Value.h`**: `struct Transform { float angle = 0.0f; float pitch = 0.0f;
  bool active = false; };` — keep `angle` as the yaw (about Y) so existing readers are
  unchanged; add `pitch` (about X). Update the doc comment.
- **`src/modules/WorldTransformNode.h`**: add a `pitch` input (a settable tilt **angle**,
  radians; the `rate` stays a continuous yaw spin). Output
  `Transform{angle_, ctx.in<float>(1), true}`. (The one explicit `Transform{...}`
  construction in the codebase — update it to the 3-field form.)
- **`src/modules/WireframeNode.cpp`** and **`src/modules/ShadedRenderNode.cpp`**: apply
  pitch as well as yaw. Where they currently compute one `angle` and build
  `rotate(angle, Y)`, compute `yaw`/`pitch` (`tf.active ? tf.angle / tf.pitch : self-spin
  yaw / 0`) and build `model = rotate(yaw, {0,1,0}) * rotate(pitch, {1,0,0})`. Pitch
  defaults to 0, so existing scenes are visually unchanged.

### Unit 2 — `src/core/CubeMap.{h,cpp}` (GL-free, unit-tested)

```cpp
namespace oss {
struct CubeSample { int face; float u; float v; };   // face 0..5 = +X,-X,+Y,-Y,+Z,-Z; u,v in [0,1]
CubeSample cubeFaceUV(const glm::vec3& dir);
}
```

The standard OpenGL cube-map major-axis face selection. For direction `d`, the dominant
absolute component picks the face; `sc`/`tc`/`ma` per the GL spec, then `u = (sc/ma+1)/2`,
`v = (tc/ma+1)/2`:

| dominant | sign | face | sc | tc | ma |
|----------|------|------|----|----|----|
| x | + | 0 (+X) | −d.z | −d.y | \|d.x\| |
| x | − | 1 (−X) | +d.z | −d.y | \|d.x\| |
| y | + | 2 (+Y) | +d.x | +d.z | \|d.y\| |
| y | − | 3 (−Y) | +d.x | −d.z | \|d.y\| |
| z | + | 4 (+Z) | +d.x | −d.y | \|d.z\| |
| z | − | 5 (−Z) | −d.x | −d.y | \|d.z\| |

The shader mirrors this exactly; a `gl_smoke` scenario cross-checks the rendered output.
(Face image orientation follows the GL cube-map convention.)

### Unit 3 — `shaders/skybox.frag` + `src/modules/SkyboxNode.h`

A `ShaderNode` (renders the fullscreen pass into its FBO → `TexRef` on output 0).

**`shaders/skybox.frag`:** from `vUV`, build a 45°-FOV ray (forward −Z, up +Y, to match
the Wireframe/Shaded camera), rotate it by `uPitch` (about X) then `uYaw` (about Y),
select the face via the same logic as `cubeFaceUV`, and sample the matching `sampler2D`
(a 6-way `if`/`else` — GLSL 410 can't dynamically index a sampler array):
```glsl
uniform sampler2D uPX, uNX, uPY, uNY, uPZ, uNZ;
uniform float uYaw, uPitch, uAspect;
vec3 rotX(vec3 d, float a){ float c=cos(a),s=sin(a); return vec3(d.x, d.y*c - d.z*s, d.y*s + d.z*c); }
vec3 rotY(vec3 d, float a){ float c=cos(a),s=sin(a); return vec3(d.x*c + d.z*s, d.y, -d.x*s + d.z*c); }
// ndc = vUV*2-1; t = tan(radians(45)*0.5);
// dir = normalize(vec3(ndc.x*uAspect*t, ndc.y*t, -1)); dir = rotY(rotX(dir,uPitch), uYaw);
// pick face + uv exactly like cubeFaceUV; sample the matching uPX..uNZ.
```
Positive `uPitch` looks up (toward +Y); the centre pixel at yaw 0 / pitch 0 looks at −Z.
Because the FOV is 45°, the +Y/−Y faces appear only when you pitch toward them.

**`src/modules/SkyboxNode.h`:** `ShaderNode("Skybox", "shaders/skybox.frag")`. `setUniforms`
binds the 6 face textures to units 0–5 (`uPX…uNZ`), computes yaw/pitch
(`tf.active ? tf.angle / tf.pitch : spin_ += dt*rotation / 0`), and sets `uYaw`/`uPitch`/
`uAspect` (`= fbo width/height`). Holds a `float spin_` for the self-rotation.

### Ports

| # | Port | Type | Default | Notes |
|---|------|------|---------|-------|
| 0–5 | `+X`, `-X`, `+Y`, `-Y`, `+Z`, `-Z` | Texture | `TexRef{}` | the 6 cube faces (unconnected → black) |
| 6 | `rotation` | Float | 0.2 | self yaw-spin rate (rad/s); used when no transform |
| 7 | `transform` | Transform | `Transform{}` | the **World Transform** (yaw + pitch) |
| out 0 | `out` | Texture | — | the rendered skybox |

### Registration / CMake

- `src/app/Application.cpp`: `#include "modules/SkyboxNode.h"`; `makeNode`
  `if (type == "Skybox") return std::make_unique<SkyboxNode>();`; add `"Skybox"` to the
  **3D** `nodeCategories` list.
- `src/main.cpp`: add a `Skybox` node to the `--screenshot` demo.
- `CMakeLists.txt`: add `src/core/CubeMap.cpp` to `APP_SOURCES`, `core_tests`, and
  `gl_smoke`; add `tests/test_cube_map.cpp` to `core_tests`. (`SkyboxNode.h` is
  header-only.) `shaders/skybox.frag` is auto-copied next to the binary.

## Data flow

```
6 face textures ─┐
World Transform ─┤ (yaw + pitch, or self yaw-spin)
                 └─► skybox.frag: ray (45° FOV, -Z fwd) -> rotX(pitch) -> rotY(yaw)
                       -> cube-face select (major axis) -> sample uPX..uNZ
                 └─► FBO texture ─► out
```

## Edge cases

- **Unconnected face** (`TexRef{}`, id 0) → sampling an unbound texture yields black; that
  face shows black. Fine.
- **Transform not connected** → `tf.active == false` → self yaw-spin at `rotation` rad/s,
  pitch 0 (mirrors Wireframe/Shaded).
- **45° FOV** → the top/bottom faces are only visible when pitched toward them; this is the
  reason pitch was added. (Matching the other renderers' FOV keeps composites aligned.)
- **`Transform` field add** → the only explicit `Transform{angle, active}` construction
  (WorldTransformNode) is updated to `Transform{angle, pitch, active}`; default
  constructions (`Transform{}`) are unaffected.

## Testing

- **`tests/test_cube_map.cpp`** (`core_tests`, GL-free): `cubeFaceUV` for the 6 axis
  directions → face 0..5 with `u==v==0.5` (centre of each face); an off-axis case (e.g.
  `(1, 0.5, 0)` → face 0, `u≈0.5`, `v≈0.25`) pins the UV math; all results have
  `u,v ∈ [0,1]`.
- **`gl_smoke`** — 6 `Colour` nodes with distinct colours (face 0 red … 5 cyan) →
  `Skybox` → `Output`. Set the skybox `transform` input to a fixed `Transform{yaw, pitch,
  true}` and read back the **centre** pixel:
  - yaw 0, pitch 0 → centre looks at −Z → face 5 colour (cyan).
  - yaw π/2, pitch 0 → centre looks at −X → face 1 colour (green).
  - yaw 0, pitch π/2 → centre looks up at +Y → face 2 colour (blue).
  This proves ray-gen + rotation (both axes) + face selection + sampling end to end.
- The existing **World Transform** `gl_smoke` Scenario 14 still passes (pitch defaults to
  0 — a regression check on the `Transform` change), and `tests/test_world_transform.cpp`
  still passes.

## Docs

- **README.md** — add a **Skybox** row (6 face textures → a cubemap background texture;
  rotated by a self-`rotation` spin or the shared **World Transform** yaw+pitch; wire `out`
  into Output or composite a scene over it). Note the World Transform now carries pitch.
- **CLAUDE.md** — an Architecture bullet: `SkyboxNode` is a `ShaderNode` sampling 6 face
  textures as a cubemap in `shaders/skybox.frag` (in-shader major-axis face selection
  mirroring the GL-free `core/CubeMap.h` `cubeFaceUV`, unit-tested + gl_smoke cross-checked);
  rotated by the shared `Transform`, which now carries **yaw + pitch** (World Transform +
  Wireframe + Shaded Render all honor pitch).
</content>
