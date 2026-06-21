# MIDI Beat Clock Sync Implementation Plan (Pass 1 of 2)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Receive MIDI Beat Clock (24-PPQN + Start/Stop/Continue + Song Position) from a selected MIDI input to drive the transport, and send it to a selected output with a dedicated timer thread, configured in a Preferences **Sync** tab.

**Architecture:** GL-free `core/MidiClock` holds the protocol math (a `BeatClockReader` + SPP/message helpers, unit-tested). An app-level `MidiSyncEngine` polls sync-in on the main thread (rtmidi timestamps the messages → accurate tempo) to drive the `Transport` (via a new `Transport::externalClock`), and sends clock from a dedicated timer thread (the thread solely owns the out port). Preferences gain `syncIn/Out` fields + a Sync tab.

**Tech Stack:** C++17, rtmidi, Dear ImGui, `std::thread`/`std::mutex`/`std::chrono`, doctest, CMake. Design: `docs/superpowers/specs/2026-06-21-midi-beat-clock-sync-design.md`. **Pass 2 (MTC)** reuses this framework.

**Notes:** `MidiClock.cpp` is GL-free core → `APP_SOURCES` + `core_tests` (NOT gl_smoke). `MidiSyncEngine.cpp` includes `<RtMidi.h>` → `APP_SOURCES` only. The app already uses `std::async`, so `std::thread` links without a CMake change (if a link error mentions pthread, add `find_package(Threads REQUIRED)` + `Threads::Threads` to the `shader_streamer` target and report it).

---

### Task 1: `Preferences` sync fields

**Files:** Modify `src/core/Preferences.h`, `src/core/Preferences.cpp`, `tests/test_preferences.cpp`.

- [ ] **Step 1: Append the failing test to `tests/test_preferences.cpp`**

```cpp
TEST_CASE("sync fields round-trip and clamp mode") {
    Preferences p;
    p.syncInMode = 1;  p.syncInSource = "IAC Bus 1";
    p.syncOutMode = 1; p.syncOutDest = "IAC Bus 2";
    Preferences r;
    REQUIRE(parsePreferences(serializePreferences(p), r));
    CHECK(r.syncInMode == 1);
    CHECK(r.syncInSource == "IAC Bus 1");
    CHECK(r.syncOutMode == 1);
    CHECK(r.syncOutDest == "IAC Bus 2");

    Preferences c;     // out-of-range mode clamps to 0 (this pass supports 0..1)
    REQUIRE(parsePreferences("oss-prefs 1\nsync-in 7 Something\n", c));
    CHECK(c.syncInMode == 0);
    CHECK(c.syncInSource == "Something");
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake -S . -B build && cmake --build build -j --target core_tests`
Expected: FAIL — `syncInMode` etc. undeclared.

- [ ] **Step 3: Add the fields (`src/core/Preferences.h`)**

In `struct Preferences`, add after `int textureHeight = 720;`:
```cpp
    int         syncInMode  = 0;   // 0 = Off, 1 = Beat Clock
    std::string syncInSource;      // MIDI input port name
    int         syncOutMode = 0;   // 0 = Off, 1 = Beat Clock
    std::string syncOutDest;       // MIDI output port name
```

- [ ] **Step 4: Serialize + parse (`src/core/Preferences.cpp`)**

In `serializePreferences`, just before `return out;` (after the `texture-size` line), add:
```cpp
    out += "sync-in "  + std::to_string(p.syncInMode)  + " " + p.syncInSource + "\n";
    out += "sync-out " + std::to_string(p.syncOutMode) + " " + p.syncOutDest  + "\n";
```
In `parsePreferences`, add two branches alongside the others (the `rest` string holds the rest-of-line `<mode> <port name>`):
```cpp
        else if (kw == "sync-in") {
            std::istringstream rs(rest); int mode = 0; rs >> mode;
            std::string name; std::getline(rs >> std::ws, name);
            out.syncInMode   = (mode < 0 || mode > 1) ? 0 : mode;
            out.syncInSource = name;
        }
        else if (kw == "sync-out") {
            std::istringstream rs(rest); int mode = 0; rs >> mode;
            std::string name; std::getline(rs >> std::ws, name);
            out.syncOutMode = (mode < 0 || mode > 1) ? 0 : mode;
            out.syncOutDest = name;
        }
```

