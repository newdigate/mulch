# MIDI Pitch Graph Node Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a **Pitch Graph** node that turns MIDI into a scrolling pitch-vs-time graph as colored line geometry (pitch-class rainbow hue, velocity brightness) streamed into the Wireframe renderer.

**Architecture:** First extend the geometry pipeline with a colored vertex format (`VertexFormat::Pos3Color3`) and a colored draw path in the Wireframe node. Then a GL-free `core/PitchGraph` holds a rolling MIDI note history and builds colored line segments; a header-only `PitchGraphNode` wraps it + the VBO. Unit-tested in `core_tests`; `gl_smoke` proves color flows through Wireframe.

**Tech Stack:** C++17, OpenGL 4.1/GLSL 410, doctest, CMake. Design: `docs/superpowers/specs/2026-06-18-midi-pitch-graph-design.md`.

**IMPORTANT for the implementer:** `src/core/` is GL-free (glm allowed). `PitchGraph.{h,cpp}` is GL-free and compiled into `core_tests` (which links no GL) — its test includes only `core/PitchGraph.h`. `PitchGraphNode.h` is the GL part (header-only). The Wireframe node has ONE VAO, so the colored path must enable attrib 1 and the Pos3 path must disable it.

---

### Task 1: Colored vertex format + Wireframe colored draw path

**Files:**
- Modify: `src/core/Value.h` (add `Pos3Color3`)
- Modify: `src/modules/WireframeNode.h` (add `program_color_`)
- Modify: `src/modules/WireframeNode.cpp` (colored program + draw branch)
- Modify: `tests/gl_smoke.cpp` (a colored-line scenario)

- [ ] **Step 1: Add the vertex format**

In `src/core/Value.h`, change the `VertexFormat` enum and its comment:

```cpp
// The per-vertex layout of a VertexRef's buffer:
//   Pos3        - 3 floats: position (stride 12)
//   Pos3Normal3 - 6 floats: position + normal (stride 24, normal at offset 12)
//   Pos3Color3  - 6 floats: position + RGB colour (stride 24, colour at offset 12)
enum class VertexFormat { Pos3, Pos3Normal3, Pos3Color3 };
```

- [ ] **Step 2: Write the failing gl_smoke colored-line scenario**

In `tests/gl_smoke.cpp`, add this scenario immediately before the final `glfwDestroyWindow(win);` line:

```cpp
    // --- Scenario 16: Wireframe draws a per-vertex-coloured line (Pos3Color3) ---
    // A hand-built coloured VBO (a red horizontal line) fed to the Wireframe node must
    // render RED, not the node's default green -- proving the Pos3Color3 colored path.
    {
        const float verts[] = {
            -0.5f, 0.0f, 0.0f,  1.0f, 0.0f, 0.0f,   // (x,y,z, r,g,b)
             0.5f, 0.0f, 0.0f,  1.0f, 0.0f, 0.0f,
        };
        GLuint vbo = 0;
        glGenBuffers(1, &vbo); glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
        glBindBuffer(GL_ARRAY_BUFFER, 0);

        Graph g;
        auto wire = std::make_unique<WireframeNode>();
        wire->inputDefault(0) = VertexRef{vbo, 2, Primitive::Lines, VertexFormat::Pos3Color3};
        wire->inputDefault(1) = 0.0f;   // spin off -> static
        auto out = std::make_unique<OutputNode>();
        wire->initGL(); out->initGL();
        int wId = g.addNode(std::move(wire));
        int oId = g.addNode(std::move(out));
        if (!g.connect(wId, 0, oId, 0)) { glfwTerminate(); return fail("connect colour-wire->output"); }
        g.evaluate(1.0f/60.0f);
        TexRef t = dynamic_cast<OutputNode*>(g.findNode(oId))->current();
        if (!t.id) { glfwTerminate(); return fail("coloured wireframe texture not produced"); }
        std::vector<unsigned char> px((size_t)t.w * t.h * 4);
        glBindTexture(GL_TEXTURE_2D, t.id);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        bool sawRed = false;
        for (size_t i = 0; i < px.size(); i += 4)
            if (px[i] > 150 && px[i+1] < 80 && px[i+2] < 80) { sawRed = true; break; }
        glDeleteBuffers(1, &vbo);
        if (!sawRed) { glfwTerminate(); return fail("coloured wireframe did not render a red line (Pos3Color3 path)"); }
        std::fprintf(stderr, "gl_smoke OK: Wireframe renders per-vertex colour (Pos3Color3)\n");
    }
```

