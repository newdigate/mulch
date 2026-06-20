# Shader Edge + Vertex Deform Node Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a `Shader` edge type, a **Vertex Shader** node (preset GLSL vertex shaders), and a **Deform** node that runs a shader over a vertex buffer via GPU transform feedback — in a new **Shader** node category.

**Architecture:** A GL-free `ShaderRef` (GLSL source string) becomes a new `Value`/`PortType`. Preset shaders live in the GL-free `core/VertexShaders.h`. The `Deform` node compiles the incoming shader as a transform-feedback program (`gfx/GLUtil` `linkFeedbackProgram`) capturing `vPosition`/`vColor`, draws the input VBO as points with rasterizer-discard, and captures into a `Pos3Color3` output VBO.

**Tech Stack:** C++17, OpenGL 4.1/GLSL 410 (transform feedback), glm, doctest, CMake. Design: `docs/superpowers/specs/2026-06-20-vertex-shader-deform-design.md`.

**IMPORTANT for the implementer:** `core/VertexShaders.h`, `VertexShaderNode.h`, `DeformNode.h` are all **header-only** (no `.cpp`, no `APP_SOURCES`/`gl_smoke` CMake source entries). `VertexShaderNode.h` and `core/VertexShaders.h` are GL-free (the node just emits a source string), so `core_tests` can include them. `DeformNode.h` is GL (only `Application.cpp` + `gl_smoke` include it). `gfx/GLUtil.cpp` is already linked everywhere — only a function is added. The only CMake change is adding `tests/test_vertex_shaders.cpp` to `core_tests`.

---

### Task 1: The `Shader` value type

**Files:**
- Create: `tests/test_vertex_shaders.cpp`
- Modify: `src/core/Value.h`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Write the failing test**

Create `tests/test_vertex_shaders.cpp`:

```cpp
#include <doctest/doctest.h>
#include "core/Value.h"
#include <string>

using namespace oss;

TEST_CASE("the Shader port type round-trips through Value and portTypeName") {
    Value v = ShaderRef{ "some glsl" };
    CHECK(typeOf(v) == PortType::Shader);
    CHECK(std::string(portTypeName(PortType::Shader)) == "Shader");
    // the other variant alternatives still map correctly (regression on the typeOf chain)
    CHECK(typeOf(Value{Transform{}}) == PortType::Transform);
    CHECK(typeOf(Value{VertexRef{}}) == PortType::Vertex);
}
```

- [ ] **Step 2: Wire the test into the build**

In `CMakeLists.txt`, in the `core_tests` target's test-file list, add `tests/test_vertex_shaders.cpp` right after `tests/test_cube_map.cpp`:

```cmake
  tests/test_cube_map.cpp
  tests/test_vertex_shaders.cpp
```

- [ ] **Step 3: Run to verify it fails**

Run: `cmake -S . -B build && cmake --build build -j --target core_tests`
Expected: FAIL — compile error (`ShaderRef` / `PortType::Shader` undeclared).

- [ ] **Step 4: Add the Shader type to `src/core/Value.h`**

Add `Shader` to the `PortType` enum:
```cpp
enum class PortType { Texture, Colour, Float, Bool, Audio, String, Midi, Vertex, Transform, Shader };
```

Add the `ShaderRef` struct right after the `Transform` struct:
```cpp
// A GLSL vertex-shader source carried on a Shader edge (a string, so core/ stays GL-free).
// Produced by a Vertex Shader node; consumed by a Deform node.
struct ShaderRef { std::string vertexSrc; };
```

Add `ShaderRef` to the `Value` variant (after `Transform`):
```cpp
using Value = std::variant<float, bool, glm::vec4, std::string, TexRef, AudioRef, MidiRef, VertexRef, Transform, ShaderRef>;
```

In `typeOf`, replace the final catch-all so `Transform` is explicit and `ShaderRef` is the else:
```cpp
        else if constexpr (std::is_same_v<T, VertexRef>)   return PortType::Vertex;
        else if constexpr (std::is_same_v<T, Transform>)   return PortType::Transform;
        else                                                return PortType::Shader;   // ShaderRef
```

In `portTypeName`, add the case before the closing brace of the switch:
```cpp
        case PortType::Transform: return "Transform";
        case PortType::Shader:    return "Shader";
```

- [ ] **Step 5: Run to verify it passes**

Run: `cmake --build build -j --target core_tests && ./build/core_tests`
Expected: PASS — all `core_tests` green, including the new Shader-type case.

