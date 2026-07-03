# HSV Adjust node — design

**Date:** 2026-07-03
**Status:** Approved (brainstorm)
**Branch:** `feat/hsv-adjust` (off `develop`)

## Goal

A new **HSV Adjust** node (Texture category): a `ShaderNode` that shifts the hue and scales the
saturation and brightness of an input texture, producing an output texture.

## Decisions (from brainstorm)

- **Controls (all Float inputs, LFO/Automation-wireable):**
  - `hue` — a signed shift in **turns**, range −1..1, default 0 (wraps; matches the codebase's
    `core/ColorHsv.h` + Kaleidoscope rotation).
  - `saturation` — **multiplier**, range 0..2, default 1 (0 = grayscale).
  - `brightness` — **multiplier**, range 0..2, default 1 (0 = black).
- **HSV (brightness = value).** Per pixel: RGB → HSV, `h = frac(h + hue)`,
  `s = clamp(s × saturation, 0, 1)`, `v = clamp(v × brightness, 0, 1)`, HSV → RGB; **alpha
  preserved**.
- **Mirror the existing GL-free reference.** `core/ColorHsv.h` already has `rgbToHsv`/`hsvToRgb`
  (hue in turns). Add a GL-free `adjustHsv(rgb, hueTurns, satMul, brightMul)` there, unit-test it,
  and have `shaders/hsv_adjust.frag` mirror it — with a `gl_smoke` cross-check so the shader and
  the reference can't drift (the same guard the Compositor uses against `BlendModes.h`).

## Architecture

| File | Change |
|---|---|
| `src/core/ColorHsv.h` | Add GL-free `adjustHsv(const glm::vec3& rgb, float hueTurns, float satMul, float brightMul)`. |
| `shaders/hsv_adjust.frag` | **New.** GLSL rgb→hsv→adjust→rgb mirroring `adjustHsv`. |
| `src/modules/HsvAdjustNode.h` | **New, header-only `ShaderNode`.** Ports + `setUniforms`. |
| `src/app/Application.cpp` | Register `"HSV Adjust"` in `makeNode()` + the `"Texture"` category; add the include. |
| `tests/test_color_hsv.cpp` | `core_tests`: `adjustHsv` cases. |
| `tests/gl_smoke.cpp` | Cross-check the shader against `adjustHsv` on a solid colour at a few settings. |
| `CLAUDE.md`, `README.md` | Document the node. |

No CMake changes (`hsv_adjust.frag` ships via the existing `shaders/` directory copy; the node is
header-only; `gl_smoke` already includes `core/ColorHsv.h`).

## Component detail

### `adjustHsv` (GL-free, `core/ColorHsv.h`)

```cpp
// Shift hue by `hueTurns` (wraps) and scale saturation/value by the given multipliers (each
// clamped to [0,1] after scaling). rgb + result in [0,1]. GL-free; mirrored by hsv_adjust.frag.
inline glm::vec3 adjustHsv(const glm::vec3& rgb, float hueTurns, float satMul, float brightMul) {
    glm::vec3 hsv = rgbToHsv(rgb.x, rgb.y, rgb.z);
    float h = hsv.x + hueTurns;                              // hsvToRgb wraps h
    float s = std::clamp(hsv.y * satMul,    0.0f, 1.0f);
    float v = std::clamp(hsv.z * brightMul, 0.0f, 1.0f);
    return hsvToRgb(h, s, v);
}
```

### `shaders/hsv_adjust.frag`

Uniforms `sampler2D uImage`, `float uHue`, `float uSat`, `float uBright`. Compact GLSL
`rgb2hsv`/`hsv2rgb` (hue in [0,1], standard formulation equivalent to `ColorHsv.h`):
`hsv.x = fract(hsv.x + uHue)`, `hsv.y = clamp(hsv.y*uSat, 0, 1)`, `hsv.z = clamp(hsv.z*uBright, 0, 1)`,
`FragColor = vec4(hsv2rgb(hsv), src.a)`.

### `HsvAdjustNode` (header-only `ShaderNode`)

Mirrors `CompositorNode`/`KaleidoscopeNode`:

```cpp
class HsvAdjustNode : public ShaderNode {
public:
    HsvAdjustNode() : ShaderNode("HSV Adjust", "shaders/hsv_adjust.frag") {
        addInput("image", PortType::Texture, TexRef{});
        addInput("hue",        PortType::Float, 0.0f, -1.0f, 1.0f);   // turns (wraps)
        addInput("saturation", PortType::Float, 1.0f,  0.0f, 2.0f);   // multiplier
        addInput("brightness", PortType::Float, 1.0f,  0.0f, 2.0f);   // multiplier
        addOutput("out", PortType::Texture);
    }
    void evaluate(EvalContext& ctx) override { render(ctx); }
protected:
    void setUniforms(EvalContext& ctx) override {
        TexRef in = ctx.in<TexRef>(0);
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, in.id);
        glUniform1i(glGetUniformLocation(program_, "uImage"), 0);
        glUniform1f(glGetUniformLocation(program_, "uHue"),    ctx.in<float>(1));
        glUniform1f(glGetUniformLocation(program_, "uSat"),    ctx.in<float>(2));
        glUniform1f(glGetUniformLocation(program_, "uBright"), ctx.in<float>(3));
    }
};
```

Registered in `makeNode()` and the `"Texture"` category (e.g. after `Kaleidoscope`).

## Data flow / error handling

A pure texture transform. An unconnected `image` samples texture 0 (black) → black output
(consistent with Compositor/Kaleidoscope). Identity settings (`hue 0`, `saturation 1`,
`brightness 1`) reproduce the input. `frac`/`clamp` keep hue/sat/value well-defined for any input.

## Testing

- **`core_tests`** (`test_color_hsv.cpp`): `adjustHsv` —
  - identity: `adjustHsv(red, 0, 1, 1) == red`;
  - hue: `adjustHsv(red, 1/3, 1, 1) ≈ green` (a +1/3-turn shift of pure red);
  - saturation 0: `adjustHsv(red, 0, 0, 1)` is gray (r==g==b, = red's value 1 → white; assert
    channels equal);
  - brightness 0: `adjustHsv(red, 0, 1, 0) == black`.
- **`gl_smoke`**: build `Colour(known) → HSV Adjust → Output` (or a small solid input), evaluate at
  a couple of `(hue, sat, bright)` settings, read back the centre pixel, and assert it matches
  `adjustHsv(input, hue, sat, bright)` within a small tolerance — proving the shader mirrors the
  reference.

## Out of scope (YAGNI)

- HSL / other colour spaces, per-channel curves, gamma, contrast, hue *replacement* (this is an
  adjustment), colour-key/selective adjustments.
