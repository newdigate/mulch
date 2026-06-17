# LFO Module — Design

**Date:** 2026-06-17
**Status:** Approved (brainstorming) — ready for implementation plan

## Goal

A Low-Frequency Oscillator node that outputs a control-rate `Float` modulation
signal, for wiring into any `Float` parameter (e.g. a Sine node's `amp`). The user
picks a waveform, runs it free (in Hz) or synced to the project BPM (in bar
divisions), and maps its swing into a `[min, max]` output range. Every control is an
input port, so waveform, rate, and sync can each be driven by another node — LFOs
chain.

## Architecture

The LFO is a pure GL-free, UI-free node (`src/modules/LfoNode.h`, header-only, like
`SineWaveNode`/`AutomationNode`). It reads the global transport (for BPM sync) and
`dt` (for free-run phase) from its `EvalContext` and writes one `Float` per frame.

Because the node editor only renders a generic inline widget per input port (and
nodes can't draw their own UI — `src/core/`/`src/modules/` are UI-free), the one
cross-cutting change is a **reusable "choice" inline widget**: a `Port` may carry a
list of string labels; when present, the editor renders an `ImGui::Combo` (editing an
integer index stored as the port's float value) instead of a slider. The waveform
menu and the sync-rate menu are both choice ports. This keeps the LFO entirely
port-based — no LFO-specific UI code — and the choice widget is available to any
future node.

Every control being an input port is what makes the chaining requirements fall out
for free: an unconnected input shows its widget; a connected input takes the wired
value. So "switch waveform via an input", "drive the rate from an input", and "chain
LFOs" are all just wiring another node's `Float` output into the relevant port.

## Components

### `src/modules/LfoNode.h` (new, GL-free, header-only)

**Ports** (indices are stable; `evaluate` reads by index):

| # | Port | Type | Default | Notes |
|---|------|------|---------|-------|
| in 0 | `waveform` | Float (choice) | 0 (Sine) | choices: Sine, Triangle, Square, Ramp Up, Ramp Down, Sample & Hold |
| in 1 | `sync` | Bool | false | off → free (Hz); on → synced (bars) |
| in 2 | `rate Hz` | Float | 1.0 | slider [0.01, 40] Hz; used when `sync` is off |
| in 3 | `rate sync` | Float (choice) | 8 ("1 bar") | choices = the 15 bar divisions below; used when `sync` is on |
| in 4 | `min` | Float | 0.0 | output-range low; slider [-1, 1] |
| in 5 | `max` | Float | 1.0 | output-range high; slider [-1, 1] |
| out 0 | `out` | Float | — | the modulation value |

The `[-1, 1]` slider range on `min`/`max` covers the stated uses (amp `[0,1]`,
gentle `[0.5,1]`, bipolar `[-1,1]`). Wider output ranges (e.g. a frequency sweep) are
a deliberate non-goal for this change.

**Waveform enum** (index order matches the `waveform` choices):
`Sine=0, Triangle=1, Square=2, RampUp=3, RampDown=4, SampleHold=5`.

**Sync division table** (period in bars, slow → fast), 15 entries:

```
index : label      : bars
 0 : "32 bars"   : 32
 1 : "24 bars"   : 24
 2 : "16 bars"   : 16
 3 : "12 bars"   : 12
 4 : "8 bars"    : 8
 5 : "6 bars"    : 6
 6 : "4 bars"    : 4
 7 : "2 bars"    : 2
 8 : "1 bar"     : 1
 9 : "1/2 bar"   : 0.5
10 : "1/4 bar"   : 0.25
11 : "1/8 bar"   : 0.125
12 : "1/16 bar"  : 0.0625
13 : "1/32 bar"  : 0.03125
14 : "1/64 bar"  : 0.015625
```

**Pure waveform helper** (free function in the header, testable; S&H is handled by
the node since it is stateful), `p` in `[0,1)` → `[0,1]`:

```cpp
inline double lfoSample(int wf, double p) {
    switch (wf) {
        case 0: return 0.5 + 0.5 * std::sin(kTwoPi * p);   // Sine
        case 1: return p < 0.5 ? 2.0 * p : 2.0 - 2.0 * p;  // Triangle (0->1->0)
        case 2: return p < 0.5 ? 1.0 : 0.0;                // Square (50% duty)
        case 3: return p;                                  // Ramp Up
        case 4: return 1.0 - p;                            // Ramp Down
        default: return 0.0;                               // Sample & Hold: node supplies
    }
}
```

**`evaluate(EvalContext& ctx)`** (pseudocode):

```
wf   = clamp(round(in<float>(0)), 0, 5)
sync = in<bool>(1)
lo   = in<float>(4);  hi = in<float>(5)
newCycle = false

if (sync):
    periodBars = kDivisionBars[ clamp(round(in<float>(3)), 0, 14) ]
    bars       = ctx.transport ? ctx.transport->bars() : 0.0
    cycles     = periodBars > 0 ? bars / periodBars : 0.0
    cyc        = floor(cycles)
    phase01    = cycles - cyc
    if (cyc != lastCycle_) { newCycle = true; lastCycle_ = cyc; }
else:
    hz      = in<float>(2)
    phase_ += hz * ctx.dt
    if (phase_ >= 1.0) newCycle = true
    phase_ -= floor(phase_)            // wrap to [0,1)
    phase01 = phase_

if (newCycle) shVal_ = uniform01(rng_)             // re-latch Sample & Hold

w01 = (wf == 5) ? shVal_ : lfoSample(wf, phase01)  // normalised [0,1]
ctx.out<float>(0, (float)(lo + w01 * (hi - lo)))
```

**State:** `double phase_ = 0.0;` (free-run phase), `long long lastCycle_ = 0;` (sync
S&H cycle tracker), `double shVal_;` (held S&H value), `std::mt19937 rng_{0x9E3779B9u};`
`std::uniform_real_distribution<double> uni_{0.0, 1.0};`. `shVal_` is seeded with one
draw in the constructor (fixed RNG seed → deterministic, testable). Constants:
`kTwoPi = 6.283185307179586`.

**Behaviour notes:**
- *Free (sync off):* integrates `hz * dt` every frame, so it free-runs continuously
  even while the transport is stopped (it's a modulator, not playback-bound).
- *Sync (sync on):* phase is derived from `transport.bars()`, so it is locked to song
  position and only advances while the transport plays — phase-aligned, deterministic.
- *Mode/rate switches* may cause a phase jump (free `phase_` and sync phase are
  independent); acceptable for an LFO.
- Waveform / sync-rate inputs round-to-nearest and clamp to a valid index, so a
  modulating source wired in sweeps cleanly through the options.

### `src/core/Port.h` (modify)

Add `std::vector<std::string> choices;` (default empty) as the last member. Existing
aggregate brace-inits (`{name, dir, type, def}` and `{…, lo, hi}`) stay valid —
`choices` falls back to its default-empty initializer. Add `#include <string>` /
`#include <vector>` if not already present.

### `src/core/Node.h` (modify)

Add a protected helper (core stays GL/UI-free — `choices` is just strings):

```cpp
// A Float input rendered as a dropdown of `labels`; its value is the selected
// index. Used for enum-like parameters (e.g. an LFO waveform).
void addChoiceInput(std::string n, std::vector<std::string> labels, int def) {
    Port p;
    p.name = std::move(n);
    p.direction = Direction::Input;
    p.type = PortType::Float;
    p.defaultValue = Value((float)def);
    p.minVal = 0.0f;
    p.maxVal = labels.empty() ? 0.0f : (float)(labels.size() - 1);
    p.choices = std::move(labels);
    inputs_.push_back(std::move(p));
}
```

### `src/ui/PortWidgets.cpp` (modify)

In the `PortType::Float` case, branch on `choices`: render a combo when present, the
existing slider otherwise.

```cpp
case PortType::Float: {
    if (!port.choices.empty()) {
        int idx = (int)std::lround(std::get<float>(v));
        idx = std::clamp(idx, 0, (int)port.choices.size() - 1);
        if (ImGui::BeginCombo("##choice", port.choices[idx].c_str())) {
            for (int k = 0; k < (int)port.choices.size(); ++k) {
                bool sel = (k == idx);
                if (ImGui::Selectable(port.choices[k].c_str(), sel)) v = Value((float)k);
                if (sel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
    } else {
        ImGui::SliderFloat("##f", &std::get<float>(v), port.minVal, port.maxVal);
    }
    break;
}
```

Add `#include <algorithm>` and `#include <cmath>` for `std::clamp`/`std::lround`.

### `src/app/Application.cpp` (modify)

- `#include "modules/LfoNode.h"`.
- In `makeNode()`: `if (type == "LFO") return std::make_unique<LfoNode>();`.
- In `nodeCategories()`: the `Control` category becomes `{ "Automation", "LFO" }`.

### `CMakeLists.txt` (modify)

Add `tests/test_lfo.cpp` to `core_tests`. (No new `.cpp`: `LfoNode.h` is header-only;
the `Port`/`Node` changes are headers; the `PortWidgets.cpp` change is already built
into `shader_streamer`.)

## Data flow

```
Sine.amp  <--- LFO.out          (tremolo: LFO maps a waveform into [min,max] -> amp)
LFO_B.out --> LFO_A.rate Hz     (chain: LFO_B modulates LFO_A's frequency)
LFO_C.out --> LFO_A.waveform    (chain: sweep LFO_A's shape; rounds to an index)

LFO.evaluate(ctx):
  sync off -> phase from integrating rate Hz * dt   (free-run, transport-independent)
  sync on  -> phase from ctx.transport->bars() / periodBars   (locked to song)
  out = min + waveform(phase) * (max - min)
```

## Edge cases

- `ctx.transport == nullptr` (unit tests, or sync on with no transport): `bars = 0`,
  so phase is 0 and the output holds the waveform's value at phase 0.
- Connected choice input carrying an out-of-range float: rounded then clamped to a
  valid index — never indexes out of bounds.
- `min == max`: output is constant at that value (degenerate range, harmless).
- `rate Hz` is slider-bounded ≥ 0.01, so free-run phase is monotonic (no negative
  rate); the wrap guard still handles any `hz*dt ≥ 1` (fast rate / long frame) by
  wrapping with `floor`.
- Sample & Hold: a wrap (`newCycle`) re-latches a new random value; within a cycle
  the value is constant.

## Testing

GL-free doctest unit tests in `tests/test_lfo.cpp` (run under `core_tests`):

- **`lfoSample`** at key phases: Sine 0→0.5, 0.25→1.0, 0.5→0.5, 0.75→0.0; Triangle
  0→0, 0.25→0.5, 0.5→1.0; Square 0.25→1.0, 0.75→0.0; Ramp Up 0.3→0.3; Ramp Down
  0.3→0.7.
- **Free mode** (build an `EvalContext` directly, `transport=nullptr`): Sine, `rate
  Hz = 1`, `min=0,max=1`; eval `dt=0` → 0.5; eval `dt=0.25` → ~1.0. Range mapping:
  `min=0,max=2`, at the peak → ~2.0.
- **Sync mode** (a `Transport` with `bpm=120` → 2 s/bar, `seconds=0.5` → `bars=0.25`;
  `rate sync = "1 bar"`, Sine): phase 0.25 → output ~1.0. Confirms BPM-derived phase.
- **Choice rounding/clamp:** `waveform` input `1.4` behaves as Triangle (index 1);
  `9.0` clamps to Sample & Hold (index 5), not out of range.
- **Sample & Hold:** value is constant across same-cycle evals and takes ≥ 2 distinct
  values across several cycles (not stuck); deterministic given the fixed seed.
- **Choice-port construction:** an `LfoNode`'s `inputs()[0].choices.size() == 6` and
  its default value is 0; `inputs()[3].choices.size() == 15`.

The combo widget itself is UI and isn't unit-tested; it's exercised in the running
app and the `--screenshot` capture.

## Docs

- **README.md** — add an **LFO** row to the module table (waveform; free Hz / BPM-sync
  bar divisions; `[min,max]` output; all controls are inputs so LFOs chain).
- **CLAUDE.md** — note the new GL-free `LfoNode`, and the reusable choice-port inline
  widget (`Port::choices` + `Node::addChoiceInput` + the combo in `PortWidgets.cpp`).