- [ ] **Step 3: Run gl_smoke to verify it FAILS**

Run: `cmake -S . -B build && cmake --build build -j --target gl_smoke && ctest --test-dir build -R gl_smoke --output-on-failure`
Expected: FAIL — the unmodified Wireframe ignores per-vertex color and draws the line green, so no red pixel is found ("coloured wireframe did not render a red line").

- [ ] **Step 4: Add the colored program member to the header**

In `src/modules/WireframeNode.h`, add a member next to `program_`:

```cpp
    GLuint program_       = 0;
    GLuint program_color_ = 0;   // per-vertex-colour variant (Pos3Color3)
```

- [ ] **Step 5: Implement the colored program + draw branch**

In `src/modules/WireframeNode.cpp`, add the colored shaders right after the existing `kWireFS` block:

```cpp
static const char* kWireColorVS = R"(#version 410 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aColor;
uniform mat4 uMVP;
out vec3 vColor;
void main() { vColor = aColor; gl_Position = uMVP * vec4(aPos, 1.0); }
)";

static const char* kWireColorFS = R"(#version 410 core
in vec3 vColor;
out vec4 FragColor;
void main() { FragColor = vec4(vColor, 1.0); }
)";
```

In the destructor, delete the colored program too:

```cpp
WireframeNode::~WireframeNode() {
    if (program_) glDeleteProgram(program_);
    if (program_color_) glDeleteProgram(program_color_);
    if (vao_) glDeleteVertexArrays(1, &vao_);
}
```

In `initGL`, compile it:

```cpp
void WireframeNode::initGL() {
    program_ = linkProgram(kWireVS, kWireFS);
    program_color_ = linkProgram(kWireColorVS, kWireColorFS);
    fbo_.create(kCanvasW, kCanvasH);
    glGenVertexArrays(1, &vao_);
}
```

Replace the draw block in `evaluate` (the `if (geo.vbo != 0 && geo.count > 0) { ... }` body) with this format-aware version:

```cpp
    if (geo.vbo != 0 && geo.count > 0) {
        float aspect = fbo_.height() ? (float)fbo_.width() / (float)fbo_.height() : 1.7778f;
        // Camera matches Shaded Render so the two views register when blended.
        glm::mat4 proj = glm::perspective(glm::radians(45.0f), aspect, 0.1f, 100.0f);
        glm::mat4 view = glm::lookAt(glm::vec3(0.0f, 0.4f, 2.8f),
                                     glm::vec3(0.0f, 0.0f, 0.0f),
                                     glm::vec3(0.0f, 1.0f, 0.0f));
        glm::mat4 model = glm::rotate(glm::mat4(1.0f), angle, glm::vec3(0.0f, 1.0f, 0.0f));
        glm::mat4 mvp = proj * view * model;

        bool colored = (geo.format == VertexFormat::Pos3Color3);
        GLuint prog = colored ? program_color_ : program_;
        glUseProgram(prog);
        glUniformMatrix4fv(glGetUniformLocation(prog, "uMVP"), 1, GL_FALSE, glm::value_ptr(mvp));

        glBindVertexArray(vao_);
        glBindBuffer(GL_ARRAY_BUFFER, geo.vbo);
        glEnableVertexAttribArray(0);
        if (colored) {
            const GLsizei stride = 6 * sizeof(float);   // pos(3) + colour(3)
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (void*)0);
            glEnableVertexAttribArray(1);
            glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, (void*)(3 * sizeof(float)));
        } else {
            glDisableVertexAttribArray(1);   // clear any stale colour attribute (one shared VAO)
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
        }
        GLenum prim = geo.primitive == Primitive::Lines     ? GL_LINES
                    : geo.primitive == Primitive::Triangles ? GL_TRIANGLES
                                                            : GL_LINE_STRIP;
        glDrawArrays(prim, 0, geo.count);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glBindVertexArray(0);
    }
```

