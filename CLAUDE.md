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
  `std::variant<float, bool, glm::vec4, std::string, TexRef, AudioRef, MidiRef, VertexRef, Transform, ShaderRef>`.
  `PortType` enumerates the same set; `typeOf(Value)` maps variant → PortType.
  Connections only join ports of equal `PortType`. Audio edges are **mono**:
  `AudioRef` is `{samples, count, sampleRate}` and `count` is the sample count;
  **stereo is two separate `left`/`right` mono edges**, with nodes exposing split
  `left`/`right` ports. The GL-free `core/AudioPan.h` pan/downmix laws
  (`panGains`/`downmixGains`) back **Audio Mix** and the new **Mono to Stereo** /
  **Stereo to Mono** bridges; `AudioClip`/`AudioFile` stay stereo internally (the
  Audio Player deinterleaves to its two outputs).
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
- **Acid Bass synth voice** — `AcidNode` (`src/modules/AcidNode.h`, header-only) is
  the first MIDI-in → audio-out synth: it wraps the GL-free `AcidVoice` DSP
  (`src/audio/AcidVoice.{h,cpp}`) — a monophonic 303-style voice (saw/square VCO +
  sub-osc → a compact 4-pole resonant `LadderFilter` modulated by an envelope /
  accent / key-track / filter-FM → VCA → a `tanh` distortion stage → output `level`, plus note slide).
  The ladder feedback and the output are `tanh`-saturated so it's BIBO-stable and
  bounded to `[-1,1]` regardless of resonance/FM. The voice is unit-tested in
  `core_tests`; the node is header-only and GL-free.
