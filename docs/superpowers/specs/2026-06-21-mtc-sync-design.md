# MTC Timecode Sync — Design (Pass 2 of 2)

**Date:** 2026-06-21
**Status:** Approved (brainstorming) — ready for implementation plan

## Goal

Add **MIDI Time Code (MTC)** as a second sync mode alongside Pass-1's MIDI Beat Clock: receive
SMPTE timecode (HH:MM:SS:FF) from a selected MIDI input to drive the transport position, and send
it to a selected output from the dedicated timer thread. Supports all four standard frame rates,
including proper **29.97 drop-frame**. Configured in the existing Preferences **Sync** tab.

## Architecture

This pass reuses the Pass-1 framework wholesale. The same shape as Beat Clock: a GL-free,
unit-tested codec + reader in `core/`, a mode branch in the app-level `MidiSyncEngine` (receive
polled on the main thread, send on the dedicated timer thread that solely owns the out port),
Preferences fields, and a Sync-tab control. The `syncInMode`/`syncOutMode` ints widen from `[0,1]`
to `[0,2]`, with **2 = MTC** (0 = Off, 1 = Beat Clock).

**The key protocol difference from Beat Clock:** MTC carries absolute SMPTE *wall-clock time*, not
tempo. So MTC drives `Transport::seconds` (the song position) + `Transport::playing`, and leaves
`Transport::bpm` untouched — beats/bars then derive from the local BPM (MTC has no musical grid).

### Unit 1 — `core/MidiTimecode.{h,cpp}` (new, GL-free, unit-tested)

The MTC analogue of `core/MidiClock`. No rtmidi, no threads — pure protocol math.

```cpp
// SMPTE frame-rate codes (the 2 bits carried in MTC's hour byte).
enum class MtcRate { Fps24 = 0, Fps25 = 1, Fps2997df = 2, Fps30 = 3 };

struct SmpteTime { int h = 0, m = 0, s = 0, f = 0; MtcRate rate = MtcRate::Fps30; };

double nominalFps(MtcRate r);        // 24 / 25 / 29.97 / 30
double frameDuration(MtcRate r);     // real seconds per frame (1/fps; 1001/30000 for 29.97 df)
double smpteToSeconds(const SmpteTime& tc);          // real wall-clock seconds
SmpteTime secondsToSmpte(double seconds, MtcRate r); // inverse

// Quarter-frame: piece 0..7 -> the 0nnn_dddd data byte that follows 0xF1.
unsigned char quarterFrameByte(int piece, const SmpteTime& tc);
// Full-frame SysEx: F0 7F 7F 01 01 hh mm ss ff F7.
void fullFrameMessage(const SmpteTime& tc, std::vector<unsigned char>& out);
bool parseFullFrame(const unsigned char* data, std::size_t n, SmpteTime& out);

// MTC RECEIVER: fed quarter-frame + full-frame messages; reassembles + advances position.
class MtcReader {
public:
    void onQuarterFrame(unsigned char data);   // a 0xF1 data byte (piece index + nibble)
    void onFullFrame(const SmpteTime& tc);     // a full-frame locate
    void onIdle(double dtSeconds);             // advance an inactivity timer (no QF -> stop)
    void reset();
    double  seconds() const;                   // current position (real wall-clock)
    bool    playing() const;                   // quarter-frames flowing recently
    MtcRate rate()    const;
};
```

**Drop-frame math.** For 24/25/30 (non-drop): `frame# = (h·3600+m·60+s)·fps + f`,
`seconds = frame#/fps`. For 29.97 drop-frame (drop frame numbers 0 and 1 at the start of every
minute except minutes divisible by 10):
```
totalMin = 60·h + m
frame#   = (h·3600 + m·60 + s)·30 + f − 2·(totalMin − totalMin/10)
seconds  = frame# · 1001 / 30000
```
`secondsToSmpte` inverts this with the standard drop-frame algorithm (compute the 30 fps frame
index from `round(seconds·30000/1001)`, add back the dropped frames, then split into h/m/s/f).

