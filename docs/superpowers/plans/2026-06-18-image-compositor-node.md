# Image Compositor Node Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a **Compositor** node that blends two textures with a selectable operator (23 blend modes) plus an `opacity`, mirroring the **Mix** node.

**Architecture:** A GL-free reference (`core/BlendModes.h`: `blendModeLabels()` + `blendPixel()`) is the source of truth and the canonical mode order, unit-tested in `core_tests`. `shaders/compositor.frag` mirrors it by the same mode ids; `CompositorNode` (a `ShaderNode`) wires two textures + a `mode` choice + `opacity` into it. A `gl_smoke` scenario cross-checks the shader output against `blendPixel` so the two implementations can't drift.

**Tech Stack:** C++17, GLSL 410, doctest, OpenGL 4.1, CMake. Design: `docs/superpowers/specs/2026-06-18-image-compositor-design.md`.

**IMPORTANT for the implementer:** `BlendModes.h` and `CompositorNode.h` are header-only (no CMake source entries). The `compositor.frag` shader is auto-copied next to the binary by the existing `copy_directory` rule and resolved from the repo root, so no CMake change is needed for it. `core_tests` links glm + doctest (no GL); `gl_smoke` already links the `ShaderNode`/`Framebuffer`/`FullscreenPass`/`GLUtil` sources and uses the header-only `ColourNode`/`OutputNode`, so the Compositor scenario needs only new `#include`s, not new CMake sources. **The C++ `blendPixel` and the GLSL shader must use identical formulas** (the plan gives both, written to match line-for-line, especially `setSat`).

---

### Task 1: GL-free blend-mode reference + unit tests

**Files:**
- Create: `tests/test_blend_modes.cpp`
- Create: `src/core/BlendModes.h`
- Modify: `CMakeLists.txt` (add the test to `core_tests`)

- [ ] **Step 1: Write the failing tests**

Create `tests/test_blend_modes.cpp`:

