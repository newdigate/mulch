# Video Player — Background Decoding — Design

**Date:** 2026-09-26
**Status:** Approved; revised during planning after a working prototype (see *Revisions during planning*)

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

## Revisions during planning

The plan was written against a working prototype, built and measured on the development machine
(unit tests, `gl_smoke`, and a ThreadSanitizer build of `gl_smoke` with zero reports). Building it
found these problems in the design as first approved, and changed it as follows:

1. **Audio reads ahead of the video.** Audio used to be decoded only as far as video had been
   demuxed. With a small frame pool (16 frames ≈ 0.5 s at 4K, 4 at 8K), a file that interleaves audio
   in ~1 s chunks would get silence gaps live, and offline renders would stall waiting for audio.
   The decoder now queues *compressed* video packets (bounded at 64 MB) so `pumpAudio()` can keep the
   audio 1 s ahead whatever the pool size. Offline audio readiness uses the decoder's "settled up to"
   watermark, so `TimedAudio::covers()` is dropped.
2. **The playhead holds at the start until the first frame is on screen.** Opening and decoder
   warm-up take ~0.4 s at 4K; running the clock meanwhile skipped the first 12–15 frames (this is what
   made 10-bit HEVC look unreliable).
3. **Live catch-up is sliced (100 ms).** At 2× on 4K HEVC the decoder (≤ 49 fps) cannot keep up with
   the playhead (60 fps), and chasing it froze the picture. Now it shows the best frame reached every
   100 ms and re-plans.
4. **Seeks only pay off for jumps over 1 s.** A seek restarts FFmpeg's frame-threading pipeline
   (~0.3–0.4 s at 4K HEVC), so shorter gaps are decoded through. The start of the next lap counts as a
   keyframe. A seek keeps the newest frame at or before its target on screen until the seek delivers a
   better one (flushing it had left the picture with nothing new to show). A seek that lands late
   retries 1 s earlier, and one that still lands late -- the target precedes the file's first frame --
   is "pinned" so it is not repeated.
5. **Reverse stretches are published together when complete.** They decode forwards, so publishing
   frames one by one showed a stretch's *earliest* frame first and then played it forwards.
6. **Stride counting starts from the frame containing a stretch's end** (the end is a boundary, not a
   frame time), and a frame at or past D ends the lap.
7. **The node exposes `hasFrame()`**: an unwrapped frame time can be negative in looping reverse, so
   `shownFrameTime()` cannot double as "no frame" (−1).
8. **The time model (`videoAdvance`, `videoPosition`) lives in `core/VideoPlan.h`**, so it is unit
   tested; it pins the loop-off lap from the position *before* the step (the first version pinned the
   next lap when loop went off on the frame that crossed the end).
9. **Acceptance criteria use measured numbers** (see below). Memory in particular is 0.6 GB at 1080p
   and 0.9–1.15 GB at 4K -- half of today's 1.7 GB, but not "512 MB plus a little": FFmpeg's
   frame-thread buffers, the packet queue and the GL textures add ~0.4 GB at 4K.

### Revisions during execution (code review of Task 1)

10. **Loop off stops just short of the lap's end** (lapLo + D − 2·10⁻⁶). The next lap's first frame,
    decoded early while looping, sits at exactly lapLo + D, and "the frame for u" picked it: turning
    loop off at the end showed the clip's first frame instead of its last.
11. **Offline renders snap `dt` to the exact frame step** (`videoFrameStep`). The renderer passes
    `1.0f / fps`; accumulating that float drifted off the frame grid by ~9·10⁻¹⁰ s a frame, past the
    10⁻⁶ tolerance after ~45 s at 25 fps -- one duplicated frame, then every frame one late.
12. **With loop off and the decoder at or past the end of the playhead's lap, the worker waits.** The
    planner gets the lap's end (`lapHi`); before, it decoded and converted the rest of that lap only for
    each frame to be recycled. "At" matters: just after a wrap the head sits exactly on `lapHi`, and a
    frame there is the next lap's first, so recycling drops frames at or past `lapHi` too.
13. **`videoSelectFrame` is the one frame-choice rule**, now a template over a time accessor, and the
    worker uses it instead of three hand-written loops, so its unit tests cover the code that runs.

### Revisions during execution (code review of Task 2)

14. **Live stride counts round onto the keyframe's grid.** Counting back from a boundary by flooring
    assumed exact frame times; containers that store rounded ones (MKV/WebM milliseconds, QuickTime
    1/600) made counts repeat and skip, and at 60 fps with 2 s keyframes on a 4K pool (stride 15) every
    stretch kept nothing -- live reverse froze. `VideoStretch` now carries `top`, the nominal time of
    the frame containing its end, and counts round from there.
15. **A prefetch stretch lies strictly below the stretch above.** Its end used to sit 1 µs below the
    keyframe above while the worker admitted frames up to end + ε, so that keyframe came back: a
    duplicate live, a lost ring slot offline. Live strides are also anchored half a frame below it, so
    rounded timestamps count from the right frame.
16. **A live stretch never ends empty**: if the stride kept nothing, it keeps its newest frame, so the
    worker cannot run on prefetching empty stretches.
17. **Fresh reverse stretches aim where the playhead will be.** They used to end at the playhead's
    position when planned; at 4K a stretch takes about as long to decode as to play, so it often landed
    behind the playhead, which restarted it -- flushing everything -- over and over (live 4K reverse
    stuck in a third of runs). A live fresh stretch now ends |rate| × the last stretch's decode time
    below the playhead (capped at 2 s, never before the clip's start with loop off), and a playhead
    still above the covered stretch is left to arrive instead of being restarted.

### Revisions during execution (second code review of Task 2: the planned Tasks 3, 5 and 7)

The reviewer drove the planned worker against files written in six containers. Each finding below was
reproduced on the prototype before it was fixed, and each fix was measured on every container.

18. **Offline, a stretch whose ring evicted frames covers only down to its oldest.** A stretch whose
    keyframe is the lap's first frame claimed coverage down to the lap start even when its ring of M
    frames had evicted everything below the newest M. Through a first keyframe interval longer than the
    ring, a loop-off render stalled, and a looping one showed the previous lap's frames (180 of 206
    wrong on a 1080p clip whose first keyframe interval is 8.3 s).
19. **A seek lands where its stretch admits frames.** The landing check allowed ε above the target
    while a prefetch admits frames only up to 1 µs below the stretch above, so the two cancelled. Where
    the index holds decode times -- FLV, fragmented MP4, B-frames without an edit list -- a seek just
    below a keyframe lands ON it. That landing was accepted, the stretch decoded nothing, and the same
    prefetch was planned again: 2,400–2,600 stretches in 3.6 s of live reverse instead of 6.
