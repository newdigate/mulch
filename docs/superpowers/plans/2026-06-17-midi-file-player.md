# MIDI File Player Node Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A node that streams a Standard MIDI File synced to the project transport — anchored at a start offset, optionally looping a region, with per-channel mute toggles — emitting a MidiRef you can wire into the Acid Bass.

**Architecture:** Three GL-free units: a from-scratch SMF parser (`core/MidiFile`) producing beat-positioned events, a loop-robust `MidiClipPlayer` (`core/MidiClip.h`) that turns the transport position into each frame's events (flushing note-offs at loop seams), and the header-only `MidiFilePlayerNode` wrapping them.

**Tech Stack:** C++17, doctest. GL-free core.

**Spec:** `docs/superpowers/specs/2026-06-17-midi-file-player-design.md`

---

## File Structure

| File | Responsibility | Change |
|---|---|---|
| `src/core/MidiFile.{h,cpp}` | SMF parser → beat-positioned `MidiSequence` | **create** |
| `src/core/MidiClip.h` | `MidiClipPlayer`: transport position → frame events | **create** (header-only) |
| `src/modules/MidiFilePlayerNode.h` | the node (ports + load + per-frame emit) | **create** (header-only) |
| `src/app/Application.cpp` | register `"MIDI File"` (factory + MIDI category) | modify |
| `src/main.cpp` | add the node to the screenshot demo | modify |
| `tests/test_midi_file.cpp` | parser tests | **create** |
| `tests/test_midi_clip.cpp` | clip-player + node tests | **create** |
| `CMakeLists.txt` | wire `MidiFile.cpp` + the tests into `core_tests` (+ `APP_SOURCES`) | modify |
| `README.md`, `CLAUDE.md` | document the node | modify |

Each task ends green (build + `ctest`).

---

### Task 1: SMF parser (`core/MidiFile`)

**Files:**
- Create: `src/core/MidiFile.h`, `src/core/MidiFile.cpp`
- Test: `tests/test_midi_file.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Write the failing test**

Create `tests/test_midi_file.cpp`:

```cpp
#include <doctest/doctest.h>
#include "core/MidiFile.h"
#include "core/Value.h"
#include <vector>

using namespace oss;

// A tiny SMF: format 0, 1 track, 480 ticks/quarter; note-on 60 @ t0, note-off 60 @
// t480, note-on 64 @ t480, note-off 64 @ t960, end-of-track.
static std::vector<unsigned char> sampleSmf() {
    return {
        'M','T','h','d', 0,0,0,6, 0,0, 0,1, 0x01,0xE0,
        'M','T','r','k', 0,0,0,22,
        0x00, 0x90,0x3C,0x64,
        0x83,0x60, 0x80,0x3C,0x00,
        0x00, 0x90,0x40,0x64,
        0x83,0x60, 0x80,0x40,0x00,
        0x00, 0xFF,0x2F,0x00
    };
}

TEST_CASE("parses a simple MIDI file into beat-positioned events") {
    auto smf = sampleSmf();
    MidiSequence s = parseMidiFile(smf.data(), smf.size());
    REQUIRE(s.ok);
    REQUIRE(s.events.size() == 4);
    CHECK(s.events[0].beats == doctest::Approx(0.0));
    CHECK(midiIsNoteOn(s.events[0].ev));
    CHECK(s.events[0].ev.data1 == 60);
    CHECK(s.events[1].beats == doctest::Approx(1.0));     // 480 / 480
    CHECK(midiIsNoteOff(s.events[1].ev));
    CHECK(s.events[1].ev.data1 == 60);
    CHECK(s.events[2].beats == doctest::Approx(1.0));
    CHECK(s.events[2].ev.data1 == 64);
    CHECK(s.events[3].beats == doctest::Approx(2.0));
    CHECK(s.lengthBeats == doctest::Approx(2.0));
}

TEST_CASE("rejects a non-MIDI buffer and a truncated header") {
    std::vector<unsigned char> bad = {'X','X','X','X', 0,0,0,6};
    CHECK_FALSE(parseMidiFile(bad.data(), bad.size()).ok);
    std::vector<unsigned char> trunc = {'M','T','h','d', 0,0};
    CHECK_FALSE(parseMidiFile(trunc.data(), trunc.size()).ok);
}

