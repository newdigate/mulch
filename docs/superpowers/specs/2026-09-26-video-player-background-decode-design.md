# Video Player — Background Decoding — Design

**Date:** 2026-09-26
**Status:** Design approved in brainstorm; awaiting written-spec review

## Goal

Loading a large video into the **Video Player** node (`src/modules/VideoPlayerNode.{h,cpp}`) makes the
whole UI stop responding. After this change:

- The UI never waits on opening, seeking or decoding a video.
- 4K30 video plays forward at full rate on the development machine (2017 MacBook Pro, i7-7820HQ
  4 cores / 8 threads, 16 GB, Homebrew FFmpeg 9.0.2).
- Reverse and unusual rates keep working. On big files they may skip frames instead of freezing.
- Offline renders stay frame-exact, forward and reverse.

## Decisions (from brainstorm)

- **Target: "no freeze + smooth 4K forward".** Not merely "never freeze" (4K would still stutter),
  and not "smooth 4K reverse" (needs a multi-GB decoded-frame cache or a pre-transcoded proxy).
- **Approach A: a background decode worker per Video Player node**, software decoding with FFmpeg's
  decode threads and threaded colour conversion. Rejected for now:
  - **B, colour conversion on the GPU** (upload YUV planes, convert in a shader). It cuts upload and
    memory by ~60% but adds colour-accuracy work. Not needed for 4K30 on this machine; kept as the
    lever for 10-bit HEVC headroom (see Out of scope).
  - **C, a time-budgeted decoder that stays on the UI thread.** Each 4K frame would still cost
    13–20 ms of UI time and seeks would still hitch, so it cannot meet the goal.
- **No hardware decode.** On this Mac, VideoToolbox decoded 4K at 29–52 fps, against 39–117 fps for
  FFmpeg's threaded software decoder (FFmpeg command line, pure decode). So it gives no wall-clock gain.
- **Memory is a fixed per-node budget** of 512 MB of frame buffers.

## Root cause

Reproduced with a headless harness that compiles the real `VideoPlayerNode.cpp` and drives
`evaluate()` the way `src/main.cpp` does: `dt` = the previous frame's wall time, with a 16.7 ms vsync
floor. It ran against generated 1080p and 4K H.264 clips with keyframes 1 s apart and ~8 s apart (the
x264 default of 250 frames), and was profiled with `sample`. See the appendix to reproduce.

### The mechanism

1. **Decoding runs synchronously on the UI thread.** All of it happens inside `evaluate()`
   (`VideoPlayerNode.h:19`), which `Application::frame` calls from the main loop.
2. **A slow frame makes the next frame slower.**
   - The playhead advances by the uncapped wall-clock `dt` (`main.cpp:290` → `VideoPlayerNode.cpp:40`).
   - When a frame's work exceeds real time, the playhead leaves the window of cached frames.
   - The miss triggers `rebuildCache` (`VideoPlayerNode.cpp:124-131`): clear the cache, seek back to a
     keyframe, decode up to 48 frames. That is the node's most expensive operation, so the next `dt`
     is larger and the next frame misses again.
   - There is no drop-frames-and-catch-up path, so it never recovers.
3. **A bug makes it permanent for common files.** `rebuildCache` stores every frame from the keyframe
   onward and stops at `kMaxFrames` = 48 (`VideoPlayerNode.cpp:154-161`). When keyframes are more than
   48 frames apart, the rebuilt cache ends *before* the playhead. The picture freezes and every frame
   does a full rebuild.

### Amplifiers (measured)

- **Debug builds** (`build.sh` configures Debug).
  - At `-O0`, libc++ destroys a `std::vector<uint8_t>` one element at a time through un-inlined calls.
  - Freeing one cached frame costs ~75 ms at 1080p and ~0.3 s at 4K.
  - 95% of main-thread samples were in `frames_.clear()` (`VideoPlayerNode.cpp:146`).
- **Single-threaded decoding.** `VideoDecoder` never sets `thread_count` (`VideoDecoder.cpp:49`), so
  libavcodec uses one thread. 4K H.264 decode + RGBA conversion takes 44 ms per frame; with automatic
  threads it takes 10.5 ms.