- **MIDI File player** — `MidiFilePlayerNode` (`src/modules/MidiFilePlayerNode.h`,
  header-only) wraps a GL-free from-scratch SMF parser (`src/core/MidiFile.{h,cpp}`,
  events positioned in beats with the file's tempo ignored) and a `MidiClipPlayer`
  (`src/core/MidiClip.h`). Each frame the player derives the clip position from
  `transport.beats()` (anchored at a start-offset, looping a region) and emits the
  events in that frame's `[prevPlay, playPos)` window, flushing note-offs at every
  loop seam / stop / mute so nothing hangs. Output is a `MidiRef` → wire into a synth.
- **Oscilloscope** — `OscilloscopeNode` (`src/modules/OscilloscopeNode.h`, header-only)
  turns an audio signal into an oscilloscope trace as streamed geometry. The GL-free
  `buildScopeVertices` (`src/core/Oscilloscope.{h,cpp}`) does the trace math over a
  rolling sample history: a mono waveform locked to a rising zero-crossing `trigger`
  (so a steady tone stands still) or an X-Y vectorscope (L→x, R→y), resampled to a fixed
  512-point line strip and scaled by `gain`. The node owns the history + an internal
  `SignalGenerator` fallback + the VBO, and publishes a `VertexRef` on output 0 → wire
  into the Wireframe renderer. The trace math is unit-tested in `core_tests`.
- **Chord Player** — `ChordPlayerNode` (`src/modules/ChordPlayerNode.h`, header-only,
  GL-free) holds **8 presets**, each a full 8-step chord progression (root pitch-class +
  octave + chord name from the 14-chord GL-free `core/Chords.h`, + a loop length). `presets_[8]`
  is the playback source of truth; the active preset is mirrored into the existing per-step input
  ports and captured back each frame, so the inline editors edit the active preset in place
  (playback reads the struct, not `ctx.in`, so a same-frame switch fires the right chord). Within
  the active preset it emits one step at a time as a chord (simultaneous note-ons) on its `midi`
  output, auto-stepping `unitAbs % length` (stateless from `transport.bars()`, so loop-robust) or
  manually selected. Switch the active preset via the 8 buttons (a generic GL-free `Node`
  button-bank hook — `buttonCount/buttonLabel/buttonActive/buttonPending` + `onButtonPressed`,
  rendered by `NodeEditorPanel`), a `select` MIDI input (note pitch-class C–G → preset), or a
  load; the switch lands on a `switch` quantize boundary (Immediate/Beat/Bar/4 Bars). It tracks
  the sounding chord's exact notes and releases them on every switch / stop, so nothing hangs;
  `saveState` carries all 8 presets + the active index. Wire it into the Arpeggiator (which folds
  the chord's note-ons into its held set) or any synth. Unit-tested in `core_tests`.
- **Compositor** — `CompositorNode` (`src/modules/CompositorNode.h`, header-only) is a
  `ShaderNode` that blends two input textures with a selectable operator. The 23 blend
  modes (arithmetic, the Photoshop-standard separable set, the HSL non-separable
  hue/saturation/color/luminosity, and bitwise and/or/xor) live in `shaders/compositor.frag`,
  which mirrors the GL-free reference `core/BlendModes.h` (`blendPixel` + `blendModeLabels`);
  the reference is unit-tested in `core_tests` and a `gl_smoke` scenario cross-checks the
  shader against it (one mode per code path) so they can't drift.
- **Pitch Graph** — `PitchGraphNode` (`src/modules/PitchGraphNode.h`, header-only) turns
  incoming MIDI into a scrolling pitch-vs-time graph as colored line geometry. The GL-free
  `core/PitchGraph` holds a rolling note history (note-on opens a segment, note-off closes
  it, old ones scroll off and prune) and builds line segments — x = time (newest at the
  right, scrolling left), y = note number (log-frequency), colour = pitch-class rainbow
  hue × velocity brightness via `hsvToRgb`. It publishes a `Pos3Color3` `VertexRef`; that
  vertex format and the Wireframe node's colored draw path were added for it (Wireframe
  branches on `format`, like Shaded Render on `Pos3Normal3`). Unit-tested in `core_tests`;
  a `gl_smoke` scenario checks the colours reach the Wireframe texture.
- **Skybox** — `SkyboxNode` (`src/modules/SkyboxNode.h`, header-only) is a `ShaderNode`
  that samples 6 face textures as a cubemap in `shaders/skybox.frag`: a per-pixel view ray
  (45° FOV, −Z forward) is rotated by yaw+pitch, then in-shader major-axis face selection
  (mirroring the GL-free `core/CubeMap.h` `cubeFaceUV`, unit-tested + gl_smoke
  cross-checked) picks and samples the face. It's rotated by a self-spin or the shared
  `Transform`, which now carries **yaw + pitch** — the World Transform produces both and
  Wireframe / Shaded Render / Skybox all apply them (`rotate(yaw,Y)·rotate(pitch,X)`).
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
- **Vertex Trail** — `VertexTrailNode` (`src/modules/VertexTrailNode.h`, header-only) keeps a
  GL-free `core/VertexTrail` queue of geometry snapshots (read back from the input VBO each
  frame with `glGetBufferSubData`), emitting one combined `Pos3Color3` buffer where the
  snapshot of age `k` (0 = newest) is offset by `k·z-spacing` in Z and hue-rotated by
  `k·hue-rate`. The HSV helpers live in the GL-free header-only `core/ColorHsv.h` (`hsvToRgb`
  + `rgbToHsv`), now shared with `PitchGraph`. Unit-tested in `core_tests`; the readback +
  queue + offsets are `gl_smoke`-verified by reading the output buffer back.
- **Project save/load** — the GL-free `core/ProjectFile` reads/writes a `.oss` project (a
  line-based text format) via an intermediate `ProjectDoc` POD, so parse→rebuild is atomic.
  `captureProject` reads the graph (nodes' type `name()`, `pos`, control-type input defaults,
  `saveState()`), connections, transport, and the `AutomationStore` channels;
  `restoreProject` rebuilds through a `makeNode` factory + an `initGL` callback (so core stays
  GL-free), remapping in-file ids → fresh ids. The `Node::saveState/loadState` hook persists
  non-port state (only the Automation node uses it, for its curves); the curve text codec is
  `encode/decodeCurve` in `core/AutoCurve.h`. `Graph::clear()` empties the graph but keeps
  `nextId_` monotonic (the editor's placement cache assumes ids are never reused). The toolbar
  (`src/ui/TransportBar.cpp`) drives it via `Application::saveProjectToFile`/`loadProjectFromFile`.
- **Preferences** — app-global settings live in the GL-free `core/Preferences` (audio
  output/input device ids + enabled-MIDI-port name sets), persisted to `preferences.oss`
  (separate from projects) and flowed to nodes via `EvalContext::prefs` (like `Transport`,
  passed by `Graph::evaluate`; `Application` owns the object via `Graph::setPreferences`). The
  **Audio Out/In** nodes keep their libsoundio context alive and reopen the device when
  `audio*DeviceId` changes; **MIDI In** merges all enabled input ports and **MIDI Out** fans
  out to all enabled output ports, reopening when the enabled set changes (virtual-port
  fallback when none selected). The `PreferencesPanel` (`src/ui/`) enumerates devices/ports
  (soundio/rtmidi confined to its `.cpp`) into Audio Output / Audio Input / MIDI tabs.
  It also carries the streaming-texture resolution (`textureWidth/Height`): `ShaderNode`,
  `WireframeNode`, and `ShadedRenderNode` recreate their FBO when it changes (fallback
  `kCanvasW×kCanvasH` when `prefs` is null), and `gfx/Framebuffer::create` is re-creation-safe.
- **MIDI sync** — the GL-free `core/MidiClock` holds the Beat Clock protocol math (a
  `BeatClockReader` deriving tempo/position/play from timestamped 24-PPQN ticks + Start/Stop/
  Continue + Song Position, plus SPP/message helpers; unit-tested). The app-level
  `MidiSyncEngine` (`src/app/`, `<RtMidi.h>` confined to its `.cpp`) polls the selected sync
  input on the main thread to drive the `Transport` via a new `Transport::externalClock` (which
  makes `advance()` a no-op), and sends clock to the selected output from a **dedicated timer
  thread** that solely owns the out port (started in the ctor, joined in the dtor before the
  port is freed). Configured by the Preferences `syncIn/Out` fields + the **Sync** tab; ticked
  from `Application::frame` before `graph_.evaluate`. (MTC is the planned Pass-2 mode.)
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
  out) that reads back the input texture and feeds the encoder while `record` is on;
  it takes `left`/`right` mono inputs and records an interleaved stereo track
  (mirroring a lone connected side).
- **`AudioFile` (`src/audio/AudioFile.{h,cpp}`)** decodes a whole audio file to a
  48 kHz stereo float buffer (FFmpeg, GL-free). The `AudioPlayerNode` plays it with
  a playhead advanced by rate*dt, reading the buffer with linear interpolation for
  forward / reverse / variable-rate — the audio analogue of the Video Player.

## Adding a node

1. Create the node class in `src/modules/`. Declare ports in the constructor; if it
   renders, derive from `ShaderNode` and add a shader under `shaders/`.
2. Register it in **both** `makeNode()` and `nodeCategories()` in
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