20. **A seek that lands late, or finds nothing, backs off 1 s, 2 s, 4 s… to the start of the file.**
    FLV and MPEG-TS find no frame when asked for the last ones, and an MPEG-TS timestamp search
    overshoots by a keyframe interval. Taken as "nothing decodable", the lap counted as covered and
    reverse raced through laps with the picture frozen (30,000 stretches); with a single retry 1 s back,
    an MPEG-TS file with 3 s keyframes still stalled on its first reverse frame. "Pinned" now means even
    the start of the file lands after the target.
21. **A stopped reverse playhead above the covered stretch restarts there.** Revision 17 leaves a
    playhead above a led stretch to arrive, but a paused one never does: it stayed a lead behind (4
    frames on 1080p). With no lead (paused, or offline), a playhead above the top of the run's first
    stretch (`coverHi`) starts a fresh stretch at itself.
22. **Decoder times count from the first video frame** (decoded by `open()`, handed out by the first
    `decodeNext()`); the keyframe index, `seek()`, the duration and the audio follow. A container that
    starts its clock late (MPEG-TS, 1.47 s in) or shows a B-frame delay without an edit list (FLV,
    fragmented MP4: 1–2 frames) put the first frame after the playhead's 0, so even a forward offline
    render stalled on its first frame. `seek(t ≤ 0)` goes to the very start of the file, which a
    timestamp search cannot overshoot; audio from before the first frame is dropped. (An FLV's duration
    counts from 0, so its laps end a B-frame delay late: the last frame is held a frame or two longer,
    never lost.)
23. **`VideoStream::reverseStretches()`** counts reverse stretches, so a test can prove live reverse
    does not spin. The new `gl_smoke` scenario writes the awkward files itself (see Testing).
24. **Task 2's anchor test admits frames the way the worker does**: strictly below the stretch above,
    not up to the anchor.

### Revisions during execution (code review of Task 3)

The reviewer drove the committed planner through the prototype worker; every finding reproduced, and
one more (31) turned up while writing the tests for them.

25. **With loop off, a decoder left a lap behind the playhead seeks into the playhead's lap.** Loop
    turned off after the playhead crossed into the next lap but before the decoder wrapped (a decoder
    slower than the playhead, or a hitch) pinned the playhead's lap, and the worker waited at the end of
    its own lap for ever: the picture froze, and offline the earlier lap's frames were reported ready.
    The planner now gets the decoder's lap end (`lapEnd`) whether looping or not, and a run ends at the
    lap's end at the end of the file instead of never.
26. **A seek ahead that landed at or behind the decoder is not repeated nearby.** Without an index --
    or with MPEG-TS's, which lists its seek probes as keyframes -- a seek ahead can land on a keyframe
    far behind; with 10 s keyframes the worker re-sought every 100 ms slice and never caught up (3 runs
    in 5). After such a landing the planner catches up until the target has moved on by the gap the
    seek revealed (`videoNoSeekBelow`, at least `kVideoSeekNoIndex`).
27. **Loop off keeps the frames past the playhead's lap; what reverse covered below it is dropped with
    its frames.** Recycling them left a hole the planner could not see: loop back on held the last
    frame for 2.45 s (forward), or showed frames 0.8 s off (reverse).
28. **`videoLapStart` is exact**: a lap start maps to itself. The division alone came out a lap low for
    3–4% of lap starts (1.4 × 3 / 1.4 < 3), and a loop-off reverse run clamped to its lap's start
    decoded the lap below.
29. **A moving reverse playhead far above what is covered restarts** (more than `kVideoRestartLeads` = 3
    leads plus a frame): after the rate dropped from −2 to −0.1 mid-stretch, the picture sat 0.9 s off
    for seconds. Three leads keep stretch-time jitter from restarting anything.
30. **`open()` seeks to the start before decoding the first frame.** Matroska and WebM (and FLV) read
    their keyframe index only when first asked to seek, so the worker could not seek ahead in the first
    lap. NUT still exposes no index; like MPEG-TS it then catches up rather than seek blind.
31. **An offline render that starts in reverse restarts the run.** Live stretches keep every stride-th
    frame, and nothing marked that: 37–44 of the first 50 offline frames after live reverse were wrong.
    The switch now plans a fresh, whole stretch, and reverse readiness requires a run built offline,
    which also closes the window before the worker sees the first offline request.
32. **The planner's contract is written down**: not looping (loop off, or the duration unknown), the
    lap must start at a finite time; a `+inf` next keyframe means none known, not none; the stretch
    budget is one function (`videoStretchBudget`) shared with the worker.
33. **Tests**: exact expectations and consistent fixtures throughout; the seek thresholds and the
    pinned target at their boundaries; rule precedence at the end of a lap; a `gl_smoke` scenario for
    loop toggles, the Matroska-cues check, and an offline-after-live-reverse check; an MPEG-TS jump in
    the acceptance steps.

### Revisions during execution (second code review of Task 3)

34. **An index keyframe counts as between the decoder and the target only when it lies three frames
    (`kVideoIndexLead`) before the target.** Indexes list B-frame streams' keyframes by decode time --
    MP4 and FLV from x264 list the keyframe shown at 20.0 at 19.92 -- so a seek decided with the target
    inside that window landed a whole keyframe interval back, and the seek guard (26) then held off the
    corrective seek: recovery took 0.39–0.62 s instead of 0.13–0.23 s.
35. **MPEG-TS and MPEG-PS indexes are ignored** (`nextKeyframeAfter` reports none known): their
    demuxers add every packet they read while searching for a seek position to the index, flagged as a
    keyframe. With 10 s keyframes the guard's window could not outrun those phantoms: the acceptance
    step's own clip took 1.6–2.4 s in 10 runs of 10, each with a seek landing 8 s back. Catching up
    instead: 0.04–0.25 s, no seeks.
36. **A wrap forgets what the last seek taught** (`noSeekBelow`): that was about the old lap.
37. **Tests**: a B-frame index window case; the reverse direction change checked in full; the
    MPEG-TS probe check in `gl_smoke`; the acceptance step's bound is 0.5 s.

### Revisions during execution (third code review of Task 3)

38. **An index keyframe counts eight frames later than listed, and the lap start stays exact.** Three
    frames covered x264 (2 frames early) but not x265, whose open GOPs list keyframes 4–6 frames early
    by decode time: recovery from a jump just below a keyframe still took 0.8–1.3 s instead of about
    0.27 s. The lead is now added to the index entry itself (`kVideoIndexLead` = 8), so the next lap's
    start -- an exact presentation time -- no longer waits out the margin.
39. **The MPEG-TS acceptance bound is 1 s**: 33 runs had a 0.055 s median but a 0.44 s worst case,
    and trusting the index took 1.6–2.4 s.

### Revisions during execution (code review of Task 4)

The reviewer drove the store through the prototype worker and node with a tone clip in five
containers, scanning the published audio for gaps and clicks; each finding reproduced before it was
fixed.

