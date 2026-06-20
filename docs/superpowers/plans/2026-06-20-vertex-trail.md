# Vertex Trail Node Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A **Vertex Trail** node that snapshots the incoming vertex buffer each frame into a queue of the last N copies, each offset in Z and hue-rotated by its age, emitted as one `Pos3Color3` buffer.

**Architecture:** GL-free queue + recolour logic in `core/VertexTrail` (unit-tested) with a thin GL node owning the VBO (mirrors `Oscilloscope`/`PitchGraph`). HSV helpers move to a shared GL-free `core/ColorHsv.h` (`hsvToRgb` + new `rgbToHsv`). The node reads the input VBO back to CPU (`glGetBufferSubData`) each frame, pushes, rebuilds, uploads.

**Tech Stack:** C++17, OpenGL 4.1, glm, doctest (`core_tests`), `gl_smoke` (headless GL), CMake. Design: `docs/superpowers/specs/2026-06-20-vertex-trail-design.md`.

**Notes for the implementer:**
- `core/ColorHsv.h` and `src/modules/VertexTrailNode.h` are **header-only** (no CMake source entry). `core/VertexTrail.cpp` is a compiled GL-free source — it goes in all three targets (`APP_SOURCES`, `core_tests`, `gl_smoke`), like `src/core/Oscilloscope.cpp`.
- `VertexFormat` / `Primitive` / `VertexRef` live in `src/core/Value.h`: `Pos3` = 3 floats/vertex; `Pos3Color3` = pos@0 + rgb@12, 6 floats, stride 24; `Pos3Normal3` = 6 floats (normal ignored here).

---

### Task 1: `core/ColorHsv.h` — shared HSV helpers

**Files:** Create `src/core/ColorHsv.h`, `tests/test_color_hsv.cpp`; Modify `src/core/PitchGraph.h`, `src/core/PitchGraph.cpp`, `CMakeLists.txt`.

- [ ] **Step 1: Write the failing test**

Create `tests/test_color_hsv.cpp`:
```cpp
#include <doctest/doctest.h>
#include "core/ColorHsv.h"

using namespace oss;

TEST_CASE("hsvToRgb hits the primary colours") {
    glm::vec3 r = hsvToRgb(0.0f, 1.0f, 1.0f);
    CHECK(r.x == doctest::Approx(1.0f)); CHECK(r.y == doctest::Approx(0.0f)); CHECK(r.z == doctest::Approx(0.0f));
    glm::vec3 g = hsvToRgb(1.0f/3.0f, 1.0f, 1.0f);
    CHECK(g.x == doctest::Approx(0.0f)); CHECK(g.y == doctest::Approx(1.0f)); CHECK(g.z == doctest::Approx(0.0f));
    glm::vec3 b = hsvToRgb(2.0f/3.0f, 1.0f, 1.0f);
    CHECK(b.x == doctest::Approx(0.0f)); CHECK(b.y == doctest::Approx(0.0f)); CHECK(b.z == doctest::Approx(1.0f));
}

TEST_CASE("rgbToHsv inverts hsvToRgb") {
    glm::vec3 hsv = rgbToHsv(1.0f, 0.0f, 0.0f);
    CHECK(hsv.x == doctest::Approx(0.0f));
    CHECK(hsv.y == doctest::Approx(1.0f));
    CHECK(hsv.z == doctest::Approx(1.0f));
    for (glm::vec3 c : { glm::vec3(0.8f,0.2f,0.4f), glm::vec3(0.1f,0.6f,0.9f), glm::vec3(0.5f,0.5f,0.2f) }) {
        glm::vec3 h  = rgbToHsv(c.x, c.y, c.z);
        glm::vec3 rt = hsvToRgb(h.x, h.y, h.z);
        CHECK(rt.x == doctest::Approx(c.x)); CHECK(rt.y == doctest::Approx(c.y)); CHECK(rt.z == doctest::Approx(c.z));
    }
}

TEST_CASE("rgbToHsv: grey has zero saturation") {
    glm::vec3 hsv = rgbToHsv(0.5f, 0.5f, 0.5f);
    CHECK(hsv.y == doctest::Approx(0.0f));
    CHECK(hsv.z == doctest::Approx(0.5f));
}
```

