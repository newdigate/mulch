# Skybox Node Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a **Skybox** node (6 texture inputs sampled as a cubemap → texture, rotated by a self-spin or the shared World Transform), and extend the shared `Transform` from yaw-only to yaw + pitch so the whole 3D scene tilts together.

**Architecture:** The cubemap is sampled in `shaders/skybox.frag` from 6 separate `sampler2D` via in-shader major-axis face selection (no cubemap object), mirroring the GL-free `core/CubeMap.h` `cubeFaceUV` (unit-tested). The shared `Transform` gains a `pitch` field that the World Transform produces and Wireframe / Shaded Render / Skybox all apply.

**Tech Stack:** C++17, OpenGL 4.1/GLSL 410, glm, doctest, CMake. Design: `docs/superpowers/specs/2026-06-18-skybox-node-design.md`.

**IMPORTANT for the implementer:** `core/CubeMap.h` is **header-only** (inline `cubeFaceUV`) — the spec mentioned a `.cpp`, but no production code calls it (the shader does the face math); only the unit test includes it, so just add the test to `core_tests`, no source/`gl_smoke` change. The `skybox.frag` shader is auto-copied next to the binary. `SkyboxNode.h` is header-only.

---

### Task 1: Shared Transform gains pitch (World Transform + Wireframe + Shaded Render)

**Files:**
- Modify: `tests/test_world_transform.cpp`
- Modify: `src/core/Value.h`
- Modify: `src/modules/WorldTransformNode.h`
- Modify: `src/modules/WireframeNode.cpp`, `src/modules/ShadedRenderNode.cpp`

- [ ] **Step 1: Update + extend the World Transform test (failing)**

In `tests/test_world_transform.cpp`, the existing test passes only the `rate` input; the node now reads a 2nd `pitch` input, so update it to supply both and assert pitch passes through. Replace the first test and add a new one:

```cpp
TEST_CASE("World Transform integrates the rate into an active transform") {
    WorldTransformNode n;
    std::vector<Value> in = { 2.0f, 0.0f };    // rate (port 0) rad/s, pitch (port 1) rad
    std::vector<Value> out(1);
    EvalContext ctx{in, out, 0.5f};            // dt = 0.5 s
    n.evaluate(ctx);
    Transform t = std::get<Transform>(out[0]);
    CHECK(t.active);
    CHECK(t.angle == doctest::Approx(1.0f));   // 2.0 * 0.5
    CHECK(t.pitch == doctest::Approx(0.0f));
    n.evaluate(ctx);                           // accumulates across frames
    CHECK(std::get<Transform>(out[0]).angle == doctest::Approx(2.0f));
}

TEST_CASE("World Transform passes pitch through to the transform") {
    WorldTransformNode n;
    std::vector<Value> in = { 0.0f, 0.7f };    // rate 0, pitch 0.7 rad
    std::vector<Value> out(1);
    EvalContext ctx{in, out, 0.5f};
    n.evaluate(ctx);
    Transform t = std::get<Transform>(out[0]);
    CHECK(t.active);
    CHECK(t.pitch == doctest::Approx(0.7f));
    CHECK(t.angle == doctest::Approx(0.0f));   // rate 0 -> no yaw
}
```

(Leave the second existing test — "a default Transform is inactive…" — unchanged.)

- [ ] **Step 2: Run to verify it fails**

Run: `cmake -S . -B build && cmake --build build -j --target core_tests`
Expected: FAIL — compile error (`Transform` has no member `pitch`).

- [ ] **Step 3: Add `pitch` to the Transform struct**

In `src/core/Value.h`, replace the `Transform` struct (and refresh its comment):

```cpp
// A shared world transform several 3D renderers can align to: a yaw rotation about Y
// plus a pitch about X (radians), with an `active` flag distinguishing a real transform
// (from a World Transform node) from the default an unconnected input carries, so a
// renderer can fall back to rotating itself when nothing is wired in.
struct Transform {
    float angle  = 0.0f;   // yaw, about Y (radians)
    float pitch  = 0.0f;   // pitch, about X (radians)
    bool  active = false;
};
```

- [ ] **Step 4: Produce pitch from the World Transform**

In `src/modules/WorldTransformNode.h`, add the `pitch` input and emit it:

```cpp
    WorldTransformNode() : Node("World Transform") {
        addInput("rate", PortType::Float, 0.5f, -2.0f, 2.0f);    // yaw spin rate (rad/s)
        addInput("pitch", PortType::Float, 0.0f, -1.5f, 1.5f);   // pitch tilt angle (radians)
        addOutput("transform", PortType::Transform);
    }

    void evaluate(EvalContext& ctx) override {
        angle_ += ctx.dt * ctx.in<float>(0);
        ctx.out<Transform>(0, Transform{angle_, ctx.in<float>(1), true});
    }
```

- [ ] **Step 5: Run the World Transform tests to verify they pass**

Run: `cmake --build build -j --target core_tests && ./build/core_tests`
Expected: PASS — all `core_tests` green (the two World Transform cases included).

- [ ] **Step 6: Apply pitch in the renderers**

In `src/modules/WireframeNode.cpp`, replace the angle computation:

```cpp
    Transform tf = ctx.in<Transform>(2);
    float angle;
    if (tf.active) { angle = tf.angle; }
    else { angle_ += ctx.dt * ctx.in<float>(1); angle = angle_; }
```
with:
```cpp
    Transform tf = ctx.in<Transform>(2);
    float yaw, pitch;
    if (tf.active) { yaw = tf.angle; pitch = tf.pitch; }
    else { angle_ += ctx.dt * ctx.in<float>(1); yaw = angle_; pitch = 0.0f; }
```
and replace its model line:
```cpp
        glm::mat4 model = glm::rotate(glm::mat4(1.0f), angle, glm::vec3(0.0f, 1.0f, 0.0f));
```
with:
```cpp
        glm::mat4 model = glm::rotate(glm::mat4(1.0f), yaw, glm::vec3(0.0f, 1.0f, 0.0f))
                        * glm::rotate(glm::mat4(1.0f), pitch, glm::vec3(1.0f, 0.0f, 0.0f));
```

In `src/modules/ShadedRenderNode.cpp`, replace the angle computation:
```cpp
    Transform tf = ctx.in<Transform>(3);
    float angle;
    if (tf.active) { angle = tf.angle; }
    else { angle_ += ctx.dt * ctx.in<float>(2); angle = angle_; }
```
with:
```cpp
    Transform tf = ctx.in<Transform>(3);
    float yaw, pitch;
    if (tf.active) { yaw = tf.angle; pitch = tf.pitch; }
    else { angle_ += ctx.dt * ctx.in<float>(2); yaw = angle_; pitch = 0.0f; }
```
and replace its model line:
```cpp
        glm::mat4 model = glm::rotate(glm::mat4(1.0f), angle, glm::vec3(0, 1, 0));
```
with:
```cpp
        glm::mat4 model = glm::rotate(glm::mat4(1.0f), yaw, glm::vec3(0, 1, 0))
                        * glm::rotate(glm::mat4(1.0f), pitch, glm::vec3(1, 0, 0));
```
(Leave the following `glm::mat3 nrm = glm::mat3(model);` line as-is — the product of two rotations is still orthonormal.)

- [ ] **Step 7: Build everything + verify no regression**

Run: `cmake --build build -j && ctest --test-dir build --output-on-failure`
Expected: clean build; all tests pass. In particular `gl_smoke` Scenario 14 (shared World Transform aligns Wireframe + Shaded) still passes — pitch defaults to 0, so the existing alignment is unchanged.

- [ ] **Step 8: Commit**

```bash
git add src/core/Value.h src/modules/WorldTransformNode.h src/modules/WireframeNode.cpp src/modules/ShadedRenderNode.cpp tests/test_world_transform.cpp
git commit -m "$(cat <<'EOF'
feat(core): add pitch to the shared World Transform (yaw + pitch)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
)"
```

---

### Task 2: GL-free cube-map face helper + unit tests

**Files:**
- Create: `tests/test_cube_map.cpp`
- Create: `src/core/CubeMap.h`
- Modify: `CMakeLists.txt` (add the test to `core_tests`)

- [ ] **Step 1: Write the failing tests**

Create `tests/test_cube_map.cpp`:

```cpp
#include <doctest/doctest.h>
#include "core/CubeMap.h"
#include <glm/vec3.hpp>

using namespace oss;

TEST_CASE("cubeFaceUV selects the right face for each axis, centred at u=v=0.5") {
    CHECK(cubeFaceUV(glm::vec3( 1, 0, 0)).face == 0);   // +X
    CHECK(cubeFaceUV(glm::vec3(-1, 0, 0)).face == 1);   // -X
    CHECK(cubeFaceUV(glm::vec3( 0, 1, 0)).face == 2);   // +Y
    CHECK(cubeFaceUV(glm::vec3( 0,-1, 0)).face == 3);   // -Y
    CHECK(cubeFaceUV(glm::vec3( 0, 0, 1)).face == 4);   // +Z
    CHECK(cubeFaceUV(glm::vec3( 0, 0,-1)).face == 5);   // -Z
    for (glm::vec3 d : { glm::vec3(1,0,0), glm::vec3(0,1,0), glm::vec3(0,0,-1) }) {
        CubeSample c = cubeFaceUV(d);
        CHECK(c.u == doctest::Approx(0.5f));
        CHECK(c.v == doctest::Approx(0.5f));
    }
}

TEST_CASE("cubeFaceUV maps an off-axis direction to the right UV") {
    CubeSample c = cubeFaceUV(glm::vec3(1.0f, 0.5f, 0.0f));   // +X dominant
    CHECK(c.face == 0);
    CHECK(c.u == doctest::Approx(0.5f));    // sc = -d.z = 0 -> 0.5
    CHECK(c.v == doctest::Approx(0.25f));   // tc = -d.y = -0.5, /ma=1 -> (-0.5+1)/2
}

TEST_CASE("cubeFaceUV keeps u,v in [0,1]") {
    for (glm::vec3 d : { glm::vec3(0.7f,0.2f,-0.6f), glm::vec3(-0.3f,0.9f,0.2f),
                         glm::vec3(0.1f,-0.2f,0.95f), glm::vec3(-0.8f,-0.5f,-0.1f) }) {
        CubeSample c = cubeFaceUV(d);
        CHECK(c.u >= 0.0f); CHECK(c.u <= 1.0f);
        CHECK(c.v >= 0.0f); CHECK(c.v <= 1.0f);
    }
}
```

- [ ] **Step 2: Wire the test into the build**

In `CMakeLists.txt`, in the `core_tests` target's test-file list, add `tests/test_cube_map.cpp` right after `tests/test_pitch_graph.cpp`:

```cmake
  tests/test_pitch_graph.cpp
  tests/test_cube_map.cpp
```

- [ ] **Step 3: Run to verify it fails**

Run: `cmake -S . -B build && cmake --build build -j --target core_tests`
Expected: FAIL — compile error `'core/CubeMap.h' file not found`.

- [ ] **Step 4: Write the header**

Create `src/core/CubeMap.h`:

```cpp
#pragma once
#include <cmath>
#include <glm/vec3.hpp>

namespace oss {

// One cube-map sample: which face a direction hits, and the [0,1] UV on that face.
struct CubeSample { int face; float u; float v; };

// OpenGL cube-map major-axis face selection. face 0..5 = +X,-X,+Y,-Y,+Z,-Z. The shader
// (shaders/skybox.frag) mirrors this exactly; keep them in sync.
inline CubeSample cubeFaceUV(const glm::vec3& d) {
    float ax = std::fabs(d.x), ay = std::fabs(d.y), az = std::fabs(d.z);
    int face; float sc, tc, ma;
    if (ax >= ay && ax >= az) {
        ma = ax;
        if (d.x > 0.0f) { face = 0; sc = -d.z; tc = -d.y; }
        else            { face = 1; sc =  d.z; tc = -d.y; }
    } else if (ay >= az) {
        ma = ay;
        if (d.y > 0.0f) { face = 2; sc =  d.x; tc =  d.z; }
        else            { face = 3; sc =  d.x; tc = -d.z; }
    } else {
        ma = az;
        if (d.z > 0.0f) { face = 4; sc =  d.x; tc = -d.y; }
        else            { face = 5; sc = -d.x; tc = -d.y; }
    }
    float inv = ma > 1e-8f ? 0.5f / ma : 0.0f;
    return { face, sc * inv + 0.5f, tc * inv + 0.5f };
}

} // namespace oss
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `cmake --build build -j --target core_tests && ./build/core_tests`
Expected: PASS — all `core_tests` green, including the 3 cube-map cases.

- [ ] **Step 6: Commit**

```bash
git add src/core/CubeMap.h tests/test_cube_map.cpp CMakeLists.txt
git commit -m "$(cat <<'EOF'
feat(core): add a GL-free cube-map face-selection helper

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
)"
```

---

### Task 3: Skybox shader + node + registration + gl_smoke + screenshot

**Files:**
- Create: `src/modules/SkyboxNode.h`, `shaders/skybox.frag`
- Modify: `tests/gl_smoke.cpp`, `src/app/Application.cpp`, `src/main.cpp`

- [ ] **Step 1: Write the node**

Create `src/modules/SkyboxNode.h`:

```cpp
#pragma once
#include <glad/gl.h>
#include <cstddef>
#include "gfx/ShaderNode.h"

