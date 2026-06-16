# OpenGL Shader Streamer

A modular, node-graph media pipeline (in the spirit of Blender's shader editor) built
with C++17, OpenGL 4.1, Dear ImGui, and imgui-node-editor. Wire shader-backed modules
together and watch textures and audio stream through the graph in real time.

## Modules

- **Colour** — colour parameter -> texture
- **Sine** — a pure sine-wave audio source (frequency / amplitude) -> audio
- **Audio In** — captures the default input device (microphone) -> audio
- **Audio Mix** — four audio inputs, each with its own level -> one mixed audio output
- **Spectrograph** — audio -> FFT -> texture, plus a second vertex-buffer output of the
  spectrum as a 3D line strip (synthesizes a test signal when its audio input is unconnected)
- **Mix** — two textures + a float factor -> blended texture
- **Mesh Loader** — loads a .obj or .gltf/.glb file (incl. Draco- and meshopt-compressed
  glTF) on a worker thread and streams it as two geometry outputs: wireframe edges and
  shaded triangles (with normals). Shows its load status under the node name (loading /
  loaded N triangles / load failed: reason)
- **Wireframe** — a streamed vertex buffer (line strip or mesh edges) -> a wireframe
  texture, drawn through a slowly rotating 3D camera
- **Shaded Render** — a streamed triangle buffer (with normals) -> a solid, diffuse-lit
  texture with a colour input, drawn through a slowly rotating 3D camera
- **Output** — displays a texture in the Viewer
- **Audio Out** — plays its audio input through the system's default output device
- **MIDI In** — receives from a hardware or virtual MIDI input port -> midi
- **Step Seq** — a 16-step drum sequencer (a basic TR-909 voice): toggle steps; set tempo /
  note / channel -> midi
- **Arpeggiator** — held MIDI notes -> a stepped note sequence (rate / gate / octaves / mode)
- **MIDI Merge** — concatenates up to four MIDI inputs into one stream (layer several Step
  Seq voices into a kit)
- **MIDI Out** — sends its MIDI input to a hardware or virtual MIDI output port

Texture nodes render a fragment shader into their own framebuffer and publish the result
as a texture; the audio and MIDI nodes carry samples and events instead. The graph is
evaluated once per frame in topological order. Audio flows between nodes as blocks of
samples and MIDI as batches of events; **Audio In**/**Audio Out** bridge libsoundio's
real-time callback thread to the graph thread through a lock-free ring buffer, while the
**MIDI In**/**MIDI Out** ports are polled/sent synchronously via RtMidi. Geometry flows as
GL vertex-buffer handles: a node uploads a VBO and a downstream node binds and draws it.

## Build

```bash
cmake -S . -B build
cmake --build build -j
```

The first configure downloads pinned dependencies via CMake FetchContent (network
required): GLFW, glad (GL 4.1 core), Dear ImGui, imgui-node-editor, glm, doctest,
libsoundio (audio I/O; CoreAudio on macOS), RtMidi (MIDI I/O; CoreMIDI on macOS),
tinyobjloader + tinygltf (mesh loading), and draco + meshoptimizer (glTF compression).
draco is a sizable build, so the first configure/build takes noticeably longer.

## Run

```bash
./build/shader_streamer    # from the repo root, or from build/ (shaders are copied there)
```

Right-click the canvas to add nodes; drag between pins to connect; unconnected inputs
show inline editors (colour picker, slider). Wire `Colour -> Mix.a`,
`Spectrograph -> Mix.b`, `Mix -> Output` to see the blend in the Viewer. For audio, add
**Sine** and **Audio Out** and wire `Sine -> Audio Out` to hear the tone (and
`Sine -> Spectrograph -> Output` to see it). Combine sources by wiring them into an
**Audio Mix**, or capture the microphone with **Audio In** (macOS will prompt for mic
access). For MIDI, wire **MIDI In -> Arpeggiator -> MIDI Out** to arpeggiate held chords
out to a synth, or wire **Step Seq -> MIDI Out** for a 16-step drum pattern -- combine
several Step Seq voices through **MIDI Merge** for a full kit (each MIDI node opens port 0,
or a virtual port if none exist). Wire the
**Spectrograph**'s geometry output into **Wireframe -> Output** to see the spectrum as a
rotating 3D line strip, or load a 3D model with **Mesh Loader** (type a .obj/.gltf path in
its file field) and wire its wireframe output into **Wireframe**, or its shaded output into
**Shaded Render**, for a rotating wireframe or lit view. Select a node or link and press
Delete or Backspace to remove it.

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
- `src/audio/` — FFT, the synthesized signal generator, and the SPSC ring buffer
- `src/modules/` — the example nodes (Colour, Sine, Audio In, Audio Mix, Spectrograph,
  Mix, Mesh Loader, Wireframe, Shaded Render, Output, Audio Out, MIDI In, Step Seq,
  Arpeggiator, MIDI Merge, MIDI Out)
- `src/gfx/MeshEdges.h`, `src/gfx/MeshLoader.{h,cpp}` — mesh edge expansion and .obj/.gltf loading
- `src/ui/` — imgui-node-editor panel and inline port widgets
- `shaders/` — fragment shaders
- `docs/superpowers/` — design spec and implementation plan