- **Single-threaded colour conversion.** 10-bit HEVC 4K → RGBA takes 25–34 ms per frame; threaded,
  8–11 ms.
- **Copies and memory.**
  - Each frame is copied again into a newly allocated buffer: 20–28 ms at 4K.
  - The upload then costs 6–8 ms.
  - The cache holds 48–96 frames, which reached 1.7 GB of resident memory at 4K.

### Observed behaviour

| Build | 1080p | 4K |
|---|---|---|
| Debug | 0.5–3.4 fps, frames up to 4.4 s | worse |
| Release | ~36 fps, recurring 90–150 ms hitches | 0.4–0.5 fps, frames up to 3.9 s, picture frozen |
| Release, plus one 600 ms hitch | keyframes 8 s apart: stuck at ~1 fps; 1 s apart: ~4.5 s to recover | — |

A Release build is therefore not a mitigation: one ordinary hitch triggers the stuck state. Examples
are a native file dialog, or a projectM preset switch (176–547 ms).

## Measurements that size the fix

**Worker loop at 4K.** Automatic decode threads plus one threaded conversion per frame into a reused
buffer:

| Clip | Decode + convert | Decode only (catch-up path) |
|---|---|---|
| H.264, keyframes 1 s / 8 s apart | 81 / 93 fps | 126 / 125 fps |
| HEVC 8-bit | 49 fps | 49 fps |
| HEVC 10-bit | 36 fps | 48 fps |
| 1080p H.264 | 374 fps | 473 fps |

Every tested format sustains 4K30 at 1× speed. Headroom ranges from 1.2× (10-bit HEVC) to 3× (H.264).
At 2× speed, 4K HEVC will skip frames.

**Threaded conversion must write top-down.**
- The portable threaded path is `sws_alloc_context` + options including `threads=0`, then
  `sws_init_context`, then `sws_scale_frame`. It works on FFmpeg ≥ 5.
  - CI builds against FFmpeg 6.1 (Ubuntu 24.04), Homebrew latest, and vcpkg latest.
  - The project already requires FFmpeg ≥ 5.1 (`ch_layout`, `swr_alloc_set_opts2`).
- This path rejects a negative-stride (bottom-up) destination with `EINVAL`. Only FFmpeg 8+'s dynamic
  mode accepts one.
- So the worker converts top-down, and the node flips vertically on the GPU when it uploads.

## Design

### Terms

| Term | Meaning |
|---|---|
| D | The clip duration from the container (0 = unknown). |
| u | The *unwrapped* playhead: lap × D + position. It keeps counting past the end instead of jumping back to 0. |
| frame time | A decoded frame's unwrapped time: lap × D + its timestamp. |
| the frame for u | The decoded frame with the greatest frame time ≤ u. This is the rule the node uses today (`nearestFrame`). |
| pool | A fixed set of reusable RGBA buffers, allocated once per opened file. |
| ready queue | Converted frames waiting to be shown, sorted by frame time. |
| checked out | The buffer the node is currently uploading from. The worker cannot reuse it until the next `frameAt()`. |
| stretch | In reverse playback: a keyframe, the frames after it up to a needed frame, decoded forward in one pass. |

### Units

**`core/VideoPlan.h`** — new, header-only, no GL or FFmpeg code. All playback *decisions* as pure
functions, unit-tested in `core_tests` (the `StepSync.h` / `BarSync.h` pattern). The API below is
indicative; the implementation plan fixes it.
- `nextStep(PlanInput) → Step`: one of `Wait`, `Fill`, `CatchUp{to}`, `Seek{to}`, `Wrap`,
  `ReverseStretch{end, stride, keep, contiguous}`. It is computed from:
  - the target, direction, loop and offline flags;
  - D and the nominal frame duration;
  - the decoder head (time of the next frame) and whether it has reached the end of file;
  - the next keyframe after the head, when known;
  - the ready-queue range, and the free and total buffer counts.
