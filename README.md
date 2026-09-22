# OpenGL Shader Streamer

Dear Imgui based node-graph media pipeline, in the spirit of Blender's shader editor. Wire
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

To write a project's output to a movie file without opening the windows — the
offline render described under [Render Video](#render-video-offline) — use
`--render` (`tests/assets/render_smoke.oss` is a tiny checked-in project to try
it on):

```bash
./build/shader_streamer --render tests/assets/render_smoke.oss out.mp4 --start 0 --end 8 --fps 30 --size 1920x1080
```

Most dependencies are fetched and pinned by CMake. The one system package is
**FFmpeg** (for the Video node) — install it first: `brew install ffmpeg` on
macOS, or your distro's `libav*-dev` packages. A network connection is needed the
first time for the fetched dependencies.

Two windows open, sharing one GL context: a **Graph** window (the node editor) and
an **Output** window showing the result fullscreen. Right-click the canvas to add
nodes, drag between pins to connect, and select a node or link and press
**Delete**/**Backspace** to remove it. Graph nodes show just their name + ports; **select a node to edit it** in the side panels — fields (integer, colour, text, dropdowns), checkboxes, toggle buttons, and its connections in **Properties**; sliders and step grids in **Controls**.
The editor panels (Node Graph, Automation, Properties, Controls, Assets, Preferences) are dockable — drag to split or tab them; the layout is saved between runs, and **View → Reset Layout** restores the default. The Output stays its own window (put it fullscreen on a second display).

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

## Downloads & CI

Three GitHub Actions workflows (`.github/workflows/build-{linux,macos,windows}.yml`) build the app
and produce native packages on every push to `main`/`develop` and on pull requests (downloadable
from the Actions run), and attach them to a GitHub Release when a `v*` tag is pushed:

- **Linux** → AppImage (`ubuntu-22.04`; bundles FFmpeg/GTK via `linuxdeploy` + `appimagetool`).
- **macOS** → `.app` zip for both Apple-Silicon **arm64** (`macos-14`) and Intel **x64** (`macos-13`);
  FFmpeg dylibs bundled with `dylibbundler`, so no Homebrew needed at runtime.
- **Windows** → Inno Setup installer (`windows-latest`, MSVC + vcpkg FFmpeg).

Packages are **unsigned**, so first launch goes through the OS "unidentified developer" prompt
(macOS: right-click → Open, or `xattr -dr com.apple.quarantine`; Windows: "More info" → "Run anyway").
Each package launches the app with `shaders/` as the working directory, so the shipped app finds its
shaders. Packaging helper files live in `packaging/{linux,macos,windows}/`.

## Modules

| | |
|---|---|
| **Colour** | colour parameter → texture |
| **Image Streamer** | load a still image (PNG/JPG/BMP/TGA/GIF/HDR/…) → texture, from the new **Image** asset-library tab |
| **Image Sequencer** | play a folder of images in sequence: one every `duration` seconds, or every `beat length` beats when `sync` is on, with an optional `fade duration` cross-dissolve between them; the next image is prefetched on a background thread so transitions stay smooth. Pick the folder from your Image assets' folders |
| **Kaleidoscope** | fold any texture into a mirrored kaleidoscopic pattern: `segments`, `rotation` (wire an LFO to spin), `zoom`, `center` |
| **HSV Adjust** | shift the hue (turns) and scale saturation & brightness of a texture; wire `hue` to an LFO to cycle colours |
| **projectM** *(optional)* | Milkdrop-compatible audio visualizer → texture: `left`/`right` audio in, a `preset` (.milk) picked from the **Presets** assets tab, **prev / next / random** buttons plus a bar-synced `sync` step, and a `burn` gate to composite a texture into the scene. Only appears in the Add menu when `libprojectM-4` is found at runtime — see [projectM (optional)](#projectm-optional) below |
| **Video** | play a video file → texture + audio; signed `rate` (negative = reverse), variable speed, loop |
| **Mix** | blend two textures by a factor |
| **Compositor** | blend two textures with a selectable operator (23 modes): add/subtract/difference/exclusion, multiply/screen/overlay, darken/lighten, dodge/burn, hard/soft light, divide/average, the HSL hue/saturation/color/luminosity, and bitwise and/or/xor; plus `opacity` |
| **Spectrograph** | audio → FFT → texture (and a 3D line-strip vertex buffer) |
| **Oscilloscope** | turns audio into an oscilloscope trace as geometry → Vertex: `left`/`right` mono inputs, `mode` (Waveform / X-Y vectorscope), a rising-edge `trigger` so a steady tone stands still, `window` (ms), and `gain`. Wire `geometry` into **Wireframe** to view it |
| **Sine** | pure sine-wave audio source |
| **Acid Bass** | 303-style monophonic synth: MIDI in → mono audio. Saw/square VCO + sub-osc → 4-pole resonant ladder filter (decay · env-mod · accent) → VCA → distortion, with note slide, filter FM (VCA → cutoff), filter key-tracking, and output `level`. Every control is an input port |
| **Spirograph Synth** | stereo oscillator straight from Spirograph curves: θ at audio rate, curve `x` → `left` / `y` → `right`. `curve type` (hypotrochoid / epitrochoid), `freq`, `ratio` (R/r, sets the bright partial), `pen` (fades that partial in), `phase`, `level`. Output is normalized so it can't clip. Every control is an input port — wire an **LFO** into `ratio` to morph |
| **Audio File** | play an audio file — mp3, wav, flac, m4a, ogg, … (any FFmpeg format), decoded to 48 kHz stereo → `left`/`right` mono outputs; signed `rate` (negative = reverse), variable speed, loop. `sync` + `length` bar-lock the clip to the transport, time-warping it to span exactly `length` bars (`rate`/`loop` ignored while synced) |
| **Audio In / Out** | capture the mic/line-in / play to the default device (libsoundio); stereo carried as `left`/`right` mono ports |
| **Audio Mix** | four inputs, each with gain + pan → `left`/`right` mono outputs (pan mono sources into the stereo field) |
| **Mono to Stereo** | pan a mono signal into a `left`/`right` pair (`pan` −1..1) |
| **Stereo to Mono** | downmix a `left`/`right` pair to one mono signal (`balance` control) |
| **Crossover Filter** | split one mono signal into `bass` / `mid` / `treble` mono outputs by two cascaded resonant crossovers; each crossover has its own `cutoff` + `resonance` (musical state-variable filter, not a phase-flat mastering crossover) |
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
| **Recorder** | inline tap: passes video + `left`/`right` audio through unchanged while recording them to a movie file (H.264/AAC mp4, interleaved stereo from the two mono sides); toggle `record`, set `file`. It records in real time — for a capture that can't miss a frame use [Render Video](#render-video-offline) |
| **Output** | marks the texture shown in the Output window |
| **Automation** | 4 stream channels (Float outputs you wire), each a mouse-drawn curve over song time (bars), sampled at the transport position. Plus ui channels created by right-clicking any node's Float parameter — bound directly to that control. Edited in the **Automation** window |
| **LFO** | low-frequency oscillator → a Float modulation signal: pick a waveform (sine/triangle/square/ramp up/down/sample & hold), run it free (Hz) or BPM-synced (32 bars … 1/64 bar), and map it into a `[min, max]` range. Every control is an input port, so waveform/rate/sync can be driven by another node — chain LFOs. A second `amplified` output gives `out` scaled by an `amplify` input. Wire `out` (or `amplified`) into any Float parameter (e.g. a Sine's `amp`) |

Texture nodes render a fragment shader into their own framebuffer; audio and MIDI
nodes carry samples and events; geometry flows as GL vertex-buffer handles. The
graph is evaluated once per frame in topological order.

### Save / Load

The toolbar's left-anchored **File** menu (**Load** / **Save** / **Save As**) uses native OS file
dialogs. **Load** and **Save As** open a file picker (filtered to `.oss`); **Save** writes the current file, or prompts
like **Save As** when the project is still untitled. A saved/loaded project file holds every node
(type, canvas position, and control values),
all connections, the transport (tempo + loop), and the automation (the Automation node's
curves and the right-click parameter-automation channels). Playback position is not saved —
a loaded project opens paused at the start. The `.oss` format is plain line-based text.

### Render Video (offline)

The File menu's **Render Video…** item writes the graph's output between a **start bar** and a
**finish bar** to an H.264/AAC `.mp4`, taking as long as it needs so **no frame is ever dropped** —
unlike the **Recorder** node, which captures in real time and stamps frames with the wall clock, so
whatever the app cannot render fast enough is simply missing from the file. Here the graph runs on
a fixed clock instead: each frame the transport is *placed* at that frame's position rather than
advanced, and every node is evaluated with `dt = 1/fps`. So every frame of the range lands in the
file, in order, and each one carries exactly `48000 / fps` audio samples — picture and sound cannot
drift apart.

Video is whatever the first **Output** node shows (the same texture as the Output window, stretched
to the render size); audio is what feeds the first **Audio Out** (a lone `left` or `right` wire is
mirrored to both channels). Audio Out is optional — with nothing connected to it when capture
starts, the file is video-only and the outcome line says so.

- **Start bar / Finish bar** — the same convention as the **Loop** fields: start 0 is the first bar
  and the finish bar is exclusive, so `0 → 8` is the first eight bars. Fractions are allowed, and
  **Use loop range** copies the loop fields.
- **Pre-roll (bars)** — bars evaluated before the start bar but *not* captured (default 1), so
  envelopes, notes that begin before the range, and file loaders have settled by the first captured
  frame. A render that starts at bar 0 pre-rolls sitting at bar 0.
- **Frame rate** — 24, 25, 30, 50 or 60: the rates that divide 48 kHz exactly.
- **Width / Height** — seeded from the Preferences texture size (**Use live size** re-copies it),
  16 to 8192 per axis and **even** (the H.264 encode needs even dimensions). For the length of the
  render, every render-to-texture node recreates its framebuffer at the render size, so shader,
  Wireframe and Shaded Render nodes genuinely draw at that size instead of being upscaled to it.
- **Output file** — the project's name with `.mp4`, editable, or picked with **Browse…**; the
  `.mp4` extension is added if you leave it off.

While the job runs, a modal **Rendering** popup shows the pre-roll and frame counters, elapsed and
estimated remaining time, and the speed as a multiple of real time; the Output window shows the
frames as they are rendered. Because the popup is modal, no graph edit can slip into the middle of a
render. **Cancel** finalises the file where it stopped — a playable partial. If a node is still
loading a file (Audio Player, Drum Machine, Mesh Loader, Image Sequencer) the render waits for it
between frames rather than capturing a stale frame, and gives up after 30 seconds naming the node.
The outcome lands on the toolbar status line: `rendered out.mp4 (480 frames, 16.0 s)`.

The same render runs headlessly from the command line, no windows:

```
shader_streamer --render <project.oss> <out.mp4> [--start B] [--end B] [--fps N] [--size WxH] [--preroll B]
```

Finish defaults to the project's Automation song length, the size to the Preferences texture size,
the frame rate to 60, and the pre-roll to 1 bar. It exits 0 when the file was written and 1 with the
reason on stderr otherwise, so it can be scripted.

Four things to know:

- **Nothing is rewound afterwards.** The graph is left exactly where the render ended, as if you had
  played the range: LFO phases, free-running playheads, synth envelopes and sequencer positions are
  not restored. The transport (position, tempo, loop) is.
- **Real-time inputs are not offline-capable.** **Audio In** and **MIDI In** capture whatever happens
  to arrive while the render runs, which has nothing to do with the bar being rendered.
- **MIDI sync is not driven during a render.** Incoming Beat Clock / MTC is ignored for the duration
  (the transport is restored when the render ends), and the sync-out sender carries on ticking from
  its last pre-render state.
- **Audio Out, MIDI Out and the Recorder are silent while rendering** — nothing goes to your sound
  card or to MIDI hardware, and the render owns the encoder. A Recorder recording in progress is
  stopped and saved when the render starts, and stays stopped afterwards until you toggle its
  `record` off and on again (restarting it would reopen and truncate the file it just saved).

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
**Audio / Video / Image / MIDI / 3D / Presets** tabs. Each tab is a table of media files — add a file, edit its
label and path inline, pick a path with the **...** Browse button (a native file dialog), or remove it.
**Add files…** opens a native multi-select dialog (filtered to the tab's media type) and adds every
chosen file at once. Each file
carries a hidden, stable id plus a label, and the whole library is saved and loaded with the
`.oss` project.
Nodes that load media — **Audio Player**, **Video Player**, **Mesh Loader**, **MIDI File**, and the
**Drum Machine**'s voices — show a **▾** next to their `file` field that picks a path from the
matching Assets tab (it copies the asset's path in; you can still type a path directly).
A **Tags** column tags each file (colored chips + an add box); each tab's tag bar filters the grid
(click tags — match any; none = show all) and right-clicking a tag recolors it. Tags and their
colors are saved with the project.
Files are shown in a **collapsible folder tree** grouped by their path — expand or collapse folders, and
**double-click** a file's name to rename it. Click a file to select it, **Ctrl/Cmd**-click to toggle, or
**Shift**-click a range; then typing a tag in any selected row (or removing one) applies it to every
selected row.

The asset library is a separate file you manage from the **Asset Library** menu: **Open Asset
Library**, **Save** / **Save As** a portable `.osslib`, and **Remap Directory** to swap a base folder
across every asset path (handy when a library was built on another machine). A project stores a
*reference* to its library and loads it on open. Set a default **Projects** folder and **Asset
library** folder under Preferences → Locations to make the file dialogs open where you keep things.

## projectM (optional)

The **projectM** node (Texture menu) runs the Milkdrop-compatible
[projectM](https://github.com/projectM-visualizer/projectm) visualizer on the graph's audio. It is
optional: the app loads `libprojectM-4` at runtime if it finds it, and the node only appears in the
add-node menu when it does. Nothing is linked or bundled.

It needs **projectM 4.2 or later**. 4.2 is not released yet (the latest release, 4.1.7, cannot
render into a framebuffer), so build it from source. This commit is the one the node was developed
against:

```bash
git clone --recurse-submodules https://github.com/projectM-visualizer/projectm.git && cd projectm && git checkout 1e7ef7803b69024d1e0656705670adda2ffac817 && git submodule update --init --recursive
```

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$HOME/.local" -DBUILD_SHARED_LIBS=ON && cmake --build build -j && cmake --install build
```

The app looks in your own `~/.local/lib` first, then `/usr/local/lib` and `/opt/homebrew/lib`; on
Linux it also asks the system loader by bare name (the ld.so cache and the usual library paths),
which is skipped on macOS because there a bare name is resolved from the current working directory
first. On Windows put `projectM-4.dll` beside the app or on `PATH`. For anywhere else, set
**Preferences → Locations → projectM library** — it must be an absolute path. The line underneath
shows what the loader found: the version it loaded, the path of any library it rejected, or
`could not load <path>: …` for a library that was found but would not load (a wrong architecture,
or a missing dependency of its own). Homebrew's `projectm` formula is 3.1.12 and will not work.

Presets are not included. Add `.milk` files in **View → Assets → Presets** and pick one on the
node; **prev / next / random** and the bar-synced `sync` step through the other presets in the same
folder. Milkdrop texture packs can be pointed to with **projectM textures** in the same
Preferences tab.

Changing preset is not free: projectM compiles each preset when it is loaded, on the same thread
that draws the app. Milkdrop-2 presets typically take 0.1-0.5 s, so a preset change (by hand, by
button, or on a `sync` bar boundary) briefly stalls the picture; older Milkdrop-1 presets load in a
few milliseconds. A smooth `blend` also renders two presets for its duration.

projectM is LGPL-2.1. This app never links against it and never distributes it: the library is
loaded at runtime from your own installation.

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
| `src/ui/` | node-editor panel + the Properties / Controls / Assets / Preferences panels |
| `shaders/` | fragment shaders |
| `docs/superpowers/` | design spec and implementation plan |

See [CLAUDE.md](CLAUDE.md) for architecture notes and conventions.
