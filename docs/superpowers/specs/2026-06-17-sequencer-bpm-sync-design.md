# Step Sequencer + Arpeggiator BPM Sync — Design

**Date:** 2026-06-17
**Status:** Approved (brainstorming) — ready for implementation plan

## Goal

Let the Step Sequencer and Arpeggiator optionally lock to the global transport: a
`sync` toggle that swaps the free-rate control for a musical step division and
derives each step from `transport.bars()`, so both nodes follow the project tempo,
start/stop with the transport, stay bar-aligned, and survive looping. Free mode (the
existing `tempo` / `rate` controls) is unchanged, so sync is opt-in and backward
compatible.

## Architecture

Mirror the LFO's free/sync split. Each node gains two **appended** input ports (so
existing port indices are unchanged): `sync` (Bool, default off) and `rate sync` (a
choice dropdown of step divisions, default `1/16`). `evaluate` branches:

- **sync off** → the existing free-running clock (`clock_ += dt`, `nextStep_`), untouched.
- **sync on** → a **stateless** position-derived step: `stepPos = transport.bars() /
  barsPerStep`, `stepAbs = floor(stepPos)`. Stateless derivation is the key choice —
  it is loop/seek-robust (the step index just follows the transport position and
  re-aligns at every loop seam), whereas an accumulating clock would drift.

The 8 divisions and their bars-per-step live in one shared GL-free helper so both
nodes use the same table.

### `src/core/StepSync.h` (new, GL-free, header-only)

```cpp
#pragma once
#include <string>
#include <vector>

namespace oss {

// Per-step musical divisions for transport-synced sequencers/arps (length of one
// step). Index matches stepDivisionBars(). Assumes 4/4 (beatsPerBar = 4).
inline const std::vector<std::string>& stepDivisionLabels() {
    static const std::vector<std::string> labels = {
        "1/4", "1/8", "1/8.", "1/8T", "1/16", "1/16.", "1/16T", "1/32"
    };
    return labels;
}

// Length of one step in bars for division index `idx` (clamped to a valid index).
inline double stepDivisionBars(int idx) {
    static const double bars[8] = {
        0.25,                 // 1/4   (a beat)
        0.125,                // 1/8
        0.1875,               // 1/8.  (dotted = 1.5x)
        0.125 * 2.0 / 3.0,    // 1/8T  (triplet = 2/3x) ~0.08333
        0.0625,               // 1/16
        0.09375,              // 1/16. (dotted)
        0.0625 * 2.0 / 3.0,   // 1/16T (triplet) ~0.04167
        0.03125               // 1/32
    };
    if (idx < 0) idx = 0;
    if (idx > 7) idx = 7;
    return bars[idx];
}

inline constexpr int kStepDivisionDefault = 4;   // "1/16"

} // namespace oss
```

### Shared sync timing model (both nodes)

State added to each node: `long long lastStepAbs_ = 0;` and `bool primed_ = false;`.
The `primed_` flag makes "press play and the step at the current position fires"
work cleanly and makes stop/pause re-prime so resume doesn't double-trigger.

Sync branch (Step Seq form; Arp is identical except the trigger picks the next note
in the held-note sequence instead of a fixed step toggle, and uses the no-channel
MIDI helpers):

```
const double barsPerStep = stepDivisionBars(div);
if (ctx.transport && ctx.transport->playing && barsPerStep > 0.0) {
    double    stepPos  = ctx.transport->bars() / barsPerStep;
    long long stepAbs  = (long long)std::floor(stepPos);
    double    frac     = stepPos - (double)stepAbs;
    bool      boundary = !primed_ || stepAbs != lastStepAbs_;

    // release the sounding note when a new step starts or its gate has elapsed
    if (active_ >= 0 && (boundary || frac >= gate)) { emit noteOff; active_ = -1; }

    if (boundary) {
        // Step Seq: int s = ((stepAbs % kSteps) + kSteps) % kSteps;
        //           if (ctx.in<bool>(s)) { emit noteOn(note,100,ch); active_=note; activeCh_=ch; }
        // Arp:      if (!seq.empty()) { int n = seq[step_ % seq.size()];
        //                              emit noteOn(n,100); active_=n; ++step_; }
        lastStepAbs_ = stepAbs;
        primed_ = true;
    }
} else {
    // paused / stopped: release any held note and re-prime so resume fires cleanly
    if (active_ >= 0) { emit noteOff; active_ = -1; }
    primed_ = false;
}
```

`gate`: Step Seq uses its constant `kGate = 0.5`; Arp uses its `gate` input
(clamped `[0,1]`).

Behaviour this produces:
- **Play from a step boundary** fires that step (the downbeat).
- **Crossing** an integer step boundary releases the previous note and fires the new
  one; within a step the note releases once `frac >= gate`.
