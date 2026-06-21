# MIDI Beat Clock Sync — Design (Pass 1 of 2)

**Date:** 2026-06-21
**Status:** Approved (brainstorming) — ready for implementation plan

## Goal

Enable/disable **receiving** MIDI Beat Clock (24-PPQN clock + Start/Stop/Continue + Song
Position Pointer) from a selected MIDI input to drive the transport's tempo, position, and
play state; and enable/disable **sending** Beat Clock from the transport to a selected MIDI
output, with precise outgoing tick timing via a dedicated sender thread. Configured in a new
**Sync** tab in Preferences. This pass builds the whole sync framework; **Pass 2 (MTC)** adds
SMPTE timecode as a second mode reusing all of it.

## Architecture

The protocol math is GL-free and unit-tested (`core/MidiClock`). The rtmidi I/O lives in an
app-level `MidiSyncEngine`: sync-**in** is polled each frame on the main thread (rtmidi
timestamps incoming messages, so derived tempo is accurate regardless of poll cadence), and
sync-**out** runs on a dedicated high-resolution timer thread (so outgoing clock ticks are
sample-tight, not frame-quantized). Preferences flow to the engine; the engine drives/reads the
`Transport`.

### Unit 1 — `core/Preferences` sync fields

Add to `struct Preferences`:
```cpp
    int         syncInMode  = 0;   // 0 = Off, 1 = Beat Clock   (2 = MTC in Pass 2)
    std::string syncInSource;      // MIDI input port name (empty = none)
    int         syncOutMode = 0;   // 0 = Off, 1 = Beat Clock
    std::string syncOutDest;       // MIDI output port name (empty = none)
```
Serialized as `sync-in <mode> <port name>` and `sync-out <mode> <port name>` lines (mode int +
rest-of-line port name); parsed back, mode clamped to `[0,1]` this pass (Pass 2 widens to 2).

### Unit 2 — `core/Transport` external clock

Add `bool externalClock = false;`. `advance(dt)` returns immediately when `externalClock` is
true — the sync engine writes `bpm`/`seconds`/`playing` directly each frame, so the local
clock doesn't double-advance. (While sync-in is active the toolbar's tempo/play reflect the
incoming sync; toolbar edits are overwritten next frame — acceptable, noted in docs.)

### Unit 3 — `core/MidiClock.{h,cpp}` (new, GL-free, unit-tested)

Pure protocol math — no rtmidi, no threads:

```cpp
// Beat Clock RECEIVER: fed timestamped clock events; derives tempo + position + play state.
class BeatClockReader {
public:
    void onTick(double tSeconds);        // a 0xF8; averages recent 24-PPQN intervals -> bpm; +1/24 beat
    void onStart();                      // playing = true, position = 0
    void onContinue();                   // playing = true
    void onStop();                       // playing = false
    void onSongPosition(int sixteenths); // SPP: position = sixteenths/4 beats; resets tick phase
    double bpm() const;                  // 0 until enough ticks seen (then the engine keeps the old bpm)
    double positionBeats() const;        // locateBeats + ticksSinceLocate/24
    bool   playing() const;
    void   reset();
};

// Message helpers (also used by the sender thread), all pure + testable:
constexpr unsigned char kMidiClock = 0xF8, kMidiStart = 0xFA, kMidiContinue = 0xFB, kMidiStop = 0xFC;
int  beatsToSixteenths(double beats);                 // round(beats * 4)
void sppMessage(int sixteenths, unsigned char out[3]);// {0xF2, lsb, msb}  (14-bit, 7 bits each)
int  sppToSixteenths(unsigned char lsb, unsigned char msb);  // (msb<<7)|lsb
```
- Tempo: keep the last K (e.g. 24) tick timestamps; `bpm = 60 / (avgInterval * 24)`.
- Position: `positionBeats = locateBeats + ticksSinceLocate/24.0`; `onSongPosition` sets
  `locateBeats = sixteenths/4` and zeroes `ticksSinceLocate`; `onTick` increments it while playing.

### Unit 4 — `app/MidiSyncEngine.{h,cpp}` (rtmidi in `.cpp`; sender thread)

Owns the sync-in `RtMidiIn`, the sync-out `RtMidiOut` + its sender thread, and a small
mutex-guarded `SyncOut` snapshot. `<RtMidi.h>` stays confined to the `.cpp`.

- **`update(Transport& t, const Preferences& p, double /*dt*/)`** — runs on the main thread each
  frame:
  - **Sync-in:** if `p.syncInMode == 0` → close the input, set `t.externalClock = false`. Else
    open `p.syncInSource` (reopen on change; `ignoreTypes(false,false,false)` so clock/SPP arrive),
    drain it with `getMessage` (rtmidi timestamps each message), dispatch into the
    `BeatClockReader` (0xF8→`onTick(stamp)`, 0xFA→`onStart`, 0xFB→`onContinue`, 0xFC→`onStop`,
    0xF2→`onSongPosition(sppToSixteenths(...))`). Then set `t.externalClock = true`,
    `t.playing = reader.playing()`, and (when the reader has a tempo) `t.bpm = reader.bpm()` and
    `t.seconds = reader.positionBeats() * t.secondsPerBeat()`.
  - **Sync-out:** publish the current `{ enabled = (syncOutMode==1), portName = syncOutDest,
    playing = t.playing, bpm = t.bpm, posBeats = t.beats() }` into the mutex-guarded `SyncOut`.