TEST_CASE("handles running status (an omitted repeated status byte)") {
    std::vector<unsigned char> smf = {
        'M','T','h','d', 0,0,0,6, 0,0, 0,1, 0x01,0xE0,
        'M','T','r','k', 0,0,0,12,
        0x00, 0x90,0x3C,0x64,           // note-on 60
        0x81,0x70, 0x40,0x64,           // +240, running status -> note-on 64
        0x00, 0xFF,0x2F,0x00
    };
    MidiSequence s = parseMidiFile(smf.data(), smf.size());
    REQUIRE(s.ok);
    REQUIRE(s.events.size() == 2);
    CHECK(s.events[0].ev.data1 == 60);
    CHECK(s.events[1].ev.data1 == 64);
    CHECK(s.events[1].beats == doctest::Approx(240.0 / 480.0));
}
```

- [ ] **Step 2: Wire the parser + test into CMake**

In `CMakeLists.txt`:
- In `set(APP_SOURCES ...)`, add `  src/core/MidiFile.cpp` (put it right after the
  `  src/core/AutomationStore.cpp` line).
- In the `add_executable(core_tests ...)` list, add `  src/core/MidiFile.cpp` after its
  own `  src/core/AutomationStore.cpp` line, and add `  tests/test_midi_file.cpp` after
  the `  tests/test_acid_voice.cpp` line.
- Do NOT add `MidiFile.cpp` to `gl_smoke`.

- [ ] **Step 3: Run to verify it fails**

Run: `cmake --build build -j 2>&1 | tail -5`
Expected: compile error — `core/MidiFile.h` does not exist.

- [ ] **Step 4: Create the header**

Create `src/core/MidiFile.h`:

```cpp
#pragma once
#include <cstddef>
#include <string>
#include <vector>
#include "core/Value.h"   // MidiEvent

namespace oss {

// One file event at an absolute position in quarter-note beats.
struct MidiFileEvent { double beats = 0.0; MidiEvent ev; };

struct MidiSequence {
    bool   ok = false;
    std::vector<MidiFileEvent> events;   // channel-voice events, sorted by beats
    double lengthBeats = 0.0;            // position of the last event
    std::string error;
};

// Parse a Standard MIDI File from a byte buffer. The file's tempo map is ignored;
// events are positioned in quarter-note beats. Channel-voice messages are kept;
// meta/sysex are skipped. Format 0 and 1 (tracks merged). GL-free.
MidiSequence parseMidiFile(const unsigned char* data, std::size_t n);

// Read a .mid from disk and parse it (ok=false with an error if unreadable/empty).
MidiSequence loadMidiFile(const std::string& path);

} // namespace oss
```

- [ ] **Step 5: Create the implementation**

Create `src/core/MidiFile.cpp`:

```cpp
#include "core/MidiFile.h"
#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iterator>

namespace oss {
namespace {

// A bounds-checked cursor over the byte buffer; `ok` goes false on overrun.
struct Reader {
    const unsigned char* p;
    const unsigned char* end;
    bool ok = true;
    unsigned char u8() { if (p >= end) { ok = false; return 0; } return *p++; }
    uint32_t u32() { uint32_t v = 0; for (int i = 0; i < 4; ++i) v = (v << 8) | u8(); return v; }
    uint16_t u16() { uint16_t hi = u8(), lo = u8(); return (uint16_t)((hi << 8) | lo); }
    uint32_t vlq() {                       // variable-length quantity (max 4 bytes)
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) { unsigned char b = u8(); v = (v << 7) | (b & 0x7Fu); if (!(b & 0x80u)) break; }
        return v;
    }
    void skip(std::size_t k) { if ((std::size_t)(end - p) < k) { p = end; ok = false; } else p += k; }
};

int channelDataBytes(unsigned char status) {   // 1 for program/channel-pressure, else 2
    unsigned char hi = status & 0xF0u;
    return (hi == 0xC0u || hi == 0xD0u) ? 1 : 2;
}

} // namespace