- [ ] **Step 6: Run gl_smoke to verify it PASSES**

Run: `cmake --build build -j --target gl_smoke && ctest --test-dir build -R gl_smoke --output-on-failure`
Expected: PASS — including `gl_smoke OK: Wireframe renders per-vertex colour (Pos3Color3)`, and the existing Wireframe/Spectrograph/mesh scenarios (Pos3 path) still pass (no regression).

- [ ] **Step 7: Commit**

```bash
git add src/core/Value.h src/modules/WireframeNode.h src/modules/WireframeNode.cpp tests/gl_smoke.cpp
git commit -m "$(cat <<'EOF'
feat(gfx): add a Pos3Color3 vertex format + Wireframe colored draw path

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
)"
```

---

### Task 2: GL-free PitchGraph (note history → colored geometry) + unit tests

**Files:**
- Create: `tests/test_pitch_graph.cpp`
- Create: `src/core/PitchGraph.h`, `src/core/PitchGraph.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Write the failing tests**

Create `tests/test_pitch_graph.cpp`:

```cpp
#include <doctest/doctest.h>
#include "core/PitchGraph.h"
#include "core/Value.h"
#include <glm/vec3.hpp>
#include <vector>
#include <cmath>

using namespace oss;

static bool approx(const glm::vec3& a, const glm::vec3& b, float e = 1e-3f) {
    return std::fabs(a.x-b.x) < e && std::fabs(a.y-b.y) < e && std::fabs(a.z-b.z) < e;
}

TEST_CASE("hsvToRgb maps the primary hues and wraps") {
    CHECK(approx(hsvToRgb(0.0f, 1, 1),       glm::vec3(1, 0, 0)));   // red
    CHECK(approx(hsvToRgb(1.0f/3.0f, 1, 1),  glm::vec3(0, 1, 0)));   // green
    CHECK(approx(hsvToRgb(2.0f/3.0f, 1, 1),  glm::vec3(0, 0, 1)));   // blue
    CHECK(approx(hsvToRgb(1.0f, 1, 1),       hsvToRgb(0.0f, 1, 1))); // hue wraps
}

TEST_CASE("a note-on then note-off makes one coloured segment at the right pitch") {
    PitchGraph pg;
    std::vector<MidiEvent> on = { midiNoteOn(60, 100) };
    pg.ingest(MidiRef{on.data(), on.size()}, 0.0f, 8.0f);   // note-on at t=0
    std::vector<MidiEvent> off = { midiNoteOff(60) };
    pg.ingest(MidiRef{off.data(), off.size()}, 1.0f, 8.0f); // note-off at t=1
    std::vector<float> v;
    int n = pg.build(8.0f, v);
    REQUIRE(n == 2);              // one segment = 2 vertices
    REQUIRE(v.size() == 12);     // 2 * 6 floats
    CHECK(v[1] == doctest::Approx(0.0f));   // y of note 60 = range centre
    CHECK(v[7] == doctest::Approx(0.0f));
    float val = 0.25f + 0.75f * 100.0f / 127.0f;
    CHECK(v[3] == doctest::Approx(val));    // r (pitch class 0 -> hue 0 -> red = value)
    CHECK(v[4] == doctest::Approx(0.0f));   // g
    CHECK(v[5] == doctest::Approx(0.0f));   // b
}

