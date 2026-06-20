# Preferences Panel Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A Preferences window (Audio Output · Audio Input · MIDI tabs) that selects the audio sound card and enables MIDI interfaces, applied **live** and persisted to `preferences.oss`.

**Architecture:** A GL-free `Preferences` object (owned by `Application`, persisted separately) flows to nodes via `EvalContext::prefs` like `Transport` does. Audio Out/In reopen their libsoundio device, and MIDI In/Out reopen their rtmidi ports, when their relevant preference changes. A `PreferencesPanel` enumerates devices/ports and edits the object.

**Tech Stack:** C++17, libsoundio, rtmidi, Dear ImGui, doctest (`core_tests`), CMake. Design: `docs/superpowers/specs/2026-06-20-preferences-design.md`.

**Notes:**
- `Preferences.cpp` is GL-free core → goes in `APP_SOURCES` + `core_tests` (NOT gl_smoke — `Graph` only stores/passes the pointer, calling no `Preferences` method; `setPreferences` is inline). `PreferencesPanel.cpp` includes soundio/rtmidi/imgui → `APP_SOURCES` only.
- The audio/MIDI node refactors are hardware-dependent → verified by a clean build + manual run, no unit test. The real automated tests are on `core/Preferences`.

---

### Task 1: `core/Preferences` + EvalContext/Graph plumbing

**Files:** Create `src/core/Preferences.{h,cpp}`, `tests/test_preferences.cpp`; Modify `src/core/Node.h`, `src/core/Graph.h`, `src/core/Graph.cpp`, `CMakeLists.txt`.

- [ ] **Step 1: Write the failing test**

Create `tests/test_preferences.cpp`:
```cpp
#include <doctest/doctest.h>
#include "core/Preferences.h"

using namespace oss;

TEST_CASE("Preferences serialize/parse round-trip") {
    Preferences p;
    p.audioOutputDeviceId = "Built-in Output id";
    p.audioInputDeviceId  = "USB Mic id";
    p.enabledMidiInputs   = {"Keystation 49", "IAC Bus 1"};
    p.enabledMidiOutputs  = {"IAC Bus 1"};
    Preferences r;
    REQUIRE(parsePreferences(serializePreferences(p), r));
    CHECK(r.audioOutputDeviceId == "Built-in Output id");
    CHECK(r.audioInputDeviceId == "USB Mic id");
    REQUIRE(r.enabledMidiInputs.size() == 2);
    CHECK(r.enabledMidiInputs[1] == "IAC Bus 1");
    REQUIRE(r.enabledMidiOutputs.size() == 1);
    CHECK(r.enabledMidiOutputs[0] == "IAC Bus 1");
}

TEST_CASE("empty Preferences round-trips and a bad header is rejected") {
    Preferences r;
    REQUIRE(parsePreferences(serializePreferences(Preferences{}), r));
    CHECK(r.audioOutputDeviceId.empty());
    CHECK(r.enabledMidiInputs.empty());
    CHECK_FALSE(parsePreferences("garbage\n", r));
    CHECK_FALSE(parsePreferences("", r));
}

TEST_CASE("MIDI enable helpers are idempotent add/remove") {
    Preferences p;
    p.setMidiInputEnabled("port A", true);
    p.setMidiInputEnabled("port A", true);          // idempotent: no duplicate
    CHECK(p.enabledMidiInputs.size() == 1);
    CHECK(p.midiInputEnabled("port A"));
    p.setMidiInputEnabled("port A", false);
    CHECK_FALSE(p.midiInputEnabled("port A"));
    CHECK(p.enabledMidiInputs.empty());
    p.setMidiOutputEnabled("out B", true);
    CHECK(p.midiOutputEnabled("out B"));
    CHECK_FALSE(p.midiOutputEnabled("missing"));
}
```

- [ ] **Step 2: Wire into the build**

In `CMakeLists.txt`:
- `core_tests` test list — add after `tests/test_project_file.cpp`:  `tests/test_preferences.cpp`
- `APP_SOURCES` — add after `src/core/AutomationStore.cpp`:  `src/core/Preferences.cpp`
- `core_tests` `src/core/*.cpp` dependency block — add after its `src/core/AutomationStore.cpp`:  `src/core/Preferences.cpp`

- [ ] **Step 3: Run to verify it fails**

Run: `cmake -S . -B build && cmake --build build -j --target core_tests`
Expected: FAIL — `'core/Preferences.h' file not found`.

- [ ] **Step 4: Create `src/core/Preferences.h`**

```cpp
#pragma once
#include <string>
#include <vector>

namespace oss {

// App-global settings (not project state). Audio devices are keyed by libsoundio's
// stable device id; MIDI interfaces by port name. GL-free.
struct Preferences {
    std::string audioOutputDeviceId;                 // "" = system default
    std::string audioInputDeviceId;                  // "" = system default
    std::vector<std::string> enabledMidiInputs;      // hardware port names
    std::vector<std::string> enabledMidiOutputs;

    bool midiInputEnabled(const std::string& name) const;
    void setMidiInputEnabled(const std::string& name, bool on);   // idempotent add/remove
    bool midiOutputEnabled(const std::string& name) const;
    void setMidiOutputEnabled(const std::string& name, bool on);
};

std::string serializePreferences(const Preferences& p);
bool        parsePreferences(const std::string& text, Preferences& out);   // false on bad header

} // namespace oss
```