- [ ] **Step 2: Wire test into the build**

In `CMakeLists.txt`, in the `core_tests` source list, add after `tests/test_vertex_shaders.cpp`:
```cmake
  tests/test_color_hsv.cpp
```

- [ ] **Step 3: Run to verify it fails**

Run: `cmake -S . -B build && cmake --build build -j --target core_tests`
Expected: FAIL — `'core/ColorHsv.h' file not found`.

- [ ] **Step 4: Create `src/core/ColorHsv.h`**

```cpp
#pragma once
#include <algorithm>
#include <cmath>
#include <glm/vec3.hpp>

namespace oss {

// HSV (each component in [0,1]; hue wraps) -> RGB in [0,1]. GL-free.
inline glm::vec3 hsvToRgb(float h, float s, float v) {
    h = h - std::floor(h);                 // wrap to [0,1)
    float i = std::floor(h * 6.0f);
    float f = h * 6.0f - i;
    float p = v * (1.0f - s);
    float q = v * (1.0f - s * f);
    float t = v * (1.0f - s * (1.0f - f));
    switch (((int)i) % 6) {
        case 0:  return glm::vec3(v, t, p);
        case 1:  return glm::vec3(q, v, p);
        case 2:  return glm::vec3(p, v, t);
        case 3:  return glm::vec3(p, q, v);
        case 4:  return glm::vec3(t, p, v);
        default: return glm::vec3(v, p, q);
    }
}

// RGB in [0,1] -> HSV (each in [0,1]; hue wraps). Inverse of hsvToRgb. GL-free.
inline glm::vec3 rgbToHsv(float r, float g, float b) {
    float mx = std::max(r, std::max(g, b));
    float mn = std::min(r, std::min(g, b));
    float d  = mx - mn;
    float h  = 0.0f;
    if (d > 1e-6f) {
        if      (mx == r) h = (g - b) / d + (g < b ? 6.0f : 0.0f);
        else if (mx == g) h = (b - r) / d + 2.0f;
        else              h = (r - g) / d + 4.0f;
        h /= 6.0f;
    }
    float s = mx > 1e-6f ? d / mx : 0.0f;
    return glm::vec3(h, s, mx);
}

} // namespace oss
```

- [ ] **Step 5: Move `hsvToRgb` out of PitchGraph**

In `src/core/PitchGraph.h`: add `#include "core/ColorHsv.h"` near the top (with the other includes), and DELETE the `hsvToRgb` forward declaration and its `// HSV ... GL-free.` comment line (the line `glm::vec3 hsvToRgb(float h, float s, float v);`).

In `src/core/PitchGraph.cpp`: DELETE the entire `hsvToRgb` function definition (the `glm::vec3 hsvToRgb(...) { ... }` block, ~lines 7–22). `PitchGraph.cpp` includes `PitchGraph.h` (which now pulls in `ColorHsv.h`), so its `build()` still resolves `hsvToRgb`. Leave the rest of the file unchanged.

- [ ] **Step 6: Run to verify it passes (and nothing regressed)**

Run: `cmake --build build -j --target core_tests && ./build/core_tests`
Expected: PASS — all core_tests green, including `test_color_hsv` AND the existing `test_pitch_graph` (which gets `hsvToRgb` via the header now).

- [ ] **Step 7: Commit**

```bash
git add src/core/ColorHsv.h src/core/PitchGraph.h src/core/PitchGraph.cpp tests/test_color_hsv.cpp CMakeLists.txt
git commit -m "$(cat <<'EOF'
refactor(core): extract HSV helpers to ColorHsv.h + add rgbToHsv

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
)"
```

---

### Task 2: `core/VertexTrail` — the GL-free snapshot queue

**Files:** Create `src/core/VertexTrail.h`, `src/core/VertexTrail.cpp`, `tests/test_vertex_trail.cpp`; Modify `CMakeLists.txt`.

- [ ] **Step 1: Write the failing test**