MidiSequence parseMidiFile(const unsigned char* data, std::size_t n) {
    MidiSequence seq;
    Reader r{data, data + n};

    if (r.u8() != 'M' || r.u8() != 'T' || r.u8() != 'h' || r.u8() != 'd') {
        seq.error = "not a MIDI file (missing MThd)"; return seq;
    }
    uint32_t hlen = r.u32();
    r.u16();                                // format (0/1 handled the same; merged)
    uint16_t ntracks = r.u16();
    uint16_t division = r.u16();
    if (hlen > 6) r.skip(hlen - 6);
    if (!r.ok) { seq.error = "truncated header"; return seq; }
    if (division & 0x8000u) { seq.error = "SMPTE division unsupported"; return seq; }
    if (division == 0) { seq.error = "zero tick division"; return seq; }
    const double tpq = (double)division;

    double maxBeats = 0.0;
    for (uint16_t t = 0; t < ntracks && r.ok; ++t) {
        unsigned char m0 = r.u8(), m1 = r.u8(), m2 = r.u8(), m3 = r.u8();
        uint32_t tlen = r.u32();
        if (!r.ok) break;
        if (m0 != 'M' || m1 != 'T' || m2 != 'r' || m3 != 'k') { r.skip(tlen); continue; }
        const unsigned char* trackEnd = r.p + tlen;
        if (trackEnd > r.end) trackEnd = r.end;
        uint32_t tick = 0;
        unsigned char running = 0;
        while (r.p < trackEnd && r.ok) {
            tick += r.vlq();
            unsigned char b = r.u8();
            if (b == 0xFFu) {                              // meta -> skip
                r.u8(); uint32_t len = r.vlq(); r.skip(len); running = 0;
            } else if (b == 0xF0u || b == 0xF7u) {         // sysex -> skip
                uint32_t len = r.vlq(); r.skip(len); running = 0;
            } else {
                unsigned char status, d1;
                if (b & 0x80u) { status = b; running = b; d1 = r.u8(); }
                else           { status = running; d1 = b; }   // running status
                if ((status & 0xF0u) < 0x80u) { r.ok = false; break; }
                unsigned char d2 = 0;
                if (channelDataBytes(status) == 2) d2 = r.u8();
                if (!r.ok) break;
                MidiFileEvent e;
                e.beats = (double)tick / tpq;
                e.ev = MidiEvent{status, d1, d2};
                seq.events.push_back(e);
                if (e.beats > maxBeats) maxBeats = e.beats;
            }
        }
        r.p = trackEnd;   // resync to the declared track end
    }

    std::stable_sort(seq.events.begin(), seq.events.end(),
        [](const MidiFileEvent& a, const MidiFileEvent& b) { return a.beats < b.beats; });
    seq.lengthBeats = maxBeats;
    seq.ok = true;
    return seq;
}