**Reader behavior.** `onQuarterFrame` reads the piece index (`(data>>4)&7`) and nibble
(`data&0xF`), stores the nibble, and advances `seconds_` by `frameDuration/4` (so the position is
smooth at quarter-frame resolution — MTC sends 4 quarter-frames per frame, 8 per full timecode over
2 frames). When a full 0..7 set has been collected, it assembles the SMPTE value (rate from the
piece-7 bits), adds a **2-frame compensation** (the assembled time is the value when piece 0 was
sent), and snaps `seconds_` to that absolute value (correcting drift). It sets `playing_ = true`
and resets the idle timer on each QF. `onIdle(dt)` accumulates inactivity and flips `playing_`
false past a timeout (a few frame durations). `onFullFrame` snaps `seconds_` to the absolute time
(a locate); it does not by itself imply playing. A partial cycle on join doesn't snap until a full
0..7 set is seen.

### Unit 2 — `core/Preferences` frame rate

- Widen the `sync-in` / `sync-out` parse clamp from `[0,1]` to `[0,2]`.
- Add `int syncFrameRate = 3;` (0=24, 1=25, 2=29.97df, 3=30). Serialized as a new line
  `sync-rate <n>`; parsed back clamped to `[0,3]`. It governs the **send** frame rate; **receive**
  auto-detects the rate from the incoming stream (the reader's `rate()`), so one setting suffices.

### Unit 3 — `MidiSyncEngine` MTC mode (`src/app/MidiSyncEngine.{h,cpp}`)

Add an `MtcReader mtcReader_;` member. The `SyncOut` snapshot gains `int mode = 1;`
`double posSeconds = 0.0;` `int frameRate = 3;`.

- **Receive (`update`, main thread).** Branch on `syncInMode`:
  - `0` → close input, `t.externalClock = false` (unchanged).
  - `1` → Beat Clock (unchanged).
  - `2` → open `syncInSource` (reopen on change; same `ignoreTypes(false,false,false)` already
    passes `0xF1` quarter-frames and SysEx), drain `getMessage`: `0xF1` →
    `mtcReader_.onQuarterFrame(msg[1])`; an MTC full-frame SysEx (`F0 7F .. 01 01 ..`) →
    `parseFullFrame` → `mtcReader_.onFullFrame`. Call `mtcReader_.onIdle(dt)` each frame (the
    update signature's so-far-unused `dt`). Then `t.externalClock = true`,
    `t.playing = mtcReader_.playing()`, `t.seconds = mtcReader_.seconds()`. **Do not set `t.bpm`.**
    On open failure, `t.externalClock = false`.
  - On a mode change between Beat Clock and MTC, reset the inactive reader so stale state can't
    leak. (Each direction reopens its port when the source/mode changes, as in Pass 1.)
- **Publish the snapshot** with `mode = syncOutMode`, `posSeconds = t.seconds`,
  `frameRate = p.syncFrameRate` (plus the existing `bpm`/`posBeats`/`playing`/`port`/`enabled`).
- **Send (`senderLoop`, timer thread).** After the mode-agnostic port open/close management, the
  loop resets its per-mode sender state when `s.mode` changes, then dispatches:
  - `mode == 1` → Beat Clock send (unchanged: SPP + Start/Continue/Stop + 24-PPQN ticks).
  - `mode == 2` → MTC send: on play false→true (or a `posSeconds` jump past a threshold) send a
    **full-frame** locate (`fullFrameMessage(secondsToSmpte(posSeconds, rate))`), seed the sender
    state, and start quarter-frames; while playing, emit a quarter-frame every `frameDuration/4`
    via `steady_clock` + the existing `secs()` helper, cycling `piece` 0→7, computing each byte
    from the SMPTE **frozen at piece 0** (refreshed from the latest `posSeconds` each new cycle); on
    play true→false, stop sending QFs (the slave stops on its own timeout). `rate` comes from
    `s.frameRate`.

### Unit 4 — `PreferencesPanel` Sync tab (`src/ui/PreferencesPanel.cpp`)

- `modes[]` becomes `{ "Off", "Beat Clock", "MTC" }`; the two `ImGui::Combo(..., modes, 2)` calls
  widen to `..., modes, 3`.
- When either In mode or Out mode is MTC (`== 2`), show a frame-rate combo
  (`{ "24", "25", "29.97 df", "30" }`) bound to `prefs.syncFrameRate`, firing `onChange()`.

### Unit 5 — Documentation

- **README.md** — extend the Sync note: a second sync mode, **MTC**, locks the transport position
  to (or drives) SMPTE timecode (24/25/29.97-drop/30), selectable per direction with a frame-rate
  picker.
- **CLAUDE.md** — extend the MIDI-sync bullet: MTC as mode 2 reusing the framework; the GL-free
  `core/MidiTimecode` (SMPTE↔seconds incl. drop-frame, quarter-frame + full-frame codec,
  `MtcReader`, unit-tested); MTC drives `Transport::seconds`/`playing` (not `bpm`); the
  `syncFrameRate` pref governs send and receive auto-detects.

## Data flow

```
external MTC ─► sync-in (poll) ─► MtcReader (8 QFs/full-frame ─► SMPTE) ─► Transport.seconds/playing ─► nodes
Transport ─(snapshot/frame)─► SyncOut{mode,posSeconds,playing,frameRate,...} ─► sender thread ─► QFs/full-frame ─► out
```

## Edge cases / concurrency

- **No tempo in MTC** → `t.bpm` is left as the project's; the toolbar's bars/beats derive from
  `seconds` + local BPM. Expected and documented.
- **2-frame reassembly latency** (~67 ms at 30 fps) is hidden by advancing position ¼-frame per QF
  between full assemblies; the absolute snap corrects drift each cycle.
- **Partial QF cycle on join** → no absolute snap until a full 0..7 set arrives; position still
  advances from the per-QF increments.
- **Drop-frame** → exact real-seconds conversion both directions; the four rates round-trip.
- **Receive rate** is taken from the stream (reader's `rate()`); **send rate** from
  `syncFrameRate`.
- **Mode is exclusive** per direction (Off / Beat Clock / MTC) — Beat Clock and MTC never run
  together on the same direction.
- **Sender-thread lifetime** unchanged from Pass 1 (ctor starts it, dtor joins before freeing the
  out port); the only shared state is the mutex-guarded `SyncOut` snapshot + the atomic running
  flag. The MTC send path adds no new shared state.
- **Sync-in off / port gone / open fails** → `externalClock=false`, local clock resumes (mirrors
  Beat Clock).

## Testing

- **`tests/test_midi_timecode.cpp`** (`core_tests`, GL-free):
  - `smpteToSeconds` / `secondsToSmpte` round-trip for all four rates; explicit non-drop checks
    (e.g. `01:00:00:00 @30 == 3600 s`, `00:00:01:15 @30 == 1.5 s`) and **drop-frame** checks
    (`01:00:00;00 @29.97 ≈ 3599.9964 s`; the first valid frame after a minute boundary is
    `00:01:00;02`, and `00:10:00;00` is *not* dropped).
  - `quarterFrameByte(piece, tc)` returns the right `0nnn_dddd` nibble for each of the 8 pieces;
    `fullFrameMessage` / `parseFullFrame` round-trip (and `parseFullFrame` rejects non-MTC SysEx).
  - `MtcReader`: feeding the 8 quarter-frames for a known timecode yields `seconds()` matching
    (within the documented 2-frame compensation); `onFullFrame` locates; `onIdle` past the timeout
    flips `playing()` false; `rate()` reflects the stream.
- **Build + manual:** the rtmidi receive/send + the sender thread's MTC path are verified by a clean
  build and a manual run against an MTC master/slave (DAW or hardware). `gl_smoke` is unaffected.

## Out of scope (YAGNI)

- Reverse-direction MTC (quarter-frames arriving 7→0); forward only.
- MTC user-bits, or MTC cueing / SysEx setup messages beyond the full-frame locate.
- A separate receive frame-rate override (receive auto-detects from the stream).
- Driving `bpm` from MTC (MTC carries no tempo) or any musical-grid inference.
- Changing the Beat Clock path, the Transport, or any non-sync surface.