```cpp
#include <doctest/doctest.h>
#include "core/BlendModes.h"
#include <glm/vec3.hpp>
#include <cmath>

using namespace oss;

static bool approx(const glm::vec3& a, const glm::vec3& b, float eps = 1e-4f) {
    return std::fabs(a.x-b.x) < eps && std::fabs(a.y-b.y) < eps && std::fabs(a.z-b.z) < eps;
}
static float lumOf(const glm::vec3& c) { return 0.3f*c.x + 0.59f*c.y + 0.11f*c.z; }

TEST_CASE("blendModeLabels has 23 modes") { CHECK(blendModeLabels().size() == 23); }

TEST_CASE("Normal returns the blend layer") {
    CHECK(approx(blendPixel(0, glm::vec3(0.1f,0.2f,0.3f), glm::vec3(0.7f,0.6f,0.5f)),
                 glm::vec3(0.7f,0.6f,0.5f)));
}

TEST_CASE("Add clamps at 1, Subtract clamps at 0") {
    CHECK(approx(blendPixel(1, glm::vec3(0.6f), glm::vec3(0.6f)), glm::vec3(1.0f)));   // 1.2 -> 1
    CHECK(approx(blendPixel(2, glm::vec3(0.3f), glm::vec3(0.6f)), glm::vec3(0.0f)));   // -0.3 -> 0
}

TEST_CASE("Multiply darkens, Screen lightens") {
    CHECK(approx(blendPixel(5, glm::vec3(0.5f), glm::vec3(0.5f)), glm::vec3(0.25f)));
    CHECK(approx(blendPixel(6, glm::vec3(0.5f), glm::vec3(0.5f)), glm::vec3(0.75f)));
}

TEST_CASE("Difference is symmetric; Darken/Lighten are min/max") {
    glm::vec3 a(0.2f,0.8f,0.5f), b(0.6f,0.1f,0.9f);
    CHECK(approx(blendPixel(3, a, b), blendPixel(3, b, a)));
    CHECK(approx(blendPixel(8, a, b), glm::vec3(0.2f,0.1f,0.5f)));
    CHECK(approx(blendPixel(9, a, b), glm::vec3(0.6f,0.8f,0.9f)));
}

TEST_CASE("Average is the midpoint") {
    CHECK(approx(blendPixel(15, glm::vec3(0.2f), glm::vec3(0.8f)), glm::vec3(0.5f)));
}

TEST_CASE("Divide and Color Dodge guard division by zero (finite, clamped)") {
    glm::vec3 r1 = blendPixel(14, glm::vec3(0.5f), glm::vec3(0.0f));   // divide by 0 -> 1
    glm::vec3 r2 = blendPixel(10, glm::vec3(0.5f), glm::vec3(1.0f));   // dodge b>=1 -> 1
    CHECK(std::isfinite(r1.x)); CHECK(r1.x == doctest::Approx(1.0f));
    CHECK(std::isfinite(r2.x)); CHECK(r2.x == doctest::Approx(1.0f));
}

TEST_CASE("bitwise AND/OR/XOR operate on 8-bit channels") {
    glm::vec3 a(240.0f/255.0f);   // 0xF0
    glm::vec3 b(15.0f/255.0f);    // 0x0F
    CHECK(approx(blendPixel(20, a, b), glm::vec3(0.0f)));            // AND -> 0x00
    CHECK(approx(blendPixel(21, a, b), glm::vec3(255.0f/255.0f)));  // OR  -> 0xFF
    CHECK(approx(blendPixel(22, a, b), glm::vec3(255.0f/255.0f)));  // XOR -> 0xFF
}

TEST_CASE("Color imposes the blend's chroma but keeps the base luminance") {
    glm::vec3 base(0.4f);                 // grey, lum 0.4
    glm::vec3 blend(0.9f, 0.2f, 0.1f);
    glm::vec3 r = blendPixel(18, base, blend);          // Color = setLum(blend, lum(base))
    CHECK(lumOf(r) == doctest::Approx(0.4f).epsilon(0.02));
}

TEST_CASE("Saturation keeps the base luminance (setSat path)") {
    glm::vec3 base(0.2f, 0.5f, 0.8f), blend(0.9f, 0.5f, 0.1f);
    glm::vec3 r = blendPixel(17, base, blend);
    CHECK(lumOf(r) == doctest::Approx(lumOf(base)).epsilon(0.02));
}
```

- [ ] **Step 2: Wire the test into the build**

In `CMakeLists.txt`, in the `core_tests` target's test-file list, add `tests/test_blend_modes.cpp` right after `tests/test_chord_player.cpp`:

```cmake
  tests/test_chord_player.cpp
  tests/test_blend_modes.cpp
```

- [ ] **Step 3: Run the tests to verify they fail**

Run: `cmake -S . -B build && cmake --build build -j --target core_tests`
Expected: FAIL — compile error `'core/BlendModes.h' file not found`.

- [ ] **Step 4: Write the header to make them pass**

Create `src/core/BlendModes.h`:

```cpp
#pragma once
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>
#include <glm/glm.hpp>

namespace oss {

// Ordered blend-mode labels for the Compositor's `mode` dropdown. The index is the mode
// id used by both blendPixel() below and the shader's switch -- keep all three in sync.
inline const std::vector<std::string>& blendModeLabels() {
    static const std::vector<std::string> labels = {
        "Normal", "Add", "Subtract", "Difference", "Exclusion",      // 0..4
        "Multiply", "Screen", "Overlay", "Darken", "Lighten",        // 5..9
        "Color Dodge", "Color Burn", "Hard Light", "Soft Light", "Divide",  // 10..14
        "Average", "Hue", "Saturation", "Color", "Luminosity",       // 15..19
        "AND", "OR", "XOR"                                           // 20..22
    };
    return labels;
}

namespace blend_detail {

inline float clamp01(float x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }

// Separable (per-channel) blend for ids 0..15. Result is clamped by the caller.
inline float channel(int id, float a, float b) {
    switch (id) {
        case 1:  return a + b;                                        // Add
        case 2:  return a - b;                                        // Subtract
        case 3:  return std::fabs(a - b);                             // Difference
        case 4:  return a + b - 2.0f * a * b;                         // Exclusion
        case 5:  return a * b;                                        // Multiply
        case 6:  return 1.0f - (1.0f - a) * (1.0f - b);               // Screen
        case 7:  return a < 0.5f ? 2.0f*a*b : 1.0f - 2.0f*(1.0f-a)*(1.0f-b);   // Overlay
        case 8:  return std::min(a, b);                               // Darken
        case 9:  return std::max(a, b);                               // Lighten
        case 10: return b >= 1.0f ? 1.0f : std::min(1.0f, a / (1.0f - b));         // Color Dodge
        case 11: return b <= 0.0f ? 0.0f : 1.0f - std::min(1.0f, (1.0f - a) / b);  // Color Burn
        case 12: return b < 0.5f ? 2.0f*a*b : 1.0f - 2.0f*(1.0f-a)*(1.0f-b);   // Hard Light
        case 13: return (1.0f - 2.0f*b)*a*a + 2.0f*b*a;               // Soft Light (Pegtop)
        case 14: return b <= 0.0f ? 1.0f : std::min(1.0f, a / b);     // Divide
        case 15: return 0.5f * (a + b);                              // Average
        default: return b;                                           // Normal (0)
    }
}

inline float lum(const glm::vec3& c) { return 0.3f*c.x + 0.59f*c.y + 0.11f*c.z; }

inline glm::vec3 clipColor(glm::vec3 c) {
    float L = lum(c);
    float n = std::min(c.x, std::min(c.y, c.z));
    float x = std::max(c.x, std::max(c.y, c.z));
    if (n < 0.0f) c = glm::vec3(L) + (c - glm::vec3(L)) * (L / (L - n));
    if (x > 1.0f) c = glm::vec3(L) + (c - glm::vec3(L)) * ((1.0f - L) / (x - L));
    return c;
}
inline glm::vec3 setLum(const glm::vec3& c, float l) { return clipColor(c + glm::vec3(l - lum(c))); }
inline float sat(const glm::vec3& c) {
    return std::max(c.x, std::max(c.y, c.z)) - std::min(c.x, std::min(c.y, c.z));
}
// Index-based SetSat -- written to match the GLSL line-for-line (no pointer sort).
inline glm::vec3 setSat(const glm::vec3& c, float s) {
    int mni = 0, mxi = 0;
    if (c[1] < c[mni]) mni = 1;  if (c[2] < c[mni]) mni = 2;
    if (c[1] > c[mxi]) mxi = 1;  if (c[2] > c[mxi]) mxi = 2;
    int mdi = 3 - mni - mxi;
    glm::vec3 o(0.0f);
    if (c[mxi] > c[mni]) {
        o[mdi] = (c[mdi] - c[mni]) * s / (c[mxi] - c[mni]);
        o[mxi] = s;
    }
    return o;   // o[mni] stays 0
}

inline int q8(float x) { return (int)std::lround(clamp01(x) * 255.0f); }

} // namespace blend_detail

// Reference blend of two RGB colours in [0,1] under mode id (clamped to a valid id).
// This is the source of truth the shader mirrors; result is clamped to [0,1].
inline glm::vec3 blendPixel(int id, const glm::vec3& base, const glm::vec3& blend) {
    using namespace blend_detail;
    int n = (int)blendModeLabels().size();
    if (id < 0) id = 0; if (id >= n) id = n - 1;

    glm::vec3 o;
    if (id <= 15) {
        o = glm::vec3(channel(id, base.x, blend.x),
                      channel(id, base.y, blend.y),
                      channel(id, base.z, blend.z));
    } else if (id == 16) { o = setLum(setSat(blend, sat(base)), lum(base)); }   // Hue
    else if (id == 17)   { o = setLum(setSat(base, sat(blend)), lum(base)); }   // Saturation
    else if (id == 18)   { o = setLum(blend, lum(base)); }                      // Color
    else if (id == 19)   { o = setLum(base, lum(blend)); }                      // Luminosity
    else {                                                                      // 20 AND/21 OR/22 XOR
        auto bw = [id](float a, float b) {
            int ia = q8(a), ib = q8(b);
            int r = (id == 20) ? (ia & ib) : (id == 21) ? (ia | ib) : (ia ^ ib);
            return (float)r / 255.0f;
        };
        o = glm::vec3(bw(base.x, blend.x), bw(base.y, blend.y), bw(base.z, blend.z));
    }
    return glm::vec3(clamp01(o.x), clamp01(o.y), clamp01(o.z));
}

} // namespace oss
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `cmake --build build -j --target core_tests && ./build/core_tests`
Expected: PASS — all `core_tests` green, including the new blend-mode cases. (Run the full binary.)

- [ ] **Step 6: Commit**

```bash
git add src/core/BlendModes.h tests/test_blend_modes.cpp CMakeLists.txt
git commit -m "$(cat <<'EOF'
feat(core): add a GL-free blend-mode reference (BlendModes)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
)"
```

---

### Task 2: Compositor shader + node + registration + gl_smoke cross-check

**Files:**
- Create: `src/modules/CompositorNode.h`
- Create: `shaders/compositor.frag`
- Modify: `tests/gl_smoke.cpp` (add the cross-check scenario)
- Modify: `src/app/Application.cpp` (include + makeNode + Texture category)
- Modify: `src/main.cpp` (screenshot demo)

- [ ] **Step 1: Write the node**

Create `src/modules/CompositorNode.h`:

```cpp
#pragma once
#include <glad/gl.h>
#include <algorithm>
#include <cmath>
#include "gfx/ShaderNode.h"
#include "core/BlendModes.h"

