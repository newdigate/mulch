# Offline Render — Design

**Date:** 2026-09-22
**Status:** Approved (brainstorm)

## Goal

An **offline render** facility that writes the graph's audio-visual output between a
**start bar** and a **finish bar** to a movie file. Unlike the real-time **Recorder** node,
which stamps frames with the wall clock and loses frames whenever the app runs slow, the
offline render drives the graph with a fixed clock and takes as long as it needs: every
frame in the range lands in the file, in order, with sample-exact audio.

## Decisions (from brainstorm)

- **Source: what you see and hear.** Video is the first **Output** node's texture; audio is
  what feeds the first **Audio Out** node. No new node to wire; the file matches the live
  output. Audio Out is optional (video-only file without one).
- **Parameters:** start bar, finish bar, pre-roll bars, frame rate (24 / 25 / 30 / 50 / 60),
  render width × height (defaults to the Preferences texture size), output file.
- **Bar numbering** follows the **Loop fields**: start 0 = the first bar, finish exclusive.
  `0 → 8` renders the first eight bars, exactly the loop range. Fractional bars allowed.
- **Loop model: incremental renderer stepped from the main loop.** An `OfflineRenderer`
  class owns the job; `Application::frame` calls `step(budget)` while a render is active and
  ImGui draws a modal progress popup with Cancel. The headless CLI drives the same class.
- **Headless command line mode** (`--render`) for scripted and testable renders.
- Frame rate choices are exactly those that divide 48 000, so audio and video stay
  sample-locked.

## Architecture

```
File > Render Video…  ──►  ui/RenderDialog  ──settings──►  app/OfflineRenderer
                                                              │ start(): snapshot transport, swap prefs, offline=true
                                                              │ step():  pose as external clock, evaluate, capture
main.cpp --render ─────────────────────────────────────────►  │ finish(): close encoder, restore everything
                                                              ▼
                             Graph::evaluate(1/fps)  →  OutputNode::current()  ─blit─►  render FBO ─readback─►  VideoEncoder
                                                     →  AudioOutputNode::lastBlock() ─────────────────────────►
core/OfflineRender.h (GL-free): RenderSettings + frame-count / clock / validation / CLI-arg helpers
```

### Clock model

The renderer poses as the transport's **external clock**, the mechanism `MidiSyncEngine`
already uses. `start()` sets `externalClock = true`, `playing = true`, `looping = false`
(advance is a no-op anyway). For every frame `k` it sets

```
transport.seconds = max(0, startBar * secondsPerBar + k / fps)
```

directly, then calls `Graph::evaluate(1.0f / fps)`. Consequences:

- `Transport::advance()` is a no-op, so there is no loop wrap and no float accumulation:
  every frame's position is computed from the frame index.
