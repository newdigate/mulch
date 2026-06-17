# MIDI File Player Node — Design

**Date:** 2026-06-17
**Status:** Approved (brainstorming) — ready for implementation plan

## Goal

A node that streams a Standard MIDI File, synced to the project transport/BPM, as a
MidiRef output you can wire into the Acid Bass (or any MIDI input). It anchors the
clip at a start offset (bars), optionally loops a region of a chosen length, and lets
each of the 16 MIDI channels be muted with a toggle.

## Architecture (three GL-free units)

Split parsing, playback math, and the node, mirroring the codebase's DSP/node split.

### `src/core/MidiFile.{h,cpp}` — Standard-MIDI-File reader (GL-free)

A small from-scratch SMF parser (no new dependency; the format is simple and fully
unit-testable). The file's own tempo map is **ignored** — events are positioned in
*beats* so the project BPM drives playback.

```cpp
struct MidiFileEvent { double beats; MidiEvent ev; };   // beats = absolute position
struct MidiSequence {
    bool   ok = false;
    std::vector<MidiFileEvent> events;   // channel-voice events, sorted by beats
    double lengthBeats = 0.0;            // end of the last event / end-of-track
    std::string error;
};

MidiSequence parseMidiFile(const unsigned char* data, std::size_t n);  // testable
MidiSequence loadMidiFile(const std::string& path);                    // reads file -> parse
```

**Algorithm:**
- Read the `MThd` chunk: 4-byte magic, 4-byte big-endian length, 2-byte format, 2-byte
  ntracks, 2-byte division. If `division & 0x8000` (SMPTE timecode) → `ok=false`,
  `error="SMPTE division unsupported"`. Else `ticksPerQuarter = division`.
- For each `MTrk` chunk (magic + big-endian length + that many data bytes): walk the
  track, accumulating absolute ticks. Each item is a variable-length-quantity (VLQ)
  delta time followed by an event:
  - **Meta** (`0xFF`): type byte + VLQ length + data → skip. (End-of-track `0x2F`
    closes the track.)
  - **SysEx** (`0xF0`/`0xF7`): VLQ length + data → skip.
  - **Channel voice** (status `0x80`–`0xEF`, plus running status): keep. Data-byte
    count is 2 for `0x80/0x90/0xA0/0xB0/0xE0`, 1 for `0xC0/0xD0`. Store as
    `MidiEvent{status, data1, data2|0}` at the current tick.
  - **Running status:** if the byte where a status is expected is `< 0x80`, reuse the
    last channel-voice status and treat the byte as the first data byte.
- Convert each kept event's tick → `beats = tick / ticksPerQuarter`, collect across all
  tracks, and `std::stable_sort` by beats. `lengthBeats` = the largest event-beat (or
  end-of-track tick / ticksPerQuarter).
- **Bounds-check every read**; on truncation/bad magic/`ticksPerQuarter==0` →
  `ok=false` with an `error`.

### `src/core/MidiClip.h` — `MidiClipPlayer` (GL-free, header-only)

Turns a transport position into the events to emit this frame. Holds the play cursor
and the set of currently-sounding notes (so it can release them across loop seams).

```cpp
class MidiClipPlayer {
public:
    // The events to emit this frame. Call once per frame with the current transport
    // position. muted[c] true => MIDI channel c is silenced. beatsPerBar from transport.
    std::vector<MidiEvent> advance(const MidiSequence& seq, double transportBeats,
                                   double beatsPerBar, double startOffsetBars,
                                   bool loop, double loopLenBars, const bool muted[16]);
private:
    std::vector<MidiEvent> emitRange(const MidiSequence&, double a, double b,
                                     const bool muted[16]);   // events in [a, b)
    std::vector<MidiEvent> flush();          // note-offs for all sounding notes; clears
    double prevPlay_ = -1.0;                 // last play position (-1 = not in clip)
    bool   active_[16][128] = {};            // sounding notes per channel/note
    bool   prevMuted_[16]   = {};            // last frame's mute state per channel
};
```

**`advance` algorithm** (all positions in beats):
```
local = transportBeats - startOffsetBars * beatsPerBar
if (!seq.ok || local < 0):                       // no clip yet (before the anchor)
    out = flush();  prevPlay_ = -1;  return out
loopLen = max(loopLenBars * beatsPerBar, 1e-6)
if (loop):  playPos = fmod(local, loopLen)
else:       if (local >= seq.lengthBeats) { out = flush(); prevPlay_ = -1; return out; }
            playPos = local

out = []
// A channel that just became muted releases its sounding notes (no hung note).
for c in 0..15: if (muted[c] && !prevMuted_[c]) append note-offs for active_[c][*], clear
copy muted -> prevMuted_

if (prevPlay_ < 0):                              // just entered the clip
    prevPlay_ = playPos                          // emit nothing this frame (next frame's
                                                 // [prevPlay_, playPos) catches beat-0 events)
elif (playPos >= prevPlay_):                     // normal forward step
    out += emitRange(seq, prevPlay_, playPos, muted)
else:                                            // wrapped / looped / scrubbed back
    out += emitRange(seq, prevPlay_, loopLen, muted)   // finish the tail
    out += flush()                                     // release sounding notes at the seam
    out += emitRange(seq, 0.0, playPos, muted)         // play the head from the loop start
prevPlay_ = playPos
return out
```
- `emitRange(seq, a, b, muted)`: for each `seq.events` with `beats ∈ [a, b)` whose
  channel (`status & 0x0F`) is not muted, append the event and update `active_`
  (a note-on with velocity > 0 sets it; note-off or note-on velocity 0 clears it).
- `flush()`: for every set `active_[c][note]`, append `MidiEvent{0x80|c, note, 0}`;
  clear all.