- **Sender thread** (started in the ctor, stopped+joined in the dtor *before* freeing the
  `RtMidiOut`): owns the `RtMidiOut`; each loop reads the `SyncOut` snapshot:
  - Open/reopen `portName` when it changes (the thread is the only toucher of the out port);
    closed when `!enabled`.
  - On `playing` false→true: send SPP(posBeats) then **Start** (posBeats≈0) or **Continue**, and
    seed the internal tick position to `posBeats`. On true→false: send **Stop**.
  - While playing: send `0xF8` at `60/(bpm*24)` intervals using `std::chrono::steady_clock` +
    `sleep_until`, advancing the internal tick position; recompute the interval when `bpm` changes.
  - If `posBeats` diverges from the internal tick position beyond a small threshold (a scrub/locate
    on the main thread): send SPP(posBeats) and reseat the internal position. When stopped/disabled,
    sleep in short slices so it stays responsive to enable/port changes.

### Unit 5 — `PreferencesPanel` "Sync" tab

A new tab with two sections (reusing the panel's enumerated `midiIns_`/`midiOuts_` lists):
- **Sync In:** a mode combo (Off / Beat Clock) and a source combo (None + each input port) →
  sets `syncInMode` / `syncInSource`.
- **Sync Out:** a mode combo (Off / Beat Clock) and a destination combo (None + each output port)
  → sets `syncOutMode` / `syncOutDest`.
Each change calls `onChange()` (persists, as the other tabs do).

### Unit 6 — `Application` wiring

Own a `MidiSyncEngine syncEngine_;`. In `frame()`, call
`syncEngine_.update(graph_.transport(), prefs_, dt)` **before** `graph_.evaluate(dt)` (so a
synced transport is set before nodes evaluate, and `externalClock` suppresses the local advance).

## Data flow

```
external MIDI ──► sync-in port (poll, timestamped) ──► BeatClockReader ──► Transport(externalClock) ──► nodes
Transport ──(snapshot each frame)──► SyncOut{bpm,playing,posBeats,port} ──► sender thread ──► sync-out port ──► external MIDI
```

## Edge cases / concurrency

- **Sender thread lifetime:** ctor starts it, dtor sets a `running=false` flag, `join()`s it,
  THEN destroys the `RtMidiOut` — the thread is the only one touching the out port, so no
  cross-thread rtmidi access and no use-after-free.
- **Shared `SyncOut`** is a tiny POD under one `std::mutex` (one writer/frame, one reader/tick —
  trivial contention); the port-name string is copied under the lock.
- **No tempo yet** (fewer than 2 ticks received) → `bpm()` returns 0; the engine keeps the
  transport's existing bpm until real ticks arrive (no divide-by-zero / 0-bpm).
- **Sync-in off** → `externalClock=false`, local clock resumes (toolbar drives it again).
- **Selected port gone / open fails** → caught; that direction is a silent no-op until the port
  returns or the selection changes (mirrors the node MIDI behavior).
- **Clock only flows while playing** (between Start and Stop) — standard; stopped → no ticks.
- **Per-frame send jitter is eliminated** by the dedicated thread (the point of this design).
- **Sync-in + sync-out to the same device** would echo the received clock back (a feedback
  loop) — the user picks distinct ports; documented, not specially prevented.

## Testing

- **`tests/test_midi_clock.cpp`** (`core_tests`, GL-free):
  - `BeatClockReader`: feed `onStart()` then 24 `onTick` at 20.8333 ms spacing → `bpm() ≈ 120`,
    `positionBeats() ≈ 1.0`, `playing()` true; `onStop()` → playing false; `onSongPosition(16)` →
    `positionBeats() == 4`.
  - Helpers: `beatsToSixteenths(4.0)==16`; `sppMessage(16,out)` → `{0xF2,16,0}`;
    `sppToSixteenths(16,0)==16`; a beats→SPP→sixteenths round-trip.
- **`tests/test_preferences.cpp`** (extend): `sync-in`/`sync-out` mode + port-name round-trip;
  a mode out of range clamps to `[0,1]`.
- **Build + manual:** the rtmidi engine + sender thread + the Sync tab are verified by a clean
  build and a manual run against an IAC bus / hardware (sync to a DAW's clock; send clock to a
  DAW and watch it follow). `gl_smoke` is unaffected (no graph/UI change there).

## Docs

- **README.md** — extend the Preferences note: a **Sync** tab to receive MIDI Beat Clock (drive
  the transport tempo/position from a MIDI input) and send it (to a MIDI output, with a precise
  timer thread).
- **CLAUDE.md** — an Architecture bullet: the GL-free `core/MidiClock` (Beat Clock receiver +
  SPP/message helpers, unit-tested); the app-level `MidiSyncEngine` (rtmidi confined to its
  `.cpp`) that polls sync-in on the main thread to drive the `Transport` (via a new
  `Transport::externalClock`) and sends clock from a dedicated timer thread; the Preferences
  `syncIn/Out` fields + Sync tab. Note MTC is the planned Pass-2 mode reusing this.
</content>
