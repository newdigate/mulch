# Offline Render — Design

**Date:** 2026-09-22
**Status:** Implemented (branch `feat/offline-render`)

> This stayed a design document: the signatures and rules below have been corrected where the
> built system differs, and **[Changes during implementation](#changes-during-implementation)**
> at the end lists the behavioural changes and why they were made. For how the shipped code is
> organised, read `CLAUDE.md`'s *Offline render* bullet and the headers themselves.

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
- **MIDI Out** — still syncs its port set (so the ports are already right the instant the render
  ends, with no reopen on that first live frame), but sends nothing while offline. It fires one
  `allNotesOff()` on the offline edge: since it stops sending note-offs, an external synth would
  otherwise hold whatever the last live frame left sounding for the whole render.
- **Recorder** — treats `record` as off while offline. A live recording in progress when a
  render starts is stopped and saved, and no second file is written during the render. It also
  **latches** a suppression flag for as long as `record` stays on: otherwise the still-armed
  toggle restarts the recording on the first live frame after the render and truncates the file
  it just saved. The latch clears only when `record` is toggled off, so re-arming is explicit.

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
   a Video node's native size or a size change mid-render never drops a frame. The whole blit
   runs inside a `GLStateGuard` (so it assumes nothing about what the caller had bound and
   leaves nothing behind for the next node or ImGui), and the FBO is cleared **first and
   unconditionally**, not only on the no-texture branch: `linkProgram` returns a live-looking
   non-zero handle even when the link failed, so a broken blit program would otherwise read
   back undefined texture memory and report a clean render. An empty `TexRef` (nothing
   connected) leaves that clear as the frame and counts a black frame.
2. **Read back** with `glReadPixels` (`GL_PACK_ALIGNMENT 1`) into a reusable buffer.
3. **Open the encoder lazily** on the first captured frame:
   `enc.open(outPath, width, height, fps, audioRate, audioRate > 0 ? 2 : 0)` where
   `audioRate` is the Audio Out's `lastSampleRate()` if it has a non-empty block, else 0. Same
   rule as the Recorder: audio is recorded only if it is connected when capture starts — the
   presence of the track *and* its sample rate are latched there for the whole render, and
   audio connected later is ignored. Otherwise the file is video-only and the status says so.
4. `enc.addVideoFrame(pixels, k / (double)fps)` — the encoder's pts is `llround(t · fps)`,
   so it is exactly `k`. A refused frame **fails the render** (`finish(Failed)` naming the
   frame and FFmpeg's reason): a full disk must not come back as exit 0 with a file quietly
   missing its tail.
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
the offline flag, and closes the encoder (idempotent). It always runs exactly once per job —
`active()` is *derived* from the phase rather than tracked separately, and `finish()` is the only
place the phase leaves a running state, so no `step()` path may return early without going
through it (a stuck offline flag would silently mute Audio Out, MIDI Out and the Recorder for the
rest of the session). A `close()` that fails downgrades a `Done` outcome to `Failed`: an
unflushed/untrailered mp4 is unplayable however many frames were captured.

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

// Frames whose start time lies in [0, durationSeconds) = ceil(durationSeconds * fps - 1e-6),
// 0 for an empty/negative range or a non-finite / unrepresentable product.
long long renderFramesOver(double durationSeconds, int fps);
// Captured frames for [startBar, endBar). At 120 bpm, 8 bars @ 60 fps = 960; 1 bar @ 30 fps = 60.
long long renderFrameCount(const RenderSettings&, double secondsPerBar);
long long prerollFrameCount(const RenderSettings&, double secondsPerBar);   // same rule over prerollBars
// Transport position for frame k (k < 0 during pre-roll), clamped at 0.
double renderFrameSeconds(const RenderSettings&, double secondsPerBar, long long k);
int    audioSamplesPerFrame(int sampleRate, int fps);   // sampleRate / fps (exact for listed rates at 48 kHz)
bool   isRenderFrameRate(int fps);
// One error string; `hasOutputNode` is supplied by the caller (core knows no node types).
// Width and height must also be EVEN: the H.264 yuv420p encode needs them.
bool   validateRenderSettings(const RenderSettings&, bool hasOutputNode, std::string& err);

// CLI: `--render <project.oss> <out.mp4> [--start B] [--end B] [--fps N] [--size WxH] [--preroll B]`.
// The parser first sets the SENTINELS endBar = -1 and width = height = 0, then overwrites
// only the fields given; the driver fills the sentinels after the project loads
// (endBar → the project's Automation song length, size → the Preferences texture size).
// endGiven/sizeGiven are the structural "was it on the command line" signal — the driver must
// NOT compare the settings back against the sentinels, or a user who types one (`--end -1`,
// `--size 0x100`) is silently defaulted instead of rejected by validateRenderSettings.
struct RenderCliArgs {
    std::string projectPath; RenderSettings settings;
    bool endGiven = false, sizeGiven = false;
};
bool   parseRenderArgs(const std::vector<std::string>& args, RenderCliArgs& out, std::string& err);
```

The epsilon in the frame-count `ceil` guards float noise (0.1 bar at 120 bpm at 60 fps is
`12.000000000000002`); a non-empty range always yields at least one frame. Counts are `long long`
because a legal but absurd range (`--end 1e18`) overflows a 32-bit `long` on Windows; a count that
is not finite and representable comes back as 0 and is rejected by `start()` (see below).

### `app/OfflineRenderer.{h,cpp}`

```cpp
class OfflineRenderer {
public:
    enum class Phase { Idle, Preroll, Rendering, Done, Failed, Cancelled };
    struct Progress {
        Phase  phase = Phase::Idle;
        long long prerollDone = 0, prerollTotal = 0;
        long long framesDone  = 0, framesTotal  = 0;   // captured frames
        int    fps = 0;                            // the job's frame rate, so a driver can show
                                                   // speed/fps without holding the RenderSettings
        long long blackFrames = 0, resizedAudioFrames = 0;
        bool   audio = false;                      // file has an audio track
        bool   waitingForLoad = false;             // this step() yielded on the loader gate
        double elapsedSeconds = 0.0;               // wall time since start()
        double speed = 0.0;                        // captured frames per wall second
        std::string outPath, status;               // status: outcome text or error
    };

    OfflineRenderer() = default;
    ~OfflineRenderer();               // cancel() if active (finalises a valid partial file)

    // Validate, snapshot state, swap prefs, arm the clock. False + `err` on a bad setting,
    // no Output node, an empty frame range at this tempo, or a bad size. GL objects are created
    // here (editor context current). The live Preferences come from g.preferences(): there is no
    // separate argument for a caller to pass a stale pointer that disagrees with the restore.
    bool start(Graph& g, const RenderSettings& s, std::string& err);

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
Encoder open failure, a GL FBO failure, a refused `addVideoFrame`/`addAudio`, a failed
`close()`, and the loader timeout all call `finish(Failed)` with the message in `status`.
Progress is updated every iteration.

Owned: the `VideoEncoder`, the blit `Framebuffer` + `FullscreenPass` + blit program, the
pixel and audio scratch buffers, `renderPrefs_`, the transport snapshot (plus `renderClock_`,
the armed clock re-asserted verbatim each frame), the live prefs pointer, `Graph*`, and the two
sink node **ids** — re-resolved through `Graph::findNode` every frame, because a project load
destroys every node and a cached `Node*` would dangle (ids are never reused).

`OfflineRenderer` is non-copyable and address-sensitive: while a job runs, the graph's
`Preferences` pointer points into `renderPrefs_`. Whatever holds one across frames must be
declared **after** its `Graph`, so `~OfflineRenderer`'s restore runs before the graph goes away.

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
    std::string    seededPath_;       // the projectPath settings_ was last seeded from
    bool           wasActive_ = false;// to notice the job ending between draws
    std::string    error_, outcome_;
};
```

Settings window ("Render Video"):

- **Start bar / Finish bar** (`InputFloat`, `%.2f`, same convention as the Loop fields) and
  a **Use loop range** button that copies `loopStartBar` / `loopEndBar`. Start seeds to 0;
  Finish seeds to `graph.automation().lengthBars()`.
- **Pre-roll (bars)**, default 1. **Frame rate** combo over `kRenderFrameRates`, default 60.
- **Width / Height** (`InputInt`), seeded from `prefs.textureWidth/Height`, with a
  **Use live size** button.
- **Output file** text + **Browse…** (`saveFileDialog("Render Video", "MP4", {"mp4"}, defName,
  startDir)` + `ensureExtension(path, "mp4")`, applied to a typed path too, since that one never
  went through the dialog). Default name is the project basename with `.mp4` (case-insensitively
  stripped, so `Foo.OSS` does not become `Foo.OSS.mp4`), or `render.mp4` when untitled.
  `startDir` is the directory currently in the field, falling back to `prefs.projectsDir` only
  before the user has pointed the dialog anywhere — otherwise every re-open snaps back to the
  projects folder. The settings are re-seeded whenever the **loaded project changes**, not just
  on first open: a stale path from the previous project would silently overwrite that project's
  video.
- A read-out line: `8.00 bars → 960 frames (16.0 s at 120 bpm)`.
- **Render** (disabled with the inline validation reason while invalid) and **Close**.
  Render calls `r.start(...)`; a failure shows `error_` inline.

Settings are kept for the session only (not persisted to the project).

Progress popup ("Rendering", `BeginPopupModal`, auto-resize, opened while `r.active()`):
a `ProgressBar` over captured frames (pre-roll shown as its own short bar first), the phase,
`frames done / total`, elapsed, estimated remaining (`(total − done) / speed`), speed as a
multiple of real time (`speed / progress.fps`), the output path, and a **Cancel** button. Until
the first captured frame there is no rate to extrapolate from — the whole pre-roll — so only
elapsed time is shown; `remaining ~0 s, 0.00x real time` would read as a stalled or instant job. Because it is
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
  The `else` branch also self-heals a stuck `graph_.offline()` — one line that makes an
  otherwise unrecoverable-without-restart failure impossible. It should never fire.
- Public accessors for the CLI: `OfflineRenderer& renderer()`, `const Preferences& preferences() const`,
  plus `showRenderDialog(bool)` so `--screenshot` opens the dialog and the capture exercises it.
- `outputTexture()` is unchanged: the Output node's `current()` is the render frame, so the
  Output window shows the render as it progresses.

### `main.cpp --render`

```
shader_streamer --render <project.oss> <out.mp4> [--start B] [--end B] [--fps N] [--size WxH] [--preroll B]
```

`runRender()` mirrors `runScreenshot()`: hidden GLFW window, ImGui context (the
`Application` panels need one), `Application app(win)` (which loads `preferences.oss`),
`app.loadProjectFromFile(project)`, fill only what the command line did not give (`!endGiven` →
the Automation song length; `!sizeGiven` → the Preferences texture size — **not** a comparison
against the sentinel values), `app.renderer().start(...)`, then

```cpp
while (r.step(1.0)) {
    glfwPollEvents();
    if (r.progress().waitingForLoad) sleep_for(1ms);   // loader gate: yield, don't spin
    /* progress line to stderr once a second */
}
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
| Finish ≤ start, fps not listed, size out of `[16,8192]`, **odd** width or height, negative pre-roll, empty path | `validateRenderSettings` fails; Render button disabled with the reason / CLI exits 1 |
| No Output node | validation error `add an Output node` |
| A range that yields < 1 frame at this tempo (`--end 1e18`, `beatsPerBar = 0`) | `start()` rejects: `the render range is empty at this tempo`. Not a completion check in `step()` — that would `finish(Done)` with no encoder ever opened, reporting success and writing no file |
| No Audio Out, or nothing connected to it at the first captured frame | video-only file; status says `video only` |
| Encoder `open` fails (bad path, codec) | `finish(Failed)` with the encoder's message; nothing written |
| `addVideoFrame` / `addAudio` refused mid-render (ENOSPC) | `finish(Failed)`: `encode failed at frame N: <reason>`; CLI exits 1 |
| `close()` fails (flush / trailer, or a sticky earlier write failure) | a `Done` outcome is downgraded to `Failed`: `could not finalise <path>: <reason>` |
| FBO creation fails at the render size | `finish(Failed)` with the GL status |
| The Output node disappears mid-render (a project load) | `finish(Failed)`: `the Output node disappeared mid-render`. An Audio Out that vanishes encodes silence instead, so the two clocks stay locked |
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

### `gl_smoke` — new scenarios

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
- **Offline sinks.** Audio Out taps a non-empty, correctly-sized, mirrored block with the device
  never touched (and a live frame afterwards proves the device path *is* otherwise reached — the
  negative check could pass for the wrong reason). A live Recorder recording is stopped and saved
  when the render starts, stays saved for the rest of it, and does **not** restart on the first
  live frame afterwards while `record` is still armed.

Added during implementation, each pinning a claim the scenarios above cannot see:

- **No prefs argument.** The render-time copy and the restored value both come from
  `g.preferences()`, so they cannot disagree.
- **The fixed clock places every frame.** Rendering from bar **2** (not 0, where dropping the
  start offset is invisible) and recording the whole transport per frame catches an accumulated
  `dt`, a `dt` taken from the step budget, and a pre-roll with the sign backwards.
- **No V-flip.** The end-to-end scenario renders a flat colour, which is flip-invariant; a
  vertically asymmetric picture says which end of the decoded frame it lands on.
- **Exactly `sampleRate/fps` audio frames.** A deliberately mis-sized source (wrong in both
  directions) proves the encoded track's duration is set by the video clock.
- **Sinks re-resolved by id.** Clearing the graph mid-render is what tells a re-resolving
  `capture()` from one holding a stale pointer — and the failure must still run `finish()`.
- **An empty range is rejected, not "completed"**, and before the graph is put offline.
- **An unopenable encoder fails the render** (never `Done`), the whole signal the CLI's exit code
  gives a batch pipeline.
- **Write failures fail the render.** `RLIMIT_FSIZE` with `SIGXFSZ` ignored is the portable
  stand-in for a full disk (EFBIG for ENOSPC), in two variants: one small enough that the failure
  surfaces at `close()` (exercising the `Done` → `Failed` downgrade) and one whose high-entropy
  frames force flushes so it surfaces mid-render. A third checks a **Recorder** take that lost
  frames but still wrote its trailer reports `save failed`, not `saved`.

### `render_cli` — ctest

Runs the binary on a checked-in `tests/assets/render_smoke.oss` (Colour → Output, Sine →
Audio Out): `--render … build/_cli_render.mp4 --end 1 --fps 24 --size 160x120 --preroll 0.25`,
expecting exit 0 — plus a `FAIL_REGULAR_EXPRESSION` on `black frames|video only`, because
`restoreProject` silently drops unknown node types and type-mismatched control lines, so a
mis-typed fixture would otherwise "succeed" as a black, video-only file. Needs a GL context like
`gl_smoke`; the three CI workflows run it in the same best-effort step (`-R "gl_smoke|render_cli"`).

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

## Changes during implementation

Everything above is the design as built, with these corrections folded in. The list is here so a
reader who remembers the original design knows what moved and why.

1. **`start()` lost its `Preferences*` argument.** Both the render-time copy and the value
   restored by `finish()` now come from `g.preferences()`, so a caller cannot pass a stale
   pointer and have the two disagree.
2. **Frame counts are `long long`**, computed by a shared `renderFramesOver(durationSeconds, fps)`
   that also returns 0 for a non-finite or unrepresentable product (casting one is undefined).
   `start()` rejects a count of 0 as `the render range is empty at this tempo` — the design had no
   such check, and a completion check in `step()` would have reported success with no file.
3. **Even width and height are required** (`width % 2 || height % 2` → `width and height must be
   even`): the H.264 yuv420p encode needs them.
4. **Encode failures propagate.** The design assumed the encoder's per-call bools; five call sites
   were discarding them. `addVideoFrame`/`addAudio` refusals now fail the render, `VideoEncoder`
   latches a sticky `writeFailed_` that `close()` consults, a failed `close()` downgrades `Done`
   to `Failed`, and the **Recorder** reports `save failed` instead of `saved` for a take that lost
   frames. A file that lost frames can never be reported as finalised.
5. **`RenderCliArgs::endGiven` / `sizeGiven`.** The design had the driver recognise "not given" by
   comparing the settings back against the parser's sentinels, which silently reinterprets a user
   who types one (`--end -1`, `--size 0x100`) as "not given" instead of rejecting it. The flags are
   now the structural signal and the sentinels are purely the parser's internal default.
6. **`Progress` gained `waitingForLoad` and `fps`.** `waitingForLoad` is a structured signal for
   the loader-gate yield, so the CLI sleeps rather than string-matching `status` or busy-spinning;
   `fps` lets a driver compute the real-time multiple without also holding the `RenderSettings`
   (the `--render` CLI can start a job the dialog never configured).
7. **The Recorder latches its offline suppression.** Stopping and saving the interrupted recording
   was not enough: the still-armed `record` toggle restarted it on the first live frame after the
   render, truncating the file just saved.
8. **`--screenshot` opens the Render dialog** (and adds an Output node so it validates), so the
   headless UI capture exercises the new window.
9. Smaller: `renderClock_` pins the whole armed transport and is re-asserted verbatim each frame;
   the two sinks are tracked by **id** and re-resolved every frame; `active()` is derived from the
   phase rather than being a second flag to keep in sync; the capture FBO is cleared before the
   blit unconditionally; the dialog re-seeds when the loaded project changes and seeds Browse from
   the directory already in the field.
