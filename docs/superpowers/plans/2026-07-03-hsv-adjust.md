# HSV Adjust node — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A new **HSV Adjust** node (Texture category) — a `ShaderNode` that shifts the hue (turns) and scales the saturation and brightness (multipliers) of an input texture into an output texture.

**Architecture:** A GL-free reference `adjustHsv` is added to the existing `core/ColorHsv.h`; `shaders/hsv_adjust.frag` mirrors it; `HsvAdjustNode` is a header-only `ShaderNode` (the Compositor/Kaleidoscope pattern). A `gl_smoke` scenario cross-checks the shader against `adjustHsv` so they can't drift.

**Tech Stack:** C++17, OpenGL 4.1 (GLSL), `core/ColorHsv.h` (already unit-tested + linked into `gl_smoke`), doctest, headless GL.

**Reference spec:** `docs/superpowers/specs/2026-07-03-hsv-adjust-design.md`

**Conventions (CLAUDE.md):**
- `src/core/` stays GL-free (`adjustHsv` is pure math). The shader lives in `shaders/`, the node in `src/modules/`.
- Conventional Commits. Branch `feat/hsv-adjust` (already created, off `develop`).
- Never `git add -A`/`git add .` — stage only the files each step names. Leave `build.sh`, `examples/`, `preferences.oss`, `project.oss`, `imgui.ini` untracked.
- Build: `cmake --build build --target <t> -j`; tests: `ctest --test-dir build --output-on-failure`.

**Context:** `core/ColorHsv.h` already has `rgbToHsv`/`hsvToRgb` (hue in [0,1] turns) and is unit-tested in `tests/test_color_hsv.cpp` (includes `core/ColorHsv.h`, `using namespace oss;`). `gl_smoke.cpp` already `#include`s `core/ColorHsv.h`, `modules/ColourNode.h`, `modules/OutputNode.h`, has `readCentre` + a `near(int,int)` lambda + a `ColourNode → node → Output` cross-check pattern (Scenario 15, Compositor). `ShaderNode` subclasses declare ports + override `setUniforms` (see `modules/CompositorNode.h`/`KaleidoscopeNode.h`); `program_` is the compiled program.

---

### Task 1: `adjustHsv` reference + test

**Files:**
- Modify: `src/core/ColorHsv.h`
- Test: `tests/test_color_hsv.cpp`

- [ ] **Step 1: Write the failing test**

Append to `tests/test_color_hsv.cpp`:

```cpp
TEST_CASE("adjustHsv shifts hue and scales saturation/value") {
    glm::vec3 red(1.0f, 0.0f, 0.0f);
    // identity: hue 0, sat 1, bright 1 -> unchanged
    glm::vec3 id = adjustHsv(red, 0.0f, 1.0f, 1.0f);
    CHECK(id.x == doctest::Approx(1.0f)); CHECK(id.y == doctest::Approx(0.0f)); CHECK(id.z == doctest::Approx(0.0f));
    // +1/3 turn hue: red -> green
    glm::vec3 g = adjustHsv(red, 1.0f/3.0f, 1.0f, 1.0f);
    CHECK(g.x == doctest::Approx(0.0f)); CHECK(g.y == doctest::Approx(1.0f)); CHECK(g.z == doctest::Approx(0.0f));
    // saturation 0 -> grayscale (all channels equal)
    glm::vec3 gray = adjustHsv(red, 0.0f, 0.0f, 1.0f);
    CHECK(gray.x == doctest::Approx(gray.y)); CHECK(gray.y == doctest::Approx(gray.z));
    // brightness 0 -> black
    glm::vec3 black = adjustHsv(red, 0.0f, 1.0f, 0.0f);
    CHECK(black.x == doctest::Approx(0.0f)); CHECK(black.y == doctest::Approx(0.0f)); CHECK(black.z == doctest::Approx(0.0f));
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build build --target core_tests -j`
Expected: FAIL to compile — `adjustHsv` not declared.

- [ ] **Step 3: Implement `adjustHsv`**

In `src/core/ColorHsv.h`, add before the closing `} // namespace oss` (after `rgbToHsv`):

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

(`<algorithm>` for `std::clamp` and `<glm/vec3.hpp>` are already included by `ColorHsv.h`.)

- [ ] **Step 4: Run to verify it passes**

Run: `cmake --build build --target core_tests -j && ctest --test-dir build -R core_tests --output-on-failure`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/core/ColorHsv.h tests/test_color_hsv.cpp
git commit -m "feat(core): adjustHsv (hue shift + sat/value scale) reference"
```

---

### Task 2: shader + `HsvAdjustNode` + registration + gl_smoke cross-check

**Files:**
- Create: `shaders/hsv_adjust.frag`
- Create: `src/modules/HsvAdjustNode.h`
- Modify: `src/app/Application.cpp`
- Modify: `tests/gl_smoke.cpp`

- [ ] **Step 1: Create the fragment shader**

Create `shaders/hsv_adjust.frag`:

```glsl
#version 410 core
in vec2 vUV;
out vec4 FragColor;