- `selectFrame(times, n, u) → index or -1`: the frame for u.
- `planStretch(keyTime, end, frameDur, budget, offline) → Stretch`.
- `poolFrames(budgetBytes, w, h)` = clamp(budget / (w·h·4), 4, 64).
  With 512 MB that gives 16 at 4K, 64 at 1080p and 4 at 8K.
- `lapStart(u, D)` = D·floor(u/D), and `wrapped(u, D)` = u − lapStart(u, D).

**`core/TimedAudio.h`** — new, header-only, no GL or FFmpeg code. A time-tagged audio store at
48 kHz mono.
- It holds contiguous chunks, each a start time u plus samples.
- `beginChunk()` makes the next append start a new chunk; seeks and loop wraps use it.
- `append(startU, samples, n)`.
- `dropBefore(u)` / `dropAfter(u)` for retention.
- `covers(u0, u1)`.
- `sample(u0, u1, out, n)` maps output sample j to time u0 + (u1 − u0)·j/n and interpolates linearly,
  exactly like today's `emitAudio`.
  - Times not covered by any chunk produce silence.
  - Where chunks overlap at a loop boundary, the later-starting chunk wins.
- Unit-tested in `core_tests`.

**`gfx/VideoStream.{h,cpp}`** — new, no GL code, owns the worker thread.

- **Owns:** a `VideoDecoder`, the worker `std::thread`, the pool (`std::unique_ptr<uint8_t[]>`
  buffers, never allocated or freed per frame), the ready queue, a `TimedAudio`, and status/info.
- **Lifecycle:** the constructor starts the worker, which opens the file. The destructor sets a stop
  flag, wakes the worker and joins it (the `MidiSyncEngine` pattern).
- **API for the graph thread:**

  | Call | Purpose |
  |---|---|
  | `state()` | `Opening` / `Ready` / `Failed`, with an error string |
  | `info()` | width, height, D, frame duration, has-audio |
  | `request(u, rate, loop, offline)` | the latest request wins; wakes the worker |
  | `frameAt(u, FrameView&)` | the frame for u as `{rgba, stride, t, serial}`, valid until the next call |
  | `frameReadyFor(u)` | true when the frame for u is decided and the audio up to u is decoded (or end of file / no audio) |
  | `waitForFrame(u, timeout)` | offline only |
  | `readAudio(u0, u1, out, n)` | samples the audio store |

- **Locking:** one mutex plus a condition variable guard the request, pool, queue and audio store.
  They are held only for bookkeeping, never while decoding, converting or uploading.

**`gfx/VideoDecoder`** — existing, small changes.
- `open(path, err, const std::atomic<bool>* abort = nullptr)`:
  - sets `thread_count = 0` (automatic) before `avcodec_open2`;
  - allocates the format context first so it can install an `AVIOInterruptCB` that returns
    `*abort`. A stop then aborts a stalled open or read.
- `decodeNext(DecodedFrame& out, audio…)`: decode the next video frame *without* converting it.
  - `DecodedFrame` is a move-only handle that owns a reference to the decoded picture, plus its time.
  - The FFmpeg type is forward-declared, so FFmpeg headers stay out of `VideoDecoder.h`.
  - Audio is appended exactly as `decodeFrame` does today.
  - Handles are cheap reference counts, so the worker can hold the previous and the next frame at once,
    which the catch-up rule needs.
- `convert(const DecodedFrame&, uint8_t* dst, int stride)`: threaded, top-down conversion through the
  portable path above. The conversion context is created lazily from the first frame's format.
- `frameDuration()`: taken from `avg_frame_rate`, then `r_frame_rate`, falling back to 1/30 s.
- `nextKeyframeAfter(t, double& key)`: looked up through `avformat_index_get_entry_from_timestamp` /
  `avformat_index_get_entry`. Returns false when the stream has no index.
- `decodeFrame()` keeps its current behaviour, including single-threaded bottom-up output, for the
  15 `gl_smoke` call sites.

**`modules/VideoPlayerNode`** — existing, simplified.
- **Kept:** the ports, the `rate`/`play`/`loop` semantics, the status line, and `playhead()`, which
  returns the wrapped position.