TEST_CASE("the same pitch class shares a hue across octaves; other notes differ") {
    PitchGraph pg;
    std::vector<MidiEvent> on = { midiNoteOn(60, 100), midiNoteOn(72, 100), midiNoteOn(61, 100) };
    pg.ingest(MidiRef{on.data(), on.size()}, 0.0f, 8.0f);
    std::vector<float> v; pg.build(8.0f, v);
    REQUIRE(v.size() == 3 * 2 * 6);                 // 3 held notes
    glm::vec3 c60(v[3],  v[4],  v[5]);              // note 60, first vertex colour
    glm::vec3 c72(v[15], v[16], v[17]);             // note 72
    glm::vec3 c61(v[27], v[28], v[29]);             // note 61
    CHECK(approx(c60, c72));                         // pitch class 0 == pitch class 0
    CHECK_FALSE(approx(c60, c61));                   // pitch class 0 != pitch class 1
}

TEST_CASE("a held note's right end stays pinned at the right edge") {
    PitchGraph pg;
    std::vector<MidiEvent> on = { midiNoteOn(60, 100) };
    pg.ingest(MidiRef{on.data(), on.size()}, 0.0f, 8.0f);
    std::vector<MidiEvent> none;
    pg.ingest(MidiRef{none.data(), none.size()}, 2.0f, 8.0f);   // 2s later, still held
    std::vector<float> v; pg.build(8.0f, v);
    REQUIRE(v.size() == 12);
    CHECK(v[6] == doctest::Approx(1.0f));   // second vertex x (xe) pinned at +1
    CHECK(v[0] == doctest::Approx(0.5f));   // first vertex x (xs) scrolled left: 2*(-2)/8+1
}

TEST_CASE("a closed note scrolled past the window is pruned") {
    PitchGraph pg;
    std::vector<MidiEvent> on = { midiNoteOn(60, 100) };
    pg.ingest(MidiRef{on.data(), on.size()}, 0.0f, 4.0f);
    std::vector<MidiEvent> off = { midiNoteOff(60) };
    pg.ingest(MidiRef{off.data(), off.size()}, 0.5f, 4.0f);     // closed at t=0.5
    std::vector<MidiEvent> none;
    pg.ingest(MidiRef{none.data(), none.size()}, 5.0f, 4.0f);   // t=5.5, cutoff=1.5 > 0.5
    std::vector<float> v; int n = pg.build(4.0f, v);
    CHECK(n == 0);
    CHECK(pg.activeCount() == 0);
}

TEST_CASE("activeCount counts only held notes") {
    PitchGraph pg;
    std::vector<MidiEvent> on = { midiNoteOn(60, 100), midiNoteOn(64, 100) };
    pg.ingest(MidiRef{on.data(), on.size()}, 0.0f, 8.0f);
    CHECK(pg.activeCount() == 2);
    std::vector<MidiEvent> off = { midiNoteOff(60) };
    pg.ingest(MidiRef{off.data(), off.size()}, 0.1f, 8.0f);
    CHECK(pg.activeCount() == 1);
}
```

- [ ] **Step 2: Write the header**

Create `src/core/PitchGraph.h`:

```cpp
#pragma once
#include <vector>
#include <glm/vec3.hpp>
#include "core/Value.h"

namespace oss {

// HSV (each component in [0,1]; hue wraps) -> RGB in [0,1]. GL-free.
glm::vec3 hsvToRgb(float h, float s, float v);

// Rolling MIDI note history -> a scrolling pitch-vs-time line graph (GL-free). Notes are
// horizontal segments at their pitch; pitch class picks a rainbow hue, velocity the
// brightness. X is time (newest at the right edge, scrolling left).
class PitchGraph {
public:
    // Advance the local clock by dt, ingest this frame's MIDI (note-on opens a record,
    // note-off closes the matching open one), and prune records scrolled off the left.
    void ingest(const MidiRef& midi, float dt, float window);

