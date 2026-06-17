# OpenGL Shader Streamer

A node-graph media pipeline, in the spirit of Blender's shader editor. Wire
shader, audio, MIDI, and geometry modules together and watch textures, sound, and
3D data stream through the graph in real time. C++17 · OpenGL 4.1 · Dear ImGui.

## Build & run

```bash
cmake -S . -B build      # first configure fetches all deps (draco makes it slow)
cmake --build build -j
./build/shader_streamer   # run from the repo root
```

For a headless capture of the UI (no interactive session), render one frame to a
PNG and exit:

```bash
./build/shader_streamer --screenshot ui.png
```

Most dependencies are fetched and pinned by CMake. The one system package is
**FFmpeg** (for the Video node) — install it first: `brew install ffmpeg` on
macOS, or your distro's `libav*-dev` packages. A network connection is needed the
first time for the fetched dependencies.

Two windows open, sharing one GL context: a **Graph** window (the node editor) and
an **Output** window showing the result fullscreen. Right-click the canvas to add
nodes, drag between pins to connect, and select a node or link and press
**Delete**/**Backspace** to remove it. Unconnected inputs show inline editors.

A **transport toolbar** runs along the top of the Graph window — an editable
decimal tempo, Play / Stop / Rewind / Fast-forward, the song position shown as
bars·beats, beats, and minutes:seconds.milliseconds, and a **Loop** toggle with
editable start/end (in bars) that wraps the position back to the loop start when it
reaches the end. It's a global clock the whole graph shares, advanced each frame
while playing; nodes can read it to sync to the beat.

The **Automation** window groups channels over one shared, global-length time axis
(set the song length in its toolbar). There are two structurally-distinct kinds of
channel. **Stream channels** come from an **Automation** node: adding one contributes
a collapsible group of its 4 channels, and you wire each channel's Float output into
any parameter (an edge). **UI channels** are bound directly to a control: right-click
any node and pick one of its Float parameters to create a channel grouped under that
module — it drives the control directly, no wiring. Draw breakpoint curves in a lane
with the mouse (click to add, drag to move, right-click to delete); a channel's left
header carries its output range, a clear button, and (for ui channels) a delete. A
channel's kind is fixed by how it was created.

Try it: wire `Sine → Audio Out` to hear a tone and `Sine → Spectrograph → Output`
to see it; or `Mesh Loader → Shaded Render → Output` to spin a 3D model.

## Modules

| | |
|---|---|
| **Colour** | colour parameter → texture |
| **Video** | play a video file → texture + audio; signed `rate` (negative = reverse), variable speed, loop |
| **Mix** | blend two textures by a factor |
| **Spectrograph** | audio → FFT → texture (and a 3D line-strip vertex buffer) |
| **Sine** | pure sine-wave audio source |
| **Audio File** | play an audio file — mp3, wav, flac, m4a, ogg, … (any FFmpeg format), decoded to 48 kHz stereo → audio; signed `rate` (negative = reverse), variable speed, loop |
| **Audio In / Out** | capture the mic/line-in (stereo if available) / play to the default device, in stereo (libsoundio) |
| **Audio Mix** | four inputs, each with gain + pan → one stereo mix (pan mono sources into the stereo field) |
| **MIDI In / Out** | hardware or virtual MIDI ports (RtMidi) |
| **Step Seq** | 16-step drum sequencer → MIDI |
| **Arpeggiator** | held notes → a stepped sequence |
| **MIDI Merge** | up to four MIDI streams → one |
| **Mesh Loader** | load .obj/.gltf/.glb (incl. Draco/meshopt) on a worker thread → geometry |
| **Text 2D / Text 3D** | type a string → vertex buffers: flat filled letters / extruded solid 3D letters (stb_truetype + earcut) |
| **World Transform** | a single rotation rate → a shared transform; wire it into several renderers' `transform` input so they rotate together and stay aligned |
| **Wireframe / Shaded Render** | a vertex buffer → a rotating wireframe / lit texture; their `transform` input takes a shared World Transform (else each self-rotates via `spin`) |
| **Recorder** | inline tap: passes video + audio through unchanged while recording them to a movie file (H.264/AAC mp4, mono or stereo per the input); toggle `record`, set `file` |
| **Output** | marks the texture shown in the Output window |
| **Automation** | 4 stream channels (Float outputs you wire), each a mouse-drawn curve over song time (bars), sampled at the transport position. Plus ui channels created by right-clicking any node's Float parameter — bound directly to that control. Edited in the **Automation** window |
| **LFO** | low-frequency oscillator → a Float modulation signal: pick a waveform (sine/triangle/square/ramp up/down/sample & hold), run it free (Hz) or BPM-synced (32 bars … 1/64 bar), and map it into a `[min, max]` range. Every control is an input port, so waveform/rate/sync can be driven by another node — chain LFOs. Wire `out` into any Float parameter (e.g. a Sine's `amp`) |

Texture nodes render a fragment shader into their own framebuffer; audio and MIDI
nodes carry samples and events; geometry flows as GL vertex-buffer handles. The
graph is evaluated once per frame in topological order.

## Test

```bash
ctest --test-dir build --output-on-failure
```

`core_tests` covers the GL-free core (graph, FFT, signal, audio/MIDI, mesh edges);
`gl_smoke` renders graphs headlessly and reads back pixels.

## Layout

| Path | Contents |
|---|---|
| `src/core/` | GL-free graph engine (Value, Port, Node, Graph) |
| `src/gfx/` | OpenGL helpers, ShaderNode base, mesh loading |
| `src/audio/` | FFT, signal generator, SPSC ring buffer |
| `src/modules/` | the nodes |
| `src/ui/` | node-editor panel and inline port widgets |
| `shaders/` | fragment shaders |
| `docs/superpowers/` | design spec and implementation plan |

See [CLAUDE.md](CLAUDE.md) for architecture notes and conventions.