- [ ] **Step 5: Create `src/core/Preferences.cpp`**

```cpp
#include "core/Preferences.h"
#include <algorithm>
#include <sstream>

namespace oss {

namespace {
bool listed(const std::vector<std::string>& v, const std::string& n) {
    return std::find(v.begin(), v.end(), n) != v.end();
}
void setListed(std::vector<std::string>& v, const std::string& n, bool on) {
    auto it = std::find(v.begin(), v.end(), n);
    if (on && it == v.end()) v.push_back(n);
    else if (!on && it != v.end()) v.erase(it);
}
} // namespace

bool Preferences::midiInputEnabled(const std::string& n) const  { return listed(enabledMidiInputs, n); }
void Preferences::setMidiInputEnabled(const std::string& n, bool on)  { setListed(enabledMidiInputs, n, on); }
bool Preferences::midiOutputEnabled(const std::string& n) const { return listed(enabledMidiOutputs, n); }
void Preferences::setMidiOutputEnabled(const std::string& n, bool on) { setListed(enabledMidiOutputs, n, on); }

std::string serializePreferences(const Preferences& p) {
    std::string out = "oss-prefs 1\n";
    if (!p.audioOutputDeviceId.empty()) out += "audio-out " + p.audioOutputDeviceId + "\n";
    if (!p.audioInputDeviceId.empty())  out += "audio-in "  + p.audioInputDeviceId  + "\n";
    for (const std::string& n : p.enabledMidiInputs)  out += "midi-in "  + n + "\n";
    for (const std::string& n : p.enabledMidiOutputs) out += "midi-out " + n + "\n";
    return out;
}

bool parsePreferences(const std::string& text, Preferences& out) {
    out = Preferences{};
    std::istringstream in(text);
    std::string line;
    if (!std::getline(in, line) || line.rfind("oss-prefs", 0) != 0) return false;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        std::istringstream ls(line);
        std::string kw; ls >> kw;
        std::string rest; std::getline(ls >> std::ws, rest);
        if      (kw == "audio-out") out.audioOutputDeviceId = rest;
        else if (kw == "audio-in")  out.audioInputDeviceId  = rest;
        else if (kw == "midi-in"  && !rest.empty()) out.enabledMidiInputs.push_back(rest);
        else if (kw == "midi-out" && !rest.empty()) out.enabledMidiOutputs.push_back(rest);
    }
    return true;
}

} // namespace oss
```

- [ ] **Step 6: Plumb `prefs` through `EvalContext` + `Graph`**

In `src/core/Node.h`: add a forward declaration `struct Preferences;` inside `namespace oss` (before `struct EvalContext`), and add a member to `EvalContext` right after the `transport` member:
```cpp
    const Preferences*        prefs     = nullptr;   // app settings (set by Graph::evaluate)
```
(This trailing defaulted member keeps every existing `EvalContext{in,out,dt}` / `{in,out,dt,&t}` construction — including all unit tests — compiling.)

In `src/core/Graph.h`: add to the `public:` section (after `transport()` accessors):
```cpp
    // App-global preferences (audio/MIDI device selection), passed to every node via
    // EvalContext. Not owned -- Application owns the Preferences object.
    void setPreferences(const Preferences* p) { prefs_ = p; }
```
and to the `private:` members (near `Transport transport_;`):
```cpp
    const Preferences* prefs_ = nullptr;
```
(`Preferences` is visible via the forward declaration in the included `core/Node.h`.)

In `src/core/Graph.cpp`, in `evaluate`, change the `EvalContext` construction line from
`EvalContext ctx{inputs, outs, dt, &transport_};` to:
```cpp
        EvalContext ctx{inputs, outs, dt, &transport_, prefs_};
```

- [ ] **Step 7: Run to verify it passes**

Run: `cmake -S . -B build && cmake --build build -j --target core_tests && ./build/core_tests`
Expected: PASS — all green, the three Preferences cases included.

- [ ] **Step 8: Full build (everything still compiles with the new EvalContext field)**

Run: `cmake --build build -j && ctest --test-dir build --output-on-failure`
Expected: clean build (`shader_streamer`, `core_tests`, `gl_smoke`); all tests pass.

- [ ] **Step 9: Commit**

```bash
git add src/core/Preferences.h src/core/Preferences.cpp tests/test_preferences.cpp src/core/Node.h src/core/Graph.h src/core/Graph.cpp CMakeLists.txt
git commit -m "$(cat <<'EOF'
feat(core): add Preferences + plumb it through EvalContext/Graph

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
)"
```

---

### Task 2: Audio Out + Audio In — live device selection