    // Emit the line-segment vertices as flat Pos3Color3 floats (x,y,z,r,g,b per vertex,
    // 2 vertices per segment). Returns the vertex count (out.size() == count*6).
    int build(float window, std::vector<float>& out) const;

    int activeCount() const;   // records currently held (for the status line)

private:
    struct Note { int note; int vel; double startT; double endT; };  // endT < 0 while held
    std::vector<Note> notes_;
    double clock_ = 0.0;
};

} // namespace oss
```

- [ ] **Step 3: Wire the build (so the test compiles and fails)**

In `CMakeLists.txt`:
- Add `src/core/PitchGraph.cpp` after **each** `src/core/Oscilloscope.cpp` line (there are two — one in the `APP_SOURCES` list, one in the `core_tests` target):
  ```cmake
    src/core/Oscilloscope.cpp
    src/core/PitchGraph.cpp
  ```
- In the **`gl_smoke`** target (the `add_executable(gl_smoke ...)` block, identifiable by `tests/gl_smoke.cpp` at its top), add `src/core/PitchGraph.cpp` right after `src/core/AutomationStore.cpp` (the line followed by `src/gfx/GLUtil.cpp`):
  ```cmake
    src/core/AutomationStore.cpp
    src/core/PitchGraph.cpp
  ```
- In the `core_tests` target's test-file list, add `tests/test_pitch_graph.cpp` after `tests/test_blend_modes.cpp`:
  ```cmake
    tests/test_blend_modes.cpp
    tests/test_pitch_graph.cpp
  ```

- [ ] **Step 4: Run the tests to verify they fail**

Run: `cmake -S . -B build && cmake --build build -j --target core_tests`
Expected: FAIL — link/compile error (`hsvToRgb`/`PitchGraph` undefined) because `PitchGraph.cpp` has no implementation yet.

- [ ] **Step 5: Write the implementation**

Create `src/core/PitchGraph.cpp`:

```cpp
#include "core/PitchGraph.h"
#include <algorithm>
#include <cmath>

