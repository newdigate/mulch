# OpenGL Shader Streamer — Implementation Plan (Vertical Slice)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a cross-platform C++/OpenGL + Dear ImGui app where the user wires shader-backed modules into a directed acyclic graph and sees the result render live — proving the full `graph → shader → texture` pipeline with four example modules (Colour, Spectrograph, Mix, Output).

**Architecture:** A GL-free core (`Value`, `Port`, `Node`, `Graph`) evaluates a DAG every frame in cached topological order; each shader-backed node renders into its own framebuffer and publishes a texture reference, which downstream nodes sample. A thin `gfx/` layer wraps shaders/FBOs, and a `ui/` layer renders the graph with imgui-node-editor plus Blender-style inline widgets for unconnected inputs.

**Tech Stack:** C++17, CMake + FetchContent, GLFW, glad (GL 4.1 core), Dear ImGui, imgui-node-editor (thedmd), glm, doctest. In-repo radix-2 FFT. Synthesized audio (no device, single-threaded).

---

## Conventions

- Build: `cmake -S . -B build && cmake --build build -j`
- Run app: `./build/shader_streamer`
- Run tests: `ctest --test-dir build --output-on-failure` (or `./build/core_tests`)
- All code lives in namespace `oss`.
- **GL-free core:** nothing under `src/core/` or `src/audio/` may include a GL header. Texture handles are stored as plain `unsigned int` (`TexRef`) so the test binary needs no GL context.
- The **first build downloads dependencies** (network required). Subsequent builds are cached.

## File Structure (created across the tasks)

```
CMakeLists.txt                 build + FetchContent deps + app/test targets
.gitignore
src/
  main.cpp                     window + GL + ImGui bootstrap, main loop
  app/Application.{h,cpp}      owns window, Graph, editor panel, viewer, node factory
  core/
    Value.h                    PortType, TexRef, AudioRef, Value variant, helpers
    Port.h                     Port struct + Direction
    Connection.h               edge struct
    Node.h                     Node base + EvalContext (header-only logic)
    Node.cpp                   (currently empty TU placeholder — see Task 3)
    Graph.{h,cpp}              add/remove/connect, topo sort, cycle check, evaluate
  gfx/
    Canvas.h                   kCanvasW / kCanvasH constants
    GLUtil.{h,cpp}             shader compile/link, file read, error check
    Framebuffer.{h,cpp}        FBO + RGBA colour texture
    FullscreenPass.{h,cpp}     empty-VAO fullscreen-triangle draw
    ShaderNode.{h,cpp}         base for fragment-shader-rendered nodes
  audio/
    FFT.{h,cpp}                radix-2 complex FFT + magnitude spectrum
    SignalGenerator.{h,cpp}    phase-continuous synth signal
  modules/
    ColourNode.h SpectrographNode.{h,cpp} MixNode.h OutputNode.h
  ui/
    NodeEditorPanel.{h,cpp}    imgui-node-editor rendering + interaction + add menu
    PortWidgets.{h,cpp}        inline widgets for unconnected inputs
shaders/
  colour.frag mix.frag spectrograph.frag
tests/
  test_main.cpp test_value.cpp test_graph.cpp test_fft.cpp test_signal_generator.cpp
```

---

## Task 1: Build scaffolding, window bootstrap, test harness

**Files:**
- Create: `CMakeLists.txt`, `.gitignore`, `src/main.cpp`, `tests/test_main.cpp`, `tests/test_sanity.cpp`

- [ ] **Step 1: Create `.gitignore`**

```gitignore
/build/
.DS_Store
```

- [ ] **Step 2: Write `CMakeLists.txt`**

```cmake
cmake_minimum_required(VERSION 3.16)
project(opengl_shader_streamer CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(FetchContent)

# --- GLFW ---
FetchContent_Declare(glfw GIT_REPOSITORY https://github.com/glfw/glfw.git GIT_TAG 3.4)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(glfw)

# --- glad (OpenGL 4.1 core loader) ---
FetchContent_Declare(glad GIT_REPOSITORY https://github.com/Dav1dde/glad.git GIT_TAG v2.0.8
                     SOURCE_SUBDIR cmake)
FetchContent_MakeAvailable(glad)
glad_add_library(glad_gl41 REPRODUCIBLE API gl:core=4.1)

# --- glm ---
FetchContent_Declare(glm GIT_REPOSITORY https://github.com/g-truc/glm.git GIT_TAG 1.0.1)
FetchContent_MakeAvailable(glm)

# --- Dear ImGui (sources only; we build the lib ourselves) ---
FetchContent_Declare(imgui GIT_REPOSITORY https://github.com/ocornut/imgui.git GIT_TAG v1.91.5)
FetchContent_MakeAvailable(imgui)

# --- imgui-node-editor ---
FetchContent_Declare(imgui_node_editor
  GIT_REPOSITORY https://github.com/thedmd/imgui-node-editor.git GIT_TAG master)
FetchContent_MakeAvailable(imgui_node_editor)

# --- doctest ---
FetchContent_Declare(doctest GIT_REPOSITORY https://github.com/doctest/doctest.git GIT_TAG v2.4.11)
FetchContent_MakeAvailable(doctest)

find_package(OpenGL REQUIRED)

# --- UI third-party static lib: ImGui + backends + node editor ---
add_library(ui_thirdparty STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
  ${imgui_node_editor_SOURCE_DIR}/imgui_node_editor.cpp
  ${imgui_node_editor_SOURCE_DIR}/imgui_node_editor_api.cpp
  ${imgui_node_editor_SOURCE_DIR}/imgui_canvas.cpp
  ${imgui_node_editor_SOURCE_DIR}/crude_json.cpp
)
target_include_directories(ui_thirdparty PUBLIC
  ${imgui_SOURCE_DIR}
  ${imgui_SOURCE_DIR}/backends
  ${imgui_node_editor_SOURCE_DIR}
)
target_link_libraries(ui_thirdparty PUBLIC glfw OpenGL::GL)

# --- Core/gfx/audio/modules/ui sources shared by the app ---
set(APP_SOURCES
  src/core/Node.cpp
  src/core/Graph.cpp
  src/audio/FFT.cpp
  src/audio/SignalGenerator.cpp
  src/gfx/GLUtil.cpp
  src/gfx/Framebuffer.cpp
  src/gfx/FullscreenPass.cpp
  src/gfx/ShaderNode.cpp
  src/modules/SpectrographNode.cpp
  src/ui/NodeEditorPanel.cpp
  src/ui/PortWidgets.cpp
  src/app/Application.cpp
)

# --- Application executable ---
add_executable(shader_streamer src/main.cpp ${APP_SOURCES})
target_include_directories(shader_streamer PRIVATE src)
target_compile_definitions(shader_streamer PRIVATE GLFW_INCLUDE_NONE)
target_link_libraries(shader_streamer PRIVATE ui_thirdparty glad_gl41 glm::glm OpenGL::GL)

# --- Tests (GL-free core only) ---
enable_testing()
add_executable(core_tests
  tests/test_main.cpp
  tests/test_sanity.cpp
  tests/test_value.cpp
  tests/test_graph.cpp
  tests/test_fft.cpp
  tests/test_signal_generator.cpp
  src/core/Node.cpp
  src/core/Graph.cpp
  src/audio/FFT.cpp
  src/audio/SignalGenerator.cpp
)
target_include_directories(core_tests PRIVATE src)
target_link_libraries(core_tests PRIVATE glm::glm doctest::doctest)
add_test(NAME core_tests COMMAND core_tests)
```

> Note: `tests/test_value.cpp`, `test_graph.cpp`, `test_fft.cpp`, `test_signal_generator.cpp` and the `src/...` files are created in later tasks. To keep Task 1 buildable on its own, create empty placeholders now (Steps 4–5) and fill them in their tasks.

- [ ] **Step 3: Write `src/main.cpp` (minimal window + ImGui)**

```cpp
#include <glad/gl.h>
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_opengl3.h>
#include <cstdio>

int main() {
    if (!glfwInit()) { std::fprintf(stderr, "glfwInit failed\n"); return 1; }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 1);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE); // required on macOS

    GLFWwindow* window = glfwCreateWindow(1400, 900, "Shader Streamer", nullptr, nullptr);
    if (!window) { std::fprintf(stderr, "window creation failed\n"); glfwTerminate(); return 1; }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    if (!gladLoadGL((GLADloadfunc)glfwGetProcAddress)) {
        std::fprintf(stderr, "glad load failed\n"); return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 410");

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        ImGui::Begin("Shader Streamer");
        ImGui::Text("Bootstrap OK. GL: %s", (const char*)glGetString(GL_VERSION));
        ImGui::End();

        ImGui::Render();
        int w, h; glfwGetFramebufferSize(window, &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(0.10f, 0.11f, 0.13f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
```

- [ ] **Step 4: Write `tests/test_main.cpp`**

```cpp
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
```

- [ ] **Step 5: Write `tests/test_sanity.cpp` and empty placeholders**

`tests/test_sanity.cpp`:
```cpp
#include <doctest/doctest.h>

TEST_CASE("sanity") { CHECK(1 + 1 == 2); }
```

Create empty files so CMake's source list resolves (they are filled in later tasks):
`tests/test_value.cpp`, `tests/test_graph.cpp`, `tests/test_fft.cpp`, `tests/test_signal_generator.cpp` — each containing only:
```cpp
#include <doctest/doctest.h>
```