- **Removed:** `frames_`, `rebuildCache`, `extendForward`, `trimFront`, `nearestFrame`, `audio_` and
  the related bookkeeping.
- **Added:**
  - a `std::unique_ptr<VideoStream>`;
  - the unwrapped playhead u and the previous u;
  - a staging texture (top-down uploads) plus the output texture (bottom-up, published);
  - two framebuffers for the flip blit;
  - the last shown serial;
  - offline bookkeeping: the pending next u and a stall flag.
- **Overrides `loading()`** — see Offline renders.

**`gfx/VideoEncoder`** — test support only. `open(…, std::string& err, int keyframeInterval = 0)`,
where 0 keeps today's one keyframe per second (`gop_size = fps`). `gl_smoke` uses it to write a clip
with widely spaced keyframes.

### Time model

- Each frame, u advances by rate·dt while `play` is on.
- **Loop on:** u runs freely, including below 0 in reverse. The displayed position is `wrapped(u, D)`.
- **Loop off:** u is clamped to the lap it is in, [L·D, (L+1)·D], with L fixed when loop was switched
  off (0 initially). The displayed position is u − L·D, so the end holds the last frame.
- **D = 0 (unknown duration):** today's behaviour is kept. u is clamped at ≥ 0, there are no laps, and
  the worker holds the last frame at end of file.
- The worker tags every frame and audio chunk with unwrapped times. While looping it does not queue
  frames whose timestamp is ≥ D, because they would overlap the next lap.

### Per-frame flow on the UI thread (live)

1. **Path changed:** destroy the old stream (a bounded join), create a new one, and set u = 0.
   Until the new file's first frame is uploaded the output is `TexRef{0}`, as today.
2. **Opening or failed:** publish an empty texture and silence, and set the status to "opening…" or
   "load failed: …".
3. **First `Ready`:** allocate the staging and output textures at the video size, plus the flip
   framebuffers.
4. Advance u as in the Time model, then call `request(u, play ? rate : 0, loop, ctx.offline)`.
5. Call `frameAt(u)`. If the serial changed:
   - upload to the staging texture with `glTexSubImage2D` (top-down rows);
   - flip into the output texture with one `glBlitFramebuffer` (source rows 0→H, destination H→0),
     inside a `GLStateGuard`.
6. Publish the output texture.
7. **Audio:** n = `audioBlockFrames(48000, dt)`.
   - Paused, or u unchanged: silence, as today.
   - Otherwise: `readAudio(uPrev, u, out, n)`.
   - A loop wrap is continuous in u, so today's one silent block per wrap goes away.
8. **Status:** position / duration × rate, plus "(buffering)" when the shown frame's time is more than
   0.2 s from u.

The UI thread never waits in live mode. When no newer frame is ready, the previous picture stays up.

### Worker: forward playback

**Direction.** The direction is the sign of the requested rate. A rate of 0 (paused) keeps the
previous direction, so pausing never re-plans or flushes anything; the worker just stops being asked
to move.

The rules below are checked in order after each snapshot of the request.

1. **Target behind everything we have** (before the first queued frame, or before the decoder head
   with an empty queue): `Seek{target}`.
2. **Target ahead of the head by more than 2 frame durations:**
   - If the next keyframe after the head is known and is ≤ the target: `Seek{target}`, because
     jumping is cheaper than decoding through.
   - If there is no keyframe index and the target is more than 2 s ahead: `Seek{target}`.
   - Otherwise: `CatchUp{target}`.
3. **End of file:** `Wrap` when loop is on and D > 0 (seek to the start of the next lap, and call
   `beginChunk()` for the audio). Otherwise `Wait`, holding the last frame.
4. **A free buffer exists:** `Fill` — decode, convert into the buffer, queue.
5. Otherwise `Wait` until the request changes, a buffer is freed, or stop is set.

**Catch-up and seeks are exact.**
- While catching up, the worker keeps the latest decoded frame unconverted.
- When the *next* decoded frame is still ≤ the target, it discards the held one without converting it.
- The first time the next frame is past the target, the held frame (the frame for the target) is
  converted and queued.
