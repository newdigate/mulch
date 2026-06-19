# Image Compositor Node — Design

**Date:** 2026-06-18
**Status:** Approved (brainstorming) — ready for implementation plan

## Goal

A **Compositor** node that blends two input textures with a selectable blend operator
(23 modes: arithmetic, Photoshop-standard separable, HSL non-separable, and bitwise),
plus an `opacity` amount. It mirrors the existing **Mix** node but with a `mode` dropdown.

## Architecture (three units)

The blend math lives in a GL-free reference (`core/BlendModes.h`) that is unit-tested and
defines the canonical mode order; the fragment shader mirrors it by the same ordering; a
`gl_smoke` scenario cross-checks that the GLSL output matches the C++ reference so the two
can't silently drift.

### `src/core/BlendModes.h` — labels + reference math (GL-free, header-only)

```cpp
namespace oss {

// Ordered blend-mode labels for the Compositor's `mode` dropdown. The index is the mode
// id used by both blendPixel() and the shader's switch(uMode) -- keep all three in sync.
inline const std::vector<std::string>& blendModeLabels();
//  0 Normal      1 Add        2 Subtract    3 Difference  4 Exclusion
//  5 Multiply    6 Screen     7 Overlay     8 Darken      9 Lighten
// 10 Color Dodge 11 Color Burn 12 Hard Light 13 Soft Light 14 Divide
// 15 Average     16 Hue       17 Saturation 18 Color      19 Luminosity
// 20 AND         21 OR        22 XOR

// Reference blend of two RGB colours in [0,1] under mode `id` (clamped to a valid id).
// This is the source of truth the shader mirrors; the result is clamped to [0,1].
glm::vec3 blendPixel(int id, const glm::vec3& base, const glm::vec3& blend);

} // namespace oss
```

**Per-channel (separable) formulas** — base `a`, blend `b`, each channel in `[0,1]`,
result clamped to `[0,1]`:

| id | name | formula |
|----|------|---------|
| 0 | Normal | `b` |
| 1 | Add | `a + b` |
| 2 | Subtract | `a - b` |
| 3 | Difference | `abs(a - b)` |
| 4 | Exclusion | `a + b - 2*a*b` |
| 5 | Multiply | `a * b` |
| 6 | Screen | `1 - (1-a)*(1-b)` |
| 7 | Overlay | `a < 0.5 ? 2*a*b : 1 - 2*(1-a)*(1-b)` |
| 8 | Darken | `min(a, b)` |
| 9 | Lighten | `max(a, b)` |
| 10 | Color Dodge | `b >= 1 ? 1 : min(1, a/(1-b))` |
| 11 | Color Burn | `b <= 0 ? 0 : 1 - min(1, (1-a)/b)` |
| 12 | Hard Light | `b < 0.5 ? 2*a*b : 1 - 2*(1-a)*(1-b)` (Overlay with a,b swapped) |
| 13 | Soft Light | Pegtop: `(1 - 2*b)*a*a + 2*b*a` |
| 14 | Divide | `b <= 0 ? 1 : min(1, a/b)` |
| 15 | Average | `(a + b) * 0.5` |

**Non-separable (HSL) modes** — operate on the whole `vec3` using the standard
W3C/PDF helpers (luma weights `0.3, 0.59, 0.11`):

```
Lum(c)        = dot(c, vec3(0.3, 0.59, 0.11))
ClipColor(c)  : L = Lum(c); n = min(c.r,c.g,c.b); x = max(c.r,c.g,c.b);
                if (n < 0) c = L + (c-L)*L/(L-n);
                if (x > 1) c = L + (c-L)*(1-L)/(x-L);   return c
SetLum(c, l)  = ClipColor(c + (l - Lum(c)))
Sat(c)        = max(c.r,c.g,c.b) - min(c.r,c.g,c.b)
SetSat(c, s)  : on the channels sorted into (mn <= md <= mx),
                if (mx > mn) { md = (md-mn)*s/(mx-mn); mx = s; } else { md = mx = 0; }
                mn = 0;  (write the channels back to their original positions)
```

| id | name | formula |
|----|------|---------|
| 16 | Hue | `SetLum(SetSat(blend, Sat(base)), Lum(base))` |
| 17 | Saturation | `SetLum(SetSat(base, Sat(blend)), Lum(base))` |
| 18 | Color | `SetLum(blend, Lum(base))` |
| 19 | Luminosity | `SetLum(base, Lum(blend))` |

**Bitwise modes** — per channel on 8-bit values (the FBO is `GL_RGBA8`, so this matches
storage exactly): `q(x) = int(round(clamp(x,0,1)*255))`, result channel
`= (q(a) OP q(b)) / 255.0`:

| id | name | op |
|----|------|----|
| 20 | AND | `q(a) & q(b)` |
| 21 | OR | `q(a) \| q(b)` |
| 22 | XOR | `q(a) ^ q(b)` |

### `shaders/compositor.frag`

Samples `uA`, `uB` at `vUV`; a `switch(uMode)` (or `if` chain) computes `blended`
(the per-mode blend of `a.rgb` and `b.rgb`) using the **same** formulas/order as
`blendPixel`. Composites:

```glsl
float amt = clamp(uOpacity, 0.0, 1.0) * b.a;   // global opacity * B's coverage
FragColor = vec4(mix(a.rgb, blended, amt), max(a.a, b.a));
```

Alpha handling is intentionally simple — this is an RGB blend compositor; for the app's
opaque textures `b.a == 1`, so `amt == uOpacity`. GLSL 410 core supports the integer
bitwise ops used by modes 20–22 (`ivec3(round(c*255)) & / | / ^`).

