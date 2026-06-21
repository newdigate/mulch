# Audio Foundation — Design (Phase A of the audio-decouple roadmap)

**Date:** 2026-06-21
**Status:** Approved (brainstorming) — ready for implementation plan

## Goal

Eliminate the intermittent audio under-runs (tiny cut-outs) by fixing the diagnosed root cause —
the per-frame audio production cap that drains the output ring under slow render frames — and lay
the foundation the later decouple phases build on: a unified, dt-tracked audio block size, a
**configurable audio buffer length** in Preferences, and an elevated-priority **MIDI timing
thread**. Audio stays on the main thread in this phase (the worker-thread decouple is Phase C).

## Background — the root cause

Audio is produced inside `Graph::evaluate()` on the main thread, once per vsync frame, and pushed
to an SPSC ring that the libsoundio real-time callback drains (`AudioOutputNode`). Each source node
caps its per-frame block at a small `kMaxBlock` (Sine **1024** = ~21 ms @48 kHz, Acid 2048, Audio
Player / Video 4096). When render frames run slower than the cap (e.g. <47 fps for the 1024 cap),
each frame consumes more real time than it produces audio — a steady deficit that drains the
~170 ms ring over ~1 s, producing a cut-out; a fast frame refills it and the cycle repeats. A hard
stall (window drag/resize, mesh load, a >ring GPU hitch) drains it instantly. The cap is also a
latent player glitch: on a capped frame the playhead advances `rate*dt` but emits fewer samples →
a skip.

This phase keeps the main-thread, per-frame production model but makes the per-frame block track
`dt` (so production keeps pace) and makes the ring cushion configurable.

## Architecture

### Unit 1 — `audio/AudioBlock.h` (new, GL-free, header-only, unit-tested)

A single source of truth for the per-frame audio block size + the ring sizing math, replacing the
scattered `std::clamp((int)std::lround(sr*dt), 1, kMaxBlock)` in the source nodes.

```cpp
#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>

namespace oss {

constexpr int    kAudioMaxBlock = 8192;   // max audio frames produced per evaluate (safety bound)
constexpr double kMaxAudioDt    = 0.085;  // clamp dt for AUDIO block sizing (~4096 frames @48k)

// Frames of audio a source should produce this frame: round(sampleRate * min(dt, kMaxAudioDt)),
// clamped to [1, kAudioMaxBlock]. The dt clamp is local to audio (the transport keeps its own dt),
// so a multi-second stall yields a bounded ~85 ms catch-up block rather than a runaway allocation.
inline int audioBlockFrames(double sampleRate, double dt) {
    double d = dt < 0.0 ? 0.0 : (dt > kMaxAudioDt ? kMaxAudioDt : dt);
    long n = std::lround(sampleRate * d);
    if (n < 1) n = 1;
    if (n > kAudioMaxBlock) n = kAudioMaxBlock;
    return (int)n;
}

// Output-ring capacity in interleaved-stereo floats for a buffer length in milliseconds.
// (The SpscRingBuffer rounds this up to a power of two internally.)
inline std::size_t audioRingFloats(int bufferMs, int sampleRate) {
    if (bufferMs < 1) bufferMs = 1;
    long frames = (long)((long long)bufferMs * sampleRate / 1000);
    if (frames < 1) frames = 1;
    return (std::size_t)frames * 2;   // L + R interleaved
}

} // namespace oss
```

Why a fixed `kAudioMaxBlock` rather than scaling with the buffer: the per-frame block only needs to
cover the (clamped) max `dt`, not the ring size — `kMaxAudioDt` (85 ms ≈ 4096 frames) sits
comfortably under `kAudioMaxBlock` (8192) and under the default ring (150 ms), so blocks never
routinely overflow the ring. The two concerns stay decoupled: `dt` clamp bounds the block; the
buffer pref sizes the ring.

### Unit 2 — Source nodes use the shared helper

Replace the local cap with `audioBlockFrames(sampleRate, ctx.dt)` and size the owned scratch
buffers to `kAudioMaxBlock`, in:
- `modules/SineWaveNode.h` (cap 1024 → shared),
- `modules/AcidNode.h` (2048 → shared),
- `modules/AudioPlayerNode.{h,cpp}` (4096 → shared),
- `modules/VideoPlayerNode.{h,cpp}` (the audio block at line ~215; 4096 → shared).

The pass-through processing nodes (`AudioMixerNode`, `MonoToStereoNode`, `StereoToMonoNode`) already
cap the *input* sample count at 8192; point their `kMaxBlock` at the shared `kAudioMaxBlock` so the
whole chain shares one bound and a source's up-to-8192-frame block is never re-clipped downstream.
The visual consumers (`SpectrographNode` FFT window, `OscilloscopeNode` history) are **not** audio
producers and are left unchanged.

### Unit 3 — `core/Preferences` audio buffer length

Add `int audioBufferMs = 150;` to `struct Preferences`. Serialized as `audio-buffer <ms>`; parsed
back clamped to **[20, 500]**. (Default 150 ms ≈ the current `1<<14`-float ring.) An old prefs file
with no `audio-buffer` line keeps the 150 ms default.

### Unit 4 — `AudioOutputNode` sizes its ring from the preference

