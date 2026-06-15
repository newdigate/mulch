# OpenGL Shader Streamer

A modular, node-graph media pipeline (in the spirit of Blender's shader editor) built
with C++17, OpenGL 4.1, Dear ImGui, and imgui-node-editor. Wire shader-backed modules
together and watch textures and audio stream through the graph in real time.

## Modules

- **Colour** — colour parameter -> texture
- **Spectrograph** — synthesized audio -> FFT -> texture (an audio input port that
  synthesizes a test signal when left unconnected)
- **Mix** — two textures + a float factor -> blended texture
- **Output** — displays a texture in the Viewer

Each node renders a fragment shader into its own framebuffer and publishes the result
as a texture; the graph is evaluated once per frame in topological order.

## Build

```bash
cmake -S . -B build
cmake --build build -j
```

The first configure downloads pinned dependencies via CMake FetchContent (network
required): GLFW, glad (GL 4.1 core), Dear ImGui, imgui-node-editor, glm, doctest.

## Run

```bash
./build/shader_streamer    # from the repo root, or from build/ (shaders are copied there)
```

Right-click the canvas to add nodes; drag between pins to connect; unconnected inputs
show inline editors (colour picker, slider). Wire `Colour -> Mix.a`,
`Spectrograph -> Mix.b`, `Mix -> Output` to see the blend in the Viewer.

## Test

```bash
ctest --test-dir build --output-on-failure
```

- `core_tests` — unit tests for the GL-free core (graph topology/evaluation, FFT,
  signal generator).
- `gl_smoke` — a headless offscreen render test that builds graphs, renders them, and
  reads back pixels to verify the Colour, Mix, and Spectrograph pipelines.
  (Requires a display/WindowServer; it is skipped only where no GL context is available.)

## Layout

- `src/core/` — GL-free graph engine (Value, Port, Node, Graph)
- `src/gfx/` — OpenGL helpers (shader/program, framebuffer, fullscreen pass, ShaderNode base)
- `src/audio/` — FFT and the synthesized signal generator
- `src/modules/` — the four example nodes
- `src/ui/` — imgui-node-editor panel and inline port widgets
- `shaders/` — fragment shaders
- `docs/superpowers/` — design spec and implementation plan