40. **Where audio chunks overlap, the most recently begun one wins** (it was the later-starting one).
    In reverse the stretch above resumes its audio up to ~0.1 s before its keyframe, out of a decoder
    that was just flushed -- a fade-in -- and "later-starting wins" played that instead of the
    unbroken audio of the stretch below: a dip and a click at every stretch seam (4–8 per 6 s). At loop
    seams and seeks the newest chunk also starts later, so nothing changes there.
41. **Chunks tile.** A chunk covers its last sample's interval (holding that sample), and a reverse
    stretch keeps the samples that *start* before the stretch above (it kept those that ended before
    it). Every MP4 loop wrap played one silent sample, and some reverse seams dropped samples.
42. **The cap is 180 s and drops the chunk farthest from the playhead** (it was 30 s, oldest first --
    and in reverse the oldest chunk is the one being played). With keyframes more than ~15 s apart,
    reverse played seconds of silence, and an offline render finished with the gaps in it. Reverse
    `retain()` also trims what the playhead has passed from the backs of chunks.
43. **`sample()` finds the chunks a span touches once per call** (it scanned every chunk for every
    output sample: ~1 ms a frame in a Debug build with ~95 chunks held).
44. **Tests**: adjacent chunks tiling, the newest chunk winning in reverse, `clipHi` at sample starts,
    reverse trimming, the cap sparing the playhead's chunk.

### Revisions during execution (second code review of Task 4)

45. **The cap works from the playhead itself and never drops what plays there.** It measured distance
    from the retain window's edge (2 s above a reverse playhead), and with keyframes more than ~90 s
    apart the only chunk it could drop was the one playing: seconds of silence again. `retain()` now
    takes the playhead; the cap drops whole chunks farthest from it (never the one playing at it, nor
    the one being filled), then cuts the far end of the one being filled, never past the playhead. That
    also bounds a single stretch: one from a keyframe ten minutes back used to hold all ten minutes.
46. **A test pins the grid tolerance** (`(2.19 − 0.01) × 48000` rounds to 104640.000000000015), and the
    Units section and the reverse-audio note are corrected.

### Revisions during execution (third code review of Task 4)

47. **The chunk being filled competes by distance too.** Revision 45 dropped whole chunks first and
    touched the one being filled only when nothing else was left -- but in live reverse the one being
    filled is the prefetch below the stretch that plays next, the farthest audio of all. With keyframes
    more than ~90 s apart the cap erased that next stretch whole: silence from the next stretch on (1×
    with a 4K-sized pool), 10.6 of 12 s at −4×. Now the farthest audio goes first: a whole chunk, or --
    when the farthest is the one being filled -- its far end, never past the playhead. Tests now pin the
    back-cut branch, both "never past the playhead" guards, farthest-first among whole chunks, and the
    grid tolerance in reverse trimming.

### Revisions during execution (code review of Task 5)

The reviewer drove the decoder, and the prototype worker through it, over about 30 generated files in
20 containers (plus 4K and 1080p AVI/MP4 and a 4K ProRes clip); each finding reproduced before it was
fixed, and each fix is pinned by a check that fails when it is reverted.

48. **An uninitialised channel layout could be freed.** `drainAudio` handed an uninitialised
    `AVChannelLayout` to `av_channel_layout_copy`, which frees its destination first: a SIGABRT on a
    fragmented MP4 and on MP4/MPEG-TS files with late audio at `-O1`, depending on what was on the
    stack. Both layouts are zero-initialised.
49. **A frame with no timestamp follows the one before it.** Decoders flush the last frames of an AVI
    (MPEG-4, H.264) or MPEG-PS with B-frames with no timestamp, and a raw H.264/HEVC stream has none at
    all; such frames read as before the first frame. The worker took the decoder for a lap behind and
    sought continuously (2,321 seeks in 10 s on a small AVI; a 4K AVI fell to 91% on time), offline
    renders showed the wrong frame at every lap's end, and reverse from a lap's end nearly froze. Such a
    frame now takes the previous frame's time plus its duration (the nominal frame duration when it has
    none). One with no frame before it since a seek into the file cannot be placed and is skipped: after
    a seek into the last keyframe interval of an MPEG-PS the decoder puts out nothing else, and placing
    it at the seek's target made the worker plan the same reverse stretch forever. Measured: live 100% on
    time with no seeks (the 4K AVI included), offline 220 of 220 frames exact, and reverse on AVI and PS
    as smooth as on MP4.
50. **A raw stream seeks by position, and a seek that cannot reach even the start fails the stream.**
    A raw H.264/HEVC stream has no times to seek by: `open()`'s seek to the start read the whole file
    (389 ms for 153 MB), failed, and left nothing to decode. A raw stream (`AVFMT_NOTIMESTAMPS`) now
    always seeks to its first byte of data, and its index is no guide, so the worker catches up forward
    instead of seeking ahead. Any failed seek falls back to the start by timestamp, then by position;
    `seek()` returns false only when even that fails, and the worker then fails the stream ("cannot
    seek in this file") rather than spin on a seek that achieves nothing. `open()` fails when no frame
    decodes rather than opening a player that never shows one.
51. **A full read-ahead settles the audio.** `pumpAudio()` stops at 64 MB of queued video, and an
    offline render waits for the audio to settle: when 2 s of audio-free video (the slack) is more than
    that -- 4K ProRes at 770 Mbps with its audio starting at 3 s -- frame 0 never became ready. When
    `pumpAudio()` stops at the cap, the audio now counts as settled up to the latest packet read; audio
    interleaved further behind than 64 MB of video would be given up, where waiting would never end.
    Frame 0 is ready in 0.08 s, and the audio still starts at 3.000 s.
52. **Audio follows its timestamps.** Decoded audio was appended as if it were continuous, so a 0.6 s
    hole in an MKV's audio put everything after it 0.6 s early. The decoder now hands audio out in runs:
    within 0.1 s of where the audio so far ends it carries straight on (FFmpeg's `aresample` uses the
    same threshold; millisecond timestamps must not click), and further off -- a hole or an overlap -- a
    new run starts at its own time. `takeAudio()` says whether a run continues the previous one, and
    the worker begins a new chunk when it does not, so a hole reads as silence and costs no memory. The
    click after the hole lands at 3.000 s (it landed at 2.403).
53. **The resampler is rebuilt when the audio format changes, and reset by a seek.** One built for
    stereo read a second plane that mono audio does not have: a crash on an MPEG-TS whose audio goes from
    stereo to mono (broadcast recordings switch between 5.1 and stereo). Across a seek it also carried
    samples from before the seek (up to 0.147 off on 44.1 kHz audio).
54. **Smaller fixes.** The audio stream is the video's (`av_find_best_stream`'s related stream), and only
    the chosen streams' packets count towards settled audio: a second program whose clock ran 1000 s
    ahead made the audio read as settled at 998 s after 0.28 s. `pkt_timebase` is set on both decoders,
    so a decoder re-times the samples it skips: Opus audio played 6.5 ms early (its pre-skip).
    `convert()` refuses a stride shorter than a row.