- Every node sees a fixed `dt = 1 / fps`. Transport-synced nodes (Step Seq, Arpeggiator,
  Chord Player, MIDI File, synced Audio Player / Image Sequencer, automation) follow the bar
  position exactly; free-running nodes (unsynced Audio Player, Video Player, free LFO,
  projectM's frame time) integrate the fixed `dt`, so they run at the correct speed.
- Every audio source sizes its block with `audioBlockFrames(sampleRate, dt)`; at 48 kHz and
  a listed fps each frame carries exactly `48000 / fps` samples (800 at 60 fps).

`bpm` is left untouched. A tempo change during a render is impossible because the progress
popup is modal.

### Pre-roll

Frames `k = -P .. -1` run before the start bar with the same fixed clock and are **not
captured**. `P = prerollFrameCount(prerollBars, secondsPerBar, fps)`. Their transport
position is clamped at zero, so a render that starts at bar 0 pre-rolls sitting at the
start (transport-synced nodes hold step 0; free-running nodes tick). Pre-roll lets synth
envelopes, MIDI note-ons that begin before the start bar, and async loaders settle, the way a
DAW's pre-roll does. Default 1 bar; 0 is allowed.

### Loader gate

A new hook on `Node`:

```cpp
// True while an asynchronous load (worker-thread decode/parse) is in flight AND NOT YET
// FINISHED, so the offline renderer can wait before advancing to the next frame. Default
// false. Must be answerable without evaluate() (the renderer polls it between frames).
virtual bool loading() const { return false; }
```

`AsyncLoader<T>` gains `bool pending() const` = `future_.valid() && wait_for(0) != ready`.
The "not yet finished" part matters: a future that has completed but has not been consumed
by the node's next `poll()` must **not** count as loading, or the renderer would wait for an
evaluate that only happens after the wait ends (deadlock). Implementations:

| Node | `loading()` |
|---|---|
| Audio Player | `loader_.pending()` |
| Drum Machine | any of the 4 `loaders_[i].pending()` |
| Mesh Loader | `loader_.pending()` |
| Image Sequencer | `fetch_.valid() && fetch_.wait_for(0) != ready` |

Before evaluating each frame (pre-roll or captured) the renderer checks every node; while
any reports loading it **yields** (returns from `step` without rendering) and re-checks on
the next call. The wait timer resets whenever a frame is evaluated. A wait longer than
`kRenderLoadTimeoutSeconds` (30 s) aborts the render with `timed out waiting for <node name>
to load`.

Video Player and projectM load synchronously on the graph thread and need no hook: a slow
preset or a decoder seek simply makes that frame take longer, which is the point of an
offline render.

### Offline flag

`Graph::setOffline(bool)` / `Graph::offline()` feed a new `EvalContext::offline` (default
false, set by `Graph::evaluate`). Three sinks honour it:

- **Audio Out** — builds its interleaved-stereo block (the existing mirror rule: a lone mono
  wire fills both sides) on **every** evaluate, before any device work, and exposes it:
  ```cpp
  const std::vector<float>& lastBlock() const;   // interleaved L,R from the last evaluate; empty = nothing connected
  int lastSampleRate() const;                    // 0 when nothing connected
  ```
  While offline it returns right after building the block: no `ensureDevice`, no event
  flush, no ring push (a faster-than-real-time render would overflow the ring anyway).
  Reordering the block build ahead of the device check also means a machine with **no audio
  device** still renders audio.
- **MIDI Out** — still syncs its port set, but sends nothing while offline.
- **Recorder** — treats `record` as off while offline. A live recording in progress when a
  render starts is stopped and saved, and no second file is written during the render.

Audio In and MIDI In are real-time inputs; during a render they yield whatever arrives on
the wall clock. That is documented as unsupported for offline use, not guarded.

### Capture path

`start()` locates the first `OutputNode` (required) and the first `AudioOutputNode`
(optional). For each captured frame `k ≥ 0`, after `evaluate`:

1. **Blit.** Bind the renderer's own `Framebuffer` at `width × height` and draw the Output
   node's texture through a `FullscreenPass` with the trivial blit shader, **without a V
   flip**: FBO textures are bottom-up and `VideoEncoder::addVideoFrame` expects bottom-up
   rows (it flips for encoding), so the orientation matches the Recorder's direct read-back.
   Stretching to the render size mirrors what the Output window does to its framebuffer, so
   a Video node's native size or a size change mid-render never drops a frame. An empty
   `TexRef` (nothing connected) clears the FBO to black and counts a black frame.
2. **Read back** with `glReadPixels` (`GL_PACK_ALIGNMENT 1`) into a reusable buffer.
3. **Open the encoder lazily** on the first captured frame:
   `enc.open(outPath, width, height, fps, audioRate, audioRate > 0 ? 2 : 0)` where
   `audioRate = audioOut ? audioOut->lastSampleRate() : 0`. Same rule as the Recorder:
   audio is recorded only if it is connected when capture starts; otherwise the file is
   video-only and the final status says so.
4. `enc.addVideoFrame(pixels, k / (double)fps)` — the encoder's pts is `llround(t · fps)`,
   so it is exactly `k`.
5. **Audio.** Copy `lastBlock()` and pad with silence or trim to exactly
   `audioSamplesPerFrame(rate, fps)` interleaved frames before `enc.addAudio`, so the
   audio clock (sample count) can never drift from the video clock even if a source
   mis-sizes a block. Frames that needed padding or trimming are counted in the status.
   Every source in the app runs at 48 kHz, where all listed rates divide exactly; a source
   at another rate would drift by the fractional remainder per frame (accepted, not guarded).

### Resolution override

`start()` copies the live `Preferences` into `renderPrefs_`, sets
`textureWidth/Height` to the render size, and calls `graph.setPreferences(&renderPrefs_)`.
`ShaderNode`, `WireframeNode`, and `ShadedRenderNode` already recreate their FBO when the
size in `ctx.prefs` changes, so the first pre-roll (or captured) frame renders at the new
size. `finish()` restores the live pointer and the next live frame recreates them at the
live size. The render size has its own bounds, `[16, 8192]` per axis
(`kRenderMinSize` / `kRenderMaxSize`), independent of `clampTextureSize`'s
`[320,1920] × [240,1080]` — a 4K offline render is a primary use of the facility.
`kRenderMaxSize` is a sanity bound, not a promise: an FBO the driver refuses fails the
render with the GL error in the status.

### State restore

`start()` snapshots the whole `Transport` struct and the graph's live `Preferences` pointer.
`finish()` (reached from success, failure, cancel, and the destructor) restores both, clears
the offline flag, and closes the encoder (idempotent). It always runs exactly once per job.

**Node-internal state is not restored.** LFO phases, synth envelopes, free-running playheads,
MIDI note tracking, and the Image Sequencer's position are left where the render ended,
exactly as if the graph had been played through the range. This is stated in the README.

## Component detail

### `core/OfflineRender.h` (GL-free, header-only)

```cpp
constexpr int    kRenderFrameRates[] = {24, 25, 30, 50, 60};   // all divide 48 000
constexpr int    kRenderMinSize = 16, kRenderMaxSize = 8192;
constexpr double kRenderLoadTimeoutSeconds = 30.0;

struct RenderSettings {
    double startBar    = 0.0;    // Loop-field convention: 0 = the first bar
    double endBar      = 8.0;    // exclusive
    double prerollBars = 1.0;    // >= 0, discarded frames before startBar
    int    fps         = 60;
    int    width       = 1280, height = 720;
    std::string outPath;
};

// Captured frames: those whose start time lies in [start, end).
// = ceil(durationSeconds * fps - 1e-6). At 120 bpm, 8 bars @ 60 fps = 960; 1 bar @ 30 fps = 60.
long   renderFrameCount(const RenderSettings&, double secondsPerBar);
long   prerollFrameCount(const RenderSettings&, double secondsPerBar);   // same rule over prerollBars
// Transport position for frame k (k < 0 during pre-roll), clamped at 0.
double renderFrameSeconds(const RenderSettings&, double secondsPerBar, long k);
int    audioSamplesPerFrame(int sampleRate, int fps);   // sampleRate / fps (exact for listed rates at 48 kHz)
bool   isRenderFrameRate(int fps);
// One error string; `hasOutputNode` is supplied by the caller (core knows no node types).
bool   validateRenderSettings(const RenderSettings&, bool hasOutputNode, std::string& err);

// CLI: `--render <project.oss> <out.mp4> [--start B] [--end B] [--fps N] [--size WxH] [--preroll B]`.
// The parser first sets the SENTINELS endBar = -1 and width = height = 0, then overwrites
// only the fields given; the driver fills the sentinels after the project loads
// (endBar → the project's Automation song length, size → the Preferences texture size).
struct RenderCliArgs { std::string projectPath; RenderSettings settings; };
bool   parseRenderArgs(const std::vector<std::string>& args, RenderCliArgs& out, std::string& err);
```

The epsilon in the frame-count `ceil` guards float noise (0.1 bar at 120 bpm at 60 fps is
`12.000000000000002`); a non-empty range always yields at least one frame.

### `app/OfflineRenderer.{h,cpp}`

```cpp
class OfflineRenderer {
public:
    enum class Phase { Idle, Preroll, Rendering, Done, Failed, Cancelled };
    struct Progress {
        Phase  phase = Phase::Idle;
        long   prerollDone = 0, prerollTotal = 0;
        long   framesDone  = 0, framesTotal  = 0;   // captured frames
        long   blackFrames = 0, resizedAudioFrames = 0;
        bool   audio = false;                      // file has an audio track
        double elapsedSeconds = 0.0;               // wall time since start()
        double speed = 0.0;                        // captured frames per wall second
        std::string outPath, status;               // status: outcome text or error
    };

    OfflineRenderer() = default;
    ~OfflineRenderer();               // cancel() if active (finalises a valid partial file)

    // Validate, snapshot state, swap prefs, arm the clock. False + `err` on a bad setting,
    // no Output node, or a bad size. GL objects are created here (editor context current).
    bool start(Graph& g, const Preferences* livePrefs, const RenderSettings& s, std::string& err);

    // Render frames until `budgetSeconds` of wall time have elapsed. Renders at least one
    // frame per call (so progress is guaranteed, even with budget 0) UNLESS a node reports
    // loading(), in which case it yields without rendering and re-checks on the next call.
    // Returns active(). The last call performs finish().
    bool step(double budgetSeconds);

    void cancel();                    // close the encoder (partial file plays), restore state
    bool active() const;
    const Progress& progress() const; // valid after the job ends too (outcome for the UI)
};
```

`step` per iteration: loader gate → set transport for frame `k` → `graph.evaluate(1.0f/fps)`
→ `capture(k)` when `k ≥ 0` → advance `k`; when `k == framesTotal`, `finish(Done)`.
Encoder open failure, a GL FBO failure, and the loader timeout call `finish(Failed)` with
the message in `status`. Progress is updated every iteration.

Owned: the `VideoEncoder`, the blit `Framebuffer` + `FullscreenPass` + blit program, the
pixel and audio scratch buffers, `renderPrefs_`, the transport snapshot, the live prefs
pointer, `Graph*`, and the two sink node pointers.

### `ui/RenderDialog.{h,cpp}`

```cpp
class RenderDialog {
public:
    // Draws the settings window when *show, and the modal progress popup while the renderer
    // is active. `projectPath` seeds the default file name; `status` receives the outcome
    // line for the toolbar.
    void draw(Graph& g, const Preferences& prefs, OfflineRenderer& r, bool* show,
              const std::string& projectPath, std::string& status);
private:
    RenderSettings settings_;
    bool           seeded_ = false;   // defaults filled on first open
    std::string    error_;
};
```

Settings window ("Render Video"):

- **Start bar / Finish bar** (`InputFloat`, `%.2f`, same convention as the Loop fields) and
  a **Use loop range** button that copies `loopStartBar` / `loopEndBar`. Start seeds to 0;
  Finish seeds to `graph.automation().lengthBars()`.
- **Pre-roll (bars)**, default 1. **Frame rate** combo over `kRenderFrameRates`, default 60.
- **Width / Height** (`InputInt`), seeded from `prefs.textureWidth/Height`, with a
  **Use live size** button.
- **Output file** text + **Browse…** (`saveFileDialog("Render Video", "MP4", {"mp4"},
  defName, prefs.projectsDir)` + `ensureExtension(path, "mp4")`). Default name is the project
  basename with `.mp4`, or `render.mp4` when untitled.
- A read-out line: `8.00 bars → 960 frames (16.0 s at 120 bpm)`.
- **Render** (disabled with the inline validation reason while invalid) and **Close**.
  Render calls `r.start(...)`; a failure shows `error_` inline.

Settings are kept for the session only (not persisted to the project).

Progress popup ("Rendering", `BeginPopupModal`, auto-resize, opened while `r.active()`):
a `ProgressBar` over captured frames (pre-roll shown as its own short bar first), the phase,
`frames done / total`, elapsed, estimated remaining (`(total − done) / speed`), speed as a
multiple of real time (`speed / fps`), the output path, and a **Cancel** button. Because it is
modal, no graph edit can slip in during the render. When the job ends the popup closes and
the outcome (`Done` / `Failed` / `Cancelled` + status) is shown in the settings window and
copied to the toolbar status line: `rendered out.mp4 (960 frames, 16.0 s)`.

### `Application` changes

- Members (declared **after** `graph_` so they are destroyed first — the renderer restores
  the graph in its destructor): `OfflineRenderer renderer_; RenderDialog renderDialog_;
  bool showRender_ = false;`.
- `ProjectBarIO::onRender` → a **File → Render Video…** item after **Save As…**, separated.
- In `frame()`, after the panels:
  ```cpp
  renderDialog_.draw(graph_, prefs_, renderer_, &showRender_, currentPath_, projectStatus_);
  if (renderer_.active()) {
      renderer_.step(kRenderStepBudget);   // an Application.cpp constant, 0.1 s: the UI keeps ~10 fps while rendering
  } else {
      syncEngine_.update(graph_.transport(), prefs_, dt);
      graph_.evaluate(dt);
  }
  ```
  The MIDI sync engine is not updated during a render (it would fight the renderer for the
  transport). Its sender thread keeps its last snapshot, so sync-out carries on at the
  pre-render state; sync-in is overridden for the duration and restored by the transport
  snapshot. Documented as a known limitation.
- Public accessors for the CLI: `OfflineRenderer& renderer()`, `const Preferences& preferences() const`.
- `outputTexture()` is unchanged: the Output node's `current()` is the render frame, so the
  Output window shows the render as it progresses.

### `main.cpp --render`

```
shader_streamer --render <project.oss> <out.mp4> [--start B] [--end B] [--fps N] [--size WxH] [--preroll B]
```

`runRender()` mirrors `runScreenshot()`: hidden GLFW window, ImGui context (the
`Application` panels need one), `Application app(win)` (which loads `preferences.oss`),
`app.loadProjectFromFile(project)`, fill the sentinels (`endBar < 0` → song length;
`width == 0` → Preferences size), `app.renderer().start(...)`, then

```cpp
while (app.renderer().active()) { glfwPollEvents(); app.renderer().step(1.0); /* progress line to stderr once a second */ }
```

`Application::frame` is never called (no UI). Exit 0 when the phase is `Done`, otherwise 1
with the status on stderr. Runs from the repo root or a package's `shaders/` CWD like the
app. No new `--render`-only dependencies.

## Data flow / behavior (one render)

1. User opens **File → Render Video…**, sets `0 → 8` bars, 60 fps, 1920×1080, `out.mp4`,
   presses **Render**.
2. `start()`: validates; finds Output + Audio Out; snapshots the transport; swaps in the
   render Preferences; `setOffline(true)`; arms the external clock. `k = −120` (1 bar
   pre-roll at 120 bpm).
3. Each UI frame calls `step(0.1)`: for ~100 ms, per frame: loader gate → position →
   evaluate → (k ≥ 0) blit, read back, encode video + padded/trimmed audio → `k++`. The modal
   popup shows progress; the Output window shows the frames.
4. `k == 960` → `finish(Done)`: encoder closed, transport and prefs restored, offline
   cleared; status `rendered out.mp4 (960 frames, 16.0 s)`. The next live frame recreates
   FBOs at the live size.

## Error handling

| Condition | Behaviour |
|---|---|
| Finish ≤ start, fps not listed, size out of `[16,8192]`, empty path | `validateRenderSettings` fails; Render button disabled with the reason / CLI exits 1 |
| No Output node | validation error `add an Output node` |
| No Audio Out, or nothing connected to it at the first captured frame | video-only file; status says `video only` |
| Encoder `open` fails (bad path, codec) | `finish(Failed)` with the encoder's message; nothing written |
| FBO creation fails at the render size | `finish(Failed)` with the GL status |
| A node reports `loading()` for > 30 s | `finish(Failed)`: `timed out waiting for <name> to load`; the partial file is finalised |
| Cancel | encoder closed → a playable partial file; `cancelled after N frames` |
| Empty Output texture on a frame | black frame captured (never skipped); counted in the status |
| Audio block ≠ `sampleRate / fps` | padded with silence / trimmed; counted in the status |
| App quits mid-render | `~OfflineRenderer` cancels → valid partial file, state restored |

## Testing

### `core_tests` — `tests/test_offline_render.cpp`

- `renderFrameCount`: 8 bars @ 120 bpm @ 60 fps = 960; 1 bar @ 100 bpm @ 30 fps = 72;
  0.5 bar @ 120 @ 60 = 60; the float-noise case (0.1 bar @ 120 @ 60 = 12, not 13); a tiny
  non-empty range yields 1.
- `prerollFrameCount`: 1 bar @ 120 @ 60 = 120; 0 bars = 0.
- `renderFrameSeconds`: `k = 0` → `startBar · spb`; negative `k` from start 0 clamps to 0;
  start 4 at 120 bpm and 60 fps, `k = −1` → `8 − 1/60`.
- `audioSamplesPerFrame(48000, fps)` is exact for every listed rate; `isRenderFrameRate(29)` false.
- `validateRenderSettings`: each failure yields a distinct message; a valid set passes.
- `parseRenderArgs`: the full option set; defaults leave the sentinels; `--size 12x`,
  an unknown flag, and a missing positional each fail with a message.

### `gl_smoke` — new scenario

- **End to end.** Colour → Output, Sine → Audio Out. Render bars `0 → 1` at 120 bpm,
  30 fps, 160×120, pre-roll 0.5 bar to `build/_offline.mp4` via a `step(0.02)` loop. Decode
  with `VideoDecoder`: exactly 60 frames, 160×120, 2 audio channels, non-silent audio, the
  Colour at the centre pixel. Progress reports `framesTotal == 60`, `audio == true`.
- **Restore.** After the job: the transport equals its pre-render snapshot (seconds,
  playing, externalClock, looping), `graph.offline()` is false, the graph's prefs pointer is
  the live one, and one live evaluate brings the Colour texture back to the live size.
- **Loader gate.** A test-only node whose `loading()` returns true for the first N polls
  (mutable counter): `step` yields (frames done stay 0) until it clears, and the final frame
  count is still exact.
- **Cancel.** `start`, one `step`, `cancel`: the file decodes with ≥ 1 and < 60 frames; the
  transport is restored; `progress().phase == Cancelled`.
- **Offline sinks.** With a Recorder set to record inline, no recorder file is written
  during the render; Audio Out's `lastBlock()` is non-empty with no device opened.

### `render_cli` — ctest

Runs the binary on a checked-in `tests/assets/render_smoke.oss` (Colour → Output, Sine →
Audio Out): `--render … build/_cli_render.mp4 --end 1 --fps 24 --size 160x120`, expecting
exit 0. Needs a GL context like `gl_smoke`; the three CI workflows run it in the same
best-effort step (`-R "gl_smoke|render_cli"`).

## Documentation

- README: a **Rendering** section (dialog, CLI, bar convention, pre-roll, what is and is not
  restored, real-time inputs unsupported, MIDI sync behaviour during a render).
- CLAUDE.md: an architecture bullet for the offline render (clock model, loader gate,
  offline flag, capture path) and the `Node::loading` hook.

## Out of scope (YAGNI)

- Persisting render settings in the project file.
- Render queues / multiple ranges, rendering the loop N times.
- Image-sequence (PNG) output, audio-only export, codec / bitrate / container options
  (the encoder's H.264 + AAC MP4 defaults, `gop = fps`).
- Restoring node-internal state after a render.
- Offline handling of Audio In / MIDI In (real-time inputs).
- Pausing the MIDI sync sender during a render.