Create `tests/test_vertex_trail.cpp`:
```cpp
#include <doctest/doctest.h>
#include "core/VertexTrail.h"
#include "core/ColorHsv.h"
#include "core/Value.h"
#include <vector>

using namespace oss;

TEST_CASE("a single Pos3 snapshot builds to a red vertex at its position") {
    VertexTrail t;
    float v[3] = {1.0f, 2.0f, 3.0f};
    t.push(v, 1, VertexFormat::Pos3, Primitive::Lines);
    std::vector<float> out;
    int n = t.build(0.5f, 0.1f, out);
    REQUIRE(n == 1);
    REQUIRE(out.size() == 6);
    CHECK(out[0] == doctest::Approx(1.0f));
    CHECK(out[1] == doctest::Approx(2.0f));
    CHECK(out[2] == doctest::Approx(3.0f));     // age 0, no z offset
    CHECK(out[3] == doctest::Approx(1.0f));     // red base
    CHECK(out[4] == doctest::Approx(0.0f));
    CHECK(out[5] == doctest::Approx(0.0f));
    CHECK(t.frameCount() == 1);
    CHECK(t.primitive() == Primitive::Lines);
}

TEST_CASE("older snapshots get z-offset and hue rotation by age") {
    VertexTrail t;
    float a[3] = {0.0f, 0.0f, 0.0f};   // pushed first  -> age 1
    float b[3] = {0.0f, 0.0f, 0.0f};   // pushed second -> age 0 (newest)
    t.push(a, 1, VertexFormat::Pos3, Primitive::Lines);
    t.push(b, 1, VertexFormat::Pos3, Primitive::Lines);
    std::vector<float> out;
    int n = t.build(0.5f, 0.1f, out);
    REQUIRE(n == 2);
    CHECK(out[2] == doctest::Approx(0.0f));     // age 0: z 0
    CHECK(out[3] == doctest::Approx(1.0f));     // age 0: red
    glm::vec3 c = hsvToRgb(0.1f, 1.0f, 1.0f);
    CHECK(out[8]  == doctest::Approx(0.5f));    // age 1: z += 0.5
    CHECK(out[9]  == doctest::Approx(c.x));     // age 1: hue rotated 0.1
    CHECK(out[10] == doctest::Approx(c.y));
    CHECK(out[11] == doctest::Approx(c.z));
}

TEST_CASE("setMaxFrames prunes the oldest snapshots") {
    VertexTrail t;
    float v[3] = {0.0f, 0.0f, 0.0f};
    for (int i = 0; i < 5; ++i) t.push(v, 1, VertexFormat::Pos3, Primitive::Lines);
    t.setMaxFrames(2);
    CHECK(t.frameCount() == 2);
    std::vector<float> out;
    CHECK(t.build(0.5f, 0.0f, out) == 2);
}

TEST_CASE("a Pos3Color3 snapshot keeps its colour at age 0") {
    VertexTrail t;
    float v[6] = {1.0f, 2.0f, 3.0f, 0.0f, 1.0f, 0.0f};   // green
    t.push(v, 1, VertexFormat::Pos3Color3, Primitive::Lines);
    std::vector<float> out;
    t.build(0.5f, 0.1f, out);
    CHECK(out[3] == doctest::Approx(0.0f));
    CHECK(out[4] == doctest::Approx(1.0f));
    CHECK(out[5] == doctest::Approx(0.0f));   // green preserved (age 0, no hue shift)
}
```

- [ ] **Step 2: Wire into the build**

In `CMakeLists.txt`:
- `core_tests` source list — add after `tests/test_color_hsv.cpp`:
```cmake
  tests/test_vertex_trail.cpp
```
- `APP_SOURCES` — add after `src/core/PitchGraph.cpp`:
```cmake
  src/core/VertexTrail.cpp
```
- `core_tests` core-deps (the `src/core/*.cpp` block inside `add_executable(core_tests ...)`) — add after its `src/core/PitchGraph.cpp` line:
```cmake
  src/core/VertexTrail.cpp
```
- `gl_smoke` source list — add after its `src/core/PitchGraph.cpp` line:
```cmake
  src/core/VertexTrail.cpp
```