55. **Tests.** The decoder scenario also checks the MPEG-TS clock rebase and the short-stride refusal,
    and SKIPs its FLV case where there is no H.264 encoder. A new scenario writes an AVI with a B-frame
    tail, a raw H.264 stream (SKIPped without an H.264 encoder), a Matroska file whose video starts 1 s in,
    a clip with a 0.6 s hole in its audio (remuxed without those packets through FFmpeg's API: `VideoEncoder`
    cannot leave one), and two MPEG-TS files end to end that change audio format and picture size. The
    `VideoStream` scenario renders the audio-hole clip offline and checks the silence and the tone after it.
56. **The read-ahead cap stays strict.** The first version of revision 51 read a packet before checking
    the cap, so at the cap every `pumpAudio()` call whose target lay past what had been read took one more
    packet: 40 calls with nothing decoded between them grew the queue by 130 MB on the ProRes file.
    `pumpAudio()` checks the cap before each read again, as it did before.

### Revisions during execution (second code review of Task 5)

57. **The audio settles through the cap only once the read-ahead is stuck.** Revision 51 settled the first
    time `pumpAudio()` met the cap, while the worker was still draining the queue into its pool, so it gave
    up audio the pool and the decoder's pipeline could have waited for -- not only audio lagging "further
    behind than 64 MB". A fragmented MOV writes each fragment's video and then its audio (OBS records this
    way): 4K ProRes in 2 s fragments rendered offline with 0.97 s of silence in 18 spans, where the code
    before revision 51 rendered it whole. Now, at the cap, the audio counts as settled only when no packet
    has been taken off the queue since `pumpAudio()` last stopped there: the caller has stopped decoding --
    its pool is full and it is waiting for this audio -- and nothing will ever drain the queue. Both
    fragmented clips render with no silence, and the late-audio clip still renders whole (frame 0 ready
    after 0.56 s: the pool fills first; peak memory 1.0 GB, the pool now fully used).
58. **Audio carries on across at most 40 ms** (`kAudioJitter`; it was 0.1 s). Holes of 65 and 86 ms were
    closed up, leaving the rest of the lap's audio that much early. 40 ms is far above timestamp rounding
    (Matroska and FLV keep milliseconds) and under the ~45 ms by which audio leading the picture starts to
    show.
59. **A raw stream's frame duration comes from its decoder.** Its demuxer knows only a default of 25 fps,
    so 30 and 60 fps raw streams told the planner 0.04 s (their frame times were right: each frame carries
    its duration). The decoder reads the real rate from the stream when `open()` decodes the first frame.
60. **Test fixes.** `VideoEncoder`'s MPEG-TS starts its clock 0.04 s in (the B-frame delay), not 1.4 s: the
    check's comment says so, and its duration tolerance (0.1 s) is 0.02 s, so a broken rebase (12.04 s)
    fails it. The awkward-files check no longer claims the raw stream decoded when that case SKIPped.
    `VideoStream`'s reverse check SKIPs its FLV parts without an H.264 encoder, like the decoder's.
61. **Tests pin the rest.** The read-ahead cap takes a test value (`setMaxQueuedBytes`): with a 1-byte cap
    and audio that starts 1.5 s in, the audio must not settle while a packet was taken since the last stop,
    and must settle once none was. A 64 ms hole must start a new run. After `seek(1.0)` a decoder that
    played 2 s of 44.1 kHz audio must hand out the same samples as a fresh one. The raw stream is written at
    30 fps and must report 1/30 s. An AVI whose last keyframe is its last frame must not make live reverse
    spin: a seek there yields only an untimed frame with nothing before it (3 stretches in 1.5 s; placed at
    the seek's target, over 300). Each check fails when its fix is reverted. The rule that only the chosen
    streams count towards settled audio stays unpinned: `VideoEncoder` cannot write a two-program MPEG-TS.

### Revisions during execution (third code review of Task 5)

62. **Only a caller waiting for audio gives it up.** Revision 57 read "no packet taken since the read-ahead
    last stopped at the cap" as "the caller is waiting for this audio", which holds only for an offline
    render: live playback and pauses stop decoding too once the pool is full, and audio given up then was
    merely late when a render started from there without a seek. On the fragmented ProRes clip a harness
    reading each frame's audio ahead of the playhead found 30–70 ms of silence; reading as the node does it
    found none, but only by timing. Now `pumpAudio(t, waitingForAudio)`: the worker passes its offline
    flag, and a call without it gives nothing up and forgets what an earlier call gave up -- so the first
    waiting call after live ones only marks the cap, and the worker, which may have just freed buffers,
    gets to decode before anything is given up. `VideoStream` takes a read-ahead size for tests; with a
    1-byte read-ahead, an offline render of a clip whose audio starts 1.5 s in must not stall. `seek()` no
    longer sets the resampler's format as well as freeing it, so the resampler check pins the line that
    resets it.

### Revisions during execution (code review of Task 6)

63. **An exact keyframe interval holds without libx264.** `x264-params scenecut=0` reaches libx264 only:
    the MPEG-4 fallback put a keyframe at every hard cut (the scenario's clip keyed 0, 10, 20…, failing it
    and stopping every later `gl_smoke` scenario), and the 300-frame clip Task 8 asks for keyframes 250
    apart keyed frame 128 instead. The other H.264 encoders cannot be relied on to hold an interval
    (VideoToolbox keys every scene cut regardless). So an interval is written with libx264 or MPEG-4 only
    (never another H.264 encoder), MPEG-4's own scene-change detection is switched off (`sc_threshold`), and
    every Nth frame sent is forced to be a keyframe, which also stops MPEG-4's B-frames moving odd intervals
    by a frame (25 gave 0, 26, 50, 74). Checked with the codec lookup intercepted: every clip the plan writes
    keys exactly as asked under libx264, MPEG-4 and VideoToolbox's stand-in; 0 is unchanged. Nothing that
    runs routinely checks the MPEG-4 path, though: every CI runner has libx264, where the scenario passes
    with or without these two lines.
64. **`nextKeyframeAfter(t)` is strictly after t.** It took the entry at or after the tick one past t's, but
    t is usually a frame's own time, worked out from its timestamp, and dividing it back by the time base
    can land a hair below that tick (1.16 s at 1/12800 s ticks) -- or a whole tick below where a tick is a
    frame (AVI) -- so it returned the keyframe at t itself: walking the index spun, and the worker would
    have seen the keyframe at its decode head as still ahead of it (a seek for nothing). It now walks on
    from the first keyframe at or after t's tick past any at t (within a microsecond). Reverse, forward,
    hitch and MPEG-TS acceptance figures are unchanged.
65. **Smaller fixes.** The `open()` comment says which encoders honour an interval and that at or below 0
    keyframes are at most a second apart (scene cuts can add more); the scenario's message says what its
    0.1 s tolerance checks (the index may list keyframes by decode time); and it walks the index of an
    every-frame-keyframe MP4 and AVI, which must visit each keyframe once. FLV's muxer flags its closing
    end-of-sequence tag as a keyframe, so its index lists a keyframe at the last frame's decode time --
    likely why a seek into an FLV's last frames finds nothing (revision 20's back-off already copes).

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
functions, unit-tested in `core_tests` (the `StepSync.h` / `BarSync.h` pattern).
- `videoNextStep(VideoPlanInput) → VideoStep`: one of `Wait`, `Fill`, `CatchUp{to}`, `Seek{to}`,
  `Wrap`, `Reverse{end, fresh}`. It is computed from:
  - the target, direction (and whether it just changed), loop flag and the loop-off lap's bounds;
  - D and the nominal frame duration;
  - the decoder head (time of the next frame), whether the lap has ended, and where it ends;
  - the next keyframe after the head, when known;
  - the earliest frame held, whether the last seek was pinned, the free and total buffer counts;
  - reverse: the range this run's stretches cover.