- This is exact for variable-frame-rate files too, and it is the same step used after a seek. So only
  the frame for the target is converted, never the frames between the keyframe and the target.

**The worker recycles frames that can never be shown, itself.** That covers:
- *superseded* frames: those older than the newest queued frame that is ≤ the target in forward
  playback (newer, in reverse);
- when loop is off, frames outside the current lap. The request carries that lap's bounds, [L·D,
  (L+1)·D].

The pool therefore cannot deadlock on frames that will never be shown.

### Worker: reverse playback

1. **Plan a stretch ending at E** (initially the target). Seek to E; the first decoded frame gives the
   keyframe time K. The estimated frame count is n = round((E − K) / frameDur) + 1.
2. **Live:** the budget is M = pool / 2 frames.
   - If n ≤ M, keep every frame in [K, E].
   - Otherwise keep every s-th frame counting back from E, s = ceil(n / M), so E itself is always kept
     and the whole stretch stays covered.
   - Frames that are not kept are decoded but not converted.
   - This spacing uses the nominal frame duration; live reverse is best-effort.
3. **Offline:** every frame in [K, E] is converted into a rolling ring of M buffers. The ring ends
   holding exactly the last M frames ≤ E. The next stretch ends just before the earliest one kept,
   which re-decodes from the same keyframe. That is slower, but frame-exact.
4. **Next stretch:** E′ = K − ε (live), which lands on the previous keyframe. Crossing below the start
   of a lap goes to the previous lap's end when looping, and holds the first frame otherwise.
5. **Prefetch:** the next stretch starts once ≥ M buffers are free.

**Expected live reverse quality with the 512 MB budget:**

| Resolution | Keyframes 1 s apart | Keyframes 8 s apart |
|---|---|---|
| 4K | every 4th frame (~8 fps) | ~1 fps |
| 1080p | smooth | every 8th frame |

**Switching direction** flushes the ready queue (except the checked-out frame) and re-plans. The
picture holds for about 0.1–1.5 s while the first stretch decodes, or while the forward seek lands;
the UI keeps running.

### Audio

- The worker appends the decoder's 48 kHz mono audio to the `TimedAudio` store as it decodes,
  anchoring each new chunk from the first audio timestamp (today's `audioStartT` rule) plus the lap
  offset.
- Seeks and wraps call `beginChunk()`.
- Audio decoded during catch-up and during reverse stretches is kept, so reverse still sweeps
  backwards, as today.
- **Retention:** live forward drops audio more than 2 s behind u; reverse drops audio more than 2 s
  ahead of it. A 30 s total cap is a safety net.
- Anything not yet decoded reads as silence.

### Offline renders

- The node records the last `ctx.offline`.
- Two things count as `loading()`:
  - the file is still opening;
  - in offline mode, `!frameReadyFor(pendingNextU)`.
- **Predicting the next frame:** after publishing frame k at u, the node computes the next
  u′ = u + (play ? rate : 0) · dt, with the same loop and clamp rules. Offline `dt` is the fixed
  1/fps, so this is the next frame's position unless the rate changes. It sets the worker's request
  to u′ and stores it as `pendingNextU`. The renderer's existing gate (`OfflineRenderer::step` yields
  while any node is `loading()`) then waits between frames without blocking the UI.
- **Exactness:** at the next `evaluate()`, when the actual u equals u′ the frame is already ready.
  When it doesn't (for example, the rate is automated), the node calls `waitForFrame(u, 2 s)` itself.
  Either way the frame and audio for u are published in the same `evaluate()`, which is the
  `Node::loading()` contract.
- **Stalls:** if that wait times out, the node latches a stall flag that keeps `loading()` true. The
  render then fails through the renderer's normal 30 s timeout naming the node, instead of finishing
  with a wrong frame. The flag clears when `ctx.offline` goes false or the file changes.
- **Reverse:** the offline stretch rule above makes reverse renders frame-exact.

### Errors