- [ ] **Step 3: Run to verify it fails**

Run: `cmake -S . -B build && cmake --build build -j --target core_tests`
Expected: FAIL — `'core/VertexTrail.h' file not found`.

- [ ] **Step 4: Create `src/core/VertexTrail.h`**

```cpp
#pragma once
#include <deque>
#include <vector>
#include "core/Value.h"   // VertexFormat, Primitive

namespace oss {

// A FIFO queue of geometry snapshots for the Vertex Trail node. GL-free. Each snapshot is
// normalised to flat Pos3Color3 floats (x,y,z,r,g,b per vertex). build() concatenates the
// queue, offsetting each snapshot in Z and rotating its hue by its age (0 = newest).
class VertexTrail {
public:
    // Capture a snapshot of `count` vertices in `fmt` layout (Pos3 = 3 floats/vertex,
    // Pos3Normal3 / Pos3Color3 = 6). A Pos3Color3 input keeps its colour; a colourless input
    // gets a red base (hue 0). Newest goes to the front; prunes beyond maxFrames.
    void push(const float* verts, int count, VertexFormat fmt, Primitive prim);
    void setMaxFrames(int n);          // clamp to >= 1 and prune the oldest beyond n
    // Concatenate all snapshots as flat Pos3Color3 floats. For age k (0 = newest):
    //   pos.z += k * zSpacing;  colour hue rotated by k * hueRate.
    // Returns the total vertex count (out.size() == returned * 6).
    int build(float zSpacing, float hueRate, std::vector<float>& out) const;
    int frameCount() const { return (int)snaps_.size(); }
    Primitive primitive() const { return snaps_.empty() ? Primitive::LineStrip : snaps_.front().prim; }

private:
    struct Snapshot { std::vector<float> pc; int count; Primitive prim; };   // pc = 6 floats/vertex
    std::deque<Snapshot> snaps_;       // front = newest (age 0)
    int maxFrames_ = 16;
};

} // namespace oss
```

- [ ] **Step 5: Create `src/core/VertexTrail.cpp`**

```cpp
#include "core/VertexTrail.h"
#include "core/ColorHsv.h"
#include <glm/vec3.hpp>
#include <cstddef>
#include <utility>

namespace oss {

void VertexTrail::push(const float* verts, int count, VertexFormat fmt, Primitive prim) {
    Snapshot s;
    s.count = count;
    s.prim  = prim;
    s.pc.resize((std::size_t)count * 6);
    int  fl       = (fmt == VertexFormat::Pos3) ? 3 : 6;
    bool hasColor = (fmt == VertexFormat::Pos3Color3);
    for (int i = 0; i < count; ++i) {
        const float* v = verts + (std::size_t)i * fl;
        float*       o = s.pc.data() + (std::size_t)i * 6;
        o[0] = v[0]; o[1] = v[1]; o[2] = v[2];
        if (hasColor) { o[3] = v[3]; o[4] = v[4]; o[5] = v[5]; }
        else          { o[3] = 1.0f; o[4] = 0.0f; o[5] = 0.0f; }   // red base (hue 0)
    }
    snaps_.push_front(std::move(s));
    while ((int)snaps_.size() > maxFrames_) snaps_.pop_back();
}

void VertexTrail::setMaxFrames(int n) {
    maxFrames_ = n < 1 ? 1 : n;
    while ((int)snaps_.size() > maxFrames_) snaps_.pop_back();
}

int VertexTrail::build(float zSpacing, float hueRate, std::vector<float>& out) const {
    out.clear();
    int total = 0, k = 0;
    for (const Snapshot& s : snaps_) {        // front (k=0) = newest
        float zoff = (float)k * zSpacing;
        float hoff = (float)k * hueRate;
        for (int i = 0; i < s.count; ++i) {
            const float* v = s.pc.data() + (std::size_t)i * 6;
            glm::vec3 hsv = rgbToHsv(v[3], v[4], v[5]);
            glm::vec3 rgb = hsvToRgb(hsv.x + hoff, hsv.y, hsv.z);
            out.push_back(v[0]);
            out.push_back(v[1]);
            out.push_back(v[2] + zoff);
            out.push_back(rgb.x);
            out.push_back(rgb.y);
            out.push_back(rgb.z);
            ++total;
        }
        ++k;
    }
    return total;
}

} // namespace oss
```