- [ ] **Step 6: Commit**

```bash
git add src/core/Value.h tests/test_vertex_shaders.cpp CMakeLists.txt
git commit -m "$(cat <<'EOF'
feat(core): add a Shader edge type (ShaderRef / PortType::Shader)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
)"
```

---

### Task 2: Preset vertex shaders + the Vertex Shader node

**Files:**
- Modify: `tests/test_vertex_shaders.cpp` (append cases)
- Create: `src/core/VertexShaders.h`, `src/modules/VertexShaderNode.h`

- [ ] **Step 1: Write the failing tests**

In `tests/test_vertex_shaders.cpp`, add these includes after the existing `#include <string>`:
```cpp
#include "core/VertexShaders.h"
#include "modules/VertexShaderNode.h"
#include "core/Node.h"
#include <vector>
#include <variant>
```
and append these two test cases at the end of the file:
```cpp
TEST_CASE("vertexShaderSource gives a complete shader for each preset") {
    REQUIRE(vertexShaderLabels().size() == 4);
    for (int i = 0; i < 4; ++i) {
        std::string src = vertexShaderSource(i);
        CHECK(src.find("void main") != std::string::npos);
        CHECK(src.find("vPosition")  != std::string::npos);
        CHECK(src.find("vColor")     != std::string::npos);
        CHECK(src.find("uPos")       != std::string::npos);
        CHECK(src.find("uColour")    != std::string::npos);
    }
    CHECK(vertexShaderSource(99) == vertexShaderSource(3));   // clamps high
    CHECK(vertexShaderSource(-1) == vertexShaderSource(0));   // clamps low
}

TEST_CASE("VertexShaderNode emits the selected preset's source") {
    VertexShaderNode node;
    std::vector<Value> in = { 1.0f };          // preset 1 (Twist)
    std::vector<Value> out(1);
    EvalContext ctx{ in, out, 0.0f };
    node.evaluate(ctx);
    CHECK(std::get<ShaderRef>(out[0]).vertexSrc == vertexShaderSource(1));
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build build -j --target core_tests`
Expected: FAIL — `'core/VertexShaders.h' file not found`.

- [ ] **Step 3: Create `src/core/VertexShaders.h`**

```cpp
#pragma once
#include <string>
#include <vector>

namespace oss {

// Preset transform names for the Vertex Shader node's `preset` dropdown.
inline const std::vector<std::string>& vertexShaderLabels() {
    static const std::vector<std::string> labels = { "Identity", "Twist", "Wave", "Bulge" };
    return labels;
}

// A complete GLSL 410 vertex shader for transform feedback (preset clamped to a valid
// index). Convention the Deform node relies on: in vec3 aPos (loc 0) + aColor (loc 1);
// uniforms uPos (the Deform `position` scalar) + uColour; outputs vPosition + vColor
// (captured by transform feedback varyings {"vPosition","vColor"}). Each preset differs
// only in how it maps aPos -> vPosition; all tint vColor by uColour.
inline std::string vertexShaderSource(int preset) {
    static const char* kPreamble =
        "#version 410 core\n"
        "layout(location = 0) in vec3 aPos;\n"
        "layout(location = 1) in vec3 aColor;\n"
        "uniform float uPos;\n"
        "uniform vec4  uColour;\n"
        "out vec3 vPosition;\n"
        "out vec3 vColor;\n"
        "void main() {\n";
    static const char* kEpilogue =
        "    vColor = aColor + uColour.rgb;\n"            // colourless input (aColor=0) -> uColour
        "    gl_Position = vec4(vPosition, 1.0);\n"       // unused under rasterizer discard
        "}\n";
    static const char* kBodies[4] = {
        "    vPosition = aPos;\n",                                                    // Identity
        "    float a = uPos * aPos.y; float c = cos(a), s = sin(a);\n"
        "    vPosition = vec3(aPos.x*c - aPos.z*s, aPos.y, aPos.x*s + aPos.z*c);\n",  // Twist
        "    vPosition = aPos + vec3(0.0, uPos * sin(aPos.x * 6.2831853), 0.0);\n",   // Wave
        "    vPosition = aPos * (1.0 + uPos * length(aPos));\n",                      // Bulge
    };
    int n = (int)vertexShaderLabels().size();
    if (preset < 0) preset = 0;
    if (preset >= n) preset = n - 1;
    return std::string(kPreamble) + kBodies[preset] + kEpilogue;
}

} // namespace oss
```