| Case | Handling |
|---|---|
| Open fails (missing file, no video stream, unsupported codec) | `Failed` with the reason; the node shows "load failed: …" as today, publishes black and silence; the worker exits |
| Frame buffers can't be allocated (`std::bad_alloc`) | `Failed`: "not enough memory for W×H frames" |
| Corrupt packets | skipped |
| Truncated file | behaves like end of file (wrap or hold) |
| `av_seek_frame` fails | seek to 0 and catch up to the target: slow but correct |
| Frame dimensions change mid-file | those frames are skipped (conversion is sized at open) |
| Anything thrown on the worker | caught at the top of the thread and turned into `Failed`; nothing escapes the thread |

### Shutdown and lifetime

- The stream is destroyed when the file changes, the node is deleted, `Graph::clear()` runs (project
  load) or the app exits: set stop, wake, join.
- The join is bounded by about one frame's decode (tens of ms at 4K). The worker checks stop between
  steps, and the interrupt callback aborts blocking I/O on slow or network drives.
- The decoder is destroyed after the join.
- The node frees its textures and framebuffers on the main thread with the editor context current (the
  existing rule), after the stream has been destroyed.
- The worker never touches GL. Framebuffers are created and used only in the editor context, where
  `evaluate()` runs; they are not shared between contexts, and that is fine.

### Memory

- 512 MB of frame buffers per node (`kVideoPoolBytes`, a named constant), plus one staging texture of
  VRAM, plus a few MB of audio.
- Today's cache reached 1.7 GB at 4K.

## Testing

### `core_tests` — new `tests/test_video_plan.cpp` and `tests/test_timed_audio.cpp`

- **`selectFrame`:** the newest frame ≤ u, exact boundaries, none ready, across a lap boundary.
- **`nextStep` forward:**
  - fill when a buffer is free; wait when full;
  - catch up when behind; seek when the next keyframe is ≤ the target;
  - with no index, seek only beyond 2 s;
  - target behind the queue → seek;
  - end of file with loop → `Wrap`; without loop → `Wait`.
- **Direction switch:** flush and re-plan.
- **`planStretch`:**
  - all frames when n ≤ M;
  - every s-th otherwise, always including E, with no gap larger than s frames;
  - contiguous offline.
- **`poolFrames`:** 16 / 64 / 4 at 4K / 1080p / 8K.
- **Lap mapping:** `lapStart` / `wrapped` for negative u, and at exact multiples of D.
- **`TimedAudio`:**
  - sampling inside a chunk matches today's `emitAudio` mapping;
  - silence where uncovered;
  - the later chunk wins at a loop boundary;
  - a reverse sweep reads backwards;
  - `covers` and retention.

### `gl_smoke`

- The existing Video Player scenario (`tests/assets/test.mp4`: picture, audio, reverse) keeps passing.
  It runs with the graph in offline mode, so frames are exact and synchronous; one live-mode check
  polls until the texture has colour.
- The existing encoder round-trip checks keep `decodeFrame()`'s bottom-up output honest.
- **A new generated clip,** `build/_video_longgop.mp4`:
  - 160×90, 25 fps, 12 s (300 frames), keyframes 250 frames apart, with a 440 Hz tone;
  - written with `VideoEncoder`'s new keyframe interval;
  - each frame paints its index in flat grey bands, so a texture read-back identifies the frame on
    screen (bands are robust to H.264 loss).

  The tests using it:
  - **Lock-up regression:** a simulated 3 s hitch lands between keyframes. `evaluate()` must return in
    < 50 ms, and within 1 s of wall time the frame on screen must match the playhead (±1 frame).
  - **Offline exactness:** through the real `OfflineRenderer` — forward play, a jump between
    keyframes, and reverse — the output frame sequence matches the expected indices (read back with
    `VideoDecoder`).
  - **Seamless loop** (offline): the frame after the last is frame 0, and no audio block across the
    loop boundary is silent.
  - **Shutdown:** changing the file, or deleting the node, mid-seek returns within 200 ms with no hang
    or crash.
- Timing limits are deliberately loose so slower CI machines pass. The old design misses them by
  seconds.