### `src/modules/CompositorNode.h`

```cpp
class CompositorNode : public ShaderNode {
public:
    CompositorNode() : ShaderNode("Compositor", "shaders/compositor.frag") {
        addInput("a", PortType::Texture, TexRef{});
        addInput("b", PortType::Texture, TexRef{});
        addChoiceInput("mode", blendModeLabels(), 0);          // Normal
        addInput("opacity", PortType::Float, 1.0f, 0.0f, 1.0f);
        addOutput("out", PortType::Texture);
    }
    void evaluate(EvalContext& ctx) override { render(ctx); }
protected:
    void setUniforms(EvalContext& ctx) override;  // bind uA/uB, set uMode (int), uOpacity
};
```

`setUniforms` mirrors `MixNode`: bind `ctx.in<TexRef>(0)` to unit 0 (`uA`),
`ctx.in<TexRef>(1)` to unit 1 (`uB`), `glUniform1i(uMode, (int)lround(ctx.in<float>(2)))`,
`glUniform1f(uOpacity, ctx.in<float>(3))`, restore active unit 0.

### Ports

| # | Port | Type | Default | Notes |
|---|------|------|---------|-------|
| 0 | `a` | Texture | `TexRef{}` | base layer |
| 1 | `b` | Texture | `TexRef{}` | blend layer |
| 2 | `mode` | Float (choice) | 0 (Normal) | `blendModeLabels()` (23) |
| 3 | `opacity` | Float | 1.0 | 0–1; blend amount |
| out 0 | `out` | Texture | — | composited result |

### Registration / CMake / shaders

- `src/app/Application.cpp`: `#include "modules/CompositorNode.h"`; `makeNode`
  `if (type == "Compositor") return std::make_unique<CompositorNode>();`; add
  `"Compositor"` to the **Texture** `nodeCategories` list (next to `"Mix"`).
- `src/main.cpp`: add a `Compositor` node to the `--screenshot` demo.
- `CMakeLists.txt`: add `tests/test_blend_modes.cpp` to the `core_tests` target. The
  `gl_smoke` cross-check goes in the existing `tests/gl_smoke.cpp` (no new test target);
  `BlendModes.h` and `CompositorNode.h` are header-only, so they compile into `gl_smoke`
  via its includes — confirm `gl_smoke` already links the `ShaderNode`/`Framebuffer`/
  `FullscreenPass`/`GLUtil` sources it needs (it does, for the existing shader nodes) and
  that `ColourNode` is reachable for the scenario. `shaders/compositor.frag` is copied
  next to the binary by the existing shader-copy rule and resolved from the repo root,
  like the other shaders.

## Data flow

```
texture a (base), texture b (blend)
   └─► compositor.frag(uMode, uOpacity)
         blended = blendPixel-equivalent(uMode, a.rgb, b.rgb)
         out.rgb = mix(a.rgb, blended, uOpacity * b.a)
         out.a   = max(a.a, b.a)
   └─► FBO texture ─► out
```

## Edge cases

- **Unconnected input** → `TexRef{}` (id 0); sampling an unbound texture yields black
  `(0,0,0,?)`. Normal mode with `b` unconnected shows `a` mixed toward black by opacity;
  this matches Mix's behaviour with an unconnected input and is acceptable.
- **`mode` out of range** → clamped to a valid id (in both `blendPixel` and via the
  choice widget's own clamp).
- **Divide / Color Dodge by ~0** → guarded (`b<=0`/`b>=1` branches) so no Inf/NaN.
- **Bitwise modes** round to 8-bit; exact on the `GL_RGBA8` FBO.
- **`opacity = 0`** → output equals `a` (the base passes through).

## Testing

- **`tests/test_blend_modes.cpp`** (`core_tests`, GL-free): drive `blendPixel` with known
  inputs across the categories — Add clamps at 1; Subtract clamps at 0; Multiply darkens;
  Screen lightens; Difference is symmetric (`blend(D,a,b)==blend(D,b,a)`); Darken/Lighten
  equal `min`/`max`; Divide/Dodge guard div-by-zero (finite, no NaN); a bitwise case with
  hand-computed 8-bit values (e.g. `XOR` of `0xF0` and `0x0F` → `0xFF`); a non-separable
  case (e.g. `Color` of a grey base with a saturated blend has `Lum == Lum(base)`); and
  `blendModeLabels().size() == 23`.
- **`gl_smoke` Compositor scenario**: build two `Colour` nodes with distinct solid colours
  → a `Compositor` → render into the hidden window; read back the centre pixel and assert
  it matches `blendPixel(mode, colourA, colourB)` within ±3/255 (8-bit quantisation) for
  one mode per category — a separable (Multiply), an HSL (Luminosity), and a bitwise
  (XOR) — with `opacity = 1`. This proves the shader agrees with the reference.

## Docs

- **README.md** — add a **Compositor** row to the module table (blend two textures with a
  selectable operator: 23 modes incl. add/subtract/multiply/screen/overlay, the HSL
  hue/saturation/color/luminosity, and bitwise and/or/xor; plus `opacity`).
- **CLAUDE.md** — a brief Architecture bullet: `CompositorNode` is a `ShaderNode` blending
  two textures via `shaders/compositor.frag`, whose 23 blend modes mirror the GL-free
  reference `core/BlendModes.h` (`blendPixel` + `blendModeLabels`); the reference is
  unit-tested and a `gl_smoke` scenario cross-checks the shader against it.
</content>