- [ ] **Step 6: Run to verify it passes**

Run: `cmake -S . -B build && cmake --build build -j --target core_tests && ./build/core_tests`
Expected: PASS — all green, including the four `VertexTrail` cases.

- [ ] **Step 7: Commit**

```bash
git add src/core/VertexTrail.h src/core/VertexTrail.cpp tests/test_vertex_trail.cpp CMakeLists.txt
git commit -m "$(cat <<'EOF'
feat(core): add VertexTrail snapshot queue (z-offset + hue by age)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
)"
```

---

### Task 3: `VertexTrailNode` + registration + gl_smoke

**Files:** Create `src/modules/VertexTrailNode.h`; Modify `tests/gl_smoke.cpp`, `src/app/Application.cpp`, `src/main.cpp`.

- [ ] **Step 1: Create `src/modules/VertexTrailNode.h`**

```cpp
#pragma once
#include <glad/gl.h>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>
#include "core/Node.h"
#include "core/Value.h"
#include "core/VertexTrail.h"

namespace oss {

// Snapshots the incoming vertex buffer each frame into a trail: a queue of up to `max frames`
// copies, each offset in Z by age * `z spacing` and hue-rotated by age * `hue rate`. Emits the
// combined trail as one Pos3Color3 buffer -> wire into Wireframe. Reads the input VBO back to
// CPU each frame (a GPU sync; suits modest vertex counts).
class VertexTrailNode : public Node {
public:
    VertexTrailNode() : Node("Vertex Trail") {
        addInput("geometry",   PortType::Vertex, VertexRef{});
        addInput("max frames", PortType::Float, 16.0f, 1.0f, 240.0f);
        addInput("z spacing",  PortType::Float, 0.15f, -2.0f, 2.0f);
        addInput("hue rate",   PortType::Float, 0.03f, -1.0f, 1.0f);
        addOutput("geometry", PortType::Vertex);
    }
    ~VertexTrailNode() override { if (outVbo_) glDeleteBuffers(1, &outVbo_); }
    void initGL() override { glGenBuffers(1, &outVbo_); }

    void evaluate(EvalContext& ctx) override {
        VertexRef in = ctx.in<VertexRef>(0);
        maxF_ = std::clamp((int)std::lround(ctx.in<float>(1)), 1, 240);
        trail_.setMaxFrames(maxF_);

        if (in.vbo != 0 && in.count > 0) {
            int fl = (in.format == VertexFormat::Pos3) ? 3 : 6;
            readback_.resize((std::size_t)in.count * fl);
            glBindBuffer(GL_ARRAY_BUFFER, in.vbo);
            glGetBufferSubData(GL_ARRAY_BUFFER, 0,
                               (GLsizeiptr)in.count * fl * sizeof(float), readback_.data());
            glBindBuffer(GL_ARRAY_BUFFER, 0);
            trail_.push(readback_.data(), in.count, in.format, in.primitive);
        }

        int total = trail_.build(ctx.in<float>(2), ctx.in<float>(3), built_);
        glBindBuffer(GL_ARRAY_BUFFER, outVbo_);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(built_.size() * sizeof(float)),
                     built_.empty() ? nullptr : built_.data(), GL_DYNAMIC_DRAW);
        glBindBuffer(GL_ARRAY_BUFFER, 0);

        frames_ = trail_.frameCount();
        out_ = VertexRef{ outVbo_, total, trail_.primitive(), VertexFormat::Pos3Color3 };
        ctx.out<VertexRef>(0, out_);
    }

    VertexRef   output() const { return out_; }     // last published geometry (for tests)
    std::string statusLine() const override { return std::to_string(frames_) + "/" + std::to_string(maxF_); }

private:
    VertexTrail trail_;
    GLuint outVbo_ = 0;
    int    frames_ = 0, maxF_ = 16;
    VertexRef out_;
    std::vector<float> readback_, built_;
};

} // namespace oss
```