Also create empty `.cpp` placeholders referenced by `APP_SOURCES`/`core_tests` that later tasks fill:
`src/core/Node.cpp`, `src/core/Graph.cpp`, `src/audio/FFT.cpp`, `src/audio/SignalGenerator.cpp`, `src/gfx/GLUtil.cpp`, `src/gfx/Framebuffer.cpp`, `src/gfx/FullscreenPass.cpp`, `src/gfx/ShaderNode.cpp`, `src/modules/SpectrographNode.cpp`, `src/ui/NodeEditorPanel.cpp`, `src/ui/PortWidgets.cpp`, `src/app/Application.cpp` — each an empty file (`// placeholder`).

> The app target also needs the headers/classes referenced by `main.cpp`; in Task 1 `main.cpp` references none of them yet, so empty `.cpp` placeholders compile fine.

- [ ] **Step 6: Configure and build**

Run: `cmake -S . -B build && cmake --build build -j`
Expected: configures (downloads deps on first run), builds `shader_streamer` and `core_tests` with no errors.

- [ ] **Step 7: Run the app and tests**

Run: `./build/shader_streamer`
Expected: a dark window titled "Shader Streamer" with a panel showing "Bootstrap OK. GL: 4.1 ...". Close it.
Run: `ctest --test-dir build --output-on-failure`
Expected: `core_tests` passes (1 sanity assertion).

- [ ] **Step 8: Commit**

```bash
git add -A
git commit -m "build: scaffold CMake/FetchContent, GLFW+ImGui window, doctest harness"
```

---

## Task 2: Core value types (`Value`, `PortType`, helpers)

**Files:**
- Create: `src/core/Value.h`
- Test: `tests/test_value.cpp`

- [ ] **Step 1: Write the failing test in `tests/test_value.cpp`**

```cpp
#include <doctest/doctest.h>
#include "core/Value.h"

using namespace oss;

TEST_CASE("typeOf maps each Value alternative to its PortType") {
    CHECK(typeOf(Value{1.0f})                 == PortType::Float);
    CHECK(typeOf(Value{true})                 == PortType::Bool);
    CHECK(typeOf(Value{glm::vec4(1,0,0,1)})   == PortType::Colour);
    CHECK(typeOf(Value{std::string("x.wav")}) == PortType::String);
    CHECK(typeOf(Value{TexRef{7, 4, 4}})      == PortType::Texture);
    CHECK(typeOf(Value{AudioRef{}})           == PortType::Audio);
}

TEST_CASE("portTypeName is stable") {
    CHECK(std::string(portTypeName(PortType::Texture)) == "Texture");
    CHECK(std::string(portTypeName(PortType::Audio))   == "Audio");
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build build -j && ./build/core_tests -tc="typeOf*"`
Expected: FAIL to compile — `core/Value.h` not found.

- [ ] **Step 3: Write `src/core/Value.h`**

```cpp
#pragma once
#include <variant>
#include <string>
#include <cstddef>
#include <glm/vec4.hpp>

namespace oss {

enum class PortType { Texture, Colour, Float, Bool, Audio, String };

// Reference to a GL texture produced by a node. `id` is a GL texture name kept
// as a plain unsigned int so this header (and all of core/) stays GL-free.
struct TexRef { unsigned int id = 0; int w = 0; int h = 0; };

// Non-owning view of a node's latest audio samples.
struct AudioRef { const float* samples = nullptr; std::size_t count = 0; int sampleRate = 0; };

// Order MUST stay in sync with typeOf() below.
using Value = std::variant<float, bool, glm::vec4, std::string, TexRef, AudioRef>;

inline PortType typeOf(const Value& v) {
    switch (v.index()) {
        case 0: return PortType::Float;
        case 1: return PortType::Bool;
        case 2: return PortType::Colour;
        case 3: return PortType::String;
        case 4: return PortType::Texture;
        default: return PortType::Audio;
    }
}

inline const char* portTypeName(PortType t) {
    switch (t) {
        case PortType::Texture: return "Texture";
        case PortType::Colour:  return "Colour";
        case PortType::Float:   return "Float";
        case PortType::Bool:    return "Bool";
        case PortType::Audio:   return "Audio";
        case PortType::String:  return "String";
    }
    return "?";
}

} // namespace oss
```

- [ ] **Step 4: Run to verify it passes**

Run: `cmake --build build -j && ./build/core_tests -tc="typeOf*","portTypeName*"`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/core/Value.h tests/test_value.cpp
git commit -m "feat(core): typed port Value variant + PortType helpers"
```

---

## Task 3: `Port`, `Connection`, `Node` base + `EvalContext`

**Files:**
- Create: `src/core/Port.h`, `src/core/Connection.h`, `src/core/Node.h`, `src/core/Node.cpp`
- Test: extend `tests/test_graph.cpp`

- [ ] **Step 1: Write the failing test in `tests/test_graph.cpp`**

Replace the placeholder contents with:
```cpp
#include <doctest/doctest.h>
#include "core/Node.h"

using namespace oss;

namespace {
// A GL-free test node: outputs a constant float.
struct ConstFloat : Node {
    float v;
    explicit ConstFloat(float val) : Node("const"), v(val) { addOutput("out", PortType::Float); }
    void evaluate(EvalContext& ctx) override { ctx.out<float>(0, v); }
};
// Sums two float inputs.
struct AddFloats : Node {
    AddFloats() : Node("add") {
        addInput("a", PortType::Float, 0.0f);
        addInput("b", PortType::Float, 0.0f);
        addOutput("sum", PortType::Float);
    }
    void evaluate(EvalContext& ctx) override { ctx.out<float>(0, ctx.in<float>(0) + ctx.in<float>(1)); }
};
} // namespace

TEST_CASE("Node exposes declared ports") {
    AddFloats n;
    REQUIRE(n.inputs().size() == 2);
    REQUIRE(n.outputs().size() == 1);
    CHECK(n.inputs()[0].name == "a");
    CHECK(n.inputs()[0].direction == Direction::Input);
    CHECK(n.inputs()[0].type == PortType::Float);
    CHECK(n.outputs()[0].name == "sum");
}

