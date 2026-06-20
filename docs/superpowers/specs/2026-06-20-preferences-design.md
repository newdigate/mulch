# Preferences Panel — Design

**Date:** 2026-06-20
**Status:** Approved (brainstorming) — ready for implementation plan

## Goal

A **Preferences** window with a tab group (Audio + MIDI) for selecting the audio output/input
device (sound card) and enabling which MIDI interfaces are used. Changes apply **live** — the
running Audio/MIDI nodes reopen their device/ports immediately. Preferences are app-global,
persisted to a `preferences.oss` text file, and structured so future settings slot in.

## Architecture

A GL-free `Preferences` value object (owned by `Application`, persisted separately from
projects) flows to nodes through `EvalContext` exactly like `Transport` does. The four device
nodes read it each frame and reopen their libsoundio stream / rtmidi ports when their relevant
preference changes. A `PreferencesPanel` (ImGui) enumerates devices/ports and edits the object.

### Unit 1 — `core/Preferences.{h,cpp}` (new, GL-free, unit-tested)

```cpp
struct Preferences {
    std::string audioOutputDeviceId;   // libsoundio device id; "" = system default
    std::string audioInputDeviceId;    // "" = system default
    std::vector<std::string> enabledMidiInputs;    // hardware port names; empty = none
    std::vector<std::string> enabledMidiOutputs;

    bool midiInputEnabled(const std::string& name) const;
    void setMidiInputEnabled(const std::string& name, bool on);   // add/remove (idempotent)
    bool midiOutputEnabled(const std::string& name) const;
    void setMidiOutputEnabled(const std::string& name, bool on);
};

std::string serializePreferences(const Preferences&);            // line-based text, header `oss-prefs 1`
bool        parsePreferences(const std::string&, Preferences&);  // false on bad header
```
File format (one keyword per line; rest-of-line is the value so device ids/port names with
spaces need no quoting):
```
oss-prefs 1
audio-out <device id>        # omitted/empty line -> system default
audio-in <device id>
midi-in <port name>          # one line per enabled input port
midi-out <port name>         # one line per enabled output port
```
Devices/ports are keyed by **stable id/name strings**, never indices (which shift as devices
come and go).

### Unit 2 — Plumbing (`EvalContext`, `Graph`, `Application`)

- **`src/core/Node.h`**: forward-declare `struct Preferences;` and add a trailing member to
  `EvalContext`: `const Preferences* prefs = nullptr;` (default keeps existing
  `EvalContext{in,out,dt}` / `{in,out,dt,&t}` constructions — incl. all unit tests — compiling).
- **`src/core/Graph.{h,cpp}`**: hold `const Preferences* prefs_ = nullptr;` + `void
  setPreferences(const Preferences* p)`; in `evaluate`, construct
  `EvalContext ctx{inputs, outs, dt, &transport_, prefs_};`. (Graph forward-declares
  `Preferences`; stays GL-free — `Preferences` is plain data.)
- **`src/app/Application.{h,cpp}`**: own `Preferences prefs_;` call `graph_.setPreferences(&prefs_)`
  in the ctor; load `preferences.oss` at startup and rewrite it whenever the panel changes
  something (`<fstream>`, `core/Preferences.h`).

### Unit 3 — Audio Out / Audio In nodes (live device selection)

`AudioOutputNode` / `AudioInputNode` are refactored so each `evaluate` *ensures the right device
is open* (cheap string compare when unchanged):
- Keep the `SoundIo*` context alive across reopens (created + connected once). Only the
  device + stream are torn down/recreated on a change.
- `want = ctx.prefs ? ctx.prefs->audio{Output,Input}DeviceId : ""`. If no stream is open or
  `currentDeviceId_ != want`: destroy the stream (this stops the RT callback), unref the device,
  then open `want` — enumerate (`soundio_*_device_count`/`get_*_device`), pick the device whose
  `id == want`, falling back to `soundio_default_*_device_index` when `want` is empty or the id is
  gone. Record `currentDeviceId_`. Start the stream. (The existing destructor already proves the
  safe teardown order.)
- No audio context available → silent no-op (as today).

### Unit 4 — MIDI In / Out nodes (live enabled-port set)

`MidiInputNode` / `MidiOutputNode` honor the **set of enabled ports** from prefs:
- **MIDI In** holds a `std::vector<RtMidiIn*>` (one per open port) + the names it has open. Each
  `evaluate`, compute the desired set = `ctx.prefs->enabledMidiInputs` ∩ currently-available
  ports; if it differs from what's open, close all and reopen the desired ports (match a name to
  its index via `getPortCount`/`getPortName`). Drain every open port's queue and **merge** into
  the frame's events. If the desired set is empty, fall back to a single virtual input port (so
  patches work with no hardware selected) — today's behavior.