namespace oss {

// Blends two input textures with a selectable operator (23 modes from core/BlendModes.h)
// and an opacity. A ShaderNode -- the per-mode math lives in shaders/compositor.frag,
// which mirrors blendPixel(); the GL-free reference is unit-tested and gl_smoke
// cross-checks the shader against it. Mirrors MixNode.
class CompositorNode : public ShaderNode {
public:
    CompositorNode() : ShaderNode("Compositor", "shaders/compositor.frag") {
        addInput("a", PortType::Texture, TexRef{});
        addInput("b", PortType::Texture, TexRef{});
        addChoiceInput("mode", blendModeLabels(), 0);            // Normal
        addInput("opacity", PortType::Float, 1.0f, 0.0f, 1.0f);
        addOutput("out", PortType::Texture);
    }
    void evaluate(EvalContext& ctx) override { render(ctx); }

protected:
    void setUniforms(EvalContext& ctx) override {
        TexRef a = ctx.in<TexRef>(0);
        TexRef b = ctx.in<TexRef>(1);
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, a.id);
        glUniform1i(glGetUniformLocation(program_, "uA"), 0);
        glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, b.id);
        glUniform1i(glGetUniformLocation(program_, "uB"), 1);
        int mode = std::clamp((int)std::lround(ctx.in<float>(2)),
                              0, (int)blendModeLabels().size() - 1);
        glUniform1i(glGetUniformLocation(program_, "uMode"), mode);
        glUniform1f(glGetUniformLocation(program_, "uOpacity"), ctx.in<float>(3));
        glActiveTexture(GL_TEXTURE0);   // restore default active unit
    }
};

} // namespace oss
```

- [ ] **Step 2: Add the gl_smoke cross-check scenario**

In `tests/gl_smoke.cpp`, add `#include "modules/CompositorNode.h"` and `#include "core/BlendModes.h"` near the other module includes (after `#include "modules/MixNode.h"`). Then add this scenario right before the final `glfwDestroyWindow(win);` line (after Scenario 14):