- [ ] **Step 4: Create `src/modules/VertexShaderNode.h`**

```cpp
#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include "core/Node.h"
#include "core/Value.h"
#include "core/VertexShaders.h"

namespace oss {

// Emits a preset vertex shader on its Shader output. Wire into a Deform node's `shader`
// input. GL-free -- it just emits a GLSL source string.
class VertexShaderNode : public Node {
public:
    VertexShaderNode() : Node("Vertex Shader") {
        addChoiceInput("preset", vertexShaderLabels(), 0);
        addOutput("shader", PortType::Shader);
    }
    void evaluate(EvalContext& ctx) override {
        int n = (int)vertexShaderLabels().size();
        preset_ = std::clamp((int)std::lround(ctx.in<float>(0)), 0, n - 1);
        ctx.out<ShaderRef>(0, ShaderRef{ vertexShaderSource(preset_) });
    }
    std::string statusLine() const override { return vertexShaderLabels()[(std::size_t)preset_]; }

private:
    int preset_ = 0;
};

} // namespace oss
```

- [ ] **Step 5: Run to verify they pass**

Run: `cmake --build build -j --target core_tests && ./build/core_tests`
Expected: PASS — all `core_tests` green, including the preset + VertexShaderNode cases.

- [ ] **Step 6: Commit**

```bash
git add src/core/VertexShaders.h src/modules/VertexShaderNode.h tests/test_vertex_shaders.cpp
git commit -m "$(cat <<'EOF'
feat(modules): add preset vertex shaders + the Vertex Shader node

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
)"
```

---

### Task 3: Transform-feedback helper + Deform node + registration + gl_smoke

**Files:**
- Modify: `src/gfx/GLUtil.h`, `src/gfx/GLUtil.cpp`
- Create: `src/modules/DeformNode.h`
- Modify: `tests/gl_smoke.cpp`, `src/app/Application.cpp`, `src/main.cpp`

- [ ] **Step 1: Add the transform-feedback program helper**

In `src/gfx/GLUtil.h`, add `#include <vector>` (after `#include <string>`) and declare the function (after `linkProgram`):
```cpp
GLuint linkFeedbackProgram(const std::string& vertSrc, const std::vector<const char*>& varyings);
```

In `src/gfx/GLUtil.cpp`, add the implementation after `linkProgram`:
```cpp
GLuint linkFeedbackProgram(const std::string& vertSrc, const std::vector<const char*>& varyings) {
    GLuint vs = compileShader(GL_VERTEX_SHADER, vertSrc);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, "#version 410 core\nvoid main() {}\n");
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs); glAttachShader(prog, fs);
    glTransformFeedbackVaryings(prog, (GLsizei)varyings.size(), varyings.data(), GL_INTERLEAVED_ATTRIBS);
    glLinkProgram(prog);
    glDeleteShader(vs); glDeleteShader(fs);
    GLint ok = 0; glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        GLint len = 0; glGetProgramiv(prog, GL_INFO_LOG_LENGTH, &len);
        std::vector<char> log(len > 1 ? len : 1);
        glGetProgramInfoLog(prog, (GLsizei)log.size(), nullptr, log.data());
        std::fprintf(stderr, "[feedback program link error]\n%s\n", log.data());
        glDeleteProgram(prog);
        return 0;
    }
    return prog;
}
```

- [ ] **Step 2: Create `src/modules/DeformNode.h`**