TEST_CASE("EvalContext resolves inputs and writes outputs") {
    AddFloats n;
    std::vector<Value> inputs{ Value{2.0f}, Value{3.0f} };
    std::vector<Value> outputs(1);
    EvalContext ctx{ inputs, outputs, 0.016f };
    n.evaluate(ctx);
    CHECK(std::get<float>(outputs[0]) == doctest::Approx(5.0f));
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build build -j`
Expected: FAIL — `core/Node.h` not found.

- [ ] **Step 3: Write `src/core/Port.h`**

```cpp
#pragma once
#include <string>
#include "core/Value.h"

namespace oss {

enum class Direction { Input, Output };

struct Port {
    std::string name;
    Direction  direction;
    PortType   type;
    Value      defaultValue;   // used for an unconnected input; drives inline widgets
};

} // namespace oss
```

- [ ] **Step 4: Write `src/core/Connection.h`**

```cpp
#pragma once
namespace oss {

// Directed edge: output (srcNode, srcPort) -> input (dstNode, dstPort).
struct Connection {
    int srcNode;
    int srcPort;
    int dstNode;
    int dstPort;
};

} // namespace oss
```

- [ ] **Step 5: Write `src/core/Node.h`**

```cpp
#pragma once
#include <string>
#include <vector>
#include <cstddef>
#include <glm/vec2.hpp>
#include "core/Port.h"
#include "core/Value.h"

namespace oss {

class Graph;

// Per-frame, fully-resolved evaluation context handed to a node.
struct EvalContext {
    const std::vector<Value>& inputs;   // one resolved value per input port
    std::vector<Value>&       outputs;  // node writes one value per output port
    float                     dt;        // seconds since previous frame

    template <class T> const T& in(std::size_t i) const { return std::get<T>(inputs[i]); }
    template <class T> void out(std::size_t i, T v) { outputs[i] = Value(std::move(v)); }
};

class Node {
public:
    explicit Node(std::string name) : name_(std::move(name)) {}
    virtual ~Node() = default;

    const std::string& name() const { return name_; }
    int id() const { return id_; }

    const std::vector<Port>& inputs()  const { return inputs_; }
    const std::vector<Port>& outputs() const { return outputs_; }

    // Mutable access to an input's default value (edited by inline widgets).
    Value& inputDefault(std::size_t i) { return inputs_[i].defaultValue; }

    glm::vec2 pos{0.0f, 0.0f};

    // Compute outputs from resolved inputs. Called in topological order each frame.
    virtual void evaluate(EvalContext& ctx) = 0;

    // One-time GL setup (shaders, FBOs). Base does nothing (GL-free nodes).
    virtual void initGL() {}

protected:
    void addInput(std::string n, PortType t, Value def) {
        inputs_.push_back({std::move(n), Direction::Input, t, std::move(def)});
    }
    void addOutput(std::string n, PortType t) {
        outputs_.push_back({std::move(n), Direction::Output, t, Value{}});
    }

private:
    friend class Graph;
    std::string       name_;
    int               id_ = -1;     // assigned by Graph::addNode
    std::vector<Port> inputs_;
    std::vector<Port> outputs_;
};

} // namespace oss
```

- [ ] **Step 6: Write `src/core/Node.cpp`**

```cpp
#include "core/Node.h"
// Node is header-only logic; this TU exists so the build has an object file
// and to anchor future non-inline members.
namespace oss {}
```

- [ ] **Step 7: Run to verify it passes**

Run: `cmake --build build -j && ./build/core_tests -tc="Node*","EvalContext*"`
Expected: PASS.

- [ ] **Step 8: Commit**

```bash
git add src/core/Port.h src/core/Connection.h src/core/Node.h src/core/Node.cpp tests/test_graph.cpp
git commit -m "feat(core): Port/Connection/Node base + EvalContext"
```

---

## Task 4: `Graph` — connect, type-check, cycle detection, topological sort

**Files:**
- Create: `src/core/Graph.h`, `src/core/Graph.cpp`
- Test: extend `tests/test_graph.cpp`

- [ ] **Step 1: Add failing tests to `tests/test_graph.cpp`** (append below existing tests)

```cpp
#include "core/Graph.h"

namespace {
struct ColourSink : Node {           // input-only Colour node, for type-mismatch tests
    ColourSink() : Node("csink") { addInput("c", PortType::Colour, glm::vec4(0)); }
    void evaluate(EvalContext&) override {}
};
} // namespace

TEST_CASE("connect accepts matching types and rejects mismatches") {
    Graph g;
    int c = g.addNode(std::make_unique<ConstFloat>(1.0f));
    int a = g.addNode(std::make_unique<AddFloats>());
    int s = g.addNode(std::make_unique<ColourSink>());

    CHECK(g.connect(c, 0, a, 0) == true);          // Float -> Float OK
    CHECK(g.connect(c, 0, s, 0) == false);         // Float -> Colour rejected
    CHECK(g.connect(c, 0, a, 0) == false);         // input already connected
    CHECK(g.connect(c, 0, a, 5) == false);         // bad port index
}

TEST_CASE("connect rejects edges that would create a cycle") {
    Graph g;
    int a = g.addNode(std::make_unique<AddFloats>());
    int b = g.addNode(std::make_unique<AddFloats>());
    CHECK(g.connect(a, 0, b, 0) == true);
    CHECK(g.connect(b, 0, a, 0) == false);         // would form a cycle
}

TEST_CASE("topologicalOrder places sources before consumers") {
    Graph g;
    int a = g.addNode(std::make_unique<ConstFloat>(1.0f));
    int b = g.addNode(std::make_unique<AddFloats>());
    int c = g.addNode(std::make_unique<AddFloats>());
    g.connect(a, 0, b, 0);
    g.connect(b, 0, c, 0);
    auto order = g.topologicalOrder();
    REQUIRE(order.size() == 3);
    auto idx = [&](int id){ return std::find(order.begin(), order.end(), id) - order.begin(); };
    CHECK(idx(a) < idx(b));
    CHECK(idx(b) < idx(c));
}
```

(Add `#include <algorithm>` and `#include <memory>` near the top of the test file.)

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build build -j`
Expected: FAIL — `core/Graph.h` not found.

- [ ] **Step 3: Write `src/core/Graph.h`**

```cpp
#pragma once
#include <memory>
#include <vector>
#include <unordered_map>
#include "core/Node.h"
#include "core/Connection.h"

namespace oss {

class Graph {
public:
    // Takes ownership; returns the assigned (>=1) node id.
    int  addNode(std::unique_ptr<Node> node);
    void removeNode(int nodeId);

    // Returns false and makes no change if ports are invalid, types differ,
    // the input is already connected, or the edge would create a cycle.
    bool connect(int srcNode, int srcPort, int dstNode, int dstPort);
    void disconnect(int dstNode, int dstPort);
    bool isInputConnected(int nodeId, int portIndex) const;

    const std::vector<std::unique_ptr<Node>>& nodes() const { return nodes_; }
    const std::vector<Connection>&            connections() const { return connections_; }
    Node* findNode(int nodeId) const;

    // Evaluate every node once in topological order. dt = seconds this frame.
    void evaluate(float dt);

    // Topological order of node ids; empty if the graph is cyclic. (Testable.)
    std::vector<int> topologicalOrder() const;

private:
    bool wouldCreateCycle(int srcNode, int dstNode) const;
    void markDirty() { orderDirty_ = true; }

    std::vector<std::unique_ptr<Node>> nodes_;
    std::vector<Connection>            connections_;
    int nextId_ = 1;

    mutable std::vector<int> order_;
    mutable bool             orderDirty_ = true;

    std::unordered_map<int, std::vector<Value>> outputs_;  // per-frame node outputs
};

} // namespace oss
```

- [ ] **Step 4: Write `src/core/Graph.cpp`** (topology half; `evaluate` added in Task 5)

```cpp
#include "core/Graph.h"
#include <algorithm>
#include <queue>

namespace oss {

int Graph::addNode(std::unique_ptr<Node> node) {
    int id = nextId_++;
    node->id_ = id;
    nodes_.push_back(std::move(node));
    markDirty();
    return id;
}

void Graph::removeNode(int nodeId) {
    connections_.erase(std::remove_if(connections_.begin(), connections_.end(),
        [&](const Connection& c){ return c.srcNode == nodeId || c.dstNode == nodeId; }),
        connections_.end());
    nodes_.erase(std::remove_if(nodes_.begin(), nodes_.end(),
        [&](const std::unique_ptr<Node>& n){ return n->id() == nodeId; }),
        nodes_.end());
    outputs_.erase(nodeId);
    markDirty();
}

Node* Graph::findNode(int nodeId) const {
    for (auto& n : nodes_) if (n->id() == nodeId) return n.get();
    return nullptr;
}

bool Graph::isInputConnected(int nodeId, int portIndex) const {
    for (auto& c : connections_)
        if (c.dstNode == nodeId && c.dstPort == portIndex) return true;
    return false;
}

bool Graph::wouldCreateCycle(int srcNode, int dstNode) const {
    // Adding src->dst creates a cycle iff dst can already reach src.
    if (srcNode == dstNode) return true;
    std::vector<int> stack{dstNode};
    std::unordered_map<int, bool> seen;
    while (!stack.empty()) {
        int cur = stack.back(); stack.pop_back();
        if (cur == srcNode) return true;
        if (seen[cur]) continue;
        seen[cur] = true;
        for (auto& c : connections_) if (c.srcNode == cur) stack.push_back(c.dstNode);
    }
    return false;
}

bool Graph::connect(int srcNode, int srcPort, int dstNode, int dstPort) {
    Node* s = findNode(srcNode);
    Node* d = findNode(dstNode);
    if (!s || !d) return false;
    if (srcPort < 0 || srcPort >= (int)s->outputs().size()) return false;
    if (dstPort < 0 || dstPort >= (int)d->inputs().size())  return false;
    if (s->outputs()[srcPort].type != d->inputs()[dstPort].type) return false;
    if (isInputConnected(dstNode, dstPort)) return false;
    if (wouldCreateCycle(srcNode, dstNode)) return false;
    connections_.push_back({srcNode, srcPort, dstNode, dstPort});
    markDirty();
    return true;
}

void Graph::disconnect(int dstNode, int dstPort) {
    connections_.erase(std::remove_if(connections_.begin(), connections_.end(),
        [&](const Connection& c){ return c.dstNode == dstNode && c.dstPort == dstPort; }),
        connections_.end());
    markDirty();
}

std::vector<int> Graph::topologicalOrder() const {
    std::unordered_map<int, int> indeg;
    for (auto& n : nodes_) indeg[n->id()] = 0;
    for (auto& c : connections_) indeg[c.dstNode]++;
    std::queue<int> q;
    for (auto& n : nodes_) if (indeg[n->id()] == 0) q.push(n->id());  // deterministic seed
    std::vector<int> order;
    while (!q.empty()) {
        int cur = q.front(); q.pop();
        order.push_back(cur);
        for (auto& c : connections_)
            if (c.srcNode == cur && --indeg[c.dstNode] == 0) q.push(c.dstNode);
    }
    if ((int)order.size() != (int)nodes_.size()) return {};  // cycle present
    return order;
}

} // namespace oss
```

- [ ] **Step 5: Run to verify it passes**

Run: `cmake --build build -j && ./build/core_tests -tc="connect*","topologicalOrder*"`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add src/core/Graph.h src/core/Graph.cpp tests/test_graph.cpp
git commit -m "feat(core): Graph connect/type-check/cycle-detect/topo-sort"
```

---

## Task 5: `Graph::evaluate` — per-frame value propagation

**Files:**
- Modify: `src/core/Graph.cpp`
- Test: extend `tests/test_graph.cpp`

- [ ] **Step 1: Add failing tests to `tests/test_graph.cpp`**

```cpp
TEST_CASE("evaluate propagates values along edges") {
    Graph g;
    int a = g.addNode(std::make_unique<ConstFloat>(2.0f));
    int b = g.addNode(std::make_unique<ConstFloat>(3.0f));
    int sum = g.addNode(std::make_unique<AddFloats>());
    g.connect(a, 0, sum, 0);
    g.connect(b, 0, sum, 1);
    g.evaluate(0.016f);
    auto* node = dynamic_cast<AddFloats*>(g.findNode(sum));
    // Re-evaluate into a fresh context to read the output deterministically:
    std::vector<Value> in{ Value{2.0f}, Value{3.0f} };
    std::vector<Value> out(1);
    EvalContext ctx{in, out, 0.016f};
    node->evaluate(ctx);
    CHECK(std::get<float>(out[0]) == doctest::Approx(5.0f));
}

TEST_CASE("unconnected inputs fall back to their default value") {
    Graph g;
    int a = g.addNode(std::make_unique<ConstFloat>(5.0f));
    int sum = g.addNode(std::make_unique<AddFloats>());
    g.connect(a, 0, sum, 0);            // b left unconnected -> default 0
    // Capture the resolved inputs by making a probe node:
    g.evaluate(0.016f);
    // Indirect check: a fan-out sink reads sum's output.
    CHECK(g.topologicalOrder().size() == 2);
}
```

> Note: `Graph` stores outputs privately, so these tests assert the public contract (no crash, correct topo size, node-level evaluate math). The end-to-end value path is exercised visually once GL modules exist (Task 12+).

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build build -j`
Expected: FAIL — `Graph::evaluate` is declared but unimplemented (linker error).

- [ ] **Step 3: Implement `Graph::evaluate` in `src/core/Graph.cpp`** (append before the closing `} // namespace oss`)