```cpp
    // --- Scenario 15: Compositor blends two colours; shader matches the C++ reference ---
    // Feed two solid colours into the Compositor and assert the rendered centre pixel
    // matches blendPixel() for one mode per code path: Multiply (separable), Hue
    // (non-separable setSat/setLum), XOR (bitwise). The reference is computed on the
    // 8-bit-quantised inputs (what the textures actually carry) so only output rounding
    // can differ; the near() tolerance is +/-3.
    {
        auto quant = [](glm::vec3 c) {
            return glm::vec3(std::round(c.x*255.0f)/255.0f,
                             std::round(c.y*255.0f)/255.0f,
                             std::round(c.z*255.0f)/255.0f);
        };
        auto check = [&](int mode, glm::vec3 ca, glm::vec3 cb) -> bool {
            Graph g;
            auto a = std::make_unique<ColourNode>(); a->inputDefault(0) = glm::vec4(ca, 1.0f);
            auto b = std::make_unique<ColourNode>(); b->inputDefault(0) = glm::vec4(cb, 1.0f);
            auto comp = std::make_unique<CompositorNode>();
            comp->inputDefault(2) = (float)mode;   // mode
            comp->inputDefault(3) = 1.0f;          // opacity
            auto out = std::make_unique<OutputNode>();
            a->initGL(); b->initGL(); comp->initGL(); out->initGL();
            int aId = g.addNode(std::move(a));
            int bId = g.addNode(std::move(b));
            int cId = g.addNode(std::move(comp));
            int oId = g.addNode(std::move(out));
            if (!g.connect(aId,0,cId,0) || !g.connect(bId,0,cId,1) || !g.connect(cId,0,oId,0)) return false;
            g.evaluate(1.0f/60.0f);
            TexRef t = dynamic_cast<OutputNode*>(g.findNode(oId))->current();
            if (!t.id) return false;
            int r, gg, bb, aa; readCentre(t, r, gg, bb, aa);
            glm::vec3 e = blendPixel(mode, quant(ca), quant(cb));
            int er = (int)std::lround(e.x*255.0f), eg = (int)std::lround(e.y*255.0f), eb = (int)std::lround(e.z*255.0f);
            std::fprintf(stderr, "gl_smoke compositor mode %d: got (%d,%d,%d) expected (%d,%d,%d)\n",
                         mode, r, gg, bb, er, eg, eb);
            return near(r,er) && near(gg,eg) && near(bb,eb);
        };
        glm::vec3 ca(0.2f, 0.5f, 0.8f), cb(0.9f, 0.3f, 0.1f);   // distinct channels (no setSat ties)
        if (!check(5,  ca, cb)) { glfwTerminate(); return fail("Compositor Multiply mismatch vs reference"); }
        if (!check(16, ca, cb)) { glfwTerminate(); return fail("Compositor Hue mismatch vs reference"); }
        if (!check(22, ca, cb)) { glfwTerminate(); return fail("Compositor XOR mismatch vs reference"); }
        std::fprintf(stderr, "gl_smoke OK: Compositor shader matches blendPixel (Multiply/Hue/XOR)\n");
    }
```

- [ ] **Step 3: Build gl_smoke and verify it FAILS (shader missing)**

Run: `cmake -S . -B build && cmake --build build -j --target gl_smoke && ctest --test-dir build -R gl_smoke --output-on-failure`
Expected: FAIL — `shaders/compositor.frag` doesn't exist yet, so the Compositor's program fails to link and the rendered pixel won't match the reference (the scenario reports a mismatch / "Compositor ... mismatch vs reference").

- [ ] **Step 4: Write the shader to make it pass**

Create `shaders/compositor.frag`:

```glsl
#version 410 core
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uA;
uniform sampler2D uB;
uniform int uMode;
uniform float uOpacity;

// Separable (per-channel) blend for ids 0..15. Mirrors BlendModes.h channel().
float channel(int id, float a, float b) {
    if (id == 1)  return a + b;
    if (id == 2)  return a - b;
    if (id == 3)  return abs(a - b);
    if (id == 4)  return a + b - 2.0*a*b;
    if (id == 5)  return a * b;
    if (id == 6)  return 1.0 - (1.0-a)*(1.0-b);
    if (id == 7)  return a < 0.5 ? 2.0*a*b : 1.0 - 2.0*(1.0-a)*(1.0-b);
    if (id == 8)  return min(a, b);
    if (id == 9)  return max(a, b);
    if (id == 10) return b >= 1.0 ? 1.0 : min(1.0, a/(1.0-b));
    if (id == 11) return b <= 0.0 ? 0.0 : 1.0 - min(1.0, (1.0-a)/b);
    if (id == 12) return b < 0.5 ? 2.0*a*b : 1.0 - 2.0*(1.0-a)*(1.0-b);
    if (id == 13) return (1.0 - 2.0*b)*a*a + 2.0*b*a;
    if (id == 14) return b <= 0.0 ? 1.0 : min(1.0, a/b);
    if (id == 15) return 0.5*(a + b);
    return b;   // Normal (0)
}

float lum(vec3 c) { return 0.3*c.r + 0.59*c.g + 0.11*c.b; }
vec3 clipColor(vec3 c) {
    float L = lum(c);
    float n = min(c.r, min(c.g, c.b));
    float x = max(c.r, max(c.g, c.b));
    if (n < 0.0) c = vec3(L) + (c - vec3(L)) * (L / (L - n));
    if (x > 1.0) c = vec3(L) + (c - vec3(L)) * ((1.0 - L) / (x - L));
    return c;
}
vec3 setLum(vec3 c, float l) { return clipColor(c + vec3(l - lum(c))); }
float sat(vec3 c) { return max(c.r, max(c.g, c.b)) - min(c.r, min(c.g, c.b)); }
// Index-based SetSat -- matches BlendModes.h setSat() line-for-line.
vec3 setSat(vec3 c, float s) {
    int mni = 0, mxi = 0;
    if (c[1] < c[mni]) mni = 1;  if (c[2] < c[mni]) mni = 2;
    if (c[1] > c[mxi]) mxi = 1;  if (c[2] > c[mxi]) mxi = 2;
    int mdi = 3 - mni - mxi;
    vec3 o = vec3(0.0);
    if (c[mxi] > c[mni]) {
        o[mdi] = (c[mdi] - c[mni]) * s / (c[mxi] - c[mni]);
        o[mxi] = s;
    }
    return o;
}

void main() {
    vec4 a = texture(uA, vUV);
    vec4 b = texture(uB, vUV);
    vec3 blended;
    if (uMode <= 15) {
        blended = vec3(channel(uMode, a.r, b.r),
                       channel(uMode, a.g, b.g),
                       channel(uMode, a.b, b.b));
    } else if (uMode == 16) { blended = setLum(setSat(b.rgb, sat(a.rgb)), lum(a.rgb)); }   // Hue
    else if (uMode == 17)   { blended = setLum(setSat(a.rgb, sat(b.rgb)), lum(a.rgb)); }   // Saturation
    else if (uMode == 18)   { blended = setLum(b.rgb, lum(a.rgb)); }                       // Color
    else if (uMode == 19)   { blended = setLum(a.rgb, lum(b.rgb)); }                       // Luminosity
    else {                                                                                 // 20 AND/21 OR/22 XOR
        ivec3 ia = ivec3(clamp(a.rgb, 0.0, 1.0) * 255.0 + 0.5);
        ivec3 ib = ivec3(clamp(b.rgb, 0.0, 1.0) * 255.0 + 0.5);
        ivec3 r = (uMode == 20) ? (ia & ib) : (uMode == 21) ? (ia | ib) : (ia ^ ib);
        blended = vec3(r) / 255.0;
    }
    blended = clamp(blended, 0.0, 1.0);
    float amt = clamp(uOpacity, 0.0, 1.0) * b.a;
    FragColor = vec4(mix(a.rgb, blended, amt), max(a.a, b.a));
}
```

- [ ] **Step 5: Rebuild and verify gl_smoke PASSES**