namespace oss {

glm::vec3 hsvToRgb(float h, float s, float v) {
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

namespace {
constexpr int         kLoNote = 24, kHiNote = 96;   // C1..C7, log-frequency (by note)
constexpr std::size_t kMaxNotes = 512;
inline float clamp11(float x) { return x < -1.0f ? -1.0f : (x > 1.0f ? 1.0f : x); }
}

void PitchGraph::ingest(const MidiRef& midi, float dt, float window) {
    clock_ += (double)dt;
    for (std::size_t i = 0; i < midi.count; ++i) {
        const MidiEvent& e = midi.events[i];
        int note = e.data1 & 0x7F;
        if (midiIsNoteOn(e)) {
            for (auto it = notes_.rbegin(); it != notes_.rend(); ++it)   // retrigger: close prior
                if (it->note == note && it->endT < 0.0) { it->endT = clock_; break; }
            notes_.push_back({note, e.data2 & 0x7F, clock_, -1.0});
            if (notes_.size() > kMaxNotes) notes_.erase(notes_.begin());
        } else if (midiIsNoteOff(e)) {
            for (auto it = notes_.rbegin(); it != notes_.rend(); ++it)
                if (it->note == note && it->endT < 0.0) { it->endT = clock_; break; }
        }
    }
    double cutoff = clock_ - (double)window;   // drop closed records scrolled off the left
    notes_.erase(std::remove_if(notes_.begin(), notes_.end(),
        [cutoff](const Note& n){ return n.endT >= 0.0 && n.endT < cutoff; }), notes_.end());
}

int PitchGraph::build(float window, std::vector<float>& out) const {
    out.clear();
    double now = clock_;
    double w = window > 1e-6f ? (double)window : 1e-6;
    for (const Note& n : notes_) {
        double s = n.startT;
        double e = (n.endT < 0.0) ? now : n.endT;
        if (e < now - w) continue;
        float xs = clamp11((float)(2.0 * (s - now) / w + 1.0));
        float xe = clamp11((float)(2.0 * (e - now) / w + 1.0));
        float y  = clamp11(2.0f * (float)(n.note - kLoNote) / (float)(kHiNote - kLoNote) - 1.0f);
        glm::vec3 c = hsvToRgb((float)(n.note % 12) / 12.0f, 1.0f,
                               0.25f + 0.75f * (float)n.vel / 127.0f);
        out.insert(out.end(), { xs, y, 0.0f, c.r, c.g, c.b });
        out.insert(out.end(), { xe, y, 0.0f, c.r, c.g, c.b });
    }
    return (int)(out.size() / 6);
}

int PitchGraph::activeCount() const {
    int n = 0; for (const Note& x : notes_) if (x.endT < 0.0) ++n; return n;
}

} // namespace oss
```

- [ ] **Step 6: Run the tests to verify they pass**

Run: `cmake --build build -j --target core_tests && ./build/core_tests`
Expected: PASS — all `core_tests` green, including the 6 pitch-graph cases. (Run the full binary.)

- [ ] **Step 7: Commit**

```bash
git add src/core/PitchGraph.h src/core/PitchGraph.cpp tests/test_pitch_graph.cpp CMakeLists.txt
git commit -m "$(cat <<'EOF'
feat(core): add a GL-free MIDI pitch-graph geometry builder

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
)"
```

---

### Task 3: Pitch Graph node + registration + gl_smoke + screenshot

**Files:**
- Create: `src/modules/PitchGraphNode.h`
- Modify: `tests/gl_smoke.cpp` (a MIDI → pitch-graph scenario)
- Modify: `src/app/Application.cpp` (include + makeNode + MIDI category)
- Modify: `src/main.cpp` (screenshot demo)

- [ ] **Step 1: Write the node**

Create `src/modules/PitchGraphNode.h`:

```cpp
#pragma once
#include <glad/gl.h>
#include <algorithm>
#include <string>
#include <vector>
#include "core/Node.h"
#include "core/Value.h"
#include "core/PitchGraph.h"

namespace oss {

// Turns incoming MIDI into a scrolling pitch-vs-time graph as colored line geometry: each
// held note is a horizontal segment at its pitch, coloured by pitch class (rainbow hue)
// and velocity (brightness), scrolling right-to-left over `window` seconds. Publishes a
// Pos3Color3 VertexRef on output 0 -> wire into the Wireframe renderer (set its spin to 0
// for a static view). The graph math is the GL-free core/PitchGraph.
class PitchGraphNode : public Node {
public:
    PitchGraphNode() : Node("Pitch Graph") {
        addInput("midi", PortType::Midi, MidiRef{});
        addInput("window", PortType::Float, 8.0f, 1.0f, 30.0f);   // seconds of history shown
        addOutput("geometry", PortType::Vertex);
    }
    ~PitchGraphNode() override { if (vbo_) glDeleteBuffers(1, &vbo_); }
    void initGL() override { glGenBuffers(1, &vbo_); }

    void evaluate(EvalContext& ctx) override {
        float window = std::clamp(ctx.in<float>(1), 1.0f, 30.0f);
        pg_.ingest(ctx.in<MidiRef>(0), ctx.dt, window);
        int n = pg_.build(window, verts_);
        glBindBuffer(GL_ARRAY_BUFFER, vbo_);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(verts_.size() * sizeof(float)),
                     verts_.empty() ? nullptr : verts_.data(), GL_DYNAMIC_DRAW);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        ctx.out<VertexRef>(0, VertexRef{vbo_, n, Primitive::Lines, VertexFormat::Pos3Color3});
        held_ = pg_.activeCount();
    }