```cpp
#pragma once
#include <glad/gl.h>
#include <string>
#include <vector>
#include <glm/vec4.hpp>
#include "core/Node.h"
#include "core/Value.h"
#include "gfx/GLUtil.h"

namespace oss {

// Runs a vertex shader (from a Shader edge) over an input vertex buffer via GPU transform
// feedback, capturing the transformed vertices into a Pos3Color3 output buffer. `position`
// (uPos) and `colour` (uColour) drive the shader; the input may be Pos3 or Pos3Color3.
// No/invalid shader -> the input passes through unchanged.
class DeformNode : public Node {
public:
    DeformNode() : Node("Deform") {
        addInput("geometry", PortType::Vertex, VertexRef{});
        addInput("position", PortType::Float, 0.5f, -2.0f, 2.0f);
        addInput("colour", PortType::Colour, glm::vec4(1.0f));
        addInput("shader", PortType::Shader, ShaderRef{});
        addOutput("geometry", PortType::Vertex);
    }
    ~DeformNode() override {
        if (program_) glDeleteProgram(program_);
        if (vao_) glDeleteVertexArrays(1, &vao_);
        if (outVbo_) glDeleteBuffers(1, &outVbo_);
    }
    void initGL() override { glGenVertexArrays(1, &vao_); glGenBuffers(1, &outVbo_); }

    void evaluate(EvalContext& ctx) override {
        const ShaderRef& sh = ctx.in<ShaderRef>(3);
        if (sh.vertexSrc != cachedSrc_) {        // (re)compile only when the source changes
            if (program_) { glDeleteProgram(program_); program_ = 0; }
            if (!sh.vertexSrc.empty())
                program_ = linkFeedbackProgram(sh.vertexSrc, { "vPosition", "vColor" });
            cachedSrc_ = sh.vertexSrc;
            status_ = sh.vertexSrc.empty() ? "no shader" : (program_ ? "ok" : "compile error");
        }

        VertexRef in = ctx.in<VertexRef>(0);
        if (program_ == 0 || in.vbo == 0 || in.count <= 0) {
            out_ = in;                            // pass through
            ctx.out<VertexRef>(0, out_);
            return;
        }

        if (in.count > outCap_) {                 // grow the capture buffer on demand
            glBindBuffer(GL_ARRAY_BUFFER, outVbo_);
            glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)in.count * 6 * sizeof(float), nullptr, GL_DYNAMIC_COPY);
            glBindBuffer(GL_ARRAY_BUFFER, 0);
            outCap_ = in.count;
        }

        glUseProgram(program_);
        glUniform1f(glGetUniformLocation(program_, "uPos"), ctx.in<float>(1));
        glm::vec4 c = ctx.in<glm::vec4>(2);
        glUniform4f(glGetUniformLocation(program_, "uColour"), c.r, c.g, c.b, c.a);

        glBindVertexArray(vao_);
        glBindBuffer(GL_ARRAY_BUFFER, in.vbo);
        bool hasColor = (in.format == VertexFormat::Pos3Color3);
        GLsizei stride = (in.format == VertexFormat::Pos3) ? 3 * sizeof(float) : 6 * sizeof(float);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (void*)0);
        if (hasColor) {
            glEnableVertexAttribArray(1);
            glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, (void*)(3 * sizeof(float)));
        } else {
            glDisableVertexAttribArray(1);
            glVertexAttrib4f(1, 0.0f, 0.0f, 0.0f, 1.0f);   // pin aColor=0 (generic attrib is context state)
        }

        glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, outVbo_);
        glEnable(GL_RASTERIZER_DISCARD);
        glBeginTransformFeedback(GL_POINTS);
        glDrawArrays(GL_POINTS, 0, in.count);
        glEndTransformFeedback();
        glDisable(GL_RASTERIZER_DISCARD);
        glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, 0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glBindVertexArray(0);

        out_ = VertexRef{ outVbo_, in.count, in.primitive, VertexFormat::Pos3Color3 };
        ctx.out<VertexRef>(0, out_);
    }

    VertexRef   output() const { return out_; }        // last published geometry (for tests)
    std::string statusLine() const override { return status_; }

private:
    GLuint program_ = 0, vao_ = 0, outVbo_ = 0;
    int    outCap_ = 0;
    VertexRef   out_;
    std::string cachedSrc_, status_ = "no shader";
};

} // namespace oss
```

- [ ] **Step 3: Add the gl_smoke scenario**