**Files:** Modify `src/modules/AudioOutputNode.{h,cpp}`, `src/modules/AudioInputNode.{h,cpp}`.

The refactor pattern for BOTH nodes: keep the `SoundIo` context alive across reopens; each `evaluate` ensures the stream is open on the preferred device, reopening only when the id changes.

- [ ] **Step 1: AudioOutputNode header**

In `src/modules/AudioOutputNode.h`: add `#include <string>` to the includes. Replace the private method declarations + the `LazyInit lazy_;` member. The private section becomes:
```cpp
private:
    bool ensureDevice(const std::string& wantId);   // open context (once) + ensure the right stream
    bool openContext();
    bool openStream(const std::string& wantId);
    void closeStream();
    int  findOutputDeviceById(const std::string& id);
    static void writeCallback(SoundIoOutStream* os, int frameMin, int frameMax);
    static void errorCallback(SoundIoOutStream* os, int err);

    SoundIo*          soundio_   = nullptr;
    SoundIoDevice*    device_    = nullptr;
    SoundIoOutStream* outstream_ = nullptr;

    SpscRingBuffer<float> ring_{1 << 14};
    std::vector<float>    scratch_;
    std::vector<float>    stereoScratch_;
    int  sampleRate_ = 48000;
    std::string currentDeviceId_;       // id the stream is currently open on ("" = default)
    bool streamOpen_   = false;
    bool contextFailed_ = false;        // no audio context -> stay a silent no-op
```
(Remove the old `LazyInit lazy_;` member and the `#include "core/LazyInit.h"` if it is no longer used — verify nothing else needs it.)

- [ ] **Step 2: AudioOutputNode .cpp**

In `src/modules/AudioOutputNode.cpp`: add `#include "core/Preferences.h"` near the top. Rewrite the destructor, `evaluate` head, and the open/close helpers (KEEP `writeCallback` and `errorCallback` and the audio-push body of `evaluate` exactly as they are):

Destructor:
```cpp
AudioOutputNode::~AudioOutputNode() {
    closeStream();                       // destroys the stream (stops the RT callback) then unrefs the device
    if (soundio_) soundio_destroy(soundio_);
}
```
Replace the first line of `evaluate` (`if (!ensureStarted()) return;`) with:
```cpp
    std::string want = ctx.prefs ? ctx.prefs->audioOutputDeviceId : std::string();
    if (!ensureDevice(want)) return;     // no device -> silent no-op
```
Keep `soundio_flush_events(soundio_);` and everything after it (the left/right read, mirror, interleave into `stereoScratch_`, `ring_.push`).

Replace the old `ensureStarted()`/`openDevice()` definitions with:
```cpp
bool AudioOutputNode::ensureDevice(const std::string& wantId) {
    if (!soundio_) {
        if (contextFailed_) return false;
        if (!openContext()) { contextFailed_ = true; return false; }
    }
    if (streamOpen_ && currentDeviceId_ == wantId) return true;
    closeStream();
    return openStream(wantId);
}

bool AudioOutputNode::openContext() {
    soundio_ = soundio_create();
    if (!soundio_) return false;
    if (int err = soundio_connect(soundio_)) {
        std::fprintf(stderr, "[AudioOut] connect failed: %s\n", soundio_strerror(err));
        soundio_destroy(soundio_); soundio_ = nullptr;
        return false;
    }
    soundio_flush_events(soundio_);
    return true;
}

int AudioOutputNode::findOutputDeviceById(const std::string& id) {
    int n = soundio_output_device_count(soundio_);
    for (int i = 0; i < n; ++i) {
        SoundIoDevice* d = soundio_get_output_device(soundio_, i);
        bool match = d && !d->is_raw && d->id && id == d->id;
        if (d) soundio_device_unref(d);
        if (match) return i;
    }
    return -1;
}

bool AudioOutputNode::openStream(const std::string& wantId) {
    soundio_flush_events(soundio_);
    int idx = wantId.empty() ? -1 : findOutputDeviceById(wantId);
    if (idx < 0) idx = soundio_default_output_device_index(soundio_);
    if (idx < 0) { std::fprintf(stderr, "[AudioOut] no output device\n"); return false; }
    device_ = soundio_get_output_device(soundio_, idx);
    if (!device_) return false;

    sampleRate_ = soundio_device_nearest_sample_rate(device_, 48000);
    outstream_ = soundio_outstream_create(device_);
    if (!outstream_) { closeStream(); return false; }
    outstream_->format         = SoundIoFormatFloat32NE;
    outstream_->sample_rate    = sampleRate_;
    outstream_->write_callback = &AudioOutputNode::writeCallback;
    outstream_->error_callback = &AudioOutputNode::errorCallback;
    outstream_->userdata       = this;
    outstream_->name           = "shader-streamer";
    if (int err = soundio_outstream_open(outstream_)) {
        std::fprintf(stderr, "[AudioOut] open failed: %s\n", soundio_strerror(err));
        closeStream(); return false;
    }
    scratch_.assign(4096, 0.0f);
    if (int err = soundio_outstream_start(outstream_)) {
        std::fprintf(stderr, "[AudioOut] start failed: %s\n", soundio_strerror(err));
        closeStream(); return false;
    }
    currentDeviceId_ = wantId;
    streamOpen_ = true;
    std::fprintf(stderr, "[AudioOut] playing on '%s': %d Hz, %d ch\n",
                 device_->name ? device_->name : "?", sampleRate_, outstream_->layout.channel_count);
    return true;
}

void AudioOutputNode::closeStream() {
    if (outstream_) { soundio_outstream_destroy(outstream_); outstream_ = nullptr; }
    if (device_)    { soundio_device_unref(device_);         device_    = nullptr; }
    streamOpen_ = false;
}
```

