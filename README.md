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

Curve segments are cubic Bézier: select a breakpoint to reveal its tangent handles, drag a
handle to shape the curve (the two stay aligned for a smooth pass-through), Alt-drag to break
the tangent for a sharp corner, and right-click a handle to reset it. Curves with untouched
handles stay straight lines, and older projects load unchanged.

Try it: wire `Sine → Audio Out` to hear a tone and `Sine → Spectrograph → Output`
to see it; or `Mesh Loader → Shaded Render → Output` to spin a 3D model.

## Modules

| | |
|---|---|
| **Colour** | colour parameter → texture |
| **Video** | play a video file → texture + audio; signed `rate` (negative = reverse), variable speed, loop |
| **Mix** | blend two textures by a factor |
| **Compositor** | blend two textures with a selectable operator (23 modes): add/subtract/difference/exclusion, multiply/screen/overlay, darken/lighten, dodge/burn, hard/soft light, divide/average, the HSL hue/saturation/color/luminosity, and bitwise and/or/xor; plus `opacity` |
| **Spectrograph** | audio → FFT → texture (and a 3D line-strip vertex buffer) |
| **Oscilloscope** | turns audio into an oscilloscope trace as geometry → Vertex: `left`/`right` mono inputs, `mode` (Waveform / X-Y vectorscope), a rising-edge `trigger` so a steady tone stands still, `window` (ms), and `gain`. Wire `geometry` into **Wireframe** to view it |
| **Sine** | pure sine-wave audio source |
| **Acid Bass** | 303-style monophonic synth: MIDI in → mono audio. Saw/square VCO + sub-osc → 4-pole resonant ladder filter (decay · env-mod · accent) → VCA → distortion, with note slide, filter FM (VCA → cutoff), filter key-tracking, and output `level`. Every control is an input port |
| **Audio File** | play an audio file — mp3, wav, flac, m4a, ogg, … (any FFmpeg format), decoded to 48 kHz stereo → `left`/`right` mono outputs; signed `rate` (negative = reverse), variable speed, loop |
| **Audio In / Out** | capture the mic/line-in / play to the default device (libsoundio); stereo carried as `left`/`right` mono ports |
| **Audio Mix** | four inputs, each with gain + pan → `left`/`right` mono outputs (pan mono sources into the stereo field) |
| **Mono to Stereo** | pan a mono signal into a `left`/`right` pair (`pan` −1..1) |
| **Stereo to Mono** | downmix a `left`/`right` pair to one mono signal (`balance` control) |
| **MIDI In / Out** | hardware or virtual MIDI ports (RtMidi) |
| **MIDI File** | streams a Standard MIDI File (.mid) synced to the project BPM → MIDI: anchor `start offset` (bars), `loop` + `loop length` (bars), and `mute 1`…`mute 16` toggles per channel. The file's own tempo is ignored. Wire `out` into a synth (e.g. **Acid Bass**) |
| **Step Seq** | 16-step drum sequencer → MIDI; `sync` toggle locks the step rate to the project BPM over musical divisions (1/4 … 1/32, incl. dotted + triplet), or runs free at its own `tempo` |
| **Drum Machine** | sample-based drum machine → stereo `left`/`right` audio: 4 sample voices (each a `file` + `vol`/`rate`/`pan`) sequenced on a 4×16 tri-state grid (off / on / **accent**), with 8 pattern slots (8 buttons or an automatable `pattern` input). `sync` locks the 16 steps to the project transport (a `rate sync` division) or free-runs at `tempo`. Wire `left`/`right` into **Audio Out** |
| **Arpeggiator** | held notes → a stepped sequence (up / down / up-down); `sync` toggle locks the step rate to the project BPM, or runs free at `rate` steps/sec |
| **Chord Player** | 8 preset chord progressions → MIDI. Each preset is an 8-step sequence (root + octave + chord, 14 types) that auto-steps (or is manually stepped) on a **Bar**/**Beat** boundary. Switch the active preset with the 8 buttons, a MIDI `select` note (C–G → preset 1–8), or save/load; the change lands **Immediately / next Beat / Bar / 4 Bars**. All 8 presets are saved with the project. Wire into the **Arpeggiator** or a synth |
| **Pitch Graph** | MIDI → a scrolling pitch-vs-time graph as colored geometry → Vertex: each held note is a horizontal line at its pitch, coloured by pitch class (rainbow hue) and velocity (brightness), scrolling over `window` seconds. Wire `geometry` into **Wireframe** (set its `spin` to 0 for a static view) |
| **MIDI Merge** | up to four MIDI streams → one |
| **Mesh Loader** | load .obj/.gltf/.glb (incl. Draco/meshopt) on a worker thread → geometry |
| **Text 2D / Text 3D** | type a string → vertex buffers: flat filled letters / extruded solid 3D letters (stb_truetype + earcut) |
| **World Transform** | a yaw spin `rate` + a `pitch` tilt → a shared transform; wire it into several renderers' `transform` input (Wireframe, Shaded Render, Skybox) so they rotate together |
| **Wireframe / Shaded Render** | a vertex buffer → a rotating wireframe / lit texture; their `transform` input takes a shared World Transform (else each self-rotates via `spin`) |
| **Skybox** | 6 face textures (`+X`…`-Z`) → a cubemap background texture; rotated by a self-`rotation` yaw-spin or the shared **World Transform** (yaw + pitch). Wire `out` into **Output**, or composite a Wireframe / Shaded Render scene over it |
| **Vertex Shader** | pick a preset transform (Identity / Twist / Wave / Bulge) → a **Shader** edge (a new input kind carrying a GLSL vertex shader). Wire `shader` into a **Deform** node |
| **Deform** | runs a vertex shader (the `shader` input) over a vertex buffer via GPU transform feedback → a colored vertex buffer; `position` and `colour` drive the shader. Wire `geometry` into **Wireframe** / **Shaded Render** |
| **Vertex Trail** | snapshots a vertex buffer each frame into a trail (queue of `max frames`); each copy is offset in Z (`z spacing`) and hue-rotated (`hue rate`) by its age, and is drawn unconnected to the others → a colored vertex buffer; wire `geometry` into **Wireframe** |
| **Recorder** | inline tap: passes video + `left`/`right` audio through unchanged while recording them to a movie file (H.264/AAC mp4, interleaved stereo from the two mono sides); toggle `record`, set `file` |
| **Output** | marks the texture shown in the Output window |
| **Automation** | 4 stream channels (Float outputs you wire), each a mouse-drawn curve over song time (bars), sampled at the transport position. Plus ui channels created by right-clicking any node's Float parameter — bound directly to that control. Edited in the **Automation** window |
| **LFO** | low-frequency oscillator → a Float modulation signal: pick a waveform (sine/triangle/square/ramp up/down/sample & hold), run it free (Hz) or BPM-synced (32 bars … 1/64 bar), and map it into a `[min, max]` range. Every control is an input port, so waveform/rate/sync can be driven by another node — chain LFOs. A second `amplified` output gives `out` scaled by an `amplify` input. Wire `out` (or `amplified`) into any Float parameter (e.g. a Sine's `amp`) |

Texture nodes render a fragment shader into their own framebuffer; audio and MIDI
nodes carry samples and events; geometry flows as GL vertex-buffer handles. The
graph is evaluated once per frame in topological order.

### Save / Load

The toolbar's **Save** / **Load** buttons (with the filename field, default `project.oss`)
write and read a project file: every node (type, canvas position, and control values),
all connections, the transport (tempo + loop), and the automation (the Automation node's
curves and the right-click parameter-automation channels). Playback position is not saved —
a loaded project opens paused at the start. The `.oss` format is plain line-based text.

### Preferences

The toolbar **View** menu's **Preferences** item opens a Preferences window with **Audio Output**, **Audio Input**,
and **MIDI** tabs: pick the output/input sound card and enable which MIDI interfaces are used.
Changes apply live (the running audio/MIDI nodes reopen their device/ports) and persist to a
`preferences.oss` file in the working directory. The Audio Output tab also has an **Audio buffer
(ms)** control (20–500) that sizes the output ring: higher trades latency for under-run headroom,
lower tightens latency. The per-frame audio block now tracks frame time, so a slow render frame no
longer starves the output.
A **Video** tab sets the streaming-texture resolution (320×240, 640×480, 1280×720, or
1920×1080); the change applies live — every render-to-texture node recreates its framebuffer.
A **Sync** tab can drive the transport from an external MIDI **Beat Clock** (receive 24-PPQN
clock + Start/Stop/Continue + Song Position from a selected input) and send Beat Clock to a
selected output (with a dedicated timer thread for steady ticks).
The Sync tab also offers **MTC** (MIDI Time Code) as a second mode: lock the transport position
to incoming SMPTE timecode, or send it to a slave, at 24 / 25 / 29.97-drop / 30 fps (a frame-rate
picker appears when MTC is selected). MTC carries position, not tempo.

### Assets

The toolbar **View** menu's **Assets** item opens an Assets window: a per-project media library with
**Audio / Video / MIDI / 3D** tabs. Each tab is a table of media files — add a file, edit its
label and path inline, pick a path with the **...** Browse button (a native file dialog), or remove it. Each file
carries a hidden, stable id plus a label, and the whole library is saved and loaded with the
`.oss` project. (A later phase will let node file controls pick from this library directly.)

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