namespace oss {

// Renders 6 face textures as a cubemap background into a texture. The fragment shader
// builds a per-pixel view ray (45 deg FOV, -Z forward to match Wireframe/Shaded),
// rotates it by yaw/pitch (the shared World Transform, or a self yaw-spin when none is
// connected), picks the cube face by major axis, and samples the matching face texture.
class SkyboxNode : public ShaderNode {
public:
    SkyboxNode() : ShaderNode("Skybox", "shaders/skybox.frag") {
        addInput("+X", PortType::Texture, TexRef{});
        addInput("-X", PortType::Texture, TexRef{});
        addInput("+Y", PortType::Texture, TexRef{});
        addInput("-Y", PortType::Texture, TexRef{});
        addInput("+Z", PortType::Texture, TexRef{});
        addInput("-Z", PortType::Texture, TexRef{});
        addInput("rotation", PortType::Float, 0.2f, -2.0f, 2.0f);   // self yaw-spin (rad/s)
        addInput("transform", PortType::Transform, Transform{});    // shared World Transform (yaw+pitch)
        addOutput("out", PortType::Texture);
    }
    void evaluate(EvalContext& ctx) override { render(ctx); }

protected:
    void setUniforms(EvalContext& ctx) override {
        Transform tf = ctx.in<Transform>(7);
        float yaw, pitch;
        if (tf.active) { yaw = tf.angle; pitch = tf.pitch; }
        else { spin_ += ctx.dt * ctx.in<float>(6); yaw = spin_; pitch = 0.0f; }

        static const char* names[6] = { "uPX", "uNX", "uPY", "uNY", "uPZ", "uNZ" };
        for (int i = 0; i < 6; ++i) {
            glActiveTexture(GL_TEXTURE0 + (GLenum)i);
            glBindTexture(GL_TEXTURE_2D, ctx.in<TexRef>((std::size_t)i).id);
            glUniform1i(glGetUniformLocation(program_, names[i]), i);
        }
        glActiveTexture(GL_TEXTURE0);
        glUniform1f(glGetUniformLocation(program_, "uYaw"), yaw);
        glUniform1f(glGetUniformLocation(program_, "uPitch"), pitch);
        float aspect = fbo_.height() ? (float)fbo_.width() / (float)fbo_.height() : 1.7778f;
        glUniform1f(glGetUniformLocation(program_, "uAspect"), aspect);
    }

private:
    float spin_ = 0.0f;
};

} // namespace oss
```

- [ ] **Step 2: Add the gl_smoke scenario**

In `tests/gl_smoke.cpp`, add `#include "modules/SkyboxNode.h"` near the other module includes (after `#include "modules/WorldTransformNode.h"`). Then add this scenario immediately before the final `glfwDestroyWindow(win);` line:

```cpp
    // --- Scenario 18: Skybox samples 6 face textures as a cubemap, rotated by yaw/pitch ---
    // Six Colour nodes (distinct colours) -> the 6 Skybox face inputs -> Output. With the
    // transform fixed, the CENTRE pixel (ray (0,0,-1) before rotation) looks at a known face:
    //   yaw 0,    pitch 0     -> -Z (face 5, cyan)
    //   yaw pi/2, pitch 0     -> -X (face 1, green)
    //   yaw 0,    pitch pi/2  -> +Y (face 2, blue)
    {
        const glm::vec4 faceCols[6] = {
            {1,0,0,1}, {0,1,0,1}, {0,0,1,1}, {1,1,0,1}, {1,0,1,1}, {0,1,1,1}   // +X,-X,+Y,-Y,+Z,-Z
        };
        auto centre = [&](float yaw, float pitch, int& r, int& g, int& b) -> bool {
            Graph gr;
            auto sky = std::make_unique<SkyboxNode>();
            sky->inputDefault(7) = Transform{ yaw, pitch, true };   // fixed view
            sky->initGL();
            int skId = gr.addNode(std::move(sky));
            for (int i = 0; i < 6; ++i) {
                auto c = std::make_unique<ColourNode>();
                c->inputDefault(0) = faceCols[i];
                c->initGL();
                int cid = gr.addNode(std::move(c));
                if (!gr.connect(cid, 0, skId, i)) return false;
            }
            auto out = std::make_unique<OutputNode>();
            out->initGL();
            int oId = gr.addNode(std::move(out));
            if (!gr.connect(skId, 0, oId, 0)) return false;
            gr.evaluate(1.0f/60.0f);
            TexRef t = dynamic_cast<OutputNode*>(gr.findNode(oId))->current();
            if (!t.id) return false;
            int a; readCentre(t, r, g, b, a);
            return true;
        };
        const float HALF_PI = 1.57079633f;
        int r, g, b;
        if (!centre(0.0f, 0.0f, r, g, b)    || !(near(r,0) && near(g,255) && near(b,255))) { glfwTerminate(); return fail("skybox centre yaw0/pitch0 not -Z (cyan)"); }
        if (!centre(HALF_PI, 0.0f, r, g, b) || !(near(r,0) && near(g,255) && near(b,0)))   { glfwTerminate(); return fail("skybox yaw pi/2 not -X (green)"); }
        if (!centre(0.0f, HALF_PI, r, g, b) || !(near(r,0) && near(g,0) && near(b,255)))   { glfwTerminate(); return fail("skybox pitch pi/2 not +Y (blue)"); }
        std::fprintf(stderr, "gl_smoke OK: Skybox samples 6 faces with yaw/pitch rotation\n");
    }
```