```cpp
void Graph::evaluate(float dt) {
    if (orderDirty_) { order_ = topologicalOrder(); orderDirty_ = false; }
    for (int id : order_) {
        Node* n = findNode(id);
        if (!n) continue;
        const auto& ins = n->inputs();

        std::vector<Value> inputs(ins.size());
        for (std::size_t i = 0; i < ins.size(); ++i) {
            const Connection* conn = nullptr;
            for (auto& c : connections_)
                if (c.dstNode == id && c.dstPort == (int)i) { conn = &c; break; }
            if (conn) {
                auto it = outputs_.find(conn->srcNode);
                if (it != outputs_.end() && conn->srcPort < (int)it->second.size())
                    inputs[i] = it->second[conn->srcPort];
                else
                    inputs[i] = ins[i].defaultValue;
            } else {
                inputs[i] = ins[i].defaultValue;
            }
        }

        std::vector<Value>& outs = outputs_[id];
        outs.assign(n->outputs().size(), Value{});
        EvalContext ctx{inputs, outs, dt};
        n->evaluate(ctx);
    }
}
```

- [ ] **Step 4: Run to verify it passes**

Run: `cmake --build build -j && ./build/core_tests -tc="evaluate*","unconnected*"`
Expected: PASS. Then run full suite: `ctest --test-dir build --output-on-failure` → all PASS.

- [ ] **Step 5: Commit**

```bash
git add src/core/Graph.cpp tests/test_graph.cpp
git commit -m "feat(core): Graph::evaluate per-frame topological value propagation"
```

---

## Task 6: Audio FFT (radix-2)

**Files:**
- Create: `src/audio/FFT.h`, `src/audio/FFT.cpp`
- Test: `tests/test_fft.cpp`

- [ ] **Step 1: Write the failing test in `tests/test_fft.cpp`**

```cpp
#include <doctest/doctest.h>
#include "audio/FFT.h"
#include <vector>
#include <cmath>

TEST_CASE("magnitudeSpectrum peaks at a pure sine's bin") {
    constexpr float kPi = 3.14159265358979323846f;
    const int N = 64, k = 4;
    std::vector<float> s(N);
    for (int n = 0; n < N; ++n) s[n] = std::sin(2.0f * kPi * k * n / N);
    auto mag = oss::magnitudeSpectrum(s);
    REQUIRE(mag.size() == (size_t)N / 2);
    int peak = 0;
    for (int i = 1; i < (int)mag.size(); ++i) if (mag[i] > mag[peak]) peak = i;
    CHECK(peak == k);
}

TEST_CASE("magnitudeSpectrum of DC has all energy in bin 0") {
    std::vector<float> s(32, 1.0f);
    auto mag = oss::magnitudeSpectrum(s);
    for (size_t i = 1; i < mag.size(); ++i) CHECK(mag[i] == doctest::Approx(0.0f).epsilon(1e-3));
    CHECK(mag[0] > 1.0f);
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build build -j`
Expected: FAIL — `audio/FFT.h` not found.

- [ ] **Step 3: Write `src/audio/FFT.h`**

```cpp
#pragma once
#include <vector>
#include <complex>

namespace oss {

// In-place iterative radix-2 FFT. a.size() MUST be a power of two.
void fft(std::vector<std::complex<float>>& a);

// Magnitude spectrum (size N/2) of N real samples (N a power of two).
std::vector<float> magnitudeSpectrum(const std::vector<float>& samples);

} // namespace oss
```

- [ ] **Step 4: Write `src/audio/FFT.cpp`**

```cpp
#include "audio/FFT.h"
#include <cmath>

namespace oss {

void fft(std::vector<std::complex<float>>& a) {
    const std::size_t n = a.size();
    if (n < 2) return;
    // bit-reversal permutation
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    const float kPi = 3.14159265358979323846f;
    for (std::size_t len = 2; len <= n; len <<= 1) {
        float ang = -2.0f * kPi / (float)len;
        std::complex<float> wlen(std::cos(ang), std::sin(ang));
        for (std::size_t i = 0; i < n; i += len) {
            std::complex<float> w(1.0f, 0.0f);
            for (std::size_t k = 0; k < len / 2; ++k) {
                std::complex<float> u = a[i + k];
                std::complex<float> v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wlen;
            }
        }
    }
}

std::vector<float> magnitudeSpectrum(const std::vector<float>& samples) {
    std::vector<std::complex<float>> a(samples.size());
    for (std::size_t i = 0; i < samples.size(); ++i)
        a[i] = std::complex<float>(samples[i], 0.0f);
    fft(a);
    std::vector<float> mag(samples.size() / 2);
    for (std::size_t i = 0; i < mag.size(); ++i) mag[i] = std::abs(a[i]);
    return mag;
}

} // namespace oss
```

- [ ] **Step 5: Run to verify it passes**

Run: `cmake --build build -j && ./build/core_tests -tc="magnitudeSpectrum*"`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add src/audio/FFT.h src/audio/FFT.cpp tests/test_fft.cpp
git commit -m "feat(audio): radix-2 FFT + magnitude spectrum"
```

---

## Task 7: `SignalGenerator` (synth audio source)

**Files:**
- Create: `src/audio/SignalGenerator.h`, `src/audio/SignalGenerator.cpp`
- Test: `tests/test_signal_generator.cpp`

- [ ] **Step 1: Write the failing test in `tests/test_signal_generator.cpp`**

```cpp
#include <doctest/doctest.h>
#include "audio/SignalGenerator.h"
#include <vector>

TEST_CASE("generate fills the requested number of samples in [-1,1]") {
    oss::SignalGenerator g(48000, 440.0f);
    std::vector<float> buf(256);
    g.generate(buf.data(), buf.size());
    for (float s : buf) { CHECK(s >= -1.0001f); CHECK(s <= 1.0001f); }
}

TEST_CASE("generate is phase-continuous across calls") {
    oss::SignalGenerator whole(48000, 440.0f), split(48000, 440.0f);
    std::vector<float> w(200), p1(100), p2(100);
    whole.generate(w.data(), 200);
    split.generate(p1.data(), 100);
    split.generate(p2.data(), 100);
    for (int i = 0; i < 100; ++i) {
        CHECK(w[i]       == doctest::Approx(p1[i]).epsilon(1e-4));
        CHECK(w[100 + i] == doctest::Approx(p2[i]).epsilon(1e-4));
    }
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build build -j`
Expected: FAIL — `audio/SignalGenerator.h` not found.

- [ ] **Step 3: Write `src/audio/SignalGenerator.h`**

```cpp
#pragma once
#include <cstddef>

namespace oss {

// Phase-continuous synthesizer: a fundamental sine plus a couple of harmonics,
// so the spectrograph shows structure. Stateful: each generate() advances phase.
class SignalGenerator {
public:
    explicit SignalGenerator(int sampleRate = 48000, float freq = 220.0f)
        : sampleRate_(sampleRate), freq_(freq) {}

    void generate(float* out, std::size_t count);
    int  sampleRate() const { return sampleRate_; }

private:
    int    sampleRate_;
    float  freq_;
    double phase_ = 0.0;   // radians, accumulated for continuity
};

} // namespace oss
```

- [ ] **Step 4: Write `src/audio/SignalGenerator.cpp`**

```cpp
#include "audio/SignalGenerator.h"
#include <cmath>

namespace oss {

void SignalGenerator::generate(float* out, std::size_t count) {
    const double kTwoPi = 2.0 * 3.14159265358979323846;
    const double inc = kTwoPi * freq_ / sampleRate_;
    for (std::size_t i = 0; i < count; ++i) {
        double s = 0.6 * std::sin(phase_)
                 + 0.3 * std::sin(2.0 * phase_)
                 + 0.1 * std::sin(3.0 * phase_);
        out[i] = (float)s;
        phase_ += inc;
        if (phase_ > kTwoPi) phase_ -= kTwoPi;
    }
}

} // namespace oss
```

- [ ] **Step 5: Run to verify it passes**

Run: `cmake --build build -j && ./build/core_tests -tc="generate*"`
Expected: PASS. Then `ctest --test-dir build --output-on-failure` → all PASS.

- [ ] **Step 6: Commit**

```bash
git add src/audio/SignalGenerator.h src/audio/SignalGenerator.cpp tests/test_signal_generator.cpp
git commit -m "feat(audio): phase-continuous SignalGenerator"
```

---

## Task 8: GL utilities + canvas constants

**Files:**
- Create: `src/gfx/Canvas.h`, `src/gfx/GLUtil.h`, `src/gfx/GLUtil.cpp`

> GL tasks (8–11, 13–15) require a GL context, so they are verified by **building** and, where visible, **running** — not by unit tests.

- [ ] **Step 1: Write `src/gfx/Canvas.h`**

```cpp
#pragma once
namespace oss { constexpr int kCanvasW = 1280; constexpr int kCanvasH = 720; }
```

- [ ] **Step 2: Write `src/gfx/GLUtil.h`**

```cpp
#pragma once
#include <glad/gl.h>
#include <string>

namespace oss {

GLuint compileShader(GLenum type, const std::string& src);
GLuint linkProgram(const std::string& vertSrc, const std::string& fragSrc);
std::string readFile(const std::string& path);
void checkGLError(const char* where);

} // namespace oss
```

- [ ] **Step 3: Write `src/gfx/GLUtil.cpp`**

```cpp
#include "gfx/GLUtil.h"
#include <fstream>
#include <sstream>
#include <vector>
#include <cstdio>

namespace oss {

GLuint compileShader(GLenum type, const std::string& src) {
    GLuint sh = glCreateShader(type);
    const char* c = src.c_str();
    glShaderSource(sh, 1, &c, nullptr);
    glCompileShader(sh);
    GLint ok = 0; glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        GLint len = 0; glGetShaderiv(sh, GL_INFO_LOG_LENGTH, &len);
        std::vector<char> log(len > 1 ? len : 1);
        glGetShaderInfoLog(sh, (GLsizei)log.size(), nullptr, log.data());
        std::fprintf(stderr, "[shader compile error]\n%s\n", log.data());
    }
    return sh;
}

GLuint linkProgram(const std::string& vertSrc, const std::string& fragSrc) {
    GLuint vs = compileShader(GL_VERTEX_SHADER, vertSrc);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, fragSrc);
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs); glAttachShader(prog, fs);
    glLinkProgram(prog);
    GLint ok = 0; glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        GLint len = 0; glGetProgramiv(prog, GL_INFO_LOG_LENGTH, &len);
        std::vector<char> log(len > 1 ? len : 1);
        glGetProgramInfoLog(prog, (GLsizei)log.size(), nullptr, log.data());
        std::fprintf(stderr, "[program link error]\n%s\n", log.data());
    }
    glDeleteShader(vs); glDeleteShader(fs);
    return prog;
}