    std::string statusLine() const override { return std::to_string(held_) + " held"; }

private:
    PitchGraph         pg_;
    std::vector<float> verts_;       // owns the VertexRef storage (Pos3Color3 floats)
    GLuint             vbo_  = 0;
    int                held_ = 0;
};

} // namespace oss
```

- [ ] **Step 2: Add the gl_smoke MIDI → pitch-graph scenario**

In `tests/gl_smoke.cpp`, add `#include "modules/PitchGraphNode.h"` near the other module includes (after `#include "modules/WireframeNode.h"`). Then add this scenario immediately before the final `glfwDestroyWindow(win);` line (it comes after Scenario 16):

```cpp
    // --- Scenario 17: Pitch Graph turns MIDI into a coloured pitch-vs-time graph ---
    // Feed three note-ons (pitch classes 0/4/7) into a Pitch Graph -> Wireframe; the
    // rendered texture must contain a RED line (note 60, pitch class 0 -> hue 0), which
    // the default-green wireframe could never produce -- proving MIDI -> coloured geometry
    // -> Wireframe end to end.
    {
        std::vector<MidiEvent> on = { midiNoteOn(60, 110), midiNoteOn(64, 110), midiNoteOn(67, 110) };
        Graph g;
        auto pg = std::make_unique<PitchGraphNode>();
        auto wire = std::make_unique<WireframeNode>();
        wire->inputDefault(1) = 0.0f;   // spin off -> static
        auto out = std::make_unique<OutputNode>();
        pg->initGL(); wire->initGL(); out->initGL();
        int pId = g.addNode(std::move(pg));
        int wId = g.addNode(std::move(wire));
        int oId = g.addNode(std::move(out));
        if (!g.connect(pId, 0, wId, 0) || !g.connect(wId, 0, oId, 0)) { glfwTerminate(); return fail("connect pitchgraph->wire->output"); }
        auto* pn = dynamic_cast<PitchGraphNode*>(g.findNode(pId));
        pn->inputDefault(0) = MidiRef{on.data(), on.size()};   // note-ons this frame
        g.evaluate(1.0f/60.0f);                                 // ingest the notes
        pn->inputDefault(0) = MidiRef{};                        // no further events
        for (int f = 0; f < 4; ++f) g.evaluate(1.0f/60.0f);    // hold + scroll a little
        TexRef t = dynamic_cast<OutputNode*>(g.findNode(oId))->current();
        if (!t.id) { glfwTerminate(); return fail("pitch graph texture not produced"); }
        std::vector<unsigned char> px((size_t)t.w * t.h * 4);
        glBindTexture(GL_TEXTURE_2D, t.id);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        bool sawRed = false;
        for (size_t i = 0; i < px.size(); i += 4)
            if (px[i] > 120 && px[i+1] < 80 && px[i+2] < 80) { sawRed = true; break; }
        if (!sawRed) { glfwTerminate(); return fail("pitch graph did not render note 60 as a red line"); }
        std::fprintf(stderr, "gl_smoke OK: Pitch Graph -> Wireframe renders MIDI as a coloured pitch graph\n");
    }
```

- [ ] **Step 3: Build gl_smoke and verify it PASSES**

Run: `cmake --build build -j --target gl_smoke && ctest --test-dir build -R gl_smoke --output-on-failure`
Expected: PASS — including `gl_smoke OK: Pitch Graph -> Wireframe renders MIDI as a coloured pitch graph`. (If it fails, the node/format/colored-path wiring is wrong — not a tolerance issue.)

- [ ] **Step 4: Register the node**

In `src/app/Application.cpp`, add the include after `#include "modules/MidiFilePlayerNode.h"`:

```cpp
#include "modules/MidiFilePlayerNode.h"
#include "modules/PitchGraphNode.h"
```

Add the `makeNode` branch right after the `if (type == "Chord Player") ...` line:

```cpp
    if (type == "Chord Player") return std::make_unique<ChordPlayerNode>();
    if (type == "Pitch Graph")  return std::make_unique<PitchGraphNode>();
```

