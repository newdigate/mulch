# CLAUDE.md

Guidance for working in this repo. Keep it current when conventions change.

## What this is

A node-graph media app: shader/audio/MIDI/geometry modules wired together in a
Dear ImGui editor, evaluated once per frame. Edges carry typed values (texture,
audio, MIDI, geometry) between nodes. C++17, OpenGL 4.1 core, GLFW, Dear ImGui +
imgui-node-editor.

## Build, run, test

```bash
cmake -S . -B build           # first configure fetches all deps (draco makes it slow)
cmake --build build -j
./build/shader_streamer        # run from repo root (shaders/ is resolved relative to CWD)
ctest --test-dir build --output-on-failure
```

Deps are pinned via CMake FetchContent, with one exception: **FFmpeg** (the Video
node's decoder) is a system package found via pkg-config (`brew install ffmpeg`) —
it doesn't FetchContent cleanly. When a fetched dep's bundled CMake is too old for
CMake 4.x, it's relaxed with `CMAKE_POLICY_VERSION_MINIMUM 3.5` around its
`FetchContent_MakeAvailable`.

## Architecture

- **`Value`** (`src/core/Value.h`) is the one currency on every edge:
  `std::variant<float, bool, glm::vec4, std::string, TexRef, AudioRef, MidiRef, VertexRef>`.
  `PortType` enumerates the same set; `typeOf(Value)` maps variant → PortType.
  Connections only join ports of equal `PortType`. Audio is interleaved float in
  `AudioRef` (`channels` = 1 mono or 2 stereo L,R,L,R; `count` = total samples,
  `frames()` = per-channel) — nodes upmix/downmix at the boundaries as needed.
  `Transform` carries a shared rotation with an `active` flag, so a renderer can
  tell a connected World Transform from the inactive default and fall back to
  self-rotation when nothing is wired in.
- **Per-frame forward evaluation.** `Graph::evaluate(dt)` runs nodes in topological
  order. Each node gets an `EvalContext` with resolved `inputs`, writable `outputs`,
  and `dt`. Read inputs with `ctx.in<T>(i)`, write outputs with `ctx.out(i, v)`.
- **Nodes** derive from `Node` (`src/core/Node.h`): declare ports in the constructor
  via `addInput/addOutput`, do GL setup in `initGL()`, compute in `evaluate()`.
  Optional `statusLine()` shows a line under the node in the editor (used by loaders).
- **Global transport** (`src/core/Transport.h`): the `Graph` owns a `Transport`
  (tempo + song position in seconds; derives beats/bars/ms). `evaluate(dt)` advances
  it and hands it to every node via `EvalContext::transport` (a `const Transport*`,
  so nodes can sync to the beat). The top toolbar (`src/ui/TransportBar.cpp`) drives
  it. `Graph::transport()` is the accessor.
- **Automation** — two structurally-distinct channel kinds over one shared,
  global-length time axis (the `Automation` window, `src/ui/AutomationPanel.cpp`,
  a collapsible grouped grid). The breakpoint curve is a GL-free
  `AutoCurve` (`src/core/AutoCurve.h`). **Stream channels** live in
  `AutomationNode` (`src/modules/AutomationNode.h`, header-only): 4 `AutoCurve`
  channels sampled at the transport's bar position, scaled per-channel, emitted on
  Float outputs you wire with edges. **UI channels** live in a GL-free
  `AutomationStore` (`src/core/AutomationStore.{h,cpp}`) owned by `Graph`: each is
  bound to one node's Float input control and created by right-clicking the node
  (`src/ui/NodeEditorPanel.cpp`); `Graph::evaluate` calls `AutomationStore::apply`
  each frame to write the sampled value straight into the control's input default
  (skipping connected inputs). A channel's kind is structural — there is no switch.
- **Choice input ports** — a `Float` input can carry dropdown labels
  (`Port::choices`, built with `Node::addChoiceInput(name, labels, defaultIndex)`);
  the editor renders it as a combo whose value is the selected index
  (`src/ui/PortWidgets.cpp`). The **LFO** (`src/modules/LfoNode.h`, header-only,
  GL-free) uses it for its waveform and BPM-sync-rate menus: a control-rate Float
  modulation source that runs free (Hz, integrating `rate*dt`) or transport-synced
  (phase from `transport.bars()`), mapped into a per-node `[min,max]`. All its
  controls are input ports, so LFOs chain.
- **Transport-synced sequencers** — the **Step Seq** and **Arpeggiator** have a
  `sync` input that locks their step rate to the global transport: each step is
  derived statelessly from `transport.bars()` at a `rate sync` division (shared
  table in `src/core/StepSync.h`), so they follow the project BPM, start/stop with
  the transport, and stay bar-aligned through loops. With `sync` off they free-run
  off their own `tempo`/`rate`, unchanged.
- **Texture nodes** derive from `ShaderNode` (`src/gfx/ShaderNode.h`): render a
  fragment shader into their own FBO and publish a `TexRef` on output 0. `ColourNode`
  is the minimal example — declare ports, override `setUniforms()`, call `render(ctx)`.

## Hard rules

- **`src/core/` and `src/audio/` stay GL-free.** No GL headers there. GL handles
  (textures, VBOs) cross these boundaries as plain `unsigned int` inside `TexRef`
  /`VertexRef` — never as GL types. GL code lives in `src/gfx/`, `src/modules/`, `src/main.cpp`.
- **Two windows, one shared GL context** (`src/main.cpp`). The **Graph** window owns
  the editor + node GL objects (FBOs, VAOs); the **Output** window blits the first
  `OutputNode`'s texture fullscreen. Textures/VBOs are *shared* across contexts; VAOs
  and FBOs are *not*, so each context makes its own. Node GL objects are freed with the
  editor context current (it owns them).
- **Real-time threads bridge through queues, not the graph.** Audio (libsoundio) and
  mesh loading (`std::async`) run off the graph thread; they hand data back via a
  lock-free SPSC ring buffer (`src/audio/SpscRingBuffer.h`) or `AsyncLoader`
  (`src/core/AsyncLoader.h`). GL uploads always happen on the main thread.
- **Text geometry** (`src/gfx/TextGeometry.{h,cpp}`, GL-free): turns a string into
  vertex buffers via `stb_truetype` (glyph outlines) + `earcut` (triangulation) —
  filled flat letters or extruded solid 3D, mirroring Mesh Loader's wireframe+shaded
  outputs. `Text2DNode`/`Text3DNode` (`src/modules/TextNode.cpp`) own the VBOs. The
  default font is one of ImGui's bundled TTFs, baked in by absolute path via the
  `OSS_DEFAULT_FONT` compile definition (no font file ships); the `font` input overrides it.
- **`VideoDecoder` (`src/gfx/VideoDecoder.{h,cpp}`) is a GL-free FFmpeg wrapper** —
  it produces CPU RGBA frames (bottom-up) + 48 kHz mono float audio; FFmpeg headers
  are confined to its `.cpp`. The `VideoPlayerNode` decodes synchronously on the
  graph thread and keeps a sliding keyframe-window frame cache to play forward,
  reverse, and at variable `rate` off a forward-only decoder.
- **`VideoEncoder` (`src/gfx/VideoEncoder.{h,cpp}`) is its mirror** — a GL-free
  FFmpeg muxer writing RGBA frames + interleaved float audio (mono or stereo) to
  an H.264/AAC mp4. The `RecorderNode` is a pass-through tap (video/audio in → same
  out) that reads back the input texture and feeds the encoder while `record` is on,
  capturing whatever channel count the input audio carries.
- **`AudioFile` (`src/audio/AudioFile.{h,cpp}`)** decodes a whole audio file to a
  48 kHz stereo float buffer (FFmpeg, GL-free). The `AudioPlayerNode` plays it with
  a playhead advanced by rate*dt, reading the buffer with linear interpolation for
  forward / reverse / variable-rate — the audio analogue of the Video Player.

## Adding a node

1. Create the node class in `src/modules/`. Declare ports in the constructor; if it
   renders, derive from `ShaderNode` and add a shader under `shaders/`.
2. Register it in **both** `makeNode()` and `nodeTypeNames()` in
   `src/app/Application.cpp` (the string key is the editor's add-node menu label).
3. Add a unit test in `tests/` (GL-free logic) and/or a `gl_smoke` scenario
   (renders + pixel-readback) where it makes sense.

## Tests

- **`core_tests`** — doctest unit tests for the GL-free core (graph topology/eval,
  FFT, signal gen, audio/MIDI nodes, mesh-edge expansion). One `tests/test_*.cpp` per area.
- **`gl_smoke`** — headless: builds graphs, renders into a hidden GLFW window, reads
  back pixels. Runs with `WORKING_DIRECTORY` = repo root so `shaders/` and `tests/assets/`
  resolve. Needs a GL context (skips where none is available); the two visible windows
  can't be exercised headlessly.

## Conventions

- Match the surrounding style; headers are mostly self-contained (`.h`-only nodes are fine).
- Commit messages: Conventional Commits (`feat(...)`, `fix(...)`). Work on a feature
  branch, not `main`.
- `shaders/` is copied next to the binary at build time and also resolved from the
  repo root, so running from either place works.