Run: `cmake --build build -j --target gl_smoke && ctest --test-dir build -R gl_smoke --output-on-failure`
Expected: PASS — including `gl_smoke OK: Compositor shader matches blendPixel (Multiply/Hue/XOR)`. If Hue is off by more than ±3, widen `near` is NOT allowed (it's shared); instead double-check `setSat`/`setLum` match the reference exactly — a >±3 miss means the formulas diverged.

- [ ] **Step 6: Register the node**

In `src/app/Application.cpp`, add the include after `#include "modules/MixNode.h"`:

```cpp
#include "modules/MixNode.h"
#include "modules/CompositorNode.h"
```

Add the `makeNode` branch right after the `if (type == "Mix") ...` line:

```cpp
    if (type == "Mix")         return std::make_unique<MixNode>();
    if (type == "Compositor")  return std::make_unique<CompositorNode>();
```

Add `"Compositor"` to the **Texture** category list, right after `"Mix"`:

```cpp
        { "Texture", { "Colour", "Video", "Mix", "Compositor", "Recorder", "Output" } },
```

- [ ] **Step 7: Add it to the screenshot demo**

In `src/main.cpp`, add this line right after the existing `app.addNodeOfType("Chord Player", ...);` demo line:

```cpp
        app.addNodeOfType("Compositor", glm::vec2(260.0f, 200.0f));
```

- [ ] **Step 8: Build everything and verify the node renders**

Run: `cmake --build build -j && ctest --test-dir build --output-on-failure`
Expected: clean build; all tests pass (`core_tests`, `gl_smoke` incl. the Compositor scenario).

Run: `./build/shader_streamer --screenshot build/_ui.png`
Expected: exit 0. Open `build/_ui.png` with the Read tool and confirm a **Compositor** node renders with inputs `a`, `b`, a `mode` dropdown (showing `Normal`), `opacity`, and an `out` output. If it overlaps another node or is clipped, move it to a clear on-screen spot and re-screenshot; report what you saw.

- [ ] **Step 9: Commit**

```bash
git add src/modules/CompositorNode.h shaders/compositor.frag tests/gl_smoke.cpp src/app/Application.cpp src/main.cpp
git commit -m "$(cat <<'EOF'
feat(modules): add the Compositor node + 23-mode blend shader

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
)"
```

---

### Task 3: Documentation

**Files:**
- Modify: `README.md`, `CLAUDE.md`

- [ ] **Step 1: Add the README module-table row**

In `README.md`, add this row immediately after the `| **Mix** | ... |` row (search for `**Mix**` in the module table):

```markdown
| **Compositor** | blend two textures with a selectable operator (23 modes): add/subtract/difference/exclusion, multiply/screen/overlay, darken/lighten, dodge/burn, hard/soft light, divide/average, the HSL hue/saturation/color/luminosity, and bitwise and/or/xor; plus `opacity` |
```

- [ ] **Step 2: Add the CLAUDE.md architecture bullet**

In `CLAUDE.md`, in the **Architecture** section, add this bullet immediately after the **Chord Player** bullet (it ends with "... Unit-tested in `core_tests`."):

```markdown
- **Compositor** — `CompositorNode` (`src/modules/CompositorNode.h`, header-only) is a
  `ShaderNode` that blends two input textures with a selectable operator. The 23 blend
  modes (arithmetic, the Photoshop-standard separable set, the HSL non-separable
  hue/saturation/color/luminosity, and bitwise and/or/xor) live in `shaders/compositor.frag`,
  which mirrors the GL-free reference `core/BlendModes.h` (`blendPixel` + `blendModeLabels`);
  the reference is unit-tested in `core_tests` and a `gl_smoke` scenario cross-checks the
  shader against it (one mode per code path) so they can't drift.
```

- [ ] **Step 3: Sanity check + commit**

Run: `ctest --test-dir build --output-on-failure`
Expected: all pass (no code changed).

```bash
git add README.md CLAUDE.md
git commit -m "$(cat <<'EOF'
docs: document the Compositor node

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
)"
```

---

## Final verification

- [ ] `cmake --build build -j` — clean build (`shader_streamer`, `core_tests`, `gl_smoke`)
- [ ] `ctest --test-dir build --output-on-failure` — all pass (incl. blend-mode unit tests + the Compositor gl_smoke cross-check)
- [ ] `./build/shader_streamer --screenshot build/_ui.png` — the Compositor node renders with its ports
- [ ] Use superpowers:finishing-a-development-branch
</content>