This makes the player loop-robust: the clip's own wrap, the project transport looping,
a stop (beats → 0), and scrubbing all read as a backward jump → tail + release + head,
with no hung notes. It only advances while the transport is playing (a paused
transport keeps `transportBeats` constant, so the window is empty).

### `src/modules/MidiFilePlayerNode.h` — the node (header-only)

Owns a `MidiSequence` (re-parsed when the `file` path changes — the file is tiny, so a
synchronous `loadMidiFile` is fine) and a `MidiClipPlayer`. A `statusLine()` reports
"loaded N events" / the parse error / "no file".

**Ports:**

| # | Port | Type | Default | Notes |
|---|------|------|---------|-------|
| 0 | `file` | String | "" | SMF path |
| 1 | `start offset` | Float | 0 | bars; the transport bar the clip's start sits at |
| 2 | `loop` | Bool | true | |
| 3 | `loop length` | Float | 4 | bars (the looped region) |
| 4…19 | `mute 1`…`mute 16` | Bool | false | checked = that MIDI channel is muted |
| out 0 | `midi` | Midi | — | wire into Acid Bass's `midi` |

**`evaluate(ctx)`:**
- Read `file`; if it differs from the cached path, `seq_ = loadMidiFile(path)`, update
  the cached path, set the status line. (Empty path → empty sequence / "no file".)
- Build `bool muted[16]` from ports 4…19 (`muted[c] = ctx.in<bool>(4 + c)`).
- `double beats = ctx.transport ? ctx.transport->beats() : 0.0;`
  `double bpb = ctx.transport ? ctx.transport->beatsPerBar : 4.0;`
- `out_ = player_.advance(seq_, beats, bpb, in<float>(1), in<bool>(2), in<float>(3), muted);`
- `ctx.out<MidiRef>(0, MidiRef{out_.data(), out_.size()});` (`out_` is a node member,
  so the `MidiRef` stays valid for the frame.)

### Registration / CMake

- `src/app/Application.cpp`: `#include "modules/MidiFilePlayerNode.h"`; `makeNode`:
  `if (type == "MIDI File") return std::make_unique<MidiFilePlayerNode>();`; add
  `"MIDI File"` to the **MIDI** `nodeCategories` list.
- `CMakeLists.txt`: add `src/core/MidiFile.cpp` to `APP_SOURCES` and `core_tests`; add
  `tests/test_midi_file.cpp` and `tests/test_midi_clip.cpp` to `core_tests`.
  (`MidiClip.h` and `MidiFilePlayerNode.h` are header-only.)

## Data flow

```
.mid path ─► loadMidiFile ─► MidiSequence (events in beats; tempo ignored)
transport.beats() ─► MidiClipPlayer.advance(offset, loop, loopLen, mutes)
                       └─► events in [prevPlay, playPos)  (+ note-off flush on wrap)
                       └─► MidiRef out ─► Acid Bass
```

## Edge cases

- No file / parse error → empty output, descriptive `statusLine()`.
- `transport == nullptr` (unit tests) → treated as beats 0 / not advancing.
- Muting a channel mid-note releases its sounding notes (rising-edge flush); unmuting
  resumes on the next note.
- Loop length ≤ 0 → clamped to a tiny positive value (no div-by-zero in `fmod`).
- Events beyond `loop length` never play in loop mode (the loop region is `[0,
  loopLen)`); with loop off the whole file plays once, then silence + a release flush.
- A note left sounding at stop/seek/loop is released by `flush()` — no hung notes
  (important since the Acid Bass is monophonic).

## Testing

GL-free doctest under `core_tests`:

- **`tests/test_midi_file.cpp`** — synthesize a tiny SMF byte buffer in the test
  (`MThd` format 0, 1 track, `ticksPerQuarter = 480`; a track with note-on 60 @ tick 0,
  note-off 60 @ tick 480, note-on 64 @ tick 480, note-off 64 @ tick 960, end-of-track).
  Assert `ok`, 4 events, beats `{0, 1, 1, 2}`, and the right notes/statuses. A buffer
  with bad magic or truncation → `ok == false`. A running-status case (a second note-on
  with the status byte omitted) parses to the same events.
- **`tests/test_midi_clip.cpp`** — hand-build a `MidiSequence` (note-on 60 @ beat 0,
  note-off 60 @ beat 1, note-on 64 @ beat 2; `lengthBeats = 3`) and drive
  `MidiClipPlayer::advance` (`beatsPerBar = 4`):
  - entry frame emits nothing; advancing past beat 0 emits the note-on; past beat 1 the
    note-off.
  - `muted[0] = true` → no events from channel 0.
  - `start offset = 1` bar → nothing until `transportBeats ≥ 4`, then the note-on.
  - with `loop` on and a `loop length` short enough that advancing the transport past
    it wraps `playPos`, the seam emits a note-off for any sounding note and then plays
    the head from the loop start again.
  - a paused transport (same `transportBeats` twice) emits nothing the second call.

(No `gl_smoke` scenario — everything here is GL-free and covered in `core_tests`. The
node renders in the editor; add an optional `--screenshot` check during implementation.)

## Docs

- **README.md** — add a **MIDI File** row to the module table (streams a .mid synced to
  the project BPM; start offset, loop + loop length in bars, per-channel mute toggles;
  wire into Acid Bass).
- **CLAUDE.md** — a brief bullet: `MidiFilePlayerNode` wraps a GL-free SMF parser
  (`core/MidiFile`) + a transport-synced `MidiClipPlayer` (`core/MidiClip.h`) that emits
  the events in each frame's beat window (the file's tempo is ignored; project BPM
  drives it), with loop-seam note-off flushing.