- **MIDI Out** holds a `std::vector<RtMidiOut*>`; reopens the enabled output ports on change and
  sends each incoming message to **all** of them. Empty set → a virtual output port. The
  all-notes-off cleanup runs on every close (so reopening never leaves hung notes).
- Port matching is by **name**; reopening only happens when the enabled set actually changes
  (cheap vector compare per frame).

### Unit 5 — `src/ui/PreferencesPanel.{h,cpp}` (new)

`void draw(Preferences& prefs, const std::function<void()>& onChange, bool* open);` — an
`ImGui::Begin("Preferences", open)` window with `BeginTabBar`:
- **Audio tab:** an "Output device" combo and an "Input device" combo. Entries: *System default*
  (id `""`) + each enumerated device `{id, name}`. Selecting one sets the matching
  `audio*DeviceId` and calls `onChange`.
- **MIDI tab:** a checkbox per available MIDI **input** port and per **output** port; toggling
  calls `prefs.setMidi{Input,Output}Enabled(name, on)` then `onChange`.
- The panel owns the enumerated lists, refreshed **on open + a Refresh button** (not every
  frame): a temporary `SoundIo` (create → connect → `flush_events` → list → destroy) for audio,
  and `RtMidiIn`/`RtMidiOut` instances for the MIDI port names. `<soundio/soundio.h>` and
  `<RtMidi.h>` stay confined to this `.cpp` (same discipline as the device nodes).

`Application` owns the `PreferencesPanel` + a `bool showPreferences_`; the transport bar gets a
**Prefs** toggle button (via the existing `ProjectBarIO`), and `frame()` draws the panel when
shown, with `onChange = [this]{ savePreferences(); }`.

## Data flow

```
Preferences panel ──edits──► Application::prefs_ ──Graph::setPreferences──► EvalContext.prefs
   Audio Out/In: reopen the libsoundio device when audio*DeviceId changes  ◄──────┤
   MIDI In/Out:  reopen rtmidi ports when the enabled set changes          ◄──────┘
Startup: read preferences.oss → prefs_ ;  any panel change → write preferences.oss
```

## Edge cases

- **Empty / unknown device id** → system default (output) / default (input). A stored id that no
  longer exists falls back to default without error.
- **No audio device at all** → the node is a silent no-op (unchanged).
- **No MIDI selected / no hardware** → virtual port fallback, so existing MIDI patches keep working.
- **Reopening MIDI** flushes all-notes-off first → no hung notes when toggling a port.
- **Malformed `preferences.oss`** (or missing) → `parsePreferences` returns false / file absent →
  start with empty `Preferences` (all defaults). A bad file never crashes startup.
- **`EvalContext` 5th field defaulted** → every existing `EvalContext{...}` construction (incl.
  unit tests that pass 3 or 4 args) keeps compiling; nodes that don't read `prefs` are unaffected.
- **Device enumeration cost** → done on panel open + Refresh only, never per frame.
- **Multiple MIDI In nodes** all opening the same enabled ports → fine on macOS CoreMIDI (multiple
  client connections to a source); typical use is one. (Noted, not specially handled.)

## Testing

- **`tests/test_preferences.cpp`** (`core_tests`, GL-free):
  - `serializePreferences`→`parsePreferences` round-trip (output+input ids, several enabled
    input/output ports, ids/names with spaces); empty `Preferences` round-trips; a bad header
    returns false.
  - The helpers: `setMidiInputEnabled(name,true)` then `midiInputEnabled` is true; setting it
    again is idempotent (no duplicate); `setMidiInputEnabled(name,false)` removes it; same for outputs.
- **Build + manual:** the live device reopen and the panel need real hardware, so they're verified
  by a clean build and a manual run (selecting a device reroutes audio; toggling a MIDI port
  starts/stops it). `gl_smoke` can't open real audio devices headlessly, so there's no automated
  device test; the panel renders in the `--screenshot` demo when shown.

## Docs

- **README.md** — a short **Preferences** note: the toolbar **Prefs** button opens a window with
  Audio (output/input device) and MIDI (enable interfaces) tabs; choices apply live and persist to
  `preferences.oss`.
- **CLAUDE.md** — an Architecture bullet: a GL-free `core/Preferences` (app-global, persisted to
  `preferences.oss`, separate from projects) flows to nodes via `EvalContext::prefs` (like
  `Transport`); Audio Out/In reopen their libsoundio device and MIDI In/Out reopen their rtmidi
  ports when the relevant preference changes; the `PreferencesPanel` (`src/ui/`) enumerates
  devices/ports (soundio/rtmidi confined to its `.cpp`).
</content>
