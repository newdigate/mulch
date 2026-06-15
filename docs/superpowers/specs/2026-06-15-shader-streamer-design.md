# OpenGL Shader Streamer — Design (Vertical Slice)

**Date:** 2026-06-15
**Status:** Approved (design); pending implementation plan

## 1. Summary

A cross-platform C++/OpenGL desktop app with a Dear ImGui front end that lets the
user build a **modular directed acyclic graph** of media-processing nodes — like
Blender's shader editor. Nodes are shader-backed modules; their input/output
**ports** are typed parameters. Edges are connections between ports. The graph
streams video (textures), audio, and scalar parameters between modules in real
time.

This document specifies the **vertical slice**: the smallest build that proves the
full `graph → shader → texture` pipeline end to end, with four example modules.
Later features (save/load, real device I/O, more modules) build on this foundation.

## 2. Goals & Non-Goals

### Goals (this slice)
- A working node-graph editor: add nodes, connect/disconnect ports, delete.
- A real evaluation engine over a DAG with per-frame rendering.
- Four example modules exercising the colour, texture, float, and audio port types
  plus a genuine multi-input edge.
- Blender-style inline widgets for unconnected input ports.
- A viewer that shows the final rendered texture, updating live.
- Unit-tested, GL-free core (graph, FFT, signal generator).

### Non-Goals (explicitly out of scope for now — YAGNI)
- Graph save/load & serialization
- Real audio-device capture, audio-file decoding, video-file streaming
- Undo/redo
- Per-node / resizable canvas resolution
- Dirty-flag / incremental re-evaluation optimization
- Additional module types beyond the four below
- Node groups, comments, subgraphs

## 3. Platform & Tech Decisions

| Decision | Choice | Notes |
|---|---|---|
| Platforms | macOS / Linux / Windows | GL **4.1 core** baseline to stay macOS-compatible (no compute shaders). |
| Window/context/input | GLFW | |
| GL loader | glad (GL 4.1 core) | |
| UI | Dear ImGui + glfw/opengl3 backends | |
| Node editor | imgui-node-editor (thedmd) | Pan/zoom, link routing, selection, context menus. |
| Math | glm | Colours as `vec4`. |
| Audio source | Synthesized test signal | Main-thread, no device, no threading for the slice. |
| FFT | In-repo radix-2 real FFT (~60 lines) | No extra dependency. |
| Tests | doctest | Tiny, header-only. |
| Build / deps | CMake + FetchContent, version-pinned | Nothing vendored; reproducible. |

## 4. Evaluation Model

**Chosen: per-frame forward evaluation over a cached topological sort.**