MidiSequence loadMidiFile(const std::string& path) {
    MidiSequence seq;
    if (path.empty()) { seq.error = "no file"; return seq; }
    std::ifstream f(path, std::ios::binary);
    if (!f) { seq.error = "cannot open file"; return seq; }
    std::vector<unsigned char> buf((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (buf.empty()) { seq.error = "empty file"; return seq; }
    return parseMidiFile(buf.data(), buf.size());
}

} // namespace oss
```

- [ ] **Step 6: Run the tests**

Run: `cmake --build build -j && ./build/core_tests --test-case="*MIDI file*,*running status*,*non-MIDI*"`
Expected: PASS (3 cases).

- [ ] **Step 7: Full suite**

Run: `ctest --test-dir build --output-on-failure`
Expected: all pass.

- [ ] **Step 8: Commit**

```bash
git add src/core/MidiFile.h src/core/MidiFile.cpp tests/test_midi_file.cpp CMakeLists.txt
git commit -m "feat(core): add a Standard MIDI File parser (MidiFile)"
```

---

### Task 2: `MidiClipPlayer`

**Files:**
- Create: `src/core/MidiClip.h`
- Test: `tests/test_midi_clip.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Write the failing test**

Create `tests/test_midi_clip.cpp`:

```cpp
#include <doctest/doctest.h>
#include "core/MidiClip.h"
#include "core/MidiFile.h"
#include "core/Value.h"
#include <vector>

using namespace oss;

static MidiSequence makeSeq() {
    MidiSequence s; s.ok = true;
    s.events = {
        { 0.0, midiNoteOn(60, 100) },
        { 1.0, midiNoteOff(60) },
        { 2.0, midiNoteOn(64, 100) },
    };
    s.lengthBeats = 3.0;
    return s;
}
static int countNoteOns(const std::vector<MidiEvent>& e) {
    int n = 0; for (auto& x : e) if (midiIsNoteOn(x)) ++n; return n;
}
static int countNoteOffs(const std::vector<MidiEvent>& e) {
    int n = 0; for (auto& x : e) if (midiIsNoteOff(x)) ++n; return n;
}

TEST_CASE("clip emits events in each frame's beat window") {
    MidiSequence s = makeSeq();
    MidiClipPlayer p;
    bool mute[16] = {};
    CHECK(p.advance(s, 0.0, 4, 0.0, false, 4, mute).empty());            // entry: nothing
    CHECK(countNoteOns(p.advance(s, 0.5, 4, 0.0, false, 4, mute)) == 1); // [0,0.5) -> note-on 60
    CHECK(countNoteOffs(p.advance(s, 1.5, 4, 0.0, false, 4, mute)) == 1);// [0.5,1.5) -> note-off 60
    CHECK(countNoteOns(p.advance(s, 2.5, 4, 0.0, false, 4, mute)) == 1); // note-on 64
}

TEST_CASE("clip mutes a channel") {
    MidiSequence s = makeSeq();          // all on channel 0
    MidiClipPlayer p;
    bool mute[16] = {}; mute[0] = true;
    p.advance(s, 0.0, 4, 0.0, false, 4, mute);
    CHECK(p.advance(s, 2.5, 4, 0.0, false, 4, mute).empty());
}

TEST_CASE("clip respects the start offset") {
    MidiSequence s = makeSeq();
    MidiClipPlayer p;
    bool mute[16] = {};
    CHECK(p.advance(s, 2.0, 4, 1.0, false, 4, mute).empty());            // offset 1 bar; local < 0
    p.advance(s, 4.0, 4, 1.0, false, 4, mute);                          // entry at local 0
    CHECK(countNoteOns(p.advance(s, 4.5, 4, 1.0, false, 4, mute)) == 1); // local 0.5 -> note-on 60
}

TEST_CASE("clip flushes a sounding note at the loop seam") {
    MidiSequence s; s.ok = true;
    s.events = { { 0.0, midiNoteOn(60, 100) } };   // note-on with no note-off in the file
    s.lengthBeats = 2.0;
    MidiClipPlayer p;
    bool mute[16] = {};
    p.advance(s, 0.0, 4, 0.0, true, 0.5, mute);                         // entry, loopLen=2 beats
    CHECK(countNoteOns(p.advance(s, 0.5, 4, 0.0, true, 0.5, mute)) == 1); // note-on 60 (beat 0)
    auto wrap = p.advance(s, 2.25, 4, 0.0, true, 0.5, mute);            // playPos wraps to 0.25
    CHECK(countNoteOffs(wrap) >= 1);                                    // released at the seam
}

TEST_CASE("a paused transport emits nothing") {
    MidiSequence s = makeSeq();
    MidiClipPlayer p;
    bool mute[16] = {};
    p.advance(s, 1.5, 4, 0.0, false, 4, mute);
    CHECK(p.advance(s, 1.5, 4, 0.0, false, 4, mute).empty());           // same position
}
```

- [ ] **Step 2: Wire the test into CMake**

In `CMakeLists.txt`, in `add_executable(core_tests ...)`, add after
`  tests/test_midi_file.cpp`:

```cmake
  tests/test_midi_clip.cpp
```

- [ ] **Step 3: Run to verify it fails**

Run: `cmake --build build -j 2>&1 | tail -5`
Expected: compile error — `core/MidiClip.h` does not exist.

- [ ] **Step 4: Create the header**

Create `src/core/MidiClip.h`:

```cpp
#pragma once
#include <cmath>
#include <vector>
#include "core/Value.h"
#include "core/MidiFile.h"

namespace oss {

// Turns a transport position into the MIDI events to emit this frame, playing a
// MidiSequence anchored at a start offset and optionally looping a region. Tracks
// sounding notes so it can release them across loop seams / stops / mutes. GL-free.
class MidiClipPlayer {
public:
    std::vector<MidiEvent> advance(const MidiSequence& seq, double transportBeats,
                                   double beatsPerBar, double startOffsetBars,
                                   bool loop, double loopLenBars, const bool muted[16]) {
        std::vector<MidiEvent> out;
        double local = transportBeats - startOffsetBars * beatsPerBar;
        if (!seq.ok || local < 0.0) { appendFlush(out); prevPlay_ = -1.0; return out; }

        double loopLen = loopLenBars * beatsPerBar;
        if (loopLen < 1e-6) loopLen = 1e-6;
        double playPos;
        if (loop) {
            playPos = std::fmod(local, loopLen);
        } else {
            if (local >= seq.lengthBeats) { appendFlush(out); prevPlay_ = -1.0; return out; }
            playPos = local;
        }

        // A channel that just became muted releases its sounding notes.
        for (int c = 0; c < 16; ++c) {
            if (muted[c] && !prevMuted_[c]) appendChannelFlush(out, c);
            prevMuted_[c] = muted[c];
        }

        if (prevPlay_ < 0.0) {
            prevPlay_ = playPos;                         // just entered; emit nothing yet
        } else if (playPos >= prevPlay_) {
            emitRange(out, seq, prevPlay_, playPos, muted);
        } else {                                         // wrapped / looped / scrubbed back
            emitRange(out, seq, prevPlay_, loopLen, muted);   // finish the tail
            appendFlush(out);                                  // release sounding notes
            emitRange(out, seq, 0.0, playPos, muted);          // play the head from the loop start
        }
        prevPlay_ = playPos;
        return out;
    }

private:
    void emitRange(std::vector<MidiEvent>& out, const MidiSequence& seq,
                   double a, double b, const bool muted[16]) {
        for (const auto& fe : seq.events) {
            if (fe.beats < a) continue;
            if (fe.beats >= b) break;                    // events are sorted by beats
            int ch = fe.ev.status & 0x0F;
            if (muted[ch]) continue;
            out.push_back(fe.ev);
            unsigned char hi = fe.ev.status & 0xF0u;
            int note = fe.ev.data1 & 0x7F;
            if (hi == 0x90u && fe.ev.data2 > 0)               active_[ch][note] = true;
            else if (hi == 0x80u || (hi == 0x90u && fe.ev.data2 == 0)) active_[ch][note] = false;
        }
    }
    void appendFlush(std::vector<MidiEvent>& out) { for (int c = 0; c < 16; ++c) appendChannelFlush(out, c); }
    void appendChannelFlush(std::vector<MidiEvent>& out, int c) {
        for (int note = 0; note < 128; ++note)
            if (active_[c][note]) {
                out.push_back(MidiEvent{(unsigned char)(0x80 | c), (unsigned char)note, 0});
                active_[c][note] = false;
            }
    }

    double prevPlay_ = -1.0;          // last play position (-1 = not in the clip)
    bool   active_[16][128] = {};     // sounding notes per channel/note
    bool   prevMuted_[16]   = {};     // last frame's mute state per channel
};

} // namespace oss
```

- [ ] **Step 5: Run the tests**

Run: `cmake --build build -j && ./build/core_tests --test-case="clip*,a paused*"`
Expected: PASS (5 cases).

- [ ] **Step 6: Full suite**

Run: `ctest --test-dir build --output-on-failure`
Expected: all pass.

- [ ] **Step 7: Commit**

```bash
git add src/core/MidiClip.h tests/test_midi_clip.cpp CMakeLists.txt
git commit -m "feat(core): add MidiClipPlayer (transport-synced clip playback)"
```

---

### Task 3: `MidiFilePlayerNode` + registration

**Files:**
- Create: `src/modules/MidiFilePlayerNode.h`
- Modify: `src/app/Application.cpp`, `src/main.cpp`
- Test: `tests/test_midi_clip.cpp` (append a node case)

- [ ] **Step 1: Append the node test**

Add these includes to the top of `tests/test_midi_clip.cpp` (after the existing
includes):

```cpp
#include "modules/MidiFilePlayerNode.h"
#include "core/Node.h"
#include "core/Transport.h"
#include <fstream>
#include <cstdio>
#include <string>
#include <variant>
```

Then append this test case to the end of the file:

```cpp
TEST_CASE("MidiFilePlayerNode loads a file and emits notes synced to the transport") {
    // A tiny SMF (note-on 60 @ beat 0) written to a temp file.
    std::vector<unsigned char> smf = {
        'M','T','h','d', 0,0,0,6, 0,0, 0,1, 0x01,0xE0,
        'M','T','r','k', 0,0,0,8,
        0x00, 0x90,0x3C,0x64,
        0x00, 0xFF,0x2F,0x00
    };
    const char* path = "oss_midi_test.mid";
    { std::ofstream f(path, std::ios::binary); f.write((const char*)smf.data(), (std::streamsize)smf.size()); }

    MidiFilePlayerNode node;
    auto eval = [&](Transport& t) {
        std::vector<Value> in(20);
        in[0] = std::string(path);
        in[1] = 0.0f;   // start offset
        in[2] = true;   // loop
        in[3] = 4.0f;   // loop length (bars)
        for (int i = 0; i < 16; ++i) in[4 + i] = false;   // no mutes
        std::vector<Value> out(1);
        EvalContext ctx{ in, out, 0.0f, &t };
        node.evaluate(ctx);
        MidiRef m = std::get<MidiRef>(out[0]);
        return std::vector<MidiEvent>(m.events, m.events + m.count);
    };
    Transport t; t.bpm = 120.0; t.play();   // 0.5 s/beat
    t.seconds = 0.0; eval(t);               // entry frame (also loads the file)
    t.seconds = 0.5; auto e = eval(t);      // 1 beat in -> window [0,1) includes beat 0
    std::remove(path);

    int ons = 0; int firstNote = -1;
    for (auto& x : e) if (midiIsNoteOn(x)) { ++ons; if (firstNote < 0) firstNote = x.data1; }
    CHECK(ons == 1);
    CHECK(firstNote == 60);
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build build -j 2>&1 | tail -5`
Expected: compile error — `modules/MidiFilePlayerNode.h` does not exist.

- [ ] **Step 3: Create `MidiFilePlayerNode.h`**

Create `src/modules/MidiFilePlayerNode.h`:

```cpp
#pragma once
#include <cstddef>
#include <string>
#include <vector>
#include "core/Node.h"
#include "core/Value.h"
#include "core/MidiFile.h"
#include "core/MidiClip.h"

namespace oss {

// Streams a Standard MIDI File synced to the project transport: anchors the clip at a
// start offset (bars), optionally loops a region (bars), and mutes any of the 16 MIDI
// channels. Emits the events in each frame's beat window as a MidiRef (wire into a
// synth such as Acid Bass). The file's own tempo is ignored -- project BPM drives it.
//
// Inputs: 0 = file (String), 1 = start offset (bars), 2 = loop (Bool),
//   3 = loop length (bars), 4..19 = mute 1..16 (Bool, checked = channel muted).
class MidiFilePlayerNode : public Node {
public:
    MidiFilePlayerNode() : Node("MIDI File") {
        addInput("file",         PortType::String, std::string(""));
        addInput("start offset", PortType::Float, 0.0f, 0.0f, 64.0f);   // bars
        addInput("loop",         PortType::Bool, true);
        addInput("loop length",  PortType::Float, 4.0f, 0.25f, 64.0f);  // bars
        for (int i = 0; i < 16; ++i)
            addInput("mute " + std::to_string(i + 1), PortType::Bool, false);
        addOutput("midi", PortType::Midi);
    }

    void evaluate(EvalContext& ctx) override {
        const std::string& path = ctx.in<std::string>(0);
        if (path != loadedPath_) {
            loadedPath_ = path;
            seq_ = loadMidiFile(path);
            status_ = seq_.ok ? ("loaded " + std::to_string(seq_.events.size()) + " events")
                              : (path.empty() ? std::string("no file") : ("error: " + seq_.error));
        }
        bool muted[16];
        for (int c = 0; c < 16; ++c) muted[c] = ctx.in<bool>((std::size_t)(4 + c));

        double beats = ctx.transport ? ctx.transport->beats() : 0.0;
        double bpb   = ctx.transport ? (double)ctx.transport->beatsPerBar : 4.0;

        out_ = player_.advance(seq_, beats, bpb, ctx.in<float>(1), ctx.in<bool>(2),
                               ctx.in<float>(3), muted);
        ctx.out<MidiRef>(0, MidiRef{out_.data(), out_.size()});
    }

    std::string statusLine() const override { return status_; }

private:
    std::string    loadedPath_;
    std::string    status_ = "no file";
    MidiSequence   seq_;
    MidiClipPlayer player_;
    std::vector<MidiEvent> out_;   // this frame's events (owns the MidiRef storage)
};

} // namespace oss
```

- [ ] **Step 4: Register the node type**

In `src/app/Application.cpp`:
- Add the include after `#include "modules/MidiOutputNode.h"`:
```cpp
#include "modules/MidiFilePlayerNode.h"
```
- In `makeNode()`, add after the `if (type == "MIDI In")` line:
```cpp
    if (type == "MIDI File")   return std::make_unique<MidiFilePlayerNode>();
```
- In `nodeCategories()`, change the `MIDI` line from:
```cpp
        { "MIDI",    { "MIDI In", "Step Seq", "Arpeggiator", "MIDI Merge", "MIDI Out" } },
```
to:
```cpp
        { "MIDI",    { "MIDI In", "MIDI File", "Step Seq", "Arpeggiator", "MIDI Merge", "MIDI Out" } },
```

- [ ] **Step 5: Add the node to the screenshot demo**

In `src/main.cpp`, in `runScreenshot`, after the
`app.addNodeOfType("Acid Bass", ...)` line (and before
`app.graph().transport().bpm = 120.0;`), add:

```cpp
        app.addNodeOfType("MIDI File", glm::vec2(40.0f, 320.0f));
```

- [ ] **Step 6: Build + run the tests**

Run: `cmake --build build -j && ./build/core_tests --test-case="MidiFilePlayerNode*" && ctest --test-dir build --output-on-failure 2>&1 | tail -4`
Expected: the node test passes; full suite green.

- [ ] **Step 7: Visual check**

Run: `./build/shader_streamer --screenshot build/_ui.png 2>&1 | tail -2`
Expected: stderr prints `wrote screenshot build/_ui.png (...)` and exits 0. Open it and
confirm a `MIDI File` node renders with a `file` text field, `start offset` / `loop` /
`loop length` controls, the 16 `mute N` checkboxes, and a `midi` output. (If headless
without a GL context, note that this step was skipped.)

- [ ] **Step 8: Commit**

```bash
git add src/modules/MidiFilePlayerNode.h src/app/Application.cpp src/main.cpp tests/test_midi_clip.cpp
git commit -m "feat(modules): add MIDI File player node + register it"
```

---

### Task 4: Documentation

**Files:**
- Modify: `README.md`, `CLAUDE.md`

- [ ] **Step 1: Add the README module-table row**

In `README.md`, in the module table, add this row immediately after the
`| **MIDI In / Out** | ... |` row:

```markdown
| **MIDI File** | streams a Standard MIDI File (.mid) synced to the project BPM → MIDI: anchor `start offset` (bars), `loop` + `loop length` (bars), and `mute 1`…`mute 16` toggles per channel. The file's own tempo is ignored. Wire `out` into a synth (e.g. **Acid Bass**) |
```

- [ ] **Step 2: Add the CLAUDE.md architecture bullet**

In `CLAUDE.md`, in the **Architecture** section, add a new bullet immediately after
the **Acid Bass synth voice** bullet (it ends with "... the node is header-only and
GL-free."):

```markdown
- **MIDI File player** — `MidiFilePlayerNode` (`src/modules/MidiFilePlayerNode.h`,
  header-only) wraps a GL-free from-scratch SMF parser (`src/core/MidiFile.{h,cpp}`,
  events positioned in beats with the file's tempo ignored) and a `MidiClipPlayer`
  (`src/core/MidiClip.h`). Each frame the player derives the clip position from
  `transport.beats()` (anchored at a start-offset, looping a region) and emits the
  events in that frame's `[prevPlay, playPos)` window, flushing note-offs at every
  loop seam / stop / mute so nothing hangs. Output is a `MidiRef` → wire into a synth.
```

- [ ] **Step 3: Sanity check + commit**

Run: `ctest --test-dir build --output-on-failure`
Expected: all pass (no code changed).

```bash
git add README.md CLAUDE.md
git commit -m "docs: document the MIDI File player node"
```

---

## Final verification

- [ ] `cmake --build build -j` — clean build
- [ ] `ctest --test-dir build --output-on-failure` — all pass (`core_tests`, `gl_smoke`)
- [ ] `./build/shader_streamer --screenshot build/_ui.png` — the MIDI File node renders with its ports
- [ ] Use superpowers:finishing-a-development-branch