- `videoSelectFrame(n, u, timeOf) → index or -1`: the frame for u among n ascending times.
- `videoPlanStretch(keyTime, end, frameDur, budget, offline) → VideoStretch`, and
  `videoStretchKeeps(stretch, t, frameDur)`.
- `videoPoolFrames(budgetBytes, w, h)` = clamp(budget / (w·h·4), 4, 64).
  With 512 MB that gives 16 at 4K, 64 at 1080p and 4 at 8K.
- `videoLapStart(u, D)` = D·floor(u/D), `videoWrapped(u, D)` = u − videoLapStart(u, D).
- The time model: `VideoPlayhead`, `videoAdvance(...)`, `videoPosition(...)`, and `videoFrameStep(dt)`
  (the exact offline step).
- Constants: `kVideoPoolBytes`, `kVideoCatchUpFrames` (2), `kVideoSeekNoIndex` (2 s),
  `kVideoSeekMinJump` (1 s), `kVideoAudioLead` (1 s), `kVideoAudioKeep` (2 s), `kVideoCatchUpSlice`
  (0.1 s), `kVideoTimeEps` (1e-6).

**`core/TimedAudio.h`** — new, header-only, no GL or FFmpeg code. A time-tagged audio store at
48 kHz mono.
- It holds contiguous chunks, each a start time u plus samples, covering up to the end of the last
  sample, so adjacent chunks tile.
- `beginChunk(startU)` starts a new chunk; seeks, loop wraps and reverse stretches use it.
- `append(samples, n, clipHi = +inf)` adds to the current chunk, keeping only the samples that start
  before `clipHi` (a reverse stretch passes the start of the stretch above it).
- `retain(lo, hi, u)` keeps the playhead u's neighbourhood: chunks wholly outside [lo, hi] go (never the
  current one), and a chunk's front (forward) or back (reverse) is cut once more than a second of it lies
  outside.
- A 180 s cap drops the audio farthest from the playhead first, never what plays at u: the farthest
  chunk goes whole -- or, if it is the one being filled, loses its far end, never past u.
- `sample(u0, u1, out, n)` maps output sample j to time u0 + (u1 − u0)·j/n and interpolates linearly,
  exactly like today's `emitAudio`.
  - Times not covered by any chunk produce silence.
  - Where chunks overlap, the most recently begun one wins.
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
  | `request(VideoRequest)` | `{u, rate, loop, offline, lapLo, lapHi}` (the lap bounds only matter with loop off); the latest request wins; wakes the worker |
  | `frameAt(u, FrameView&)` | the frame for u as `{rgba, t, serial}` (rows top-down, width·4 apart), valid until the next call |
  | `frameReadyFor(u)` | offline: the frame for u is decided and held, and (forward) no more audio can arrive for times up to u |
  | `waitForFrame(u, timeout)` | offline only: blocks until `frameReadyFor(u)` or the timeout |
  | `readAudio(u0, u1, out, n)` | samples the audio store |

- **Locking:** one mutex plus a condition variable guard the request, pool, queue and audio store.
  They are held only for bookkeeping, never while decoding, converting or uploading.

**`gfx/VideoDecoder`** — existing, small changes.
- `open(path, err, const std::atomic<bool>* abort = nullptr)`:
  - sets `thread_count = 0` (automatic) and `pkt_timebase` before `avcodec_open2`;
  - allocates the format context first so it can install an `AVIOInterruptCB` that returns
    `*abort`. A stop then aborts a stalled open or read;
  - fails when no frame decodes.
- Video packets are read into a bounded queue (64 MB) instead of being decoded straight away.
- `decodeNext(DecodedFrame& out)`: decode the next video frame *without* converting it.
  - `DecodedFrame` is a move-only handle that owns a reference to the decoded picture, plus its time.
  - The FFmpeg type is forward-declared, so FFmpeg headers stay out of `VideoDecoder.h`.
  - Audio met on the way is decoded into pending runs.
  - Handles are cheap reference counts, so the worker can hold the previous and the next frame at once,
    which the catch-up rule needs.
  - A frame with no timestamp takes the previous frame's time plus its duration; one with no frame
    before it since a seek into the file cannot be placed, and is skipped.
- `seek(t) → bool`: to the keyframe at or before t; failing that, the start of the file by timestamp,
  then by byte position -- the only way a raw stream (`AVFMT_NOTIMESTAMPS`) seeks. False when even that
  fails.
- `pumpAudio(t, waitingForAudio)`: read ahead (queueing video packets) until the audio is settled up to
  source time t. Only a caller waiting for the audio -- the worker in an offline render -- lets it give
  audio up when stuck (below); any other call gives nothing up and forgets what was given up before.
- `audioSettledUpTo()`: the source time up to which no more audio will arrive -- the decoded audio's
  end, or, once the demuxer has read 2 s past a point without audio for it (counting only the chosen
  streams' packets), that point; once a waiting caller is stuck at the 64 MB cap (no packet taken off the
  queue since `pumpAudio(t, true)` last stopped there: its frames all wait on this audio), the latest
  packet read; +inf at the end of the input or with no audio stream. `setMaxQueuedBytes()` sets the cap for
  tests (and `VideoStream` takes a read-ahead size to pass on).
- `takeAudio(out, startT, continues)`: move out the next run of audio decoded since the last call, with
  its start time. Runs follow the source's timestamps: audio within 40 ms of where the audio so far ends
  carries straight on, and a hole or an overlap starts a new run (`continues` false). The resampler is
  rebuilt whenever the audio's format changes, and reset by a seek.
- `convert(const DecodedFrame&, uint8_t* dst, int stride)`: threaded, top-down conversion through the
  portable path above (the stride at least a row). The conversion context is created lazily from the
  first frame's format.
- `frameDuration()`: taken from `avg_frame_rate`, then `r_frame_rate`, falling back to 1/30 s; a raw
  stream's from its decoder, which reads the rate from the stream (its demuxer knows only 25 fps).