- On any topology change (node/edge add/remove), recompute a topological order once
  (Kahn's algorithm); cache it until the next change.
- Connections are type-checked and cycle-checked at creation: the graph is a DAG by
  construction, and the editor refuses edges that would mismatch types or form a cycle.
- Each frame, `Graph::evaluate(dt)` walks nodes in topo order. For each input port it
  resolves a value (the connected output's published value, else the port's default).
  It calls `node.evaluate()`, then stores the node's outputs in a per-port value map
  for downstream nodes.
- Re-rendering everything every frame is acceptable: this is a real-time streaming app
  where audio/video content changes every frame. Dirty-flag/pull-based culling can be
  added later without changing the data model.

## 5. Project Layout

```
src/
  main.cpp                     window + GL + ImGui bootstrap, main loop
  app/Application.*            owns window, Graph, editor panel, viewer
  core/
    Value.h                    typed port value (variant)
    Port.*                     {name, direction, PortType, default Value}
    Node.*                     base module: id, ports, evaluate(EvalContext&)
    Graph.*                    nodes + connections, topo sort, evaluate(dt)
    Connection.h               edge: (srcNode,srcPort) -> (dstNode,dstPort)
  gfx/
    GLUtil.*                   shader compile/link + GL error checks
    Framebuffer.*              FBO + colour-texture wrapper
    FullscreenPass.*           fullscreen-triangle VAO + draw
    ShaderNode.*               base for fragment-shader-rendered nodes
  audio/
    SignalGenerator.*          synthesized test signal (phase-continuous)
    FFT.*                      real radix-2 FFT
  modules/
    ColourNode.*  SpectrographNode.*  MixNode.*  OutputNode.*
  ui/
    NodeEditorPanel.*          imgui-node-editor rendering of the graph
    PortWidgets.*              inline widgets for unconnected inputs
shaders/                       .vert/.frag (loaded at startup)
tests/                         doctest: Graph, FFT, SignalGenerator
```

## 6. Core Data Model

- **PortType**: `Texture, Colour, Float, Bool, Audio, String` (covers every type in the
  vision; "number" = `Float`, filename = `String`).
- **Value**: a `std::variant` over:
  - `float`
  - `bool`
  - `glm::vec4` (colour)
  - `std::string`
  - `TexRef { GLuint id; int w, h; }` (texture handle + size)
  - `AudioRef { const float* samples; size_t count; int sampleRate; }`
- **Port**: `{ name, direction (Input|Output), PortType, Value defaultValue }`. The
  default drives the inline widget when an input is unconnected.
- **Node** (base module): `id`, display name, canvas position, `inputs[]`, `outputs[]`,
  and `virtual void evaluate(EvalContext&)`. Texture-producing nodes own a
  `Framebuffer` and publish its colour texture as a `TexRef` output.
- **Connection**: a type-checked edge `(srcNode, srcPort) -> (dstNode, dstPort)`. An
  output may fan out to many inputs; each input accepts exactly one connection.
- **EvalContext**: passed into `evaluate()`; exposes resolved input values
  (`ctx.input<T>(i)`) and lets the node publish output values.

## 7. Rendering

- A shared canvas resolution constant (e.g. **1280×720**) is used by every texture node
  in the slice. GLSL `#version 410 core`.
- `Framebuffer` wraps an FBO with one RGBA colour-texture attachment.
- `FullscreenPass` owns a VAO that draws a single fullscreen triangle.
- `ShaderNode` (base for shader-backed modules): binds its program, sets uniforms from
  resolved inputs, binds input textures to texture units, and renders into its own FBO.
- **OutputNode** is the exception: it owns no FBO. It displays its input `TexRef` via
  `ImGui::Image` in a "Viewer" window.

## 8. The Four Modules

| Module | Inputs | Output | Behavior |
|---|---|---|---|
| **Colour** | `Colour colour` (default orange) | `Texture` | Shader fills the FBO with the colour uniform. |
| **Spectrograph** | `Audio audio` (unconnected default = internal synth signal) | `Texture` | Each frame: take the latest sample window → FFT → upload magnitude spectrum to a texture → shader draws bars/curve into the FBO. |
| **Mix** | `Texture a`, `Texture b`, `Float factor` (default 0.5) | `Texture` | Shader = `mix(a, b, factor)`. The multi-input DAG proof. |
| **Output** | `Texture in` | — | Viewer sink; displays the texture via `ImGui::Image`. |

The Spectrograph's `Audio` input has an unconnected default that synthesizes the test
signal internally, so it works standalone **and** honors the audio port type as a real
graph edge. A dedicated Audio Source node feeding this input is a trivial later add.

## 9. UI Behavior

- imgui-node-editor draws each node with input pins on the left and output pins on the
  right, and links for connections. Right-click on empty canvas opens an **Add node**
  menu (the four module types). Links and nodes are deletable.
- Link creation is rejected on type mismatch (and on cycle creation).
- **Inline widgets (Blender-style)**: an input port that is *not* connected renders its
  editor inside the node, editing that port's default value:
  - `Colour` → `ImGui::ColorEdit4`
  - `Float` → `ImGui::SliderFloat`
  - `Bool` → `ImGui::Checkbox`
  - `String` → `ImGui::InputText`
  When the input is connected, the widget hides and only the pin shows.
- A "Viewer" window shows the OutputNode's texture.

## 10. Audio

- `SignalGenerator`: a phase-continuous synthesizer (sum of sines / frequency sweep).
  Per frame it produces `ceil(dt · sampleRate)` samples into a ring buffer; the
  Spectrograph reads the latest window (e.g. 1024 samples → 512 magnitude bins).
- All on the main thread — no audio device, no separate audio thread for the slice.

## 11. Testing Strategy

- **TDD the GL-free core** (written test-first, before implementation):
  - `Graph`: topological order correctness, cycle rejection, type-checked
    connect/disconnect, output fan-out, default-value fallback for unconnected inputs.
  - `FFT`: a pure sine input produces a single dominant magnitude bin at the expected
    frequency.
  - `SignalGenerator`: correct sample counts and phase continuity across frames.
- GL/UI glue (shaders, FBOs, node editor, viewer) is verified by **running the app**
  against the success criteria. A headless GL smoke test is out of scope for the slice.

## 12. Success Criteria

Launch the app, then:
1. Right-click → add Colour, Spectrograph, Mix, and Output nodes.
2. Wire `Colour → Mix.a`, `Spectrograph → Mix.b`, `Mix → Output`.
3. The Viewer shows the two textures blended; the spectrograph animates from the synth
   signal; the colour edits live via the inline picker; the `factor` slider blends live.
4. A type-mismatched connection attempt is refused by the editor.

## 13. Future Extensions (not in this slice)

Graph serialization (save/load), an Audio Source node + live mic / file / video input,
undo/redo, per-node resolution, dirty-flag optimization, a wider module library, and
node grouping. The data model (typed ports, DAG, per-frame evaluation) is designed so
these slot in without restructuring.