- [ ] **Step 2: Add the gl_smoke scenario**

In `tests/gl_smoke.cpp`, add these includes with the other module/core includes:
```cpp
#include "modules/VertexTrailNode.h"
#include "core/ColorHsv.h"
```
Add this scenario immediately before the final `glfwDestroyWindow(win);`:
```cpp
    // --- Scenario: Vertex Trail queues snapshots, offset in z + hue-rotated by age ---
    // Push the same 1-vertex Pos3 input 3 frames; the trail holds 3 copies at z = 0.3 / 0.8 / 1.3
    // with colours red / hue 0.1 / hue 0.2. Read the output VBO back and verify.
    {
        const float inPos[3] = { 0.1f, 0.2f, 0.3f };
        GLuint inVbo = 0;
        glGenBuffers(1, &inVbo); glBindBuffer(GL_ARRAY_BUFFER, inVbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(inPos), inPos, GL_STATIC_DRAW);
        glBindBuffer(GL_ARRAY_BUFFER, 0);

        Graph g;
        auto tr = std::make_unique<VertexTrailNode>();
        tr->inputDefault(0) = VertexRef{ inVbo, 1, Primitive::Lines, VertexFormat::Pos3 };
        tr->inputDefault(1) = 8.0f;    // max frames
        tr->inputDefault(2) = 0.5f;    // z spacing
        tr->inputDefault(3) = 0.1f;    // hue rate
        tr->initGL();
        int tId = g.addNode(std::move(tr));
        auto* tn = dynamic_cast<VertexTrailNode*>(g.findNode(tId));
        for (int f = 0; f < 3; ++f) g.evaluate(1.0f / 60.0f);

        VertexRef out = tn->output();
        if (out.vbo == 0 || out.count != 3 || out.format != VertexFormat::Pos3Color3) {
            glDeleteBuffers(1, &inVbo); glfwTerminate(); return fail("Vertex Trail wrong output shape");
        }
        float o[18];
        glBindBuffer(GL_ARRAY_BUFFER, out.vbo);
        glGetBufferSubData(GL_ARRAY_BUFFER, 0, 18 * sizeof(float), o);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        auto af = [](float a, float b) { return std::fabs(a - b) < 1e-3f; };
        glm::vec3 c1 = hsvToRgb(0.1f, 1.0f, 1.0f);   // age 1
        glm::vec3 c2 = hsvToRgb(0.2f, 1.0f, 1.0f);   // age 2
        bool ok =
            af(o[0],0.1f)  && af(o[1],0.2f) && af(o[2],0.3f) && af(o[3],1.0f)  && af(o[4],0.0f)  && af(o[5],0.0f)   &&  // age0 red
            af(o[6],0.1f)  && af(o[7],0.2f) && af(o[8],0.8f) && af(o[9],c1.x)  && af(o[10],c1.y) && af(o[11],c1.z)  &&  // age1
            af(o[12],0.1f) && af(o[13],0.2f)&& af(o[14],1.3f)&& af(o[15],c2.x) && af(o[16],c2.y) && af(o[17],c2.z);     // age2
        if (!ok) {
            std::fprintf(stderr, "Vertex Trail got z=(%.3f,%.3f,%.3f)\n", o[2], o[8], o[14]);
            glDeleteBuffers(1, &inVbo); glfwTerminate(); return fail("Vertex Trail z-offset/hue wrong");
        }
        glDeleteBuffers(1, &inVbo);
        std::fprintf(stderr, "gl_smoke OK: Vertex Trail queued snapshots with z-offset + hue rotation\n");
    }
```
Ensure `<cmath>` and `<glm/vec3.hpp>` are available in gl_smoke (both already are via existing includes; `ColorHsv.h` pulls in glm too).

- [ ] **Step 3: Build gl_smoke + verify it PASSES**