- [ ] **Step 3: AudioInputNode header + .cpp (mirror)**

In `src/modules/AudioInputNode.h`: same edits — add `#include <string>`; replace the private methods + drop `LazyInit lazy_;`:
```cpp
private:
    bool ensureDevice(const std::string& wantId);
    bool openContext();
    bool openStream(const std::string& wantId);
    void closeStream();
    int  findInputDeviceById(const std::string& id);
    static void readCallback(SoundIoInStream* is, int frameMin, int frameMax);
    static void errorCallback(SoundIoInStream* is, int err);

    SoundIo*         soundio_  = nullptr;
    SoundIoDevice*   device_   = nullptr;
    SoundIoInStream* instream_ = nullptr;

    SpscRingBuffer<float> ring_{1 << 14};
    std::vector<float>    block_;
    std::vector<float>    outL_, outR_;
    int  sampleRate_ = 48000;
    int  channels_   = 1;
    std::string currentDeviceId_;
    bool streamOpen_    = false;
    bool contextFailed_ = false;
```
In `src/modules/AudioInputNode.cpp`: add `#include "core/Preferences.h"`. Destructor:
```cpp
AudioInputNode::~AudioInputNode() {
    closeStream();
    if (soundio_) soundio_destroy(soundio_);
}
```
(If the file has no explicit destructor today, add this one and declare `~AudioInputNode() override;` in the header — check the header; it already declares `~AudioInputNode() override;`.)

Replace `evaluate`'s opening (`if (!ensureStarted()) { ctx.out... silence ...; return; }`) with:
```cpp
    std::string want = ctx.prefs ? ctx.prefs->audioInputDeviceId : std::string();
    if (!ensureDevice(want)) {
        ctx.out<AudioRef>(0, AudioRef{});
        ctx.out<AudioRef>(1, AudioRef{});
        return;
    }
```
Keep the rest of `evaluate` (the `soundio_flush_events`, ring pop, deinterleave, emit) unchanged. Replace the old `ensureStarted()`/`openDevice()` with:
```cpp
bool AudioInputNode::ensureDevice(const std::string& wantId) {
    if (!soundio_) {
        if (contextFailed_) return false;
        if (!openContext()) { contextFailed_ = true; return false; }
    }
    if (streamOpen_ && currentDeviceId_ == wantId) return true;
    closeStream();
    return openStream(wantId);
}

bool AudioInputNode::openContext() {
    soundio_ = soundio_create();
    if (!soundio_) return false;
    if (int err = soundio_connect(soundio_)) {
        std::fprintf(stderr, "[AudioIn] connect failed: %s\n", soundio_strerror(err));
        soundio_destroy(soundio_); soundio_ = nullptr;
        return false;
    }
    soundio_flush_events(soundio_);
    return true;
}

int AudioInputNode::findInputDeviceById(const std::string& id) {
    int n = soundio_input_device_count(soundio_);
    for (int i = 0; i < n; ++i) {
        SoundIoDevice* d = soundio_get_input_device(soundio_, i);
        bool match = d && !d->is_raw && d->id && id == d->id;
        if (d) soundio_device_unref(d);
        if (match) return i;
    }
    return -1;
}

bool AudioInputNode::openStream(const std::string& wantId) {
    soundio_flush_events(soundio_);
    int idx = wantId.empty() ? -1 : findInputDeviceById(wantId);
    if (idx < 0) idx = soundio_default_input_device_index(soundio_);
    if (idx < 0) { std::fprintf(stderr, "[AudioIn] no input device\n"); return false; }
    device_ = soundio_get_input_device(soundio_, idx);
    if (!device_) return false;

    sampleRate_ = soundio_device_nearest_sample_rate(device_, 48000);
    instream_ = soundio_instream_create(device_);
    if (!instream_) { closeStream(); return false; }
    instream_->format         = SoundIoFormatFloat32NE;
    instream_->sample_rate    = sampleRate_;
    instream_->read_callback   = &AudioInputNode::readCallback;
    instream_->error_callback  = &AudioInputNode::errorCallback;
    instream_->userdata        = this;
    instream_->name            = "shader-streamer";
    if (int err = soundio_instream_open(instream_)) {
        std::fprintf(stderr, "[AudioIn] open failed: %s\n", soundio_strerror(err));
        closeStream(); return false;
    }
    channels_ = instream_->layout.channel_count >= 2 ? 2 : 1;
    if (int err = soundio_instream_start(instream_)) {
        std::fprintf(stderr, "[AudioIn] start failed (mic access denied?): %s\n", soundio_strerror(err));
        closeStream(); return false;
    }
    currentDeviceId_ = wantId;
    streamOpen_ = true;
    std::fprintf(stderr, "[AudioIn] capturing from '%s': %d Hz, %d ch\n",
                 device_->name ? device_->name : "?", sampleRate_, channels_);
    return true;
}

void AudioInputNode::closeStream() {
    if (instream_) { soundio_instream_destroy(instream_); instream_ = nullptr; }
    if (device_)   { soundio_device_unref(device_);        device_   = nullptr; }
    streamOpen_ = false;
}
```