- [ ] **Step 3: Build gl_smoke and verify it FAILS (shader missing)**

Run: `cmake -S . -B build && cmake --build build -j --target gl_smoke && ctest --test-dir build -R gl_smoke --output-on-failure`
Expected: FAIL — `shaders/skybox.frag` doesn't exist yet, so the Skybox program fails to link and the centre pixel won't be the expected face colour.

- [ ] **Step 4: Write the shader**

Create `shaders/skybox.frag`:

```glsl
#version 410 core
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uPX, uNX, uPY, uNY, uPZ, uNZ;
uniform float uYaw, uPitch, uAspect;

vec3 rotX(vec3 d, float a) { float c = cos(a), s = sin(a); return vec3(d.x, d.y*c - d.z*s, d.y*s + d.z*c); }
vec3 rotY(vec3 d, float a) { float c = cos(a), s = sin(a); return vec3(d.x*c + d.z*s, d.y, -d.x*s + d.z*c); }

void main() {
    vec2 ndc = vUV * 2.0 - 1.0;
    float t = tan(radians(45.0) * 0.5);
    vec3 d = normalize(vec3(ndc.x * uAspect * t, ndc.y * t, -1.0));
    d = rotY(rotX(d, uPitch), uYaw);                 // pitch (about X) then yaw (about Y)

    // Major-axis cube-face selection -- mirrors core/CubeMap.h cubeFaceUV().
    float ax = abs(d.x), ay = abs(d.y), az = abs(d.z);
    int face; float sc, tc, ma;
    if (ax >= ay && ax >= az) {
        ma = ax;
        if (d.x > 0.0) { face = 0; sc = -d.z; tc = -d.y; }
        else           { face = 1; sc =  d.z; tc = -d.y; }
    } else if (ay >= az) {
        ma = ay;
        if (d.y > 0.0) { face = 2; sc =  d.x; tc =  d.z; }
        else           { face = 3; sc =  d.x; tc = -d.z; }
    } else {
        ma = az;
        if (d.z > 0.0) { face = 4; sc =  d.x; tc = -d.y; }
        else           { face = 5; sc = -d.x; tc = -d.y; }
    }
    vec2 uv = vec2(sc, tc) / ma * 0.5 + 0.5;

    vec3 col;
    if      (face == 0) col = texture(uPX, uv).rgb;
    else if (face == 1) col = texture(uNX, uv).rgb;
    else if (face == 2) col = texture(uPY, uv).rgb;
    else if (face == 3) col = texture(uNY, uv).rgb;
    else if (face == 4) col = texture(uPZ, uv).rgb;
    else                col = texture(uNZ, uv).rgb;
    FragColor = vec4(col, 1.0);
}
```

- [ ] **Step 5: Rebuild and verify gl_smoke PASSES**

Run: `cmake --build build -j --target gl_smoke && ctest --test-dir build -R gl_smoke --output-on-failure`
Expected: PASS — including `gl_smoke OK: Skybox samples 6 faces with yaw/pitch rotation`. If a face is wrong, the shader's face/rotation math diverged from the spec — re-check `rotX`/`rotY` and the face table against `core/CubeMap.h`.

- [ ] **Step 6: Register the node**

In `src/app/Application.cpp`, add the include after `#include "modules/WorldTransformNode.h"`:

```cpp
#include "modules/WorldTransformNode.h"
#include "modules/SkyboxNode.h"
```

Add the `makeNode` branch right after the `if (type == "Shaded Render") ...` line:

```cpp
    if (type == "Shaded Render") return std::make_unique<ShadedRenderNode>();
    if (type == "Skybox")        return std::make_unique<SkyboxNode>();
```

Add `"Skybox"` to the **3D** category list, at the end:

```cpp
        { "3D",      { "Mesh Loader", "Text 2D", "Text 3D", "World Transform", "Wireframe", "Shaded Render", "Skybox" } },
```

- [ ] **Step 7: Add it to the screenshot demo**

In `src/main.cpp`, add this line right after the existing `app.addNodeOfType("Pitch Graph", ...);` demo line:

```cpp
        app.addNodeOfType("Skybox", glm::vec2(760.0f, 470.0f));
```

- [ ] **Step 8: Build everything and verify the node renders**

Run: `cmake --build build -j && ctest --test-dir build --output-on-failure`
Expected: clean build; all tests pass (`core_tests`, `gl_smoke` incl. Scenario 18).

Run: `./build/shader_streamer --screenshot build/_ui.png`
Expected: exit 0. Open `build/_ui.png` with the Read tool and confirm a **Skybox** node renders with its six face inputs (`+X`…`-Z`), a `rotation` slider, a `transform` input, and an `out` output. If it overlaps another node or is clipped, move it to a clear on-screen spot and re-screenshot; report what you saw.

- [ ] **Step 9: Commit**

```bash
git add src/modules/SkyboxNode.h shaders/skybox.frag tests/gl_smoke.cpp src/app/Application.cpp src/main.cpp
git commit -m "$(cat <<'EOF'
feat(modules): add the Skybox node + cubemap shader

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
)"
```

---

### Task 4: Documentation

**Files:**
- Modify: `README.md`, `CLAUDE.md`

- [ ] **Step 1: Add the README module-table row**

In `README.md`, add this row immediately after the `| **Wireframe / Shaded Render** | ... |` row (search for `Shaded Render` in the module table):

```markdown
| **Skybox** | 6 face textures (`+X`…`-Z`) → a cubemap background texture; rotated by a self-`rotation` yaw-spin or the shared **World Transform** (now yaw + pitch). Wire `out` into **Output**, or composite a Wireframe / Shaded Render scene over it |
```

Also update the **World Transform** row to mention pitch — change its text to:

```markdown
| **World Transform** | a yaw spin `rate` + a `pitch` tilt → a shared transform; wire it into several renderers' `transform` input (Wireframe, Shaded Render, Skybox) so they rotate together |
```

- [ ] **Step 2: Add the CLAUDE.md architecture bullet**

In `CLAUDE.md`, in the **Architecture** section, add this bullet immediately after the **Pitch Graph** bullet (it ends with "... checks the colours reach the Wireframe texture."):

```markdown
- **Skybox** — `SkyboxNode` (`src/modules/SkyboxNode.h`, header-only) is a `ShaderNode`
  that samples 6 face textures as a cubemap in `shaders/skybox.frag`: a per-pixel view ray
  (45° FOV, −Z forward) is rotated by yaw+pitch, then in-shader major-axis face selection
  (mirroring the GL-free `core/CubeMap.h` `cubeFaceUV`, unit-tested + gl_smoke
  cross-checked) picks and samples the face. It's rotated by a self-spin or the shared
  `Transform`, which now carries **yaw + pitch** — the World Transform produces both and
  Wireframe / Shaded Render / Skybox all apply them (`rotate(yaw,Y)·rotate(pitch,X)`).
```

- [ ] **Step 3: Sanity check + commit**

Run: `ctest --test-dir build --output-on-failure`
Expected: all pass (no code changed).

```bash
git add README.md CLAUDE.md
git commit -m "$(cat <<'EOF'
docs: document the Skybox node + yaw/pitch World Transform

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
)"
```

---

## Final verification

- [ ] `cmake --build build -j` — clean build (`shader_streamer`, `core_tests`, `gl_smoke`)
- [ ] `ctest --test-dir build --output-on-failure` — all pass (cube-map + World Transform unit tests, gl_smoke Scenarios 14 + 18)
- [ ] `./build/shader_streamer --screenshot build/_ui.png` — the Skybox node renders with its 6 faces + rotation + transform
- [ ] Use superpowers:finishing-a-development-branch
</content>