## Acceptance criteria

Measured on the development machine, in Debug and Release, with the investigation harness and clips
(see the appendix):

1. 4K H.264 and 4K 8-bit HEVC play forward at 1× with the picture within one frame of the playhead in
   steady state.
2. 4K 10-bit HEVC does the same on an otherwise idle machine (36 fps capacity against 30 needed).
3. Once a file is open, no `evaluate()` exceeds 25 ms at 4K, and the mean is < 10 ms.
4. After a single 600 ms hitch, 1080p with keyframes 8 s apart is back in step within 0.5 s.
5. The process stays within the 512 MB frame budget plus modest overhead at 4K.
6. `ctest` passes on all three CI platforms.
7. A local ThreadSanitizer build of the new tests reports no data races in project code.
8. CLAUDE.md's Video Player and `VideoDecoder` notes describe the worker design. They currently
   describe synchronous decoding and the sliding keyframe window.

## Out of scope and future work

- **GPU colour conversion (option B).** The next lever if 10-bit HEVC 4K headroom proves too thin: it
  moves that clip from 36 fps toward its 48 fps decode-only ceiling, and cuts upload and memory by ~60%.
- Hardware decode (VideoToolbox / D3D11VA / VAAPI).
- Asynchronous upload through pixel buffer objects.
- Smooth reverse at 4K with widely spaced keyframes.
- Transport sync for the Video Player.
- The swept audio after a large hitch: an existing behaviour, unchanged.
- Local build configuration. `build.sh` builds Debug; this design removes the Debug-only
  per-byte-free cost from the video path, and switching local builds to RelWithDebInfo is a separate
  choice.

## Risks

- **Thread count.** Each node runs a worker plus FFmpeg decode and conversion threads, so several
  Video Players mean many threads. This is acceptable; `thread_count` can be capped later if needed.
- **Thin 10-bit HEVC 4K headroom** on this machine (1.2×). Heavy concurrent CPU use will drop frames.
  They are dropped, not frozen.
- **Container duration** can differ slightly from the stream's real end. Frames at or beyond D are not
  queued while looping, and the later chunk wins at audio seams.

## Appendix: reproducing the measurements

Test clips (12 s each; 4K clips are ~60 MB):

```bash
gen() { ffmpeg -y -f lavfi -i "testsrc2=size=$2:rate=30" -f lavfi -i "sine=frequency=330:sample_rate=48000" \
  -t 12 -vf "noise=alls=10:allf=t" -c:v libx264 -preset veryfast $3 -b:v $4 -maxrate $4 -bufsize $4 \
  -pix_fmt yuv420p -c:a aac -ac 2 -movflags +faststart "$1.mp4"; }
gen v4k_gop250 3840x2160 "" 40M;       gen v4k_gop30 3840x2160 "-g 30" 40M
gen v1080_gop250 1920x1080 "" 12M;     gen v1080_gop30 1920x1080 "-g 30" 12M
ffmpeg -y -f lavfi -i "testsrc2=size=3840x2160:rate=30" -t 6 -vf "noise=alls=10:allf=t,format=nv12" \
  -c:v hevc_videotoolbox -b:v 40M -tag:v hvc1 v4k_hevc8.mp4
ffmpeg -y -f lavfi -i "testsrc2=size=3840x2160:rate=30" -t 6 -vf "noise=alls=10:allf=t,format=p010le" \
  -c:v hevc_videotoolbox -profile:v main10 -b:v 40M -tag:v hvc1 v4k_hevc10.mp4
```

The harness compiles `src/modules/VideoPlayerNode.cpp` into its own translation unit with `private`
opened up, for inspection only. It creates a hidden GL 4.1 context. Each loop it calls `evaluate()`
with `dt` = the previous iteration's wall time, with a 16.7 ms floor, and logs:
- the time `evaluate()` took, including a `glFinish` for the upload;
- the playhead;
- the window range and whether it covers the playhead;
- resident memory.

An optional argument injects one artificial stall to test recovery. Build it twice, with the app's
Debug flags and with `-O2`.