- `nextKeyframeAfter(t, double& key)`: looked up through `avformat_index_get_entry_from_timestamp` /
  `avformat_index_get_entry`, strictly after t (an entry at t itself is passed over, however dividing t
  back into ticks rounds). Returns false when the stream has no index; `key` is +inf when none is
  known after t, which includes MPEG-TS and MPEG-PS, whose indexes list seek probes as keyframes, and
  raw streams, which only seek to their start. An entry may be a keyframe's decode time, a frame or two
  early.
- `decodeFrame()` keeps its current behaviour, including single-threaded bottom-up output, for the
  15 `gl_smoke` call sites (it is now built on `decodeNext()` + `takeAudio()`).
- `convert()` passes the caller's buffer to `sws_scale_frame()` wrapped in a reference-counted buffer
  whose free callback does nothing: FFmpeg 5–7 allocate a new buffer for a destination frame that has
  none, which would silently write the pixels somewhere else.

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
- **Test accessors:** `playhead()` (wrapped), `hasFrame()`, `shownFrameTime()` (unwrapped; valid when
  `hasFrame()`), `audioOut()`.
- **Overrides `loading()`** — see Offline renders.

**`gfx/VideoEncoder`** — test support only. `open(…, std::string& err, int keyframeInterval = 0)`,
where 0 (or less) keeps today's keyframes at most a second apart (`gop_size = fps`). An interval is exact:
the clip is written with libx264 (scene cuts off) or MPEG-4 (its scene-change detection off), never another
H.264 encoder, and every Nth frame is forced to be a keyframe. `gl_smoke` uses it to write clips with
widely spaced keyframes.

### Time model

- Each frame, u advances by rate·dt while `play` is on.
- **Loop on:** u runs freely, including below 0 in reverse. The displayed position is `wrapped(u, D)`.
- **Loop off:** u is clamped to the lap it is in, [L·D, (L+1)·D − 2·10⁻⁶], with L fixed when loop was
  switched off (0 initially) -- taken from the position *before* that frame's step. The displayed
  position is u − L·D, and stopping just short of (L+1)·D means the end holds this lap's last frame,
  not the next lap's first (which the worker may already have decoded).
- **Offline:** u advances by `videoFrameStep(dt)`, the exact 1/fps, so long renders stay on the frame
  grid.
- **Start:** u does not advance until the first frame of the file is on screen, so opening and decoder
  warm-up do not skip the clip's first frames.
- **D = 0 (unknown duration):** today's behaviour is kept. u is clamped at ≥ 0, there are no laps, and
  the worker holds the last frame at end of file.
- The worker tags every frame and audio chunk with unwrapped times. While looping it does not queue
  frames whose timestamp is ≥ D, because they would overlap the next lap.
- **Times count from the first video frame.** The decoder rebases every time it hands out or takes in
  (frames, audio, seeks, the keyframe index, the duration) on its first frame, so u = 0 always has a
  frame, whatever time the container's clock gives it.

### Per-frame flow on the UI thread (live)

1. **Path changed:** destroy the old stream (a bounded join), create a new one, and set u = 0.
   Until the new file's first frame is uploaded the output is `TexRef{0}`, as today.
2. **Opening or failed:** publish an empty texture and silence, and set the status to "opening…" or
   "load failed: …".
3. **First `Ready`:** allocate the staging and output textures at the video size, plus the flip
   framebuffers.
4. Advance u as in the Time model (held until a first frame is on screen), then call
   `request(u, play ? rate : 0, loop, ctx.offline)`.
5. Call `frameAt(u)`. If the serial changed:
   - upload to the staging texture with `glTexSubImage2D` (top-down rows);
   - flip into the output texture with one `glBlitFramebuffer` (source rows 0→H, destination H→0),
     inside a `GLStateGuard`.
6. Publish the output texture.
7. **Audio:** n = `audioBlockFrames(48000, dt)`.
   - Paused, or u unchanged: silence, as today.
   - Otherwise: `readAudio(uPrev, u, out, n)`.
   - A loop wrap is continuous in u, so today's one silent block per wrap goes away.
8. **Status:** position / duration × rate, plus "(buffering)" when playing forward and the shown frame
   is more than max(0.2 s, 2 frames) behind u.

The UI thread never waits in live mode. When no newer frame is ready, the previous picture stays up.

### Worker: forward playback

**Direction.** The direction is the sign of the requested rate. A rate of 0 (paused) keeps the
previous direction, so pausing never re-plans or flushes anything; the worker just stops being asked
to move.

The rules below are checked in order after each snapshot of the request.

1. **A direction change, or a target behind everything held** (the frame on screen and the queue):
   `Seek{target}` -- unless the last seek for a target at or before this one was pinned. With loop off,
   also when the decoder is in an earlier lap than the playhead (it fell behind while looping):
   nothing it decodes there can be shown.
2. **Target ahead of the head by more than 2 frame durations:**
   - If a keyframe lies between the head and the target (the next lap's start counts) and the gap is
     more than 1 s: `Seek{target}`. A seek restarts FFmpeg's frame-threading pipeline, which costs more
     than decoding through a shorter gap.
   - An index keyframe counts as eight frames later than listed: B-frame streams' indexes list
     keyframes by decode time (x264 2 frames early, x265 up to 6), and a seek for a target just below
     the one shown would land a whole interval back. The next lap's start is exact.
   - If there is no keyframe index and the target is more than 2 s ahead: `Seek{target}`.
   - Not, though, within the lap while an earlier seek ahead has landed at or behind the decoder and
     the target has not moved on by the gap it revealed (`videoNoSeekBelow`): a seek would land there
     again.
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
- **Live, catch-up is sliced.** It chases a moving playhead for at most 100 ms, then shows the best
  frame reached and returns to the planner. A decoder slower than the playhead therefore still moves
  the picture (skipping frames) instead of chasing forever.
- **A seek keeps what is still useful.** Queued frames after the target are released; the newest one
  at or before it stays up until the seek delivers a better one.
- **A late landing retries further back.** If the first frame after a seek is later than the target
  (a decode-time index lands a seek just below a keyframe ON it; an MPEG-TS timestamp search overshoots
  a keyframe interval), or there is none (FLV and MPEG-TS near the end), the seek is retried 1 s, 2 s,
  4 s… earlier, down to the start of the file. Only if even that lands late does the target precede
  the first frame: that frame is shown for it, and the seek is pinned.

**The worker recycles frames that can never be shown, itself.** That covers:
- *superseded* frames: those older than the newest queued frame that is ≤ the target in forward
  playback (newer, in reverse);
- when loop is off, frames before the current lap. The request carries that lap's bounds, [L·D,
  (L+1)·D]. Frames past its end stay: the clamped playhead never shows them, and they are the next
  ones if loop comes back on. What reverse covered below the lap goes with its frames.