In `tests/gl_smoke.cpp`, add these includes near the other module includes (after `#include "modules/WorldTransformNode.h"`):
```cpp
#include "modules/DeformNode.h"
#include "modules/VertexShaderNode.h"
#include "core/VertexShaders.h"
```
Then add this scenario immediately before the final `glfwDestroyWindow(win);` line:
```cpp
    // --- Scenario 19: Deform runs a vertex shader over a VBO via transform feedback ---
    // A 1-vertex input VBO (Pos3) + a known preset shader -> Deform; read the transform-
    // feedback output (Pos3Color3, 6 floats) back and verify the GPU transform exactly.
    {
        const float inPos[3] = { 0.2f, 0.3f, 0.4f };
        GLuint inVbo = 0;
        glGenBuffers(1, &inVbo); glBindBuffer(GL_ARRAY_BUFFER, inVbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(inPos), inPos, GL_STATIC_DRAW);
        glBindBuffer(GL_ARRAY_BUFFER, 0);

        auto runDeform = [&](int preset, float pos, glm::vec4 col, float o[6]) -> bool {
            Graph g;
            auto def = std::make_unique<DeformNode>();
            def->inputDefault(0) = VertexRef{ inVbo, 1, Primitive::Lines, VertexFormat::Pos3 };
            def->inputDefault(1) = pos;
            def->inputDefault(2) = col;
            def->inputDefault(3) = ShaderRef{ vertexShaderSource(preset) };
            def->initGL();
            int dId = g.addNode(std::move(def));
            g.evaluate(1.0f / 60.0f);
            VertexRef out = dynamic_cast<DeformNode*>(g.findNode(dId))->output();
            if (out.vbo == 0 || out.count != 1 || out.format != VertexFormat::Pos3Color3) return false;
            glBindBuffer(GL_ARRAY_BUFFER, out.vbo);
            glGetBufferSubData(GL_ARRAY_BUFFER, 0, 6 * sizeof(float), o);
            glBindBuffer(GL_ARRAY_BUFFER, 0);
            return true;
        };
        auto af = [](float a, float b) { return std::fabs(a - b) < 1e-3f; };

        float o[6];
        // Identity: vPosition = aPos; vColor = aColor(0) + uColour.rgb = colour.
        if (!runDeform(0, 0.5f, glm::vec4(0.6f, 0.7f, 0.8f, 1.0f), o)) { glfwTerminate(); return fail("Deform identity produced no output"); }
        if (!(af(o[0],0.2f) && af(o[1],0.3f) && af(o[2],0.4f) && af(o[3],0.6f) && af(o[4],0.7f) && af(o[5],0.8f))) {
            std::fprintf(stderr, "Deform identity got (%.3f,%.3f,%.3f, %.3f,%.3f,%.3f)\n", o[0],o[1],o[2],o[3],o[4],o[5]);
            glDeleteBuffers(1, &inVbo); glfwTerminate(); return fail("Deform identity transform wrong");
        }
        // Wave: y += uPos*sin(x*2pi); x=0.2, uPos=0.5 -> y = 0.3 + 0.5*sin(0.2*2pi); x,z unchanged.
        if (!runDeform(2, 0.5f, glm::vec4(0, 0, 0, 1), o)) { glfwTerminate(); return fail("Deform wave produced no output"); }
        float ey = 0.3f + 0.5f * std::sin(0.2f * 6.2831853f);
        if (!(af(o[0],0.2f) && af(o[1],ey) && af(o[2],0.4f))) {
            std::fprintf(stderr, "Deform wave got y=%.4f expected %.4f\n", o[1], ey);
            glDeleteBuffers(1, &inVbo); glfwTerminate(); return fail("Deform wave transform wrong");
        }
        glDeleteBuffers(1, &inVbo);

        // The new Shader edge wires end to end (VertexShader -> Deform.shader).
        {
            Graph g;
            auto vs = std::make_unique<VertexShaderNode>();
            auto def = std::make_unique<DeformNode>();
            def->initGL();
            int vsId = g.addNode(std::move(vs));
            int dId  = g.addNode(std::move(def));
            if (!g.connect(vsId, 0, dId, 3)) { glfwTerminate(); return fail("Shader edge VertexShader->Deform did not connect"); }
        }
        std::fprintf(stderr, "gl_smoke OK: Deform applies a vertex shader via transform feedback\n");
    }
```

- [ ] **Step 4: Build gl_smoke and verify it PASSES**

Run: `cmake -S . -B build && cmake --build build -j --target gl_smoke && ctest --test-dir build -R gl_smoke --output-on-failure`
Expected: PASS — including `gl_smoke OK: Deform applies a vertex shader via transform feedback`. If the identity readback is wrong, the transform-feedback setup (varyings / attrib binding / the `glVertexAttrib4f` pin) diverged from the plan.

- [ ] **Step 5: Register the nodes**

In `src/app/Application.cpp`, add the includes after `#include "modules/WorldTransformNode.h"`:
```cpp
#include "modules/VertexShaderNode.h"
#include "modules/DeformNode.h"
```
Add the `makeNode` branches right after the `if (type == "Skybox") ...` line:
```cpp
    if (type == "Skybox")        return std::make_unique<SkyboxNode>();
    if (type == "Vertex Shader") return std::make_unique<VertexShaderNode>();
    if (type == "Deform")        return std::make_unique<DeformNode>();
```
Add a new **Shader** category to `nodeCategories()`, after the `Control` entry:
```cpp
        { "Control", { "Automation", "LFO" } },
        { "Shader",  { "Vertex Shader", "Deform" } },
```