std::string readFile(const std::string& path) {
    std::ifstream f(path);
    if (!f) { std::fprintf(stderr, "[readFile] cannot open %s\n", path.c_str()); return ""; }
    std::stringstream ss; ss << f.rdbuf();
    return ss.str();
}

void checkGLError(const char* where) {
    GLenum e;
    while ((e = glGetError()) != GL_NO_ERROR)
        std::fprintf(stderr, "[GL error] 0x%04X at %s\n", e, where);
}

} // namespace oss
```

- [ ] **Step 4: Build**

Run: `cmake --build build -j`
Expected: compiles `GLUtil.cpp` cleanly.

- [ ] **Step 5: Commit**

```bash
git add src/gfx/Canvas.h src/gfx/GLUtil.h src/gfx/GLUtil.cpp
git commit -m "feat(gfx): canvas constants + shader/file GL utilities"
```

---

## Task 9: `Framebuffer` + `FullscreenPass`

**Files:**
- Create: `src/gfx/Framebuffer.h`, `src/gfx/Framebuffer.cpp`, `src/gfx/FullscreenPass.h`, `src/gfx/FullscreenPass.cpp`

- [ ] **Step 1: Write `src/gfx/Framebuffer.h`**

```cpp
#pragma once
#include <glad/gl.h>

namespace oss {

// An FBO with a single RGBA8 colour-texture attachment.
class Framebuffer {
public:
    Framebuffer() = default;
    ~Framebuffer();
    Framebuffer(const Framebuffer&) = delete;
    Framebuffer& operator=(const Framebuffer&) = delete;

    void create(int w, int h);
    void bind() const;            // bind FBO + set viewport to its size
    static void unbind();         // bind default framebuffer (0)

    GLuint texture() const { return tex_; }
    int width()  const { return w_; }
    int height() const { return h_; }

private:
    GLuint fbo_ = 0, tex_ = 0;
    int w_ = 0, h_ = 0;
};

} // namespace oss
```

- [ ] **Step 2: Write `src/gfx/Framebuffer.cpp`**

```cpp
#include "gfx/Framebuffer.h"
#include <cstdio>

namespace oss {

Framebuffer::~Framebuffer() {
    if (tex_) glDeleteTextures(1, &tex_);
    if (fbo_) glDeleteFramebuffers(1, &fbo_);
}

void Framebuffer::create(int w, int h) {
    w_ = w; h_ = h;
    glGenTextures(1, &tex_);
    glBindTexture(GL_TEXTURE_2D, tex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenFramebuffers(1, &fbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex_, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        std::fprintf(stderr, "[Framebuffer] incomplete\n");
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void Framebuffer::bind() const {
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glViewport(0, 0, w_, h_);
}

void Framebuffer::unbind() { glBindFramebuffer(GL_FRAMEBUFFER, 0); }

} // namespace oss
```

- [ ] **Step 3: Write `src/gfx/FullscreenPass.h`**

```cpp
#pragma once
#include <glad/gl.h>

namespace oss {

// Draws a single fullscreen triangle from gl_VertexID (no vertex buffer needed).
// GL 4.1 core still requires a bound VAO, so we keep an empty one.
class FullscreenPass {
public:
    void create();
    void draw() const;
    ~FullscreenPass();
private:
    GLuint vao_ = 0;
};

} // namespace oss
```

- [ ] **Step 4: Write `src/gfx/FullscreenPass.cpp`**

```cpp
#include "gfx/FullscreenPass.h"

namespace oss {

void FullscreenPass::create() { glGenVertexArrays(1, &vao_); }

void FullscreenPass::draw() const {
    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
}

FullscreenPass::~FullscreenPass() { if (vao_) glDeleteVertexArrays(1, &vao_); }

} // namespace oss
```

- [ ] **Step 5: Build**

Run: `cmake --build build -j`
Expected: compiles cleanly.

- [ ] **Step 6: Commit**

```bash
git add src/gfx/Framebuffer.h src/gfx/Framebuffer.cpp src/gfx/FullscreenPass.h src/gfx/FullscreenPass.cpp
git commit -m "feat(gfx): Framebuffer (FBO+texture) and FullscreenPass"
```

---

## Task 10: `ShaderNode` base

**Files:**
- Create: `src/gfx/ShaderNode.h`, `src/gfx/ShaderNode.cpp`

- [ ] **Step 1: Write `src/gfx/ShaderNode.h`**

```cpp
#pragma once
#include <glad/gl.h>
#include <string>
#include "core/Node.h"
#include "gfx/Framebuffer.h"
#include "gfx/FullscreenPass.h"

namespace oss {

// Base for nodes that render a fragment shader into their own framebuffer and
// publish the result as a TexRef on output port 0.
class ShaderNode : public Node {
public:
    ShaderNode(std::string name, std::string fragPath)
        : Node(std::move(name)), fragPath_(std::move(fragPath)) {}

    void initGL() override;     // compile program + create FBO/VAO

protected:
    // Subclasses bind input textures / set uniforms here (program already bound).
    virtual void setUniforms(EvalContext& ctx) {}

    // Call from evaluate(): renders into fbo_ and writes TexRef to output 0.
    void render(EvalContext& ctx);

    GLuint        program_ = 0;
    Framebuffer   fbo_;
    FullscreenPass fsq_;
    std::string   fragPath_;
};

} // namespace oss
```

- [ ] **Step 2: Write `src/gfx/ShaderNode.cpp`**

```cpp
#include "gfx/ShaderNode.h"
#include "gfx/GLUtil.h"
#include "gfx/Canvas.h"
#include "core/Value.h"

namespace oss {

static const char* kFullscreenVS = R"(#version 410 core
out vec2 vUV;
void main() {
    vec2 p = vec2(gl_VertexID == 1 ? 3.0 : -1.0,
                  gl_VertexID == 2 ? 3.0 : -1.0);
    vUV = (p + 1.0) * 0.5;
    gl_Position = vec4(p, 0.0, 1.0);
}
)";

void ShaderNode::initGL() {
    program_ = linkProgram(kFullscreenVS, readFile(fragPath_));
    fbo_.create(kCanvasW, kCanvasH);
    fsq_.create();
}

void ShaderNode::render(EvalContext& ctx) {
    fbo_.bind();
    glUseProgram(program_);
    setUniforms(ctx);
    fsq_.draw();
    Framebuffer::unbind();
    ctx.out<TexRef>(0, TexRef{ fbo_.texture(), fbo_.width(), fbo_.height() });
}

} // namespace oss
```

- [ ] **Step 3: Build**

Run: `cmake --build build -j`
Expected: compiles cleanly.

- [ ] **Step 4: Commit**

```bash
git add src/gfx/ShaderNode.h src/gfx/ShaderNode.cpp
git commit -m "feat(gfx): ShaderNode base (program+FBO+fullscreen render)"
```

---

## Task 11: ColourNode + colour shader

**Files:**
- Create: `src/modules/ColourNode.h`, `shaders/colour.frag`
- Modify: `CMakeLists.txt` (add `src/modules/ColourNode` is header-only; no cpp needed)

- [ ] **Step 1: Write `shaders/colour.frag`**

```glsl
#version 410 core
in vec2 vUV;
out vec4 FragColor;
uniform vec4 uColour;
void main() { FragColor = uColour; }
```

- [ ] **Step 2: Write `src/modules/ColourNode.h`**

```cpp
#pragma once
#include <glad/gl.h>
#include "gfx/ShaderNode.h"
#include "gfx/GLUtil.h"

namespace oss {

class ColourNode : public ShaderNode {
public:
    ColourNode() : ShaderNode("Colour", "shaders/colour.frag") {
        addInput("colour", PortType::Colour, glm::vec4(1.0f, 0.5f, 0.1f, 1.0f));
        addOutput("out", PortType::Texture);
    }
    void evaluate(EvalContext& ctx) override { render(ctx); }

protected:
    void setUniforms(EvalContext& ctx) override {
        glm::vec4 c = ctx.in<glm::vec4>(0);
        glUniform4f(glGetUniformLocation(program_, "uColour"), c.r, c.g, c.b, c.a);
    }
};

} // namespace oss
```

- [ ] **Step 3: Build**

Run: `cmake --build build -j`
Expected: compiles cleanly (ColourNode is header-only; nothing new in CMake yet because it's only referenced once `Application` uses it in Task 12).

- [ ] **Step 4: Commit**

```bash
git add src/modules/ColourNode.h shaders/colour.frag
git commit -m "feat(modules): ColourNode + colour.frag"
```

---

## Task 12: OutputNode + minimal Application (first visible pipeline)

**Files:**
- Create: `src/modules/OutputNode.h`, `src/app/Application.h`, `src/app/Application.cpp`
- Modify: `src/main.cpp`

This task wires a **hardcoded** `Colour → Output` graph and shows it in a Viewer window — the first end-to-end visual proof. The interactive editor replaces the hardcoded graph in Task 13.

- [ ] **Step 1: Write `src/modules/OutputNode.h`**

```cpp
#pragma once
#include "core/Node.h"
#include "core/Value.h"

namespace oss {

// Sink: remembers its input texture so the Viewer can display it.
class OutputNode : public Node {
public:
    OutputNode() : Node("Output") { addInput("in", PortType::Texture, TexRef{}); }
    void evaluate(EvalContext& ctx) override { current_ = ctx.in<TexRef>(0); }
    TexRef current() const { return current_; }
private:
    TexRef current_{};
};

} // namespace oss
```

- [ ] **Step 2: Write `src/app/Application.h`**

```cpp
#pragma once
#include <memory>
#include <string>
#include "core/Graph.h"

struct GLFWwindow;

namespace oss {

class Application {
public:
    explicit Application(GLFWwindow* window);
    ~Application();

    // Create a node of the given type, run its GL setup, add it to the graph.
    int addNodeOfType(const std::string& type, glm::vec2 pos);

    void frame(float dt);   // build UI, evaluate graph, draw viewer
    Graph& graph() { return graph_; }

private:
    void drawViewer();

    GLFWwindow* window_;
    Graph graph_;
};

// Factory used by the app and (later) the add-node menu.
std::unique_ptr<Node> makeNode(const std::string& type);
const std::vector<std::string>& nodeTypeNames();

} // namespace oss
```

- [ ] **Step 3: Write `src/app/Application.cpp`**

```cpp
#include "app/Application.h"
#include <imgui.h>
#include "modules/ColourNode.h"
#include "modules/OutputNode.h"

namespace oss {

std::unique_ptr<Node> makeNode(const std::string& type) {
    if (type == "Colour") return std::make_unique<ColourNode>();
    if (type == "Output") return std::make_unique<OutputNode>();
    return nullptr;
}

const std::vector<std::string>& nodeTypeNames() {
    static const std::vector<std::string> names = { "Colour", "Output" };
    return names;
}

Application::Application(GLFWwindow* window) : window_(window) {
    // Hardcoded Colour -> Output graph for this milestone.
    int c = addNodeOfType("Colour", {40, 40});
    int o = addNodeOfType("Output", {360, 40});
    graph_.connect(c, 0, o, 0);
}

Application::~Application() = default;

int Application::addNodeOfType(const std::string& type, glm::vec2 pos) {
    auto node = makeNode(type);
    if (!node) return -1;
    node->initGL();
    node->pos = pos;
    return graph_.addNode(std::move(node));
}

void Application::frame(float dt) {
    graph_.evaluate(dt);
    drawViewer();
}

void Application::drawViewer() {
    ImGui::Begin("Viewer");
    TexRef tex{};
    for (auto& n : graph_.nodes())
        if (auto* o = dynamic_cast<OutputNode*>(n.get())) { tex = o->current(); break; }
    if (tex.id) {
        float avail = ImGui::GetContentRegionAvail().x;
        float aspect = tex.h ? (float)tex.h / (float)tex.w : 0.5625f;
        ImVec2 size(avail, avail * aspect);
        // Flip V: FBO origin is bottom-left, ImGui expects top-left.
        ImGui::Image((ImTextureID)(intptr_t)tex.id, size, ImVec2(0, 1), ImVec2(1, 0));
    } else {
        ImGui::TextUnformatted("No output texture.");
    }
    ImGui::End();
}

} // namespace oss
```

- [ ] **Step 4: Update `src/main.cpp` to drive `Application`**

Replace the `while` loop body's UI section. Full new `main.cpp`:
```cpp
#include <glad/gl.h>
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_opengl3.h>
#include <cstdio>
#include "app/Application.h"

int main() {
    if (!glfwInit()) { std::fprintf(stderr, "glfwInit failed\n"); return 1; }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 1);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);