The output ring becomes a `std::unique_ptr<SpscRingBuffer<float>> ring_;` recreated on stream
(re)open with `audioRingFloats(prefs.audioBufferMs, sampleRate_)`. The node tracks
`currentBufferMs_` (like `currentDeviceId_`) and `ensureDevice` reopens the stream when **either**
the device id **or** the buffer ms changes. The recreate happens inside `closeStream`/`openStream`,
when the stream (and its RT callback) is stopped — so no concurrent access to the ring during
reallocation. `evaluate()` guards `if (ring_) ring_->push(...)`; `writeCallback` derefs
`self->ring_` (only ever called while the stream — hence a live ring — is started).

*Scope:* the **output** ring only this phase (the reported under-run). The `AudioInputNode` capture
ring and the `RecorderNode` are left at their fixed sizes; sizing them from the same preference is a
candidate fast-follow, not part of Phase A.

### Unit 5 — `PreferencesPanel` Audio Output tab

Add an **"Audio buffer (ms)"** slider (range 20–500) bound to `prefs.audioBufferMs`, firing
`onChange()` (which persists and, via the reopen-on-change in Unit 4, applies live). A short help
note: higher = more headroom against stalls + more latency; lower = tighter latency, less cushion.

### Unit 6 — `app/ThreadPriority.h` + elevated MIDI timing thread

A small best-effort, header-only helper:
```cpp
// Best-effort: raise the calling thread to a time-critical scheduling class. No-op / tolerant of
// failure where unsupported or not permitted. Used by the MIDI sender (and, later, the audio thread).
void setThisThreadTimeCritical();
```
macOS (`__APPLE__`): `pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0)`. Other platforms:
no-op for now (documented). Call it once at the top of `MidiSyncEngine::senderLoop()`. The output
audio device callback already runs at OS realtime priority (libsoundio/CoreAudio), so it needs no
change.

## Data flow (unchanged topology, corrected amounts)

```
source node: audioBlockFrames(sr, dt) frames  ─►  ... processing ...  ─►  AudioOutputNode
AudioOutputNode.evaluate (main thread): push to ring (sized audioRingFloats(audioBufferMs, sr))
libsoundio RT callback: pop from ring  ─►  device   (already OS-realtime priority)
buffer-ms change in Preferences  ─►  reopen stream  ─►  recreate ring at the new capacity
MidiSyncEngine sender thread: setThisThreadTimeCritical() at start  ─►  steady clock/MTC ticks
```

## Edge cases

- **Multi-second stall** → `audioBlockFrames` clamps the audio `dt` to 85 ms, so the catch-up block
  is bounded (no giant allocation, no minutes-of-latency dump into the ring). The transport's own
  `dt` is unchanged (it may still jump, as today).
- **Tiny buffer (20 ms)** → the user trading cushion for latency; an 85 ms catch-up block partially
  overflows and the excess is dropped by the ring (acceptable at that setting).
- **Buffer change while playing** → reopens the stream (a brief gap, like a device change); the new
  ring takes effect immediately.
- **No `audio-buffer` line in an old project's prefs** → default 150 ms.
- **`setThisThreadTimeCritical` not permitted / non-Apple** → silently a no-op; correctness is
  unaffected (it's a scheduling hint only).
- **`ring_` null before the first successful stream open** → `evaluate` guards the push; silent.

## Testing

- **`tests/test_audio_block.cpp`** (new, `core_tests`, GL-free):
  - `audioBlockFrames(48000, 1.0/60)` ≈ 800; `audioBlockFrames(48000, 0.025)` == 1200 (**not**
    clamped — the bug fix); `audioBlockFrames(48000, 1.0)` == clamp(48000*0.085)=4080 (bounded);
    `audioBlockFrames(48000, 0.0)` == 1 (floor); never exceeds `kAudioMaxBlock`.
  - `audioRingFloats(150, 48000)` == 14400 (7200 stereo frames ×2); `audioRingFloats(20,48000)`
    == 1920; monotonic in `bufferMs`.
- **`tests/test_preferences.cpp`** (extend): `audio-buffer` round-trip; a value of 5 clamps to 20,
  9999 clamps to 500; a missing line leaves 150.
- **Build + manual:** the ring recreate / reopen-on-change, the live buffer slider, and the thread
  priority are verified by a clean build + a manual run (raise/lower the buffer; confirm under-runs
  stop under heavy render load). `gl_smoke` is unaffected (no graph/UI change there; the audio nodes
  it builds keep producing — now via the shared helper).

## Docs

- **README.md** — extend the Preferences note: an **Audio buffer (ms)** control on the Audio Output
  tab trades latency for under-run headroom; the audio block now tracks frame time so slow frames
  don't starve the output.
- **CLAUDE.md** — a note: the GL-free `audio/AudioBlock.h` (`audioBlockFrames`/`audioRingFloats`,
  shared `kAudioMaxBlock`) sizes every source node's per-frame block to `dt` (bounded by
  `kMaxAudioDt`); `AudioOutputNode`'s ring is sized from `Preferences::audioBufferMs` (recreated on
  stream reopen); the MIDI sender thread is raised to a time-critical QoS via `app/ThreadPriority.h`.
  Note this is Phase A of the audio-decouple roadmap (B = audio-subgraph compile/snapshot,
  C = dedicated audio thread, D = render frame-drop).

## Out of scope (YAGNI — later phases)

- Moving audio off the main thread / a dedicated audio worker thread (Phase C).
- Partitioning/compiling the audio subgraph + snapshot (Phase B).
- Dropping render frames under load (Phase D).
- Sizing the input-capture ring and the Recorder from the preference (candidate fast-follow).
- Per-device or automatic buffer sizing; sample-rate changes beyond the existing nearest-rate logic.