The pool therefore cannot deadlock on frames that will never be shown.

### Worker: reverse playback

1. **Plan a stretch.** A *fresh* one (a new run; the playhead fell below what is covered; or it is
   stranded above the top of what is covered -- stopped, paused or offline, where it would never
   arrive, or more than three leads plus a frame above, where arriving would take several stretches) ends
   at E = the playhead minus a lead: live and moving, |rate| × the last stretch's decode time, so it
   lands where the playhead will be; otherwise 0. Otherwise the stretch lies strictly below the stretch
   above. Seek to E, landing where the stretch admits frames (so a prefetch never lands on the
   keyframe above); the first decoded frame gives the keyframe time K. The frame count is
   n = floor((E − K) / frameDur) + 1, and `top` is the nominal time of the last one, on K's grid (live
   prefetch stretches anchor E half a frame below the stretch above).
2. **Live:** the budget is M = pool / 2 frames.
   - If n ≤ M, keep every frame in [K, E].
   - Otherwise keep every s-th frame counting back from `top`, s = ceil(n / M), rounding each frame's
     count onto K's grid (containers round frame times by up to half a frame), so the top frame is kept
     and the whole stretch stays evenly covered. If nothing was kept, the newest frame is.
   - Frames that are not kept are decoded but not converted.
   - This spacing uses the nominal frame duration; live reverse is best-effort.
3. **Offline:** every frame in [K, E] is converted into a rolling ring of M buffers. The ring ends
   holding exactly the last M frames ≤ E, so the stretch covers only down to the earliest of them (to
   K -- or the lap's start, when K is the lap's first frame -- only if nothing was evicted). The next
   stretch ends just before the earliest one kept, which re-decodes from the same keyframe. That is
   slower, but frame-exact.
4. **Next stretch:** E′ = K − ε (live), which lands on the previous keyframe. Crossing below the start
   of a lap goes to the previous lap's end when looping, and holds the first frame otherwise.