- **Stop/pause** releases the held note and re-primes; **resume** fires the step at
  the current position once (no double-trigger, no stuck note).
- **Loop/seek**: `stepAbs` follows the position (it may decrease), the boundary fires
  the step at the loop point, and everything re-aligns — no drift, no hung notes.
- A frame longer than one step lands on the **current** step rather than replaying
  every skipped one (sub-frame steps aren't audible; a minor, deliberate departure
  from free mode's catch-up loop).

### `src/modules/StepSequencerNode.h` (modify)

- `#include "core/StepSync.h"`.
- Constructor: after the `channel` input add
  `addInput("sync", PortType::Bool, false);` and
  `addChoiceInput("rate sync", stepDivisionLabels(), kStepDivisionDefault);`
  → new input indices: `sync` = `kSteps + 3` (19), `rate sync` = `kSteps + 4` (20).
- Add members `long long lastStepAbs_ = 0;` and `bool primed_ = false;`.
- `evaluate`: read `note`/`channel` (unchanged), then
  `bool sync = ctx.in<bool>(kSteps + 3);`
  `int div = std::clamp((int)std::lround(ctx.in<float>(kSteps + 4)), 0, 7);`
  and branch: sync → the model above; else → the existing free body verbatim (which
  reads `tempo = ctx.in<float>(kSteps + 0)`). Output the `MidiRef` as today.

### `src/modules/ArpeggiatorNode.h` (modify)

- `#include "core/StepSync.h"`.
- Constructor: after the `mode` input add the same two ports → `sync` = 5,
  `rate sync` = 6.
- Add members `long long lastStepAbs_ = 0;` and `bool primed_ = false;`.
- `evaluate`: keep the held-note folding (step 1) at the top; hoist
  `std::vector<int> seq = buildSequence(octaves, mode);` before the branch; read
  `sync` / `div`; branch as above (Arp form) else the existing free body. `step_`
  stays the running index into `seq` in both modes.

## Data flow

```
transport (BPM, bars, playing) ──► StepSeq / Arp (sync on)
    stepPos = bars / barsPerStep ; trigger on floor(stepPos) change ; release at frac >= gate
free `tempo`/`rate` inputs ──► StepSeq / Arp (sync off)   [unchanged clock]
```

## Edge cases

- `transport == nullptr` (e.g. a bare unit test) with sync on → treated as
  not-playing: releases any note, emits nothing.
- `div` out of range → clamped to `[0,7]` by both the reader and `stepDivisionBars`.
- Switching sync on/off mid-run: each path is self-consistent; a note held by one
  path is released by the other's gate/boundary logic (at worst one extra frame).
- Loop wrap mid-note: the boundary change releases the old note before firing the
  loop-point step — no hung note.

## Testing

GL-free doctest, extending the existing files (drive a `Transport` via a manual
`EvalContext{in, out, dt, &t}`, same idiom as the LFO sync test):

- **Update the existing test helpers** (REQUIRED — the node now has more inputs):
  - `tests/test_step_sequencer.cpp` `step()` → build `std::vector<Value> in(21)`,
    with `in[19] = false` (sync off) and `in[20] = (float)kStepDivisionDefault`. The
    existing free-mode assertions then pass unchanged.
  - `tests/test_arpeggiator.cpp` `step()` → build `in(7)` with `in[5] = false`,
    `in[6] = (float)kStepDivisionDefault`.
- **New Step Seq sync case:** pattern with steps 0 and 1 on, `sync` on, `div` = 0
  (`1/4` → 0.25 bar/step), `Transport` at 120 BPM playing. At `bars 0` → one note-on
  (downbeat, step 0). At `bars 0.25` (`seconds = 0.5`) → a note-off + one note-on
  (step 1). At `bars 0.5` → a note-off, no note-on (step 2 off). With the transport
  paused → no note-on (and any active note released).
- **New Arp sync case:** hold `{60, 64}`, `sync` on, `div` = 0, 120 BPM playing. At
  `bars 0` → note-on 60; `bars 0.25` → note-on 64; `bars 0.5` → note-on 60. Confirms
  bar-aligned stepping through the held-note sequence.

No new test files or CMake changes (header-only additions; tests extend existing
files already wired into `core_tests`).

## Docs

- **README.md** — the Step Seq and Arpeggiator rows note the `sync` toggle (lock to
  project BPM over straight/dotted/triplet step divisions; free `tempo`/`rate`
  otherwise).
- **CLAUDE.md** — a brief note that Step Seq/Arp can sync to the transport via
  `core/StepSync.h` (shared division table), deriving the step from `transport.bars()`.