Run: `cmake -S . -B build && cmake --build build -j --target gl_smoke && ctest --test-dir build -R gl_smoke --output-on-failure`
Expected: PASS, including `gl_smoke OK: Vertex Trail queued snapshots with z-offset + hue rotation`. If the z values are wrong, the queue order (newest-first) or z-offset diverged; debug — don't weaken the assertions.

- [ ] **Step 4: Register the node**

In `src/app/Application.cpp`: add the include with the other `#include "modules/..."` lines:
```cpp
#include "modules/VertexTrailNode.h"
```
In `makeNode(...)`, add near the other 3D-geometry node returns (e.g. after Wireframe / Deform):
```cpp
    if (type == "Vertex Trail") return std::make_unique<VertexTrailNode>();
```
In `nodeCategories()`, add `"Vertex Trail"` to the **3D** category's braced list (the one containing "Wireframe"/"Shaded Render"/"Skybox").

- [ ] **Step 5: Add it to the screenshot demo**

In `src/main.cpp`, add after another 3D demo node (e.g. the Skybox / Deform line):
```cpp
        app.addNodeOfType("Vertex Trail", glm::vec2(440.0f, 470.0f));
```

- [ ] **Step 6: Full build + tests + screenshot**

Run: `cmake --build build -j && ctest --test-dir build --output-on-failure`
Expected: clean build; ALL tests pass (core_tests + gl_smoke incl. the new scenario).

Run: `./build/shader_streamer --screenshot build/_ui.png`
Expected: exit 0. Open `build/_ui.png` with the Read tool and confirm a **Vertex Trail** node renders with `geometry`/`max frames`/`z spacing`/`hue rate` inputs + a `geometry` output. Reposition + re-screenshot if clipped; report what you saw.

- [ ] **Step 7: Commit**

```bash
git add src/modules/VertexTrailNode.h tests/gl_smoke.cpp src/app/Application.cpp src/main.cpp
git commit -m "$(cat <<'EOF'
feat(modules): add the Vertex Trail node + register it

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
)"
```

---

### Task 4: Documentation

**Files:** Modify `README.md`, `CLAUDE.md`.

- [ ] **Step 1: README.md**

In the module table, add a row immediately after the **Deform** row (search `**Deform**`), matching the table's exact 2-column layout:
```markdown
| **Vertex Trail** | snapshots a vertex buffer each frame into a trail (queue of `max frames`); each copy is offset in Z (`z spacing`) and hue-rotated (`hue rate`) by its age → a colored vertex buffer. Wire `geometry` into **Wireframe** |
```

- [ ] **Step 2: CLAUDE.md**

In the **Architecture** section, add this bullet immediately after the **Shader edge + Deform** bullet:
```markdown
- **Vertex Trail** — `VertexTrailNode` (`src/modules/VertexTrailNode.h`, header-only) keeps a
  GL-free `core/VertexTrail` queue of geometry snapshots (read back from the input VBO each
  frame with `glGetBufferSubData`), emitting one combined `Pos3Color3` buffer where the
  snapshot of age `k` (0 = newest) is offset by `k·z-spacing` in Z and hue-rotated by
  `k·hue-rate`. The HSV helpers live in the GL-free header-only `core/ColorHsv.h` (`hsvToRgb`
  + `rgbToHsv`), now shared with `PitchGraph`. Unit-tested in `core_tests`; the readback +
  queue + offsets are `gl_smoke`-verified by reading the output buffer back.
```

- [ ] **Step 3: Verify + commit**

Run: `ctest --test-dir build --output-on-failure`
Expected: all pass (no code changed).

```bash
git add README.md CLAUDE.md
git commit -m "$(cat <<'EOF'
docs: document the Vertex Trail node + shared ColorHsv helpers

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
)"
```

---

## Final verification

- [ ] `cmake --build build -j` — clean build (`shader_streamer`, `core_tests`, `gl_smoke`)
- [ ] `ctest --test-dir build --output-on-failure` — all pass (ColorHsv, VertexTrail, gl_smoke trail scenario, existing pitch-graph)
- [ ] `./build/shader_streamer --screenshot build/_ui.png` — the Vertex Trail node renders
- [ ] Use superpowers:finishing-a-development-branch
</content>