5. **Prefetch:** the next stretch starts once ≥ M buffers are free.
6. **A stretch's frames are queued together when it is complete.** Decoding runs forwards, so queueing
   them one by one would show the earliest first and then play the stretch forwards.

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
  offset. The decoder's runs follow the source's timestamps; where one does not continue the previous
  run (a hole or an overlap in the source's audio), the worker begins a new chunk at its own time, so
  a hole reads as silence.
- **Playing forward, audio is read ahead** with `pumpAudio(u + 1 s)` every worker step and during
  catch-up, independent of how many frames the pool holds.
- Seeks, wraps and reverse stretches begin a new chunk; a reverse stretch clips its audio at the start
  of the stretch above it (keeping the samples that start before it). A chunk covers up to the end of
  its last sample, so adjacent chunks tile, and where chunks overlap the most recently begun one wins:
  the new lap at a loop seam, the stretch below in reverse (the one above resumes its audio a little
  before its keyframe, out of a just-flushed decoder).
- Audio decoded during catch-up and during reverse stretches is kept, so reverse still sweeps
  backwards, as today.
- **Retention:** live forward drops audio more than 2 s behind u; reverse drops audio more than 2 s
  ahead of it, trimming the backs of chunks. A 180 s cap is a safety net; it drops the audio farthest
  from the playhead first, never what plays there.
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
- **Reverse:** the offline stretch rule above makes reverse renders frame-exact. A render that starts
  while live reverse is playing restarts the run, since live stretches keep every stride-th frame;
  until an offline stretch has landed, reverse readiness says no.
- **Readiness** (`frameReadyFor(u)`): the frame for u lies in the run of consecutive decided frames
  and is still held; playing forward, the audio is also settled up to u (or u is in an earlier lap,
  whose audio is complete). In reverse the stretch that brings the frame also brought its audio.

### Errors

| Case | Handling |
|---|---|
| Open fails (missing file, no video stream, unsupported codec, no frame decodes) | `Failed` with the reason; the node shows "load failed: …" as today, publishes black and silence; the worker exits |
| Frame buffers can't be allocated (`std::bad_alloc`) | `Failed`: "not enough memory for W×H frames" |
| Corrupt packets | skipped |
| Truncated file | behaves like end of file (wrap or hold) |
| `av_seek_frame` fails | seek to the start (by timestamp, then by byte position) and catch up to the target: slow but correct. A raw stream always seeks this way |
| Not even the start can be reached | `Failed`: "cannot seek in this file" (the worker would otherwise seek again every step) |
| Frames with no timestamp | placed after the previous frame; one with no frame before it since a seek into the file is skipped |
| Holes or overlaps in the audio's timestamps | a new chunk at the audio's own time; a hole is silence |
| The audio format changes mid-file | the resampler is rebuilt |
| A seek lands late or finds no frame (decode-time indexes, MPEG-TS) | retried 1 s, 2 s, 4 s… earlier, down to the start of the file |
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
- **`nextStep` rules added in planning:** a target in the next lap seeks (index or not) unless the gap
  is short; a short gap with a keyframe in it is decoded through; a pinned seek is not repeated.
- **`videoStretchKeeps`:** an end between frames counts from the frame containing it.
- **`nextStep` reverse:** a fresh stretch aims `lead` below a moving playhead; a moving playhead above
  the covered stretch is left to arrive, a stopped or far-stranded one restarts there.
- **`nextStep` forward, added in execution:** loop off with the decoder a lap behind seeks; a seek ahead
  that landed behind the decoder is not repeated nearby (`videoNoSeekBelow`); the seek thresholds and
  the pinned target at their boundaries; seeking back comes before the end-of-lap rules.
- **`videoLapStart`** maps every lap start to itself, for durations that do not divide evenly.
- **The time model:** loop on runs past the end and below 0; loop off clamps to the lap captured
  before the step; paused does not move; unknown duration clamps at 0.
- **`TimedAudio`:**
  - sampling inside a chunk matches today's `emitAudio` mapping;
  - silence where uncovered;
  - the most recently begun chunk wins where chunks overlap (a loop seam; reverse stretches);
  - adjacent chunks tile;
  - a reverse sweep reads backwards;
  - `clipHi` keeps the samples that start before it; retention forward and reverse; the 180 s cap
    spares the playhead's chunk.

### `gl_smoke`

- The existing Video Player scenario (`tests/assets/test.mp4`: picture, audio, reverse) keeps passing.
  It runs with the graph in offline mode, so frames are exact and synchronous; one live-mode check
  polls until the texture has colour.
- The existing encoder round-trip checks keep `decodeFrame()`'s bottom-up output honest.
- **`VideoDecoder` split decode:** `decodeNext()` + `convert()` equal `decodeFrame()` flipped,
  byte for byte, and a stride shorter than a row is refused; the keyframe lookup; a second of audio read
  ahead after one video frame; an FLV with B-frames (first frame a frame in; SKIPped without an H.264
  encoder) counts from its first frame -- at 0, with the keyframe index, the duration, `seek(0)` and the
  audio counting from it too; so does an MPEG-TS whose clock starts 1.4 s in.
- **`VideoDecoder` on awkward files:** an AVI's untimed B-frame tail follows the frame before it, every
  lap; a raw H.264 stream at 30 fps (SKIPped without an H.264 encoder) decodes 50 frames 1/30 s apart,
  reports a 1/30 s frame duration and seeks to its start; in a Matroska file whose video starts 1 s in,
  `seek(2.02)` lands on the keyframe 2 s after the first frame and the audio starts with that frame; holes
  of 0.6 s and 64 ms in the audio are kept -- the audio after each resumes at its own time as a new run,
  and `pumpAudio` settles through them; with a 1-byte read-ahead cap and audio starting 1.5 s in, the audio
  settles through the cap only once a waiting caller has taken no packet since the last stop, a caller not
  waiting gives nothing up and forgets what was given up, and the first waiting stop after it only marks
  the cap; after `seek(1.0)` the
  44.1 kHz audio is the same whatever played before; two MPEG-TS files end to end (stereo 64×48, then
  mono 80×64) decode through, the second half's frames refused by `convert()`, its audio resampled.
- **`VideoEncoder` keyframe interval:** with hard cuts every 10 frames, keyframes land exactly every
  50 frames (no scene-cut extras); walking the index of an MP4 and an AVI whose every frame is a keyframe
  (`nextKeyframeAfter` of the key it gave last) visits each keyframe once, strictly forward.
- **`VideoStream`:** a missing file ends `Failed` with a reason; `test.mp4` opens with the right info;
  offline, the frame for 0.73 s is the one at 0.7 s, with audio before it; rendered offline, a clip with
  a hole in its audio is silent across the hole and plays the tone after it; with a 1-byte read-ahead, an
  offline render of a clip whose audio starts 1.5 s in does not stall.
- **`VideoStream` reverse through awkward files** (written by the scenario with the indexed-clip
  writer): offline reverse is exact through a first keyframe interval longer than the stretch ring (loop
  on and off) and across the loop seam of an FLV with B-frames; live reverse over that FLV decodes a
  handful of stretches in 2.5 s (a spinning worker decodes thousands) and follows the playhead across
  the seam (the FLV parts SKIP without an H.264 encoder); paused, the picture settles on the playhead's
  frame and decoding stops; an offline render straight after live reverse is exact from its first frame;
  live reverse from the last frame of an AVI whose last keyframe is its last frame does not spin.
- **`VideoStream` loop toggles:** loop off with the playhead a lap ahead of the decoder renders that
  lap's frames; loop off and back on near a lap's end keeps the next lap's frames (offline exact); the
  same at a lap's start in reverse goes on into the lap below.
- **`VideoDecoder`:** a 12 s Matroska file knows its keyframes past 7 s right after `open()`.
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

Measured on the development machine, in Debug and Release, with the acceptance harness and clips
(see the appendix). The prototype met every one; the figures in brackets are what it measured.

1. 4K H.264 and 4K 8-bit and 10-bit HEVC play forward at 1× with the picture within one frame of the
   playhead in steady state [100% on time in both builds; UI 57–58 fps, vsync-bound].
2. After a file's first frame, no `evaluate()` exceeds 25 ms at 4K, and the mean is < 10 ms
   [worst 15–22 ms, mean 7–9 ms; 1080p mean 2.2 ms]. The first frame of a file allocates two
   textures and takes ~50 ms once.
3. After a single 600 ms hitch, 1080p with keyframes 8 s apart is back in step within 0.5 s [on the
   next frame].
4. At 2× on 4K HEVC the picture keeps moving [7–11 changes/s; before the slicing fix it froze].
5. Resident memory at 4K is at most 1.2 GB [0.88 GB H.264, 1.15 GB 10-bit HEVC; was 1.7 GB].
6. `ctest` passes on all three CI platforms.
7. A local ThreadSanitizer build of `gl_smoke` reports no data races [zero reports].
7a. MPEG-TS with 10 s keyframes: after reverse and a 4 s jump ahead, the picture is back in step
    within 1 s with at most one seek [0.04–0.31 s, no seeks, in 10 runs; worst 0.44 s in a reviewer's
    33; trusting the MPEG-TS index took 1.6–2.4 s].
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
- **Reverse audio seams on some files.** Each reverse stretch restarts the audio decoder. On all-intra
  video (a stretch per frame) every stretch's audio starts with the decoder's fade-in; decoding a little
  audio before each seek point (a pre-roll) would fix that. Where a container rounds audio times to the
  millisecond (MKV, FLV), two stretches holding the same audio disagree by up to half a millisecond, so
  the switch between them can click however early decoding starts; that needs the chunks anchored on
  the codec's frame grid, or a short crossfade where one gives way to the next.
- **Open-GOP MPEG-2 and MPEG-4 Part 2 after a seek.** FFmpeg's first frames after a seek into such a
  file are the frame after the keyframe or broken B-frames, and in an MPEG-PS they can be labelled up to
  0.1 s early, so reverse on those files can be a couple of frames off where a stretch starts.
- Local build configuration. `build.sh` builds Debug; this design removes the Debug-only
  per-byte-free cost from the video path, and switching local builds to RelWithDebInfo is a separate
  choice.

## Risks

- **Thread count.** Each node runs a worker plus FFmpeg decode and conversion threads, so several
  Video Players mean many threads. This is acceptable; `thread_count` can be capped later if needed.
- **Thin 10-bit HEVC 4K headroom** on this machine (1.2×). Heavy concurrent CPU use will drop frames.
  They are dropped, not frozen.
- **Memory overhead beyond the pool** (~0.4 GB at 4K) comes from FFmpeg's frame-thread buffers, the
  packet queue and the GL textures. Capping `thread_count` or the pool budget are the levers if several
  4K players must run at once.
- **Container duration** can differ slightly from the stream's real end. Frames at or beyond D are not
  queued while looping, and the newest chunk wins at audio seams.

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

**Investigation harness.** It compiled the old `src/modules/VideoPlayerNode.cpp` into its own
translation unit with `private` opened up, for inspection only, created a hidden GL 4.1 context, and
each loop called `evaluate()` with `dt` = the previous iteration's wall time (16.7 ms floor), logging
the time `evaluate()` took (with a `glFinish` for the upload), the playhead, the cached window and
whether it covered the playhead, and resident memory.

**Acceptance harness.** The same loop against the new node's public API (`playhead()`, `hasFrame()`,
`shownFrameTime()`), reporting UI fps, time to the first frame, `evaluate()` mean / p99 / worst (and
worst after the first half-second), the share of frames within one frame of the playhead, how often the
picture changes, and resident memory. An optional argument injects one artificial stall. Build it with
the app's Debug flags and with `-O2`; the implementation plan's last task gives its source.