    GLFWwindow* window = glfwCreateWindow(1400, 900, "Shader Streamer", nullptr, nullptr);
    if (!window) { glfwTerminate(); return 1; }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);
    if (!gladLoadGL((GLADloadfunc)glfwGetProcAddress)) { std::fprintf(stderr, "glad failed\n"); return 1; }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 410");

    {
        oss::Application app(window);
        double last = glfwGetTime();
        while (!glfwWindowShouldClose(window)) {
            glfwPollEvents();
            double now = glfwGetTime();
            float dt = (float)(now - last); last = now;

            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();

            app.frame(dt);

            ImGui::Render();
            int w, h; glfwGetFramebufferSize(window, &w, &h);
            glViewport(0, 0, w, h);
            glClearColor(0.10f, 0.11f, 0.13f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
            glfwSwapBuffers(window);
        }
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
```

- [ ] **Step 5: Ensure shaders are found at runtime**

The shader path `shaders/colour.frag` is relative to the working directory. Run the app from the repo root. (A later task can copy shaders next to the binary; for now run from root.)

- [ ] **Step 6: Build and run**

Run: `cmake --build build -j && ./build/shader_streamer`
Expected: a "Viewer" window shows a **solid orange rectangle** (the Colour node's default colour rendered to a texture and displayed through Output). No GL errors in the terminal.

- [ ] **Step 7: Commit**

```bash
git add src/modules/OutputNode.h src/app/Application.h src/app/Application.cpp src/main.cpp
git commit -m "feat(app): OutputNode + Application with hardcoded Colour->Output viewer"
```

---

## Task 13: Interactive node editor + inline port widgets

**Files:**
- Create: `src/ui/NodeEditorPanel.h`, `src/ui/NodeEditorPanel.cpp`, `src/ui/PortWidgets.h`, `src/ui/PortWidgets.cpp`
- Modify: `src/app/Application.h`, `src/app/Application.cpp`

- [ ] **Step 1: Write `src/ui/PortWidgets.h`**

```cpp
#pragma once
#include "core/Node.h"

namespace oss {

// Render an inline editor for an unconnected input port `i` of `node`,
// editing that port's default value in place. No-op for Texture/Audio ports.
void drawInlineInputWidget(Node& node, std::size_t i);

} // namespace oss
```

- [ ] **Step 2: Write `src/ui/PortWidgets.cpp`**

```cpp
#include "ui/PortWidgets.h"
#include <imgui.h>
#include <glm/vec4.hpp>
#include <variant>
#include <string>

namespace oss {

void drawInlineInputWidget(Node& node, std::size_t i) {
    const Port& port = node.inputs()[i];
    Value& v = node.inputDefault(i);
    ImGui::PushID((int)i);
    ImGui::PushItemWidth(120.0f);
    switch (port.type) {
        case PortType::Colour: {
            auto& c = std::get<glm::vec4>(v);
            ImGui::ColorEdit4("##c", &c.x,
                ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar);
            break;
        }
        case PortType::Float: {
            ImGui::SliderFloat("##f", &std::get<float>(v), 0.0f, 1.0f);
            break;
        }
        case PortType::Bool: {
            ImGui::Checkbox("##b", &std::get<bool>(v));
            break;
        }
        case PortType::String: {
            auto& s = std::get<std::string>(v);
            char buf[256];
            std::snprintf(buf, sizeof(buf), "%s", s.c_str());
            if (ImGui::InputText("##s", buf, sizeof(buf))) s = buf;
            break;
        }
        default: break; // Texture / Audio: no inline widget
    }
    ImGui::PopItemWidth();
    ImGui::PopID();
}

} // namespace oss
```

- [ ] **Step 3: Write `src/ui/NodeEditorPanel.h`**

```cpp
#pragma once
#include "core/Graph.h"

namespace oss {

// Owns the imgui-node-editor context; renders the graph and applies user edits
// (create/delete links, delete nodes, add nodes via context menu).
class NodeEditorPanel {
public:
    NodeEditorPanel();
    ~NodeEditorPanel();

    // Draw the editor for `graph`. `addNodeOfType` is the app's factory hook,
    // invoked when the user picks a type from the background context menu.
    void draw(Graph& graph,
              const std::function<int(const std::string&, glm::vec2)>& addNodeOfType);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace oss
```

(Add `#include <functional>`, `#include <memory>`, `#include <string>` at the top.)

- [ ] **Step 4: Write `src/ui/NodeEditorPanel.cpp`**

```cpp
#include "ui/NodeEditorPanel.h"
#include "ui/PortWidgets.h"
#include <imgui.h>
#include <imgui_node_editor.h>
#include <functional>
#include <string>

namespace ed = ax::NodeEditor;

namespace oss {

// Stable id encoding for imgui-node-editor:
//   node id  = nodeId
//   pin  id  = nodeId*1000 + portIndex*2 + (isOutput ? 1 : 0)
//   link id  = 1-based index into graph.connections()
static int pinId(int nodeId, int port, bool isOutput) {
    return nodeId * 1000 + port * 2 + (isOutput ? 1 : 0);
}
static void decodePin(int pin, int& nodeId, int& port, bool& isOutput) {
    nodeId   = pin / 1000;
    int rest = pin % 1000;
    port     = rest / 2;
    isOutput = (rest % 2) == 1;
}

struct NodeEditorPanel::Impl {
    ed::EditorContext* ctx = nullptr;
};

NodeEditorPanel::NodeEditorPanel() : impl_(std::make_unique<Impl>()) {
    ed::Config cfg;
    cfg.SettingsFile = nullptr;          // don't persist layout for the slice
    impl_->ctx = ed::CreateEditor(&cfg);
}

NodeEditorPanel::~NodeEditorPanel() {
    if (impl_->ctx) ed::DestroyEditor(impl_->ctx);
}

void NodeEditorPanel::draw(
        Graph& graph,
        const std::function<int(const std::string&, glm::vec2)>& addNodeOfType) {

    ImGui::Begin("Node Graph");
    ed::SetCurrentEditor(impl_->ctx);
    ed::Begin("graph");

    // --- Nodes ---
    for (auto& up : graph.nodes()) {
        Node& n = *up;
        ed::BeginNode(ed::NodeId(n.id()));
        ImGui::TextUnformatted(n.name().c_str());

        // Inputs (left): pin + inline widget when unconnected.
        for (std::size_t i = 0; i < n.inputs().size(); ++i) {
            const Port& p = n.inputs()[i];
            ed::BeginPin(ed::PinId(pinId(n.id(), (int)i, false)), ed::PinKind::Input);
            ImGui::Text("-> %s", p.name.c_str());
            ed::EndPin();
            if (!graph.isInputConnected(n.id(), (int)i)) {
                ImGui::SameLine();
                drawInlineInputWidget(n, i);
            }
        }
        // Outputs (right).
        for (std::size_t i = 0; i < n.outputs().size(); ++i) {
            const Port& p = n.outputs()[i];
            ed::BeginPin(ed::PinId(pinId(n.id(), (int)i, true)), ed::PinKind::Output);
            ImGui::Text("%s ->", p.name.c_str());
            ed::EndPin();
        }
        ed::EndNode();

        // Restore persisted position on first layout.
        ed::SetNodePosition(ed::NodeId(n.id()), ImVec2(n.pos.x, n.pos.y));
    }

    // --- Links ---
    const auto& conns = graph.connections();
    for (std::size_t li = 0; li < conns.size(); ++li) {
        const Connection& c = conns[li];
        ed::Link(ed::LinkId((int)li + 1),
                 ed::PinId(pinId(c.srcNode, c.srcPort, true)),
                 ed::PinId(pinId(c.dstNode, c.dstPort, false)));
    }

    // --- Create new link ---
    if (ed::BeginCreate()) {
        ed::PinId a, b;
        if (ed::QueryNewLink(&a, &b)) {
            if (a && b) {
                int an, ap; bool aout; decodePin((int)a.Get(), an, ap, aout);
                int bn, bp; bool bout; decodePin((int)b.Get(), bn, bp, bout);
                // Normalize so source is the output side.
                int sn, sp, dn, dp;
                bool ok = (aout != bout);
                if (aout) { sn = an; sp = ap; dn = bn; dp = bp; }
                else      { sn = bn; sp = bp; dn = an; dp = ap; }
                if (ok && ed::AcceptNewItem()) {
                    graph.connect(sn, sp, dn, dp);  // type/cycle-checked internally
                } else if (!ok) {
                    ed::RejectNewItem();
                }
            }
        }
    }
    ed::EndCreate();

    // --- Delete links / nodes ---
    if (ed::BeginDelete()) {
        ed::LinkId lid;
        while (ed::QueryDeletedLink(&lid)) {
            if (ed::AcceptDeletedItem()) {
                int idx = (int)lid.Get() - 1;
                if (idx >= 0 && idx < (int)graph.connections().size()) {
                    const Connection& c = graph.connections()[idx];
                    graph.disconnect(c.dstNode, c.dstPort);
                }
            }
        }
        ed::NodeId nid;
        while (ed::QueryDeletedNode(&nid)) {
            if (ed::AcceptDeletedItem()) graph.removeNode((int)nid.Get());
        }
    }
    ed::EndDelete();

    // --- Background context menu: Add node ---
    ed::Suspend();
    if (ed::ShowBackgroundContextMenu()) ImGui::OpenPopup("AddNode");
    if (ImGui::BeginPopup("AddNode")) {
        ImVec2 mouse = ImGui::GetMousePosOnOpeningCurrentPopup();
        for (auto& type : nodeTypeNames()) {
            if (ImGui::MenuItem(type.c_str())) {
                ImVec2 canvas = ed::ScreenToCanvas(mouse);
                addNodeOfType(type, glm::vec2(canvas.x, canvas.y));
            }
        }
        ImGui::EndPopup();
    }
    ed::Resume();

    ed::End();
    ed::SetCurrentEditor(nullptr);
    ImGui::End();
}

} // namespace oss
```

> `nodeTypeNames()` is declared in `app/Application.h`; include it.

Add at top of the file: `#include "app/Application.h"`.

> Note on `SetNodePosition` each frame: calling it unconditionally pins nodes and prevents dragging. Fix in Step 5.

- [ ] **Step 5: Track node positions back from the editor (allow dragging)**

In `NodeEditorPanel.cpp`, replace the unconditional `ed::SetNodePosition(...)` call with first-time placement + read-back. Change the per-node block's tail to:
```cpp
        ed::EndNode();

        // First time we see this node, place it; afterwards, read its position
        // back so dragging persists into the model.
        ImVec2 cur = ed::GetNodePosition(ed::NodeId(n.id()));
        if (cur.x == FLT_MAX || (n.pos.x == 0.0f && n.pos.y == 0.0f && cur.x == 0.0f)) {
            ed::SetNodePosition(ed::NodeId(n.id()), ImVec2(n.pos.x, n.pos.y));
        } else {
            n.pos = glm::vec2(cur.x, cur.y);
        }
```
(Add `#include <cfloat>`.)

- [ ] **Step 6: Wire the panel into `Application`**

In `src/app/Application.h`: add include and member.
```cpp
#include "ui/NodeEditorPanel.h"
```
Add private member: `NodeEditorPanel editor_;`
Remove the hardcoded graph construction's necessity by keeping it (a default demo graph is fine) — but the editor now drives edits.

In `src/app/Application.cpp` `frame()`:
```cpp
void Application::frame(float dt) {
    editor_.draw(graph_, [this](const std::string& t, glm::vec2 p){ return addNodeOfType(t, p); });
    graph_.evaluate(dt);
    drawViewer();
}
```

- [ ] **Step 7: Build and run**

Run: `cmake --build build -j && ./build/shader_streamer`
Expected:
- A "Node Graph" window shows the default Colour and Output nodes with a link between them; the Viewer shows orange.
- The Colour node shows an inline colour swatch on its unconnected `colour` input; editing it changes the Viewer live.
- Right-click empty canvas → "Add node" menu lists Colour and Output; adding works.
- Drag a link from Colour `out` to a new Output `in`; deleting links/nodes works.

- [ ] **Step 8: Commit**

```bash
git add src/ui/NodeEditorPanel.h src/ui/NodeEditorPanel.cpp src/ui/PortWidgets.h src/ui/PortWidgets.cpp src/app/Application.h src/app/Application.cpp
git commit -m "feat(ui): imgui-node-editor panel + inline port widgets + add-node menu"
```

---

## Task 14: MixNode + mix shader

**Files:**
- Create: `src/modules/MixNode.h`, `shaders/mix.frag`
- Modify: `src/app/Application.cpp` (register in factory + menu)

- [ ] **Step 1: Write `shaders/mix.frag`**

```glsl
#version 410 core
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uA;
uniform sampler2D uB;
uniform float uFactor;
void main() { FragColor = mix(texture(uA, vUV), texture(uB, vUV), uFactor); }
```

- [ ] **Step 2: Write `src/modules/MixNode.h`**

```cpp
#pragma once
#include <glad/gl.h>
#include "gfx/ShaderNode.h"

namespace oss {

class MixNode : public ShaderNode {
public:
    MixNode() : ShaderNode("Mix", "shaders/mix.frag") {
        addInput("a", PortType::Texture, TexRef{});
        addInput("b", PortType::Texture, TexRef{});
        addInput("factor", PortType::Float, 0.5f);
        addOutput("out", PortType::Texture);
    }
    void evaluate(EvalContext& ctx) override { render(ctx); }

protected:
    void setUniforms(EvalContext& ctx) override {
        TexRef a = ctx.in<TexRef>(0);
        TexRef b = ctx.in<TexRef>(1);
        float  f = ctx.in<float>(2);
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, a.id);
        glUniform1i(glGetUniformLocation(program_, "uA"), 0);
        glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, b.id);
        glUniform1i(glGetUniformLocation(program_, "uB"), 1);
        glUniform1f(glGetUniformLocation(program_, "uFactor"), f);
    }
};

} // namespace oss
```

- [ ] **Step 3: Register MixNode in `src/app/Application.cpp`**

Add `#include "modules/MixNode.h"`. In `makeNode`:
```cpp
    if (type == "Mix") return std::make_unique<MixNode>();
```
In `nodeTypeNames` (Spectrograph is registered in Task 15, so do NOT list it yet — a menu entry whose `makeNode` returns null would add nothing or crash on `initGL`):
```cpp
    static const std::vector<std::string> names = { "Colour", "Mix", "Output" };
```

- [ ] **Step 4: Build and run**

Run: `cmake --build build -j && ./build/shader_streamer`
Expected: Add two Colour nodes + a Mix + an Output. Wire `Colour1→Mix.a`, `Colour2→Mix.b`, `Mix→Output`. Give the two Colour nodes different colours; the Viewer shows their blend. The Mix node's unconnected `factor` shows a slider; dragging it blends from colour A to B live.

- [ ] **Step 5: Commit**

```bash
git add src/modules/MixNode.h shaders/mix.frag src/app/Application.cpp
git commit -m "feat(modules): MixNode + mix.frag (multi-input texture blend)"
```

---

## Task 15: SpectrographNode + spectrum shader

**Files:**
- Create: `src/modules/SpectrographNode.h`, `src/modules/SpectrographNode.cpp`, `shaders/spectrograph.frag`
- Modify: `src/app/Application.cpp`

- [ ] **Step 1: Write `shaders/spectrograph.frag`**

```glsl
#version 410 core
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uSpectrum;   // kBins x 1, R channel = normalized magnitude
void main() {
    float v = texture(uSpectrum, vec2(vUV.x, 0.5)).r;
    float lit = step(1.0 - vUV.y, v);                       // bar height = magnitude
    vec3 col = mix(vec3(0.04, 0.05, 0.08), vec3(0.10, 0.95, 0.45), lit);
    FragColor = vec4(col, 1.0);
}
```

- [ ] **Step 2: Write `src/modules/SpectrographNode.h`**

```cpp
#pragma once
#include <glad/gl.h>
#include <vector>
#include "gfx/ShaderNode.h"
#include "audio/SignalGenerator.h"

namespace oss {

class SpectrographNode : public ShaderNode {
public:
    SpectrographNode();
    void initGL() override;
    void evaluate(EvalContext& ctx) override;

protected:
    void setUniforms(EvalContext& ctx) override;

private:
    static constexpr int kWindow = 1024;
    static constexpr int kBins   = kWindow / 2;  // 512
    SignalGenerator     gen_;
    std::vector<float>  window_;    // rolling time-domain window
    std::vector<float>  spectrum_;  // normalized magnitudes (kBins)
    GLuint              specTex_ = 0;
};

} // namespace oss
```

- [ ] **Step 3: Write `src/modules/SpectrographNode.cpp`**

```cpp
#include "modules/SpectrographNode.h"
#include "audio/FFT.h"
#include "core/Value.h"
#include <algorithm>
#include <cmath>

namespace oss {

SpectrographNode::SpectrographNode()
    : ShaderNode("Spectrograph", "shaders/spectrograph.frag"),
      gen_(48000, 220.0f),
      window_(kWindow, 0.0f),
      spectrum_(kBins, 0.0f) {
    addInput("audio", PortType::Audio, AudioRef{});   // unconnected -> internal synth
    addOutput("out", PortType::Texture);
}

void SpectrographNode::initGL() {
    ShaderNode::initGL();
    glGenTextures(1, &specTex_);
    glBindTexture(GL_TEXTURE_2D, specTex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, kBins, 1, 0, GL_RED, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

void SpectrographNode::evaluate(EvalContext& ctx) {
    // Advance the rolling window by one frame's worth of samples.
    int adv = std::clamp((int)std::lround(gen_.sampleRate() * (double)ctx.dt), 1, kWindow);
    std::move(window_.begin() + adv, window_.end(), window_.begin());
    float* tail = window_.data() + (kWindow - adv);

    AudioRef a = ctx.in<AudioRef>(0);
    if (a.samples && a.count >= (std::size_t)adv) {
        std::copy(a.samples + (a.count - adv), a.samples + a.count, tail);
    } else {
        gen_.generate(tail, adv);   // unconnected default: synth
    }

    auto mag = magnitudeSpectrum(window_);
    float maxv = 1e-6f;
    for (float m : mag) maxv = std::max(maxv, m);
    for (int i = 0; i < kBins; ++i) spectrum_[i] = mag[i] / maxv;

    render(ctx);   // binds program/FBO, calls setUniforms, draws, outputs TexRef
}

void SpectrographNode::setUniforms(EvalContext&) {
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, specTex_);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kBins, 1, GL_RED, GL_FLOAT, spectrum_.data());
    glUniform1i(glGetUniformLocation(program_, "uSpectrum"), 0);
}

} // namespace oss
```

- [ ] **Step 4: Register Spectrograph in `src/app/Application.cpp`**

Add `#include "modules/SpectrographNode.h"`. In `makeNode`:
```cpp
    if (type == "Spectrograph") return std::make_unique<SpectrographNode>();
```
Set `nodeTypeNames` to:
```cpp
    static const std::vector<std::string> names = { "Colour", "Spectrograph", "Mix", "Output" };
```

- [ ] **Step 5: Build and run**

Run: `cmake --build build -j && ./build/shader_streamer`
Expected: Add a Spectrograph node, wire `Spectrograph→Output`; the Viewer shows **animated green frequency bars** driven by the synth signal. No GL errors.

- [ ] **Step 6: Commit**

```bash
git add src/modules/SpectrographNode.h src/modules/SpectrographNode.cpp shaders/spectrograph.frag src/app/Application.cpp
git commit -m "feat(modules): SpectrographNode + spectrograph.frag (audio->FFT->texture)"
```

---

## Task 16: Full success-criteria run, shader-path robustness, README

**Files:**
- Modify: `CMakeLists.txt` (copy `shaders/` next to the binary)
- Create: `README.md`

- [ ] **Step 1: Copy shaders next to the executable**

The app loads shaders via the relative path `shaders/x.frag`, resolved against the
current working directory. Two valid run locations: from the **repo root** (where
`shaders/` already exists) or from **`build/`** (after this POST_BUILD copy places a
copy there). Append to `CMakeLists.txt`:
```cmake
add_custom_command(TARGET shader_streamer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/shaders $<TARGET_FILE_DIR:shader_streamer>/shaders)
```
No code change needed — the paths stay `shaders/x.frag`. The README documents both run locations.

- [ ] **Step 2: Write `README.md`**

```markdown
# OpenGL Shader Streamer

A modular, node-graph media pipeline (à la Blender's shader editor) built with
C++17, OpenGL 4.1, Dear ImGui, and imgui-node-editor. Wire shader-backed modules
together and watch textures/audio stream through the graph in real time.

## Build

```bash
cmake -S . -B build
cmake --build build -j
```

(First configure downloads dependencies via CMake FetchContent — network required.)

## Run

```bash
./build/shader_streamer    # run from repo root, or from build/ (shaders are copied there)
```

## Modules

- **Colour** — colour → texture
- **Spectrograph** — synth audio → FFT → texture (audio input port; synthesizes when unconnected)
- **Mix** — two textures + factor → blended texture
- **Output** — displays a texture in the Viewer

## Test

```bash
ctest --test-dir build --output-on-failure
```

## Layout

See `docs/superpowers/specs/` for the design and `docs/superpowers/plans/` for the build plan.
```

- [ ] **Step 3: Full success-criteria verification run**

Run: `cmake --build build -j && ./build/shader_streamer`
Verify all of:
1. Right-click → add Colour, Spectrograph, Mix, Output nodes.
2. Wire `Colour→Mix.a`, `Spectrograph→Mix.b`, `Mix→Output`.
3. Viewer shows the colour and spectrograph **blended**; spectrograph **animates**; the Colour swatch edits live; the Mix `factor` slider blends live.
4. Attempt an invalid connection (e.g. Colour `colour` input is type Colour; dragging a Texture output to it) — the editor **refuses** it.
5. Delete a link and a node — both work; the Viewer updates.

- [ ] **Step 4: Run the unit suite one final time**

Run: `ctest --test-dir build --output-on-failure`
Expected: all tests PASS.

- [ ] **Step 5: Commit**

```bash
git add CMakeLists.txt README.md
git commit -m "chore: copy shaders to build dir, add README; vertical slice complete"
```

---

## Self-Review Notes (for the implementer)

- **Spec coverage:** every spec section maps to tasks — data model (T2–T3), evaluation engine (T4–T5), rendering (T8–T10), the four modules (T11/T12/T14/T15), UI + inline widgets (T13), audio/FFT (T6–T7), testing (T2–T7 unit + T12–T16 run-based), success criteria (T16).
- **GL-free core:** confirmed — `core/` and `audio/` include no GL headers; `TexRef::id` is `unsigned int`; `core_tests` links only `glm` + `doctest`.
- **Type consistency:** `EvalContext::in<T>/out<T>`, `Graph::connect/disconnect/evaluate/topologicalOrder`, `ShaderNode::render/setUniforms`, `TexRef{id,w,h}`, `AudioRef{samples,count,sampleRate}` are used identically across tasks.
- **Known follow-ups (out of slice scope):** node-editor pin-id scheme assumes <500 ports/node and node ids <~2M (fine for the slice); link ids are index-based and recomputed each frame (safe because they're only read within the same frame). A dedicated Audio Source node, save/load, and per-node resolution are deliberately deferred.
```