Add `"Pitch Graph"` to the **MIDI** category list, at the end:

```cpp
        { "MIDI",    { "MIDI In", "MIDI File", "Step Seq", "Chord Player", "Arpeggiator", "MIDI Merge", "MIDI Out", "Pitch Graph" } },
```

- [ ] **Step 5: Add it to the screenshot demo**

In `src/main.cpp`, add this line right after the existing `app.addNodeOfType("Compositor", ...);` demo line:

```cpp
        app.addNodeOfType("Pitch Graph", glm::vec2(620.0f, 200.0f));
```

- [ ] **Step 6: Build everything and verify the node renders**

Run: `cmake --build build -j && ctest --test-dir build --output-on-failure`
Expected: clean build; all tests pass (`core_tests`, `gl_smoke` incl. Scenarios 16 + 17).

Run: `./build/shader_streamer --screenshot build/_ui.png`
Expected: exit 0. Open `build/_ui.png` with the Read tool and confirm a **Pitch Graph** node renders with a `midi` input, a `window` slider, a `geometry` output, and a status line (`0 held`). If it overlaps another node or is clipped, move it to a clear on-screen spot and re-screenshot; report what you saw.

- [ ] **Step 7: Commit**

```bash
git add src/modules/PitchGraphNode.h tests/gl_smoke.cpp src/app/Application.cpp src/main.cpp
git commit -m "$(cat <<'EOF'
feat(modules): add the Pitch Graph node + register it

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
)"
```

---

### Task 4: Documentation

**Files:**
- Modify: `README.md`, `CLAUDE.md`

- [ ] **Step 1: Add the README module-table row**

In `README.md`, add this row immediately after the `| **Chord Player** | ... |` row (search for `**Chord Player**` in the module table):

```markdown
| **Pitch Graph** | MIDI → a scrolling pitch-vs-time graph as colored geometry → Vertex: each held note is a horizontal line at its pitch, coloured by pitch class (rainbow hue) and velocity (brightness), scrolling over `window` seconds. Wire `geometry` into **Wireframe** (set its `spin` to 0 for a static view) |
```

- [ ] **Step 2: Add the CLAUDE.md architecture bullet**

In `CLAUDE.md`, in the **Architecture** section, add this bullet immediately after the **Compositor** bullet (it ends with "... so they can't drift."):

```markdown
- **Pitch Graph** — `PitchGraphNode` (`src/modules/PitchGraphNode.h`, header-only) turns
  incoming MIDI into a scrolling pitch-vs-time graph as colored line geometry. The GL-free
  `core/PitchGraph` holds a rolling note history (note-on opens a segment, note-off closes
  it, old ones scroll off and prune) and builds line segments — x = time (newest at the
  right, scrolling left), y = note number (log-frequency), colour = pitch-class rainbow
  hue × velocity brightness via `hsvToRgb`. It publishes a `Pos3Color3` `VertexRef`; that
  vertex format and the Wireframe node's colored draw path were added for it (Wireframe
  branches on `format`, like Shaded Render on `Pos3Normal3`). Unit-tested in `core_tests`;
  a `gl_smoke` scenario checks the colours reach the Wireframe texture.
```

- [ ] **Step 3: Sanity check + commit**

Run: `ctest --test-dir build --output-on-failure`
Expected: all pass (no code changed).

```bash
git add README.md CLAUDE.md
git commit -m "$(cat <<'EOF'
docs: document the Pitch Graph node

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
)"
```

---

## Final verification

- [ ] `cmake --build build -j` — clean build (`shader_streamer`, `core_tests`, `gl_smoke`)
- [ ] `ctest --test-dir build --output-on-failure` — all pass (pitch-graph unit tests + gl_smoke Scenarios 16/17)
- [ ] `./build/shader_streamer --screenshot build/_ui.png` — the Pitch Graph node renders with its ports
- [ ] Use superpowers:finishing-a-development-branch
</content>
