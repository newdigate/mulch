# Chord Player Presets — Design

**Date:** 2026-06-21
**Status:** Approved (brainstorming) — ready for implementation plan

## Goal

Add **8 presets** to the `ChordPlayerNode`, each holding a complete chord progression (the
node's existing 8-step root/oct/chord sequence + a loop length). Eight buttons in the node body
switch the active preset; a MIDI input can switch it too; switching is quantized to
**Immediate / Next Beat / Next Bar / Next 4 Bars**. All 8 presets are seeded with real musical
progressions and are saved/loaded with the project. The node stays GL-free and unit-tested.

## Architecture

The node keeps its current within-preset behavior unchanged: an 8-step sequence that auto-steps
(or is manually stepped) on a Bar/Beat boundary, releasing prior notes cleanly. A **preset layer**
sits on top: `presets_[8]` (each a full sequence) is the source of truth for playback; the
**active** preset is mirrored into the existing per-step input ports so the current inline chord
editors edit it in place.

Two layering facts drive the design:
- **`core`/`modules` must stay ImGui-free** (the node is unit-tested in `core_tests`), so the
  "8 buttons" cannot be drawn by the node. A small **generic, GL-free button-bank hook** is added
  to `Node` (ints/strings only, like `statusLine()`); `NodeEditorPanel` renders it for any node.
- **`ctx.in` is resolved at frame start.** Writing an input default mid-`evaluate` does not change
  `ctx.in` that frame. So **playback reads from the `presets_` struct**, not from `ctx.in`; the
  ports are purely an editable mirror, captured into the active preset each frame.

### Unit 1 — Generic `Node` button-bank hook (`src/core/Node.h`)

Add five virtuals (defaults make them inert for every existing node — no other node changes):
```cpp
    virtual int         buttonCount() const { return 0; }       // 0 = no button bank
    virtual std::string buttonLabel(int /*i*/) const { return {}; }
    virtual int         buttonActive() const { return -1; }     // index drawn highlighted
    virtual int         buttonPending() const { return -1; }    // index drawn as "pending" (optional)
    virtual void        onButtonPressed(int /*i*/) {}           // a button was clicked
```
GL-free (only `int`/`std::string`), so `core` stays clean and the node remains testable.

### Unit 2 — `NodeEditorPanel` renders the button bank (`src/ui/NodeEditorPanel.cpp`)

In the per-node body, after the `statusLine()` line and before the input pins, if
`n.buttonCount() > 0` draw a row of `buttonCount()` small buttons (`buttonLabel(i)`), wrapping a
few per row. Style the `buttonActive()` button with a pushed accent colour and the
`buttonPending()` button (if `>= 0` and `!= active`) with a dimmer accent. On click call
`n.onButtonPressed(i)`. This is the only editor change; it is generic (any future node can expose a
button bank). All ImGui stays in the `.cpp`.

### Unit 3 — `ChordPlayerNode` preset model + switching (`src/modules/ChordPlayerNode.h`)

**Data.** A preset is `struct Preset { int root[8], oct[8], chord[8]; int length; };` and the node
owns `Preset presets_[8]`, `int activePreset_`, `int requestedPreset_`. `presets_` is the playback
source of truth.

**Ports.** Keep the existing 0..27 ports (24 step controls + `mode` 24, `quantize` 25, `length` 26,
`pattern` 27) so existing project files keep their indices. **Append** two new inputs:
- `28 switch` — `addChoiceInput("switch", {"Immediate","Beat","Bar","4 Bars"}, 2)` (default Bar).
- `29 select` — `addInput("select", PortType::Midi)`; a MIDI input whose note-ons request a preset.

**Button bank.** `buttonCount()=8`, `buttonLabel(i)=to_string(i+1)`, `buttonActive()=activePreset_`,
`buttonPending()= requestedPreset_ != activePreset_ ? requestedPreset_ : -1`,
`onButtonPressed(i){ requestedPreset_ = clamp(i,0,7); }`.

**Mirror.** `writePorts(const Preset&)` sets the 24 step input defaults + the `length` default via
`inputDefault(i)`. `captureActive(EvalContext&)` reads the active preset's 8 steps + length from
`ctx.in<float>` back into `presets_[activePreset_]` (so live edits — and any automation on a step
control — flow into the active preset). The constructor seeds `presets_` with the defaults (Unit 4)
and `writePorts(presets_[0])` so a fresh node shows preset 1.

**`evaluate(ctx)` order** (replaces the body, keeping the existing stepper):
1. Read `mode`, step-`quantize`, `length`, manual `pattern`, and `switch`.
2. Scan the `select` MIDI input (`ctx.in<MidiRef>(29)`) for note-ons; for each, `pc = note % 12`;
   if `pc` in `[0,7]` set `requestedPreset_ = pc` (last note-on in the frame wins; C–G → 1–8).
3. `captureActive(ctx)` — persist the active preset's current port values into `presets_`.
4. **Preset switch:** with `switch` mapping to a unit (`Immediate→0`, `Beat→1/beatsPerBar`,
   `Bar→1`, `4 Bars→4`): if `requestedPreset_ != activePreset_` and (`Immediate`, or the transport
   is stopped, or a switch-unit boundary was crossed this frame — `floor(bars/unit)` changed), then
   `activePreset_ = requestedPreset_`, `writePorts(presets_[activePreset_])`, and force a chord
   re-fire (release the sounding notes, build + note-on the new active preset's current step).
5. **Within-preset stepping (existing logic, now reading `presets_[activePreset_]`):** auto-step
   `unitAbs % length` on the Bar/Beat unit, or use the manual `pattern`; on a step change or a
   forced re-fire, note-off the old notes and note-on the new chord; release everything on stop.
6. Emit the frame's events on the `midi` output.

Playback (`buildChordNotes`) reads `root/oct/chord` from `presets_[activePreset_]`, not `ctx.in`,
so a same-frame switch fires the correct new chord despite `ctx.in` being a frame stale.

### Unit 4 — Default progressions (`src/modules/ChordPlayerNode.h`)

Seed the 8 presets from the 14-chord set (`maj min dim aug sus2 sus4 maj7 min7 dom7 6 m6 m7b5
dim7 add9`; roots `C..B`), octave 4, in a GL-free `defaultPresets()` helper:

| # | Name | Steps | len |
|---|------|-------|-----|
| 1 | Pop I–V–vi–IV | C·G·Am·F | 4 |
| 2 | Doo-wop I–vi–IV–V | C·Am·F·G | 4 |
| 3 | Jazz ii–V–I turnaround | Dm7·G7·Cmaj7·Am7 | 4 |
| 4 | Andalusian i–♭VII–♭VI–V | Am·G·F·E | 4 |
| 5 | vi–IV–I–V | Am·F·C·G | 4 |
| 6 | 12-bar blues | C7·C7·C7·C7·F7·F7·C7·G7 | 8 |
| 7 | Pachelbel | C·G·Am·Em·F·C·F·G | 8 |
| 8 | Minor ii°–V–i | Bm7♭5·E7·Am | 3 |

(Steps beyond a preset's `length` are filled with that preset's first chord so a manual step or a
later length increase is still musical.)

### Unit 5 — Persistence (`src/modules/ChordPlayerNode.h`)

`saveState()` serializes the active index + all 8 presets as a comma/semicolon-delimited string:
`"<active>;<p0>;…;<p7>"`, each preset `"<length>,<r0>,<o0>,<c0>,…,<r7>,<o7>,<c7>"` (25 ints). For
the **active** preset it reads the live port values (const access to `inputs()[i].defaultValue`) so
an edit made in the same frame as a save isn't lost; the other 7 come from `presets_`. No spaces /
`:` / `|` (the project file escapes the state line anyway). `loadState(s)` parses, clamps, fills
`presets_` + `activePreset_`, and `writePorts(presets_[activePreset_])`.

The global controls (`mode`, `quantize`, `length`, `pattern`, `switch`) persist as control-input
defaults through the existing project save, unchanged.

**Migration.** Old projects have the 24 step ports but no node-state line → `loadState` isn't
called, so `presets_` keeps its constructor defaults while the ports hold the old chords; frame 1's
`captureActive` copies those old chords into `presets_[0]`, so an old project's single sequence
becomes preset 1 with no special-case code.

## Data flow

```
buttons / select-MIDI ──► requestedPreset_ ──(quantize boundary)──► activePreset_
                                                     │
ports (active preset, editable) ──captureActive──► presets_[8] (truth) ──build chord──► midi out
                                  ◄──writePorts── (on switch / load)
saveState/loadState ◄──► all 8 presets + active index (escaped node-state line)
```

## Edge cases

- **Switch while stopped** → applies immediately (no transport to quantize against); nothing sounds
  until play, then the active preset's current step fires.
- **Same-frame switch** → chord built from `presets_` (not the frame-stale `ctx.in`), so the new
  preset's chord is correct; old notes are released first (no hang), matching today's behavior.
- **MIDI notes G♯–B** (`pc` 8–11) on `select` → ignored (only C–G select). Note-offs ignored.
- **Pending request never reached** (e.g. user clicks several buttons before the next bar) →
  `requestedPreset_` just holds the latest; only the final value switches at the boundary.
- **Connected/automated step control** → `captureActive` reads `ctx.in`, so automation on a step
  flows into the active preset; rare but well-defined.
- **`length` differs per preset** → stepping is stateless `unitAbs % length`, so switching to a
  preset with a different length stays bar-aligned and loop-robust.
- **Stop** → all sounding notes released and the stepper re-primed (unchanged); `activePreset_` is
  retained.

## Testing

`tests/test_chord_player.cpp` (extend, `core_tests`, GL-free — drive the node with a mock
`Transport` + `EvalContext`, as the existing tests do):
- Switching the active preset (via `onButtonPressed`) with `switch=Immediate` changes the emitted
  chord and releases the previous notes (note-offs for the old chord, note-ons for the new).
- `switch=Bar` defers the switch to the next bar boundary off a mock transport (no change mid-bar;
  change at the boundary).
- The `select` MIDI input maps note-on C–G → presets 1–8 and respects the quantize.
- `saveState()` → `loadState()` round-trips all 8 presets (every step's root/oct/chord + length)
  and the active index; a fresh node's `loadState("")` leaves the constructor defaults intact.
- The button hook: `buttonCount()==8`, `buttonActive()` follows `activePreset_`, `buttonPending()`
  reflects a not-yet-applied request, `onButtonPressed(i)` requests preset `i`.
- A default-content sanity check: each `defaultPresets()` entry has a valid length and in-range
  root/oct/chord indices.

The button **rendering** in `NodeEditorPanel` is interactive ImGui — build-and-manual-verified
(like the rest of the editor); a `--screenshot` run confirms the node + button row draw without
crashing.

## Documentation

- **README.md** — rewrite the Chord Player module row: 8 preset progressions, switch via the 8
  buttons / a MIDI `select` input (note C–G), quantized Immediate/Beat/Bar/4 Bars; each preset is
  the existing 8-step sequence; all presets saved/loaded.
- **CLAUDE.md** — update the Chord Player bullet: the preset layer (`presets_[8]` truth + active
  mirrored to the step ports, captured each frame), the generic GL-free `Node` button-bank hook
  rendered by `NodeEditorPanel`, the MIDI `select` mapping, the quantized switch, and `saveState`
  carrying all 8 presets.

## Out of scope (YAGNI)

- Per-preset `mode`/`quantize`/`pattern` (these stay global).
- Renaming presets, copy/paste between presets, or more/fewer than 8 presets or 8 steps.
- A Float index input (the chosen switch input is MIDI note + the buttons).
- Changing the chord set, the within-preset stepping, or the note-release behavior.