- [ ] **Step 4: Build**

Run: `cmake --build build -j && ctest --test-dir build --output-on-failure`
Expected: clean build; all tests pass (these nodes aren't in `core_tests`/`gl_smoke`, so this confirms the app still compiles + links). Then run `./build/shader_streamer --screenshot build/_ui.png` and confirm exit 0 (the default-device path still opens as before — check stderr shows `[AudioOut] playing on ...`).

- [ ] **Step 5: Commit**

```bash
git add src/modules/AudioOutputNode.h src/modules/AudioOutputNode.cpp src/modules/AudioInputNode.h src/modules/AudioInputNode.cpp
git commit -m "$(cat <<'EOF'
feat(audio): Audio Out/In open the preferred device + reopen on change

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
)"
```

---

### Task 3: MIDI In + Out — live enabled-port set

**Files:** Modify `src/modules/MidiInputNode.{h,cpp}`, `src/modules/MidiOutputNode.{h,cpp}`.

- [ ] **Step 1: MidiInputNode header**

In `src/modules/MidiInputNode.h`: add `#include <string>`. Replace the private members + methods:
```cpp
private:
    void syncPorts(const Preferences* prefs);     // open/close to match the enabled set
    void closeAll();
    std::vector<RtMidiIn*>   ins_;                 // one per open port
    std::vector<std::string> open_;                // descriptor of the current open set
    std::vector<MidiEvent>   events_;
```
Drop `RtMidiIn* midiin_ = nullptr;`, `bool ensureStarted(); bool openDevice();`, and the `LazyInit lazy_;` (remove `#include "core/LazyInit.h"`). Add a forward declaration `struct Preferences;` inside `namespace oss` (before the class).

- [ ] **Step 2: MidiInputNode .cpp**

Rewrite `src/modules/MidiInputNode.cpp`:
```cpp
#include "modules/MidiInputNode.h"
#include "core/Preferences.h"
#include <RtMidi.h>
#include <cstdio>
#include <vector>

namespace oss {

namespace { const std::string kVirtual = "\x01virtual"; }   // sentinel for the virtual-port fallback

MidiInputNode::MidiInputNode() : Node("MIDI In") {
    addOutput("midi", PortType::Midi);
}

MidiInputNode::~MidiInputNode() { closeAll(); }

void MidiInputNode::closeAll() {
    for (RtMidiIn* mi : ins_) delete mi;   // RtMidiIn dtor closes the port
    ins_.clear();
    open_.clear();
}

static int portIndexByName(RtMidiIn& mi, const std::string& name) {
    unsigned int n = mi.getPortCount();
    for (unsigned int i = 0; i < n; ++i)
        if (mi.getPortName(i) == name) return (int)i;
    return -1;
}

void MidiInputNode::syncPorts(const Preferences* prefs) {
    std::vector<std::string> desired;
    if (prefs && !prefs->enabledMidiInputs.empty()) desired = prefs->enabledMidiInputs;
    else desired = { kVirtual };
    if (desired == open_) return;          // nothing changed
    closeAll();
    for (const std::string& name : desired) {
        try {
            RtMidiIn* mi = new RtMidiIn(RtMidi::UNSPECIFIED, "shader-streamer");
            mi->ignoreTypes(true, true, true);
            if (name == kVirtual) {
                mi->openVirtualPort("shader-streamer in");
            } else {
                int idx = portIndexByName(*mi, name);
                if (idx < 0) { delete mi; continue; }    // port gone -> skip
                mi->openPort((unsigned int)idx, "in");
            }
            ins_.push_back(mi);
        } catch (RtMidiError& err) {
            std::fprintf(stderr, "[MidiIn] open '%s' failed: %s\n", name.c_str(), err.getMessage().c_str());
        }
    }
    open_ = desired;
}

void MidiInputNode::evaluate(EvalContext& ctx) {
    events_.clear();
    syncPorts(ctx.prefs);
    std::vector<unsigned char> msg;
    for (RtMidiIn* mi : ins_) {
        for (;;) {
            mi->getMessage(&msg);
            if (msg.empty()) break;
            MidiEvent e{};
            e.status = msg[0];
            if (msg.size() > 1) e.data1 = msg[1];
            if (msg.size() > 2) e.data2 = msg[2];
            events_.push_back(e);
        }
    }
    ctx.out<MidiRef>(0, MidiRef{events_.data(), events_.size()});
}

} // namespace oss
```

- [ ] **Step 3: MidiOutputNode header**

In `src/modules/MidiOutputNode.h`: replace the private members + methods:
```cpp
private:
    void syncPorts(const Preferences* prefs);
    void closeAll();
    std::vector<RtMidiOut*>  outs_;
    std::vector<std::string> open_;
```
Drop `RtMidiOut* midiout_`, `ensureStarted/openDevice`, `LazyInit lazy_;` (+ its include). Add `#include <string>` and `#include <vector>`, and a `struct Preferences;` forward declaration in `namespace oss`.

- [ ] **Step 4: MidiOutputNode .cpp**

Rewrite `src/modules/MidiOutputNode.cpp`:
```cpp
#include "modules/MidiOutputNode.h"
#include "core/Preferences.h"
#include <RtMidi.h>
#include <cstdio>
#include <vector>

namespace oss {

namespace { const std::string kVirtual = "\x01virtual"; }

MidiOutputNode::MidiOutputNode() : Node("MIDI Out") {
    addInput("midi", PortType::Midi, MidiRef{});
}

MidiOutputNode::~MidiOutputNode() { closeAll(); }

void MidiOutputNode::closeAll() {
    for (RtMidiOut* mo : outs_) {
        try {                                        // all-notes-off so nothing hangs
            for (int ch = 0; ch < 16; ++ch) {
                std::vector<unsigned char> m = {(unsigned char)(0xB0u | ch), 123, 0};
                mo->sendMessage(&m);
            }
        } catch (...) {}
        delete mo;
    }
    outs_.clear();
    open_.clear();
}

static int portIndexByName(RtMidiOut& mo, const std::string& name) {
    unsigned int n = mo.getPortCount();
    for (unsigned int i = 0; i < n; ++i)
        if (mo.getPortName(i) == name) return (int)i;
    return -1;
}

void MidiOutputNode::syncPorts(const Preferences* prefs) {
    std::vector<std::string> desired;
    if (prefs && !prefs->enabledMidiOutputs.empty()) desired = prefs->enabledMidiOutputs;
    else desired = { kVirtual };
    if (desired == open_) return;
    closeAll();
    for (const std::string& name : desired) {
        try {
            RtMidiOut* mo = new RtMidiOut(RtMidi::UNSPECIFIED, "shader-streamer");
            if (name == kVirtual) {
                mo->openVirtualPort("shader-streamer out");
            } else {
                int idx = portIndexByName(*mo, name);
                if (idx < 0) { delete mo; continue; }
                mo->openPort((unsigned int)idx, "out");
            }
            outs_.push_back(mo);
        } catch (RtMidiError& err) {
            std::fprintf(stderr, "[MidiOut] open '%s' failed: %s\n", name.c_str(), err.getMessage().c_str());
        }
    }
    open_ = desired;
}

void MidiOutputNode::evaluate(EvalContext& ctx) {
    syncPorts(ctx.prefs);
    MidiRef in = ctx.in<MidiRef>(0);
    std::vector<unsigned char> m(3);
    for (std::size_t i = 0; i < in.count; ++i) {
        const MidiEvent& e = in.events[i];
        m[0] = e.status; m[1] = e.data1; m[2] = e.data2;
        for (RtMidiOut* mo : outs_) { try { mo->sendMessage(&m); } catch (...) {} }
    }
}

} // namespace oss
```

- [ ] **Step 5: Build**

Run: `cmake --build build -j && ctest --test-dir build --output-on-failure`
Expected: clean build; all tests pass. `./build/shader_streamer --screenshot build/_ui.png` exits 0 (stderr should still show a MIDI virtual/hardware port opening, since no prefs file yet → empty enabled set → virtual fallback).

- [ ] **Step 6: Commit**

```bash
git add src/modules/MidiInputNode.h src/modules/MidiInputNode.cpp src/modules/MidiOutputNode.h src/modules/MidiOutputNode.cpp
git commit -m "$(cat <<'EOF'
feat(midi): MIDI In merges / MIDI Out fans out the enabled ports; reopen on change

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
)"
```

---

### Task 4: PreferencesPanel + Application integration + toolbar Prefs button

**Files:** Create `src/ui/PreferencesPanel.{h,cpp}`; Modify `src/ui/TransportBar.{h,cpp}`, `src/app/Application.{h,cpp}`, `CMakeLists.txt`.

- [ ] **Step 1: Create `src/ui/PreferencesPanel.h`**

```cpp
#pragma once
#include <functional>
#include <string>
#include <vector>

namespace oss {

struct Preferences;

// Preferences window: Audio Output / Audio Input / MIDI tabs. Enumerates devices and
// MIDI ports on open + a Refresh button. Edits the Preferences in place and calls
// onChange() whenever a setting changes (the app persists on change).
class PreferencesPanel {
public:
    void draw(Preferences& prefs, const std::function<void()>& onChange, bool* open);
private:
    void refresh();
    struct Dev { std::string id, name; };
    std::vector<Dev>         outDevices_, inDevices_;
    std::vector<std::string> midiIns_, midiOuts_;
    bool loaded_ = false;
};

} // namespace oss
```

- [ ] **Step 2: Create `src/ui/PreferencesPanel.cpp`**

```cpp
#include "ui/PreferencesPanel.h"
#include "core/Preferences.h"
#include <imgui.h>
#include <soundio/soundio.h>
#include <RtMidi.h>

namespace oss {

void PreferencesPanel::refresh() {
    outDevices_.clear(); inDevices_.clear(); midiIns_.clear(); midiOuts_.clear();

    SoundIo* sio = soundio_create();
    if (sio && soundio_connect(sio) == 0) {
        soundio_flush_events(sio);
        int no = soundio_output_device_count(sio);
        for (int i = 0; i < no; ++i) {
            SoundIoDevice* d = soundio_get_output_device(sio, i);
            if (d && !d->is_raw) outDevices_.push_back({d->id ? d->id : "", d->name ? d->name : "?"});
            if (d) soundio_device_unref(d);
        }
        int ni = soundio_input_device_count(sio);
        for (int i = 0; i < ni; ++i) {
            SoundIoDevice* d = soundio_get_input_device(sio, i);
            if (d && !d->is_raw) inDevices_.push_back({d->id ? d->id : "", d->name ? d->name : "?"});
            if (d) soundio_device_unref(d);
        }
    }
    if (sio) soundio_destroy(sio);

    try { RtMidiIn  mi; unsigned int n = mi.getPortCount(); for (unsigned int i = 0; i < n; ++i) midiIns_.push_back(mi.getPortName(i)); } catch (...) {}
    try { RtMidiOut mo; unsigned int n = mo.getPortCount(); for (unsigned int i = 0; i < n; ++i) midiOuts_.push_back(mo.getPortName(i)); } catch (...) {}
}

void PreferencesPanel::draw(Preferences& prefs, const std::function<void()>& onChange, bool* open) {
    if (!open || !*open) return;
    if (!loaded_) { refresh(); loaded_ = true; }

    if (!ImGui::Begin("Preferences", open)) { ImGui::End(); return; }

    if (ImGui::Button("Refresh devices")) refresh();

    auto deviceCombo = [&](const char* label, std::vector<Dev>& devs, std::string& curId) {
        std::string cur = "System default";
        if (!curId.empty()) for (const Dev& d : devs) if (d.id == curId) cur = d.name;
        if (ImGui::BeginCombo(label, cur.c_str())) {
            if (ImGui::Selectable("System default", curId.empty())) { curId.clear(); onChange(); }
            for (const Dev& d : devs) {
                bool sel = (d.id == curId);
                if (ImGui::Selectable((d.name + "##" + d.id).c_str(), sel)) { curId = d.id; onChange(); }
            }
            ImGui::EndCombo();
        }
    };

    if (ImGui::BeginTabBar("prefs_tabs")) {
        if (ImGui::BeginTabItem("Audio Output")) {
            deviceCombo("Output device", outDevices_, prefs.audioOutputDeviceId);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Audio Input")) {
            deviceCombo("Input device", inDevices_, prefs.audioInputDeviceId);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("MIDI")) {
            ImGui::TextUnformatted("Inputs");
            for (const std::string& name : midiIns_) {
                bool on = prefs.midiInputEnabled(name);
                if (ImGui::Checkbox((name + "##in").c_str(), &on)) { prefs.setMidiInputEnabled(name, on); onChange(); }
            }
            ImGui::Separator();
            ImGui::TextUnformatted("Outputs");
            for (const std::string& name : midiOuts_) {
                bool on = prefs.midiOutputEnabled(name);
                if (ImGui::Checkbox((name + "##out").c_str(), &on)) { prefs.setMidiOutputEnabled(name, on); onChange(); }
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

} // namespace oss
```

- [ ] **Step 3: TransportBar — a Prefs toggle button**

In `src/ui/TransportBar.h`, add a field to `ProjectBarIO` (after `std::string status;`):
```cpp
    bool*       showPreferences = nullptr;   // toggled by the Prefs button (if non-null)
```
In `src/ui/TransportBar.cpp`, inside the `if (io) { ... }` block (e.g. right after the Load button line), add:
```cpp
        if (io->showPreferences) {
            ImGui::SameLine();
            if (ImGui::Button("Prefs")) *io->showPreferences = !*io->showPreferences;
        }
```

- [ ] **Step 4: Application — own + wire the Preferences**

In `src/app/Application.h`: add includes `#include "core/Preferences.h"` and `#include "ui/PreferencesPanel.h"`. Add to `public:` (after the save/load methods):
```cpp
    void loadPreferences();
    void savePreferences();
```
Add to `private:` members:
```cpp
    Preferences     prefs_;
    PreferencesPanel preferences_;
    bool            showPreferences_ = false;
```

In `src/app/Application.cpp` (`<fstream>`/`<sstream>` are already included from save/load; `core/Preferences.h` comes via the header):
- In the constructor, after `graph_.connect(c, 0, o, 0);`, add:
```cpp
    loadPreferences();
    graph_.setPreferences(&prefs_);
```
- Add the two methods (near `saveProjectToFile`):
```cpp
void Application::loadPreferences() {
    std::ifstream f("preferences.oss");
    if (!f) return;
    std::stringstream ss; ss << f.rdbuf();
    parsePreferences(ss.str(), prefs_);    // bad/missing file -> prefs_ stays default
}

void Application::savePreferences() {
    std::ofstream f("preferences.oss");
    if (f) f << serializePreferences(prefs_);
}
```
- In `frame()`, after `io.status = projectStatus_;` add `io.showPreferences = &showPreferences_;` (before the `drawTransportBar(...)` call), and after the `automation_.draw(graph_);` line add:
```cpp
    preferences_.draw(prefs_, [this]{ savePreferences(); }, &showPreferences_);
```

- [ ] **Step 5: CMake**

In `CMakeLists.txt`, `APP_SOURCES`, add after `src/ui/AutomationPanel.cpp`:
```cmake
  src/ui/PreferencesPanel.cpp
```

- [ ] **Step 6: Build + screenshot the panel**

Run: `cmake --build build -j && ctest --test-dir build --output-on-failure`
Expected: clean build; all tests pass.

To verify the panel renders (it's hidden by default), temporarily change the member default to `bool showPreferences_ = true;` in `Application.h`, rebuild, and:
```bash
cmake --build build -j && ./build/shader_streamer --screenshot build/_ui.png
```
Open `build/_ui.png` with the Read tool: confirm a **Preferences** window with **Audio Output / Audio Input / MIDI** tabs (the Audio tabs show a device combo; MIDI shows Inputs/Outputs checkboxes), and a **Prefs** button in the top toolbar. Report what you saw. Then **revert** the default back to `bool showPreferences_ = false;`, rebuild, and re-run the screenshot to confirm it exits 0 with the panel closed (the Prefs button still present).

- [ ] **Step 7: Commit**

```bash
git add src/ui/PreferencesPanel.h src/ui/PreferencesPanel.cpp src/ui/TransportBar.h src/ui/TransportBar.cpp src/app/Application.h src/app/Application.cpp CMakeLists.txt
git commit -m "$(cat <<'EOF'
feat(ui): add the Preferences panel (Audio Output/Input + MIDI tabs)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
)"
```

---

### Task 5: Documentation

**Files:** Modify `README.md`, `CLAUDE.md`.

- [ ] **Step 1: README.md**

Add a short **Preferences** note near the Save/Load section (match surrounding style):
```markdown
### Preferences

The toolbar **Prefs** button opens a Preferences window with **Audio Output**, **Audio Input**,
and **MIDI** tabs: pick the output/input sound card and enable which MIDI interfaces are used.
Changes apply live (the running audio/MIDI nodes reopen their device/ports) and persist to a
`preferences.oss` file in the working directory.
```

- [ ] **Step 2: CLAUDE.md**

Add this Architecture bullet after the **Project save/load** bullet:
```markdown
- **Preferences** — app-global settings live in the GL-free `core/Preferences` (audio
  output/input device ids + enabled-MIDI-port name sets), persisted to `preferences.oss`
  (separate from projects) and flowed to nodes via `EvalContext::prefs` (like `Transport`,
  passed by `Graph::evaluate`; `Application` owns the object via `Graph::setPreferences`). The
  **Audio Out/In** nodes keep their libsoundio context alive and reopen the device when
  `audio*DeviceId` changes; **MIDI In** merges all enabled input ports and **MIDI Out** fans
  out to all enabled output ports, reopening when the enabled set changes (virtual-port
  fallback when none selected). The `PreferencesPanel` (`src/ui/`) enumerates devices/ports
  (soundio/rtmidi confined to its `.cpp`) into Audio Output / Audio Input / MIDI tabs.
```

- [ ] **Step 3: Verify + commit**

Run: `ctest --test-dir build --output-on-failure`
Expected: all pass (no code changed).

```bash
git add README.md CLAUDE.md
git commit -m "$(cat <<'EOF'
docs: document the Preferences panel + core/Preferences

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
)"
```

---

## Final verification

- [ ] `cmake --build build -j` — clean build (`shader_streamer`, `core_tests`, `gl_smoke`)
- [ ] `ctest --test-dir build --output-on-failure` — all pass (Preferences round-trip + helpers; existing suites)
- [ ] `./build/shader_streamer --screenshot build/_ui.png` — exits 0; the toolbar shows a **Prefs** button
- [ ] Manual: open Prefs, switch the Output device (audio reroutes), toggle a MIDI input (starts/stops); relaunch and confirm the choice persisted via `preferences.oss`
- [ ] Use superpowers:finishing-a-development-branch
</content>