- [ ] **Step 5: Run to verify it passes**

Run: `cmake --build build -j --target core_tests && ./build/core_tests`
Expected: PASS — all green (the existing "empty Preferences round-trips" test still passes; the new sync lines default to `sync-in 0 ` / `sync-out 0 ` and don't touch its assertions).

- [ ] **Step 6: Commit**

```bash
git add src/core/Preferences.h src/core/Preferences.cpp tests/test_preferences.cpp
git commit -m "$(cat <<'EOF'
feat(core): add sync-in/out mode + port fields to Preferences

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
)"
```

---

### Task 2: `Transport::externalClock` + `core/MidiClock`

**Files:** Modify `src/core/Transport.h`, `CMakeLists.txt`; Create `src/core/MidiClock.h`, `src/core/MidiClock.cpp`, `tests/test_midi_clock.cpp`.

- [ ] **Step 1: Write the failing test `tests/test_midi_clock.cpp`**

```cpp
#include <doctest/doctest.h>
#include "core/MidiClock.h"
#include "core/Transport.h"

using namespace oss;

TEST_CASE("BeatClockReader derives tempo, position, and play state") {
    BeatClockReader r;
    r.onStart();
    CHECK(r.playing());
    for (int i = 0; i < 24; ++i) r.onTick(i * (0.5 / 24.0));   // 24 ticks over 0.5 s -> 120 BPM, 1 beat
    CHECK(r.bpm() == doctest::Approx(120.0).epsilon(0.02));
    CHECK(r.positionBeats() == doctest::Approx(1.0));
    r.onStop();
    CHECK_FALSE(r.playing());
    r.onSongPosition(16);                                       // 16 sixteenths = 4 beats
    CHECK(r.positionBeats() == doctest::Approx(4.0));
}

TEST_CASE("SPP + message helpers") {
    CHECK(beatsToSixteenths(4.0) == 16);
    unsigned char m[3];
    sppMessage(16, m);
    CHECK(m[0] == 0xF2);
    CHECK(m[1] == 16);
    CHECK(m[2] == 0);
    CHECK(sppToSixteenths(16, 0) == 16);
    CHECK(sppToSixteenths(0x7F, 0x01) == 255);                 // 14-bit: (1<<7)|127
}

TEST_CASE("Transport externalClock suppresses local advance") {
    Transport t; t.playing = true; t.seconds = 5.0; t.externalClock = true;
    t.advance(1.0);
    CHECK(t.seconds == doctest::Approx(5.0));                  // frozen: engine owns the clock
    t.externalClock = false;
    t.advance(1.0);
    CHECK(t.seconds == doctest::Approx(6.0));                  // local advance resumes
}
```

- [ ] **Step 2: Wire the test into the build (`CMakeLists.txt`)**

- `core_tests` test list — add after `tests/test_preferences.cpp`:  `tests/test_midi_clock.cpp`
- `APP_SOURCES` — add after `src/core/Preferences.cpp`:  `src/core/MidiClock.cpp`
- `core_tests` `src/core/*.cpp` dependency block — add after its `src/core/Preferences.cpp`:  `src/core/MidiClock.cpp`

- [ ] **Step 3: Run to verify it fails**

Run: `cmake -S . -B build && cmake --build build -j --target core_tests`
Expected: FAIL — `'core/MidiClock.h' file not found` / `externalClock` undeclared.

- [ ] **Step 4: `Transport::externalClock` (`src/core/Transport.h`)**

Add a member after `int beatsPerBar = 4;`:
```cpp
    bool   externalClock = false; // driven by MIDI sync -> advance() is a no-op
```
At the very top of `void advance(double dt) {`, add:
```cpp
        if (externalClock) return;   // the sync engine sets bpm/seconds/playing directly
```

- [ ] **Step 5: Create `src/core/MidiClock.h`**

```cpp
#pragma once
#include <cstddef>
#include <deque>

namespace oss {

// MIDI System Real-Time / Common bytes for Beat Clock.
constexpr unsigned char kMidiClock = 0xF8, kMidiStart = 0xFA, kMidiContinue = 0xFB, kMidiStop = 0xFC;

// --- pure Beat Clock message helpers (GL-free, testable) ---
int  beatsToSixteenths(double beats);                  // round(beats*4), >= 0
void sppMessage(int sixteenths, unsigned char out[3]); // {0xF2, lsb, msb} (14-bit, 7 bits each)
int  sppToSixteenths(unsigned char lsb, unsigned char msb);

// Beat Clock RECEIVER: fed timestamped clock events, derives tempo + position + play state.
class BeatClockReader {
public:
    void onTick(double tSeconds);          // a 0xF8 at absolute time tSeconds
    void onStart();                        // playing = true, position = 0
    void onContinue();                     // playing = true
    void onStop();                         // playing = false
    void onSongPosition(int sixteenths);   // SPP: position = sixteenths/4 beats; resets tick phase
    void reset();

    double bpm() const { return bpm_; }                 // 0 until >= 2 ticks seen
    double positionBeats() const { return locateBeats_ + (double)ticksSinceLocate_ / 24.0; }
    bool   playing() const { return playing_; }

private:
    std::deque<double> tickTimes_;          // recent tick absolute times (tempo window)
    double bpm_ = 0.0;
    bool   playing_ = false;
    double locateBeats_ = 0.0;
    long   ticksSinceLocate_ = 0;
};

} // namespace oss
```

- [ ] **Step 6: Create `src/core/MidiClock.cpp`**

```cpp
#include "core/MidiClock.h"
#include <cmath>

namespace oss {

int beatsToSixteenths(double beats) {
    int s = (int)std::lround(beats * 4.0);
    return s < 0 ? 0 : s;
}
void sppMessage(int sixteenths, unsigned char out[3]) {
    if (sixteenths < 0) sixteenths = 0;
    if (sixteenths > 0x3FFF) sixteenths = 0x3FFF;
    out[0] = 0xF2;
    out[1] = (unsigned char)(sixteenths & 0x7F);
    out[2] = (unsigned char)((sixteenths >> 7) & 0x7F);
}
int sppToSixteenths(unsigned char lsb, unsigned char msb) {
    return ((int)(msb & 0x7F) << 7) | (int)(lsb & 0x7F);
}

namespace { constexpr std::size_t kTempoWindow = 24; }   // average over ~1 quarter note

void BeatClockReader::reset() {
    tickTimes_.clear(); bpm_ = 0.0; playing_ = false; locateBeats_ = 0.0; ticksSinceLocate_ = 0;
}
void BeatClockReader::onTick(double tSeconds) {
    tickTimes_.push_back(tSeconds);
    while (tickTimes_.size() > kTempoWindow) tickTimes_.pop_front();
    if (tickTimes_.size() >= 2) {
        double span = tickTimes_.back() - tickTimes_.front();
        double intervals = (double)(tickTimes_.size() - 1);
        if (span > 0.0) bpm_ = 60.0 / ((span / intervals) * 24.0);
    }
    if (playing_) ++ticksSinceLocate_;
}
void BeatClockReader::onStart()    { playing_ = true; locateBeats_ = 0.0; ticksSinceLocate_ = 0; }
void BeatClockReader::onContinue() { playing_ = true; }
void BeatClockReader::onStop()     { playing_ = false; }
void BeatClockReader::onSongPosition(int sixteenths) {
    locateBeats_ = sixteenths / 4.0;
    ticksSinceLocate_ = 0;
}

} // namespace oss
```

- [ ] **Step 7: Run to verify it passes**

Run: `cmake -S . -B build && cmake --build build -j --target core_tests && ./build/core_tests`
Expected: PASS — all green, including the MidiClock + Transport cases.

- [ ] **Step 8: Full build (nothing else broke)**

Run: `cmake --build build -j && ctest --test-dir build --output-on-failure`
Expected: clean build; all pass (`gl_smoke` unaffected — `externalClock` defaults false, so the local clock is unchanged).

- [ ] **Step 9: Commit**

```bash
git add src/core/Transport.h src/core/MidiClock.h src/core/MidiClock.cpp tests/test_midi_clock.cpp CMakeLists.txt
git commit -m "$(cat <<'EOF'
feat(core): add Transport::externalClock + MidiClock (Beat Clock codec)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
)"
```

---

### Task 3: `MidiSyncEngine` (poll-in + dedicated sender thread)

**Files:** Create `src/app/MidiSyncEngine.h`, `src/app/MidiSyncEngine.cpp`; Modify `CMakeLists.txt`.

- [ ] **Step 1: Create `src/app/MidiSyncEngine.h`**

```cpp
#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include "core/MidiClock.h"

class RtMidiIn;    // opaque; <RtMidi.h> stays out of this header
class RtMidiOut;

namespace oss {

struct Transport;
struct Preferences;

// Receives MIDI Beat Clock from a selected input (polled on the main thread, driving the
// Transport via Transport::externalClock) and sends it to a selected output from a dedicated
// timer thread (which solely owns the output port, so no cross-thread rtmidi access).
class MidiSyncEngine {
public:
    MidiSyncEngine();
    ~MidiSyncEngine();
    MidiSyncEngine(const MidiSyncEngine&) = delete;
    MidiSyncEngine& operator=(const MidiSyncEngine&) = delete;

    // Main thread, once per frame: poll sync-in -> drive `t`; publish the sync-out snapshot.
    void update(Transport& t, const Preferences& p, double dt);

private:
    void senderLoop();

    // sync-in (main thread only)
    RtMidiIn*       in_ = nullptr;
    std::string     inPort_;          // open input port name ("" = none)
    double          inClock_ = 0.0;   // accumulated rtmidi delta-times (absolute tick time)
    BeatClockReader reader_;

    // sync-out snapshot (main thread writes; sender thread reads)
    struct SyncOut { bool enabled = false; std::string port; bool playing = false; double bpm = 120.0; double posBeats = 0.0; };
    std::mutex        outMutex_;
    SyncOut           out_;
    std::atomic<bool> running_{true};
    std::thread       senderThread_;
};

} // namespace oss
```

- [ ] **Step 2: Create `src/app/MidiSyncEngine.cpp`**

```cpp
#include "app/MidiSyncEngine.h"
#include "core/Transport.h"
#include "core/Preferences.h"
#include <RtMidi.h>
#include <chrono>
#include <cmath>
#include <vector>

namespace oss {

using clk = std::chrono::steady_clock;
static clk::duration secs(double s) {
    return std::chrono::duration_cast<clk::duration>(std::chrono::duration<double>(s));
}
static int inPortIndex(RtMidiIn& mi, const std::string& name) {
    unsigned int n = mi.getPortCount();
    for (unsigned int i = 0; i < n; ++i) if (mi.getPortName(i) == name) return (int)i;
    return -1;
}
static int outPortIndex(RtMidiOut& mo, const std::string& name) {
    unsigned int n = mo.getPortCount();
    for (unsigned int i = 0; i < n; ++i) if (mo.getPortName(i) == name) return (int)i;
    return -1;
}

MidiSyncEngine::MidiSyncEngine() {
    senderThread_ = std::thread([this]{ senderLoop(); });
}

MidiSyncEngine::~MidiSyncEngine() {
    running_.store(false);
    if (senderThread_.joinable()) senderThread_.join();   // thread owns out port -> joined first
    delete in_;                                           // RtMidiIn dtor closes the port
}

void MidiSyncEngine::update(Transport& t, const Preferences& p, double /*dt*/) {
    // --- sync-in (main thread) ---
    if (p.syncInMode != 1) {
        if (in_) { delete in_; in_ = nullptr; inPort_.clear(); }
        t.externalClock = false;
    } else {
        if (!in_ || inPort_ != p.syncInSource) {
            if (in_) { delete in_; in_ = nullptr; }
            inPort_.clear(); reader_.reset(); inClock_ = 0.0;
            try {
                in_ = new RtMidiIn(RtMidi::UNSPECIFIED, "shader-streamer-sync");
                in_->ignoreTypes(false, false, false);    // we WANT clock + SPP
                int idx = inPortIndex(*in_, p.syncInSource);
                if (idx >= 0) { in_->openPort((unsigned int)idx, "sync in"); inPort_ = p.syncInSource; }
                else { delete in_; in_ = nullptr; }
            } catch (RtMidiError&) { delete in_; in_ = nullptr; }
        }
        if (in_) {
            std::vector<unsigned char> msg;
            for (;;) {
                double d = in_->getMessage(&msg);
                if (msg.empty()) break;
                inClock_ += d;
                unsigned char s = msg[0];
                if      (s == kMidiClock)    reader_.onTick(inClock_);
                else if (s == kMidiStart)    reader_.onStart();
                else if (s == kMidiContinue) reader_.onContinue();
                else if (s == kMidiStop)     reader_.onStop();
                else if (s == 0xF2 && msg.size() >= 3) reader_.onSongPosition(sppToSixteenths(msg[1], msg[2]));
            }
            t.externalClock = true;
            t.playing = reader_.playing();
            if (reader_.bpm() > 0.0) {
                t.bpm     = reader_.bpm();
                t.seconds = reader_.positionBeats() * t.secondsPerBeat();
            }
        } else {
            t.externalClock = false;   // couldn't open -> local clock
        }
    }

    // --- publish sync-out snapshot ---
    {
        std::lock_guard<std::mutex> lk(outMutex_);
        out_.enabled  = (p.syncOutMode == 1);
        out_.port     = p.syncOutDest;
        out_.playing  = t.playing;
        out_.bpm      = t.bpm;
        out_.posBeats = t.beats();
    }
}

void MidiSyncEngine::senderLoop() {
    RtMidiOut*  out = nullptr;
    std::string openPort;
    bool        wasPlaying = false;
    double      tickPos = 0.0;                 // beats already covered by sent ticks
    auto        nextTick = clk::now();

    auto sendByte = [&](unsigned char b){ std::vector<unsigned char> m{b}; try { out->sendMessage(&m); } catch (...) {} };
    auto sendSpp  = [&](double beats){ unsigned char m3[3]; sppMessage(beatsToSixteenths(beats), m3);
                                       std::vector<unsigned char> m(m3, m3 + 3); try { out->sendMessage(&m); } catch (...) {} };

    while (running_.load()) {
        SyncOut s;
        { std::lock_guard<std::mutex> lk(outMutex_); s = out_; }

        if (!s.enabled || s.port.empty()) {
            if (out) { delete out; out = nullptr; openPort.clear(); wasPlaying = false; }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        if (!out || openPort != s.port) {
            if (out) { delete out; out = nullptr; }
            openPort.clear(); wasPlaying = false;
            try {
                out = new RtMidiOut(RtMidi::UNSPECIFIED, "shader-streamer-sync");
                int idx = outPortIndex(*out, s.port);
                if (idx >= 0) { out->openPort((unsigned int)idx, "sync out"); openPort = s.port; }
                else { delete out; out = nullptr; }
            } catch (...) { if (out) { delete out; out = nullptr; } }
            if (!out) { std::this_thread::sleep_for(std::chrono::milliseconds(20)); continue; }
        }

        // play-state transitions
        if (s.playing && !wasPlaying) {
            sendSpp(s.posBeats);
            sendByte(s.posBeats < 1e-6 ? kMidiStart : kMidiContinue);
            tickPos = s.posBeats;
            nextTick = clk::now();
            wasPlaying = true;
        } else if (!s.playing && wasPlaying) {
            sendByte(kMidiStop);
            wasPlaying = false;
        }
        if (!s.playing) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); continue; }

        // locate: transport jumped away from our tick position -> relocate the slave
        if (std::fabs(s.posBeats - tickPos) > (2.0 / 24.0)) {
            sendSpp(s.posBeats);
            tickPos = s.posBeats;
            nextTick = clk::now();
        }

        // emit one tick when due, then fine-grained sleep for accuracy + responsiveness
        double secPerTick = 60.0 / ((s.bpm > 0.0 ? s.bpm : 120.0) * 24.0);
        auto now = clk::now();
        if (now >= nextTick) {
            sendByte(kMidiClock);
            tickPos += 1.0 / 24.0;
            nextTick += secs(secPerTick);
            if (nextTick < now) nextTick = now + secs(secPerTick);   // catch up after a stall
        }
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
    if (out) delete out;
}

} // namespace oss
```

- [ ] **Step 3: Wire into the build (`CMakeLists.txt`)**

In `APP_SOURCES`, add `src/app/MidiSyncEngine.cpp` right before `src/app/Application.cpp`. (Do NOT add it to `core_tests` or `gl_smoke` — it pulls in `<RtMidi.h>`.)

- [ ] **Step 4: Build (the engine compiles + links into the app, unused for now)**

Run: `cmake -S . -B build && cmake --build build -j && ctest --test-dir build --output-on-failure`
Expected: clean build; all tests pass. If linking fails on `std::thread`/pthread, add `find_package(Threads REQUIRED)` near the top of `CMakeLists.txt` and `Threads::Threads` to `target_link_libraries(shader_streamer ...)`, rebuild, and report it.

- [ ] **Step 5: Commit**

```bash
git add src/app/MidiSyncEngine.h src/app/MidiSyncEngine.cpp CMakeLists.txt
git commit -m "$(cat <<'EOF'
feat(app): add MidiSyncEngine (Beat Clock receive + threaded send)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
)"
```

---

### Task 4: Preferences "Sync" tab + Application wiring

**Files:** Modify `src/ui/PreferencesPanel.cpp`, `src/app/Application.h`, `src/app/Application.cpp`.

- [ ] **Step 1: Add the Sync tab (`src/ui/PreferencesPanel.cpp`)**

Inside `draw(...)`, in the `if (ImGui::BeginTabBar("prefs_tabs")) {` block, add a tab AFTER the existing tabs (e.g. after the Video/MIDI tab's `EndTabItem()`) and before `ImGui::EndTabBar();`:
```cpp
        if (ImGui::BeginTabItem("Sync")) {
            const char* modes[] = { "Off", "Beat Clock" };
            auto portCombo = [&](const char* label, std::vector<std::string>& ports, std::string& cur) {
                if (ImGui::BeginCombo(label, cur.empty() ? "None" : cur.c_str())) {
                    if (ImGui::Selectable("None", cur.empty())) { cur.clear(); onChange(); }
                    for (const std::string& nm : ports) {
                        bool sel = (nm == cur);
                        if (ImGui::Selectable((nm + std::string("##") + label).c_str(), sel)) { cur = nm; onChange(); }
                    }
                    ImGui::EndCombo();
                }
            };
            ImGui::TextUnformatted("Receive (slave)");
            if (ImGui::Combo("In mode", &prefs.syncInMode, modes, 2)) onChange();
            portCombo("Sync source", midiIns_, prefs.syncInSource);
            ImGui::Separator();
            ImGui::TextUnformatted("Send (master)");
            if (ImGui::Combo("Out mode", &prefs.syncOutMode, modes, 2)) onChange();
            portCombo("Sync destination", midiOuts_, prefs.syncOutDest);
            ImGui::EndTabItem();
        }
```

- [ ] **Step 2: Application owns + drives the engine**

In `src/app/Application.h`: add `#include "app/MidiSyncEngine.h"` near the other includes, and add to the `private:` members (after `PreferencesPanel preferences_;`):
```cpp
    MidiSyncEngine  syncEngine_;
```
In `src/app/Application.cpp`, in `frame()`, add this line immediately BEFORE `graph_.evaluate(dt);` (and after `preferences_.draw(...)`):
```cpp
    syncEngine_.update(graph_.transport(), prefs_, dt);   // MIDI clock sync in/out
```

- [ ] **Step 3: Build + screenshot the Sync tab**

Run: `cmake --build build -j && ctest --test-dir build --output-on-failure` — clean build, all pass.

To verify the tab (panel hidden by default), TEMPORARILY set `bool showPreferences_ = true;` in `src/app/Application.h`, rebuild, and:
```bash
cmake --build build -j && ./build/shader_streamer --screenshot build/_ui.png
```
Open `build/_ui.png` with the Read tool: confirm the Preferences window now has a **Sync** tab header (alongside Audio Output / Audio Input / MIDI / Video). Report what you saw. Then **REVERT** `showPreferences_` to `false`, rebuild, and re-screenshot to confirm exit 0. The committed code MUST have `showPreferences_ = false`. (The app launches and quits cleanly — confirming the `MidiSyncEngine` ctor starts and dtor joins the sender thread without hanging.)

- [ ] **Step 4: Commit**

```bash
git add src/ui/PreferencesPanel.cpp src/app/Application.h src/app/Application.cpp
git commit -m "$(cat <<'EOF'
feat(ui): Preferences Sync tab + wire MidiSyncEngine into the frame loop

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
)"
```

---

### Task 5: Documentation

**Files:** Modify `README.md`, `CLAUDE.md`.

- [ ] **Step 1: README.md**

In the **Preferences** subsection, add a sentence (match the surrounding prose):
```markdown
A **Sync** tab can drive the transport from an external MIDI **Beat Clock** (receive 24-PPQN
clock + Start/Stop/Continue + Song Position from a selected input) and send Beat Clock to a
selected output (with a dedicated timer thread for steady ticks).
```

- [ ] **Step 2: CLAUDE.md**

Add this Architecture bullet after the **Preferences** bullet:
```markdown
- **MIDI sync** — the GL-free `core/MidiClock` holds the Beat Clock protocol math (a
  `BeatClockReader` deriving tempo/position/play from timestamped 24-PPQN ticks + Start/Stop/
  Continue + Song Position, plus SPP/message helpers; unit-tested). The app-level
  `MidiSyncEngine` (`src/app/`, `<RtMidi.h>` confined to its `.cpp`) polls the selected sync
  input on the main thread to drive the `Transport` via a new `Transport::externalClock` (which
  makes `advance()` a no-op), and sends clock to the selected output from a **dedicated timer
  thread** that solely owns the out port (started in the ctor, joined in the dtor before the
  port is freed). Configured by the Preferences `syncIn/Out` fields + the **Sync** tab; ticked
  from `Application::frame` before `graph_.evaluate`. (MTC is the planned Pass-2 mode.)
```

- [ ] **Step 3: Verify + commit**

Run: `ctest --test-dir build --output-on-failure`
Expected: all pass.

```bash
git add README.md CLAUDE.md
git commit -m "$(cat <<'EOF'
docs: document MIDI Beat Clock sync (Preferences Sync tab)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
)"
```

---

## Final verification

- [ ] `cmake --build build -j` — clean build (`shader_streamer`, `core_tests`, `gl_smoke`)
- [ ] `ctest --test-dir build --output-on-failure` — all pass (MidiClock + Transport externalClock + sync Preferences fields; gl_smoke unaffected)
- [ ] `./build/shader_streamer --screenshot build/_ui.png` — exit 0 (app launches + quits cleanly; the sender thread joins on exit); Preferences has a **Sync** tab
- [ ] Manual: in Prefs → Sync, set Sync source to an external clock (DAW/IAC) and confirm the transport follows its tempo + play; set Sync destination and confirm a DAW slaves to our clock
- [ ] Use superpowers:finishing-a-development-branch
</content>