uniform sampler2D uImage;
uniform float uHue;     // hue shift in turns (wraps)
uniform float uSat;     // saturation multiplier
uniform float uBright;  // brightness (value) multiplier

// rgb<->hsv (hue in [0,1]); equivalent to core/ColorHsv.h.
vec3 rgb2hsv(vec3 c) {
    vec4 K = vec4(0.0, -1.0 / 3.0, 2.0 / 3.0, -1.0);
    vec4 p = mix(vec4(c.bg, K.wz), vec4(c.gb, K.xy), step(c.b, c.g));
    vec4 q = mix(vec4(p.xyw, c.r), vec4(c.r, p.yzx), step(p.x, c.r));
    float d = q.x - min(q.w, q.y);
    float e = 1.0e-10;
    return vec3(abs(q.z + (q.w - q.y) / (6.0 * d + e)), d / (q.x + e), q.x);
}
vec3 hsv2rgb(vec3 c) {
    vec3 rgb = clamp(abs(mod(c.x * 6.0 + vec3(0.0, 4.0, 2.0), 6.0) - 3.0) - 1.0, 0.0, 1.0);
    return c.z * mix(vec3(1.0), rgb, c.y);
}

void main() {
    vec4 src = texture(uImage, vUV);
    vec3 hsv = rgb2hsv(src.rgb);
    hsv.x = fract(hsv.x + uHue);
    hsv.y = clamp(hsv.y * uSat,    0.0, 1.0);
    hsv.z = clamp(hsv.z * uBright, 0.0, 1.0);
    FragColor = vec4(hsv2rgb(hsv), src.a);
}
```

- [ ] **Step 2: Create the node**

Create `src/modules/HsvAdjustNode.h`:

```cpp
#pragma once
#include <glad/gl.h>
#include "gfx/ShaderNode.h"

namespace oss {

// Shifts the hue and scales the saturation/brightness of an input texture. A ShaderNode -- the
// colour math lives in shaders/hsv_adjust.frag, which mirrors the GL-free core/ColorHsv.h
// (`adjustHsv`); a gl_smoke scenario cross-checks the two. Mirrors CompositorNode.
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
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, in.id);
        glUniform1i(glGetUniformLocation(program_, "uImage"), 0);
        glUniform1f(glGetUniformLocation(program_, "uHue"),    ctx.in<float>(1));
        glUniform1f(glGetUniformLocation(program_, "uSat"),    ctx.in<float>(2));
        glUniform1f(glGetUniformLocation(program_, "uBright"), ctx.in<float>(3));
    }
};

} // namespace oss
```

- [ ] **Step 3: Register the node**

In `src/app/Application.cpp`:

(a) Add the include next to the other Texture-node includes (near `#include "modules/KaleidoscopeNode.h"`):
```cpp
#include "modules/HsvAdjustNode.h"
```
(b) In `makeNode`, after the `"Kaleidoscope"` line:
```cpp
    if (type == "HSV Adjust") return std::make_unique<HsvAdjustNode>();
```
(c) In `nodeCategories`, add `"HSV Adjust"` to the `"Texture"` list (after `"Kaleidoscope"`) so it reads:
```cpp
        { "Texture", { "Colour", "Image Streamer", "Image Sequencer", "Video", "Mix", "Compositor", "Kaleidoscope", "HSV Adjust", "Recorder", "Output" } },
```

- [ ] **Step 4: Add the gl_smoke cross-check scenario**

In `tests/gl_smoke.cpp`:

(a) Add the node include near the other module includes at the top (e.g. next to `#include "modules/CompositorNode.h"`):
```cpp
#include "modules/HsvAdjustNode.h"
```

(b) Add a new scenario just before the final `glfwTerminate();`/`return 0;` at the end of `main()` (or after the Compositor scenario, Scenario 15). It mirrors the Compositor cross-check:

```cpp
    // --- Scenario: HSV Adjust shader matches the adjustHsv reference ---
    // Feed a solid colour through the node and assert the rendered centre pixel matches
    // adjustHsv() computed on the 8-bit-quantised input (what the texture carries). near() +/-3.
    {
        auto quant = [](glm::vec3 c) {
            return glm::vec3(std::round(c.x*255.0f)/255.0f,
                             std::round(c.y*255.0f)/255.0f,
                             std::round(c.z*255.0f)/255.0f);
        };
        auto check = [&](glm::vec3 in, float hue, float sat, float bright) -> bool {
            Graph g;
            auto col = std::make_unique<ColourNode>(); col->inputDefault(0) = glm::vec4(in, 1.0f);
            auto adj = std::make_unique<HsvAdjustNode>();
            adj->inputDefault(1) = hue; adj->inputDefault(2) = sat; adj->inputDefault(3) = bright;
            auto out = std::make_unique<OutputNode>();
            col->initGL(); adj->initGL(); out->initGL();
            int cId = g.addNode(std::move(col));
            int aId = g.addNode(std::move(adj));
            int oId = g.addNode(std::move(out));
            if (!g.connect(cId,0,aId,0) || !g.connect(aId,0,oId,0)) return false;
            g.evaluate(1.0f/60.0f);
            TexRef t = dynamic_cast<OutputNode*>(g.findNode(oId))->current();
            if (!t.id) return false;
            int r, gg, bb, aa; readCentre(t, r, gg, bb, aa);
            glm::vec3 e = adjustHsv(quant(in), hue, sat, bright);
            int er = (int)std::lround(e.x*255.0f), eg = (int)std::lround(e.y*255.0f), eb = (int)std::lround(e.z*255.0f);
            std::fprintf(stderr, "gl_smoke hsv (h%.2f s%.2f v%.2f): got (%d,%d,%d) expected (%d,%d,%d)\n",
                         hue, sat, bright, r, gg, bb, er, eg, eb);
            return near(r,er) && near(gg,eg) && near(bb,eb);
        };
        if (!check(glm::vec3(1.0f,0.0f,0.0f),   1.0f/3.0f, 1.0f, 1.0f)) { glfwTerminate(); return fail("HSV Adjust hue-shift (red->green) mismatch"); }
        if (!check(glm::vec3(0.2f,0.5f,0.8f),   0.1f,      1.0f, 0.8f)) { glfwTerminate(); return fail("HSV Adjust hue+brightness mismatch"); }
        if (!check(glm::vec3(0.6f,0.3f,0.9f),   0.0f,      0.0f, 1.0f)) { glfwTerminate(); return fail("HSV Adjust desaturate mismatch"); }
        std::fprintf(stderr, "gl_smoke OK: HSV Adjust shader matches adjustHsv (hue/sat/bright)\n");
    }
```

- [ ] **Step 5: Build (app + gl_smoke) and run gl_smoke**

Run: `cmake --build build --target shader_streamer gl_smoke -j && ctest --test-dir build -R gl_smoke --output-on-failure`
Expected: both build; `gl_smoke` PASSES, printing `gl_smoke OK: HSV Adjust shader matches adjustHsv (hue/sat/bright)`. (No-GL environments skip; confirm it built. You can also `./build/gl_smoke 2>&1 | grep -i "hsv\|adjust"`.)

- [ ] **Step 6: Commit**

```bash
git add shaders/hsv_adjust.frag src/modules/HsvAdjustNode.h src/app/Application.cpp tests/gl_smoke.cpp
git commit -m "feat(modules): HSV Adjust node (hue/saturation/brightness shader)"
```

---

### Task 3: Documentation

**Files:**
- Modify: `CLAUDE.md`
- Modify: `README.md`

- [ ] **Step 1: Update CLAUDE.md**

In `CLAUDE.md`, in the **Compositor** bullet (or right after it in the node list), add a new bullet:

```markdown
- **HSV Adjust** — `HsvAdjustNode` (`src/modules/HsvAdjustNode.h`, header-only) is a `ShaderNode`
  that shifts the hue (turns) and scales the saturation/brightness (multipliers) of an input
  texture in `shaders/hsv_adjust.frag`, which mirrors the GL-free `core/ColorHsv.h` `adjustHsv`
  (`rgbToHsv` → shift/scale/clamp → `hsvToRgb`); a `gl_smoke` scenario cross-checks the shader
  against `adjustHsv` (like the Compositor guards against `BlendModes.h`). In the **Texture** category.
```

- [ ] **Step 2: Update README.md**

In `README.md`, add a row to the Texture-nodes table (after the **Kaleidoscope** row):

```markdown
| **HSV Adjust** | shift the hue (turns) and scale saturation & brightness of a texture; wire `hue` to an LFO to cycle colours |
```

- [ ] **Step 3: Commit**

```bash
git add CLAUDE.md README.md
git commit -m "docs: HSV Adjust node"
```

---

## Final verification (after all tasks)

- [ ] Full build: `cmake --build build -j`
- [ ] All tests: `ctest --test-dir build --output-on-failure` — `core_tests` + `gl_smoke` pass (or gl_smoke cleanly skips where no GL context).
- [ ] Manual (optional, needs a display): run `./build/shader_streamer`, add **HSV Adjust** from the Texture menu, wire an Image Streamer / Colour → HSV Adjust → Output, and sweep `hue`/`saturation`/`brightness`; wire an LFO into `hue` to cycle colours.
- [ ] Hand off to `superpowers:finishing-a-development-branch`.

## Notes for the implementer

- **Only stage the files each step names.** Never `git add -A`. Leave `build.sh`, `examples/`, `preferences.oss`, `project.oss`, `imgui.ini` untracked.
- No CMake changes: `hsv_adjust.frag` ships via the existing `shaders/` directory copy; the node is header-only; `gl_smoke` already links `ShaderNode.cpp` + includes `core/ColorHsv.h`.
- The gl_smoke cross-check computes the reference on the **8-bit-quantised** input (`quant`) — the same value the `ColourNode` texture carries — so only output rounding can differ; the `near()` tolerance is ±3. The two HSV formulations (`ColorHsv.h` switch-based vs the shader's compact form) agree well within that.
