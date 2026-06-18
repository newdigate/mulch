# MIDI Chord Player Node — Design

**Date:** 2026-06-18
**Status:** Approved (brainstorming) — ready for implementation plan

## Goal

A transport-synced MIDI node that holds 8 chord patterns and plays one at a time as a
chord (several simultaneous note-ons) on its `midi` output, switching patterns on a
quantized boundary (next **bar**, or **beat** if selected). Each pattern is a root note
+ octave + chord name. Patterns either **auto-progress** (advance one per boundary,
looping the first `length` patterns) or are **manually selected**. Wire the output into
the **Arpeggiator** (which folds the chord's note-ons into its held-note set) or any
synth.

## Architecture (two GL-free units, mirroring Step Seq / Arpeggiator)

The Arpeggiator already turns held note-ons/offs into an arpeggio, so the Chord Player
only needs to emit a chord's note-ons, hold them, and release/replace them at each
boundary. Both units are GL-free and header-only and unit-tested directly in
`core_tests` (exactly like `ArpeggiatorNode` / `StepSequencerNode` and their tests).

### `src/core/Chords.h` — chord helper (GL-free, header-only)

Mirrors `src/core/StepSync.h` (inline tables + helpers, no `.cpp`).

```cpp
namespace oss {

// 12 pitch-class labels for a root-note dropdown (index = semitone within an octave).
inline const std::vector<std::string>& rootNoteLabels();   // {"C","C#",...,"B"}

// 14 chord-type labels for a chord dropdown (index = chord type).
inline const std::vector<std::string>& chordNames();
//   { "maj","min","dim","aug","sus2","sus4","maj7","min7","dom7",
//     "6","m6","m7b5","dim7","add9" }

// Append the MIDI notes of a chord to `out` (does not clear it). rootPitchClass 0..11,
// octave with C4 = 60 (note = (octave+1)*12 + rootPitchClass + interval), chordIndex
// into chordNames(). Out-of-range notes (>127) are dropped; indices are clamped to range.
void buildChordNotes(int rootPitchClass, int octave, int chordIndex, std::vector<int>& out);

} // namespace oss
```

**Chord intervals** (semitones from the root):

| idx | name | intervals | idx | name | intervals |
|----:|------|-----------|----:|------|-----------|
| 0 | maj | 0,4,7 | 7 | min7 | 0,3,7,10 |
| 1 | min | 0,3,7 | 8 | dom7 | 0,4,7,10 |
| 2 | dim | 0,3,6 | 9 | 6 | 0,4,7,9 |
| 3 | aug | 0,4,8 | 10 | m6 | 0,3,7,9 |
| 4 | sus2 | 0,2,7 | 11 | m7b5 | 0,3,6,10 |
| 5 | sus4 | 0,5,7 | 12 | dim7 | 0,3,6,9 |
| 6 | maj7 | 0,4,7,11 | 13 | add9 | 0,4,7,14 |

Notes past 127 are dropped (so a high octave never emits an out-of-range note); for
octaves 0–8 the root always fits, so a valid chord always yields at least the root.

### `src/modules/ChordPlayerNode.h` — the node (GL-free, header-only)

A transport-synced chord sequencer. Holds the currently-sounding pattern index + its
exact note set so it can release them on a switch / stop.

**Per-frame `evaluate(ctx)`:**
1. `out_.clear();`
2. Read globals: `mode` (0 Auto / 1 Manual), `quantize` (0 Bar / 1 Beat),
   `length` = clamp(round(`length` input), 1, 8), `manualSel` = clamp(round(`pattern`
   input), 0, 7) (the `pattern` choice value is already the 0-based pattern index).
3. `unitBars = (quantize == Beat) ? 1.0 / beatsPerBar : 1.0;` (beatsPerBar from the
   transport, default 4).
4. If `ctx.transport && ctx.transport->playing && unitBars > 0`:
   - `unitPos = bars() / unitBars; unitAbs = floor(unitPos);`
   - `boundary = !primed_ || unitAbs != lastUnitAbs_;`
   - On `boundary`: compute `target` =
     - Auto: `((unitAbs % length) + length) % length`
     - Manual: `manualSel`
     If `!primed_ || target != activePattern_`: emit note-offs for every note in
     `activeNotes_`; build the new chord from pattern `target`'s `root`/`oct`/`chord`
     controls via `buildChordNotes`; emit those note-ons (velocity 100, channel 0);
     set `activeNotes_` = the new notes, `activePattern_ = target`. Else: sustain (emit
     nothing). Then `lastUnitAbs_ = unitAbs; primed_ = true;`
5. Else (paused/stopped): emit note-offs for every `activeNotes_` note, clear
   `activeNotes_`, `activePattern_ = -1`, `primed_ = false`.
6. `ctx.out<MidiRef>(0, MidiRef{out_.data(), out_.size()});`

`activeNotes_` holds the precise MIDI notes currently sounding, so a switch releases
exactly what it played (no hung notes on a downstream poly synth; the Arpeggiator
likewise erases them from its held set). Velocity is fixed at 100 and channel at 0,
matching the Arp / Step Seq.

`statusLine()` → e.g. `Auto · C maj · ▸4/8` — mode, the active pattern's root+chord,
and (active pattern index + 1)/length. Before the first boundary it shows just the mode.

### Ports

| # | Port | Type | Default | Range / labels |
|---|------|------|---------|----------------|
| 0 | `root 1` | Float (choice) | 0 (C) | rootNoteLabels() (12) |
| 1 | `oct 1` | Float | 4 | 0–8 |
| 2 | `chord 1` | Float (choice) | 0 (maj) | chordNames() (14) |
| 3…23 | `root 2`,`oct 2`,`chord 2` … `root 8`,`oct 8`,`chord 8` | … | per-pattern defaults: root C, oct 4, chord maj | patterns 2–8 |
| 24 | `mode` | Float (choice) | 0 (Auto) | {Auto, Manual} |
| 25 | `quantize` | Float (choice) | 0 (Bar) | {Bar, Beat} |
| 26 | `length` | Float | 8 | 1–8 (auto loop length) |
| 27 | `pattern` | Float (choice) | 0 (1) | {1,2,3,4,5,6,7,8} (manual selection) |
| out 0 | `midi` | Midi | — | wire into **Arpeggiator** / a synth |

Pattern `i` (0-based) uses inputs `3*i` (root), `3*i+1` (oct), `3*i+2` (chord). The 8
patterns are added in a constructor loop; `mode`/`quantize`/`pattern` use
`addChoiceInput`, `length` uses the ranged `addInput`.

### Registration / CMake

- `src/app/Application.cpp`: `#include "modules/ChordPlayerNode.h"`; in `makeNode`,
  `if (type == "Chord Player") return std::make_unique<ChordPlayerNode>();`; add
  `"Chord Player"` to the **MIDI** `nodeCategories` list, before `"Arpeggiator"`
  (it feeds the Arp).
- `src/main.cpp`: add a `Chord Player` node to the `--screenshot` demo graph.
- `CMakeLists.txt`: add `tests/test_chord_player.cpp` to `core_tests`. (Both
  `Chords.h` and `ChordPlayerNode.h` are header-only — no source/`APP_SOURCES` change;
  `ChordPlayerNode.h` is pulled in by `Application.cpp` and by the test.)

## Data flow

```
8 patterns (root/oct/chord)         transport.bars()
        │                                  │
        └──► pick pattern at each unit boundary ──► quantize = Bar | Beat
                 Auto: unitAbs % length
                 Manual: pattern selector
        └──► buildChordNotes(root, oct, chord)
        └──► note-offs(old chord) + note-ons(new chord)  ─► MidiRef ─► Arpeggiator
```

## Edge cases

- **Target unchanged at a boundary** (auto with `length == 1`, or manual unchanged) →
  the chord sustains; no retrigger, no note spam.
- **Transport paused/stopped** (`playing == false`) → flush all sounding notes
  (note-offs) and re-prime; resume re-fires the current pattern on the first boundary.
- **Editing a pattern's controls while it sustains** → takes effect the next time that
  pattern becomes active (a boundary). Consistent with "takes effect on the next bar".
- **`length`** clamped to `[1,8]`; **`pattern`/`root`/`oct`/`chord`** clamped to valid
  ranges; **chord notes** clamped to `[0,127]` (out-of-range notes dropped).
- **`transport == nullptr`** (unit tests with no transport) → treated as not playing →
  flush + no output, until a playing transport is supplied.
- **Transport looping/scrubbing** → the auto pattern is `unitAbs % length`, derived
  from the absolute bar/beat position, so it stays aligned through loops and seeks.

## Testing

GL-free doctest under `core_tests` — `tests/test_chord_player.cpp`:

- **`buildChordNotes`** — `C, oct 4, maj` → `{60,64,67}`; `A, oct 4, min` → `{69,72,76}`;
  a 7th chord yields 4 notes; raising the octave by 1 adds 12 to every note; a very high
  octave clamps/drops notes past 127 but still returns the root.
- **First play fires the chord** — drive the node with a `Transport` (playing, 120 BPM)
  at bar 0; assert the frame emits the pattern-1 chord's note-ons (count + notes).
- **Auto-progress advances per bar** — step the transport to bar 1; assert the frame
  emits note-offs for pattern-1's notes and note-ons for pattern-2's notes (with the two
  patterns set to different chords). With `length = 1`, crossing a bar emits nothing (the
  chord sustains).
- **Manual selection quantizes** — `mode = Manual`; set `pattern = 3` mid-bar (no
  boundary) → no switch that frame; cross the next bar boundary → switch to pattern 3.
- **Stop flushes** — after a chord is sounding, set `playing = false`; assert the frame
  emits note-offs for all sounding notes and nothing is left hanging.

(No `gl_smoke` scenario — the node is GL-free and fully covered in `core_tests`. The node
renders in the editor; verify with `--screenshot` during implementation.)

## Docs

- **README.md** — add a **Chord Player** row to the module table (8 chord patterns →
  MIDI; per-pattern root+octave+chord, quantized Bar/Beat switching, auto-progress with a
  `length` loop or manual `pattern` selection; wire into the Arpeggiator).
- **CLAUDE.md** — a brief Architecture bullet: `ChordPlayerNode`
  (`src/modules/ChordPlayerNode.h`, header-only, GL-free) holds 8 root+octave+chord
  patterns and emits one as a chord on its `midi` output, switching on a transport-synced
  Bar/Beat boundary (auto-progress `unitAbs % length` or manual selection), releasing the
  prior chord's notes on every switch/stop; chord intervals live in the GL-free
  `core/Chords.h`. Wire into the Arpeggiator.
</content>
