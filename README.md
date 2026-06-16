# OpenGL Shader Streamer

A modular, node-graph media pipeline (in the spirit of Blender's shader editor) built
with C++17, OpenGL 4.1, Dear ImGui, and imgui-node-editor. Wire shader-backed modules
together and watch textures and audio stream through the graph in real time.

## Modules

- **Colour** — colour parameter -> texture
- **Sine** — a pure sine-wave audio source (frequency / amplitude) -> audio
- **Audio In** — captures the default input device (microphone) -> audio
- **Audio Mix** — four audio inputs, each with its own level -> one mixed audio output
- **Spectrograph** — audio -> FFT -> texture (synthesizes a test signal when its audio
  input is left unconnected; wire an audio source in to drive it with a live signal)
- **Mix** — two textures + a float factor -> blended texture
- **Output** — displays a texture in the Viewer
- **Audio Out** — plays its audio input through the system's default output device
- **MIDI In** — receives from a hardware or virtual MIDI input port -> midi
- **Arpeggiator** — held MIDI notes -> a stepped note sequence (rate / gate / octaves / mode)
- **MIDI Out** — sends its MIDI input to a hardware or virtual MIDI output port

Texture nodes render a fragment shader into their own framebuffer and publish the result
as a texture; the audio and MIDI nodes carry samples and events instead. The graph is
evaluated once per frame in topological order. Audio flows between nodes as blocks of
samples and MIDI as batches of events; **Audio In**/**Audio Out** bridge libsoundio's
real-time callback thread to the graph thread through a lock-free ring buffer, while the
**MIDI In**/**MIDI Out** ports are polled/sent synchronously via RtMidi.

## Build

```bash
cmake -S . -B build
cmake --build build -j
```

The first configure downloads pinned dependencies via CMake FetchContent (network
required): GLFW, glad (GL 4.1 core), Dear ImGui, imgui-node-editor, glm, doctest,
libsoundio (audio I/O; CoreAudio on macOS), and RtMidi (MIDI I/O; CoreMIDI on macOS).

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
out to a synth (each MIDI node opens port 0, or a virtual port if none exist). Select a
node or link and press Delete or Backspace to remove it.

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
  Mix, Output, Audio Out, MIDI In, Arpeggiator, MIDI Out)
- `src/ui/` — imgui-node-editor panel and inline port widgets
- `shaders/` — fragment shaders
- `docs/superpowers/` — design spec and implementation plan