- [ ] **Step 6: Add them to the screenshot demo**

In `src/main.cpp`, add these lines right after the existing `app.addNodeOfType("Skybox", ...);` demo line:
```cpp
        app.addNodeOfType("Vertex Shader", glm::vec2(40.0f, 470.0f));
        app.addNodeOfType("Deform", glm::vec2(240.0f, 470.0f));
```

- [ ] **Step 7: Build everything + verify the nodes render**

Run: `cmake --build build -j && ctest --test-dir build --output-on-failure`
Expected: clean build; all tests pass (`core_tests`, `gl_smoke` incl. Scenario 19).

Run: `./build/shader_streamer --screenshot build/_ui.png`
Expected: exit 0. Open `build/_ui.png` with the Read tool and confirm a **Vertex Shader** node (with a `preset` dropdown + `shader` output) and a **Deform** node (with `geometry`/`position`/`colour`/`shader` inputs + a `geometry` output) render. If they overlap another node or are clipped, move them to a clear on-screen spot and re-screenshot; report what you saw.

- [ ] **Step 8: Commit**

```bash
git add src/gfx/GLUtil.h src/gfx/GLUtil.cpp src/modules/DeformNode.h tests/gl_smoke.cpp src/app/Application.cpp src/main.cpp
git commit -m "$(cat <<'EOF'
feat(modules): add the Deform node + transform-feedback program helper

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
)"
```

---

### Task 4: Documentation

**Files:**
- Modify: `README.md`, `CLAUDE.md`

- [ ] **Step 1: Add the README module-table rows**

In `README.md`, add these two rows immediately after the `| **Skybox** | ... |` row (search for `**Skybox**` in the module table):

```markdown
| **Vertex Shader** | pick a preset transform (Identity / Twist / Wave / Bulge) → a **Shader** edge (a new input kind carrying a GLSL vertex shader). Wire `shader` into a **Deform** node |
| **Deform** | runs a vertex shader (the `shader` input) over a vertex buffer via GPU transform feedback → a colored vertex buffer; `position` and `colour` drive the shader. Wire `geometry` into **Wireframe** / **Shaded Render** |
```

- [ ] **Step 2: Add the CLAUDE.md architecture bullet**

In `CLAUDE.md`, in the **Architecture** section, add this bullet immediately after the **Skybox** bullet (it ends with "... (`rotate(yaw,Y)·rotate(pitch,X)`)."):

```markdown
- **Shader edge + Deform** — a new `Shader` `PortType` carries a `ShaderRef` (a GLSL
  vertex-shader source string, GL-free) on an edge. The **Vertex Shader** node
  (`src/modules/VertexShaderNode.h`, header-only, GL-free) emits a preset from the GL-free
  `core/VertexShaders.h` (Identity/Twist/Wave/Bulge). The **Deform** node
  (`src/modules/DeformNode.h`, header-only) compiles the incoming shader as a
  transform-feedback program (`gfx/GLUtil` `linkFeedbackProgram`, capturing
  `vPosition`/`vColor`), draws the input VBO as `GL_POINTS` with `GL_RASTERIZER_DISCARD`,
  and captures the transformed vertices into a `Pos3Color3` output VBO (driven by the
  `position`/`colour` uniforms; `aColor` pinned to 0 when the input has no colour). Both
  live in the new **Shader** node category. Unit-tested in `core_tests`; the transform
  feedback is `gl_smoke`-verified by reading the output buffer back.
```

- [ ] **Step 3: Sanity check + commit**

Run: `ctest --test-dir build --output-on-failure`
Expected: all pass (no code changed).

```bash
git add README.md CLAUDE.md
git commit -m "$(cat <<'EOF'
docs: document the Shader edge + Vertex Shader / Deform nodes

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
)"
```

---

## Final verification

- [ ] `cmake --build build -j` — clean build (`shader_streamer`, `core_tests`, `gl_smoke`)
- [ ] `ctest --test-dir build --output-on-failure` — all pass (Shader type + preset/node unit tests, gl_smoke Scenario 19)
- [ ] `./build/shader_streamer --screenshot build/_ui.png` — the Vertex Shader + Deform nodes render
- [ ] Use superpowers:finishing-a-development-branch
</content>
