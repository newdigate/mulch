# Offline Render Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Render the graph's audio-visual output between a start bar and a finish bar to an MP4 with a fixed clock, so every frame lands in the file with sample-locked audio, from a **File → Render Video…** dialog or a headless `--render` command line.

**Architecture:** A GL-free `core/OfflineRender.h` holds the settings, frame-count/clock math, validation and CLI parsing. An `app/OfflineRenderer` job poses as the transport's external clock, waits on async loaders (new `Node::loading()` hook), mutes the real-time sinks (new `EvalContext::offline`), and captures the Output node's texture through a render-sized FBO plus the Audio Out block into a `VideoEncoder`. `Application::frame` steps it behind a modal progress popup; `main.cpp --render` steps the same class in a plain loop.

**Tech Stack:** C++17, OpenGL 4.1 core (glad), GLFW, Dear ImGui, FFmpeg (existing `VideoEncoder`/`VideoDecoder`), doctest (`core_tests`), headless `gl_smoke`.

**Spec:** `docs/superpowers/specs/2026-09-22-offline-render-design.md`

**Branch:** `feat/offline-render` (already created from `develop`; the spec is committed on it).

---

## File map

| File | Responsibility |
|---|---|
| `src/core/OfflineRender.h` (new) | `RenderSettings`, frame-rate list, frame counts, frame→seconds, samples/frame, validation, CLI parse. GL-free, header-only. |
| `src/core/AsyncLoader.h` | `+ pending()` (in flight AND not finished). |
| `src/core/Node.h` | `+ EvalContext::offline`, `+ virtual bool loading() const`. |
| `src/core/Graph.{h,cpp}` | `+ setOffline()/offline()`, passed into every `EvalContext`. |
| `src/modules/{AudioPlayerNode,DrumMachineNode,MeshLoaderNode,ImageSequencerNode}.h` | `loading()` overrides. |
| `src/modules/AudioOutputNode.{h,cpp}` | Build the stereo block before any device work; `lastBlock()`/`lastSampleRate()`; no device work while offline. |
| `src/modules/MidiOutputNode.cpp` | Send nothing while offline. |
| `src/modules/RecorderNode.cpp` | `record` is off while offline. |
| `src/app/OfflineRenderer.{h,cpp}` (new) | The job: start / step / cancel / progress; encoder, blit FBO, transport + prefs snapshot/restore. |
| `src/ui/RenderDialog.{h,cpp}` (new) | Settings window + modal progress popup. |
| `src/ui/TransportBar.{h,cpp}` | `ProjectBarIO::onRender` → **File → Render Video…**. |
| `src/app/Application.{h,cpp}` | Owns the renderer + dialog; `frame()` steps the render instead of evaluating; accessors for the CLI. |
| `src/main.cpp` | `--render` headless driver. |
| `tests/test_offline_render.cpp` (new), `tests/test_async_loader.cpp` (new) | doctest coverage of the GL-free pieces. |
| `tests/gl_smoke.cpp` | Offline-sink checks, renderer end-to-end / gate / timeout / cancel scenarios. |
| `tests/assets/render_smoke.oss` (new) | Fixture project for the `render_cli` ctest. |
| `CMakeLists.txt`, `.github/workflows/build-*.yml` | Sources, tests, best-effort `render_cli`. |
| `README.md`, `CLAUDE.md` | Docs. |

## Conventions used below

- Build: `cmake --build build -j` (configure first with `cmake -S . -B build` if `build/` is missing).
- Unit tests: `ctest --test-dir build -R core_tests --output-on-failure`; a single case: `./build/core_tests -tc="<case name>"`.
- Headless GL test: `ctest --test-dir build -R gl_smoke --output-on-failure` (needs a display; skips otherwise).
- Every `gl_smoke` failure follows the file's pattern: `{ glfwTerminate(); return fail("..."); }`. The lambda `near(v, t)` (±3) defined near the top of `main()` is in scope for every scenario.
- Commit after each task with a Conventional Commits message ending in `Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>`.

---

### Task 0: Baseline

**Files:** none.

- [ ] **Step 1: Confirm the branch and a green baseline**

Run:
```bash
git branch --show-current && cmake -S . -B build && cmake --build build -j && ctest --test-dir build -R core_tests --output-on-failure
```
Expected: `feat/offline-render`; build succeeds; `100% tests passed`.

---

### Task 1: Frame-count and clock helpers (`core/OfflineRender.h`)

**Files:**
- Create: `src/core/OfflineRender.h`
- Create: `tests/test_offline_render.cpp`
- Modify: `CMakeLists.txt` (core_tests source list, after `tests/test_bar_sync.cpp`)

- [ ] **Step 1: Write the failing tests**

Create `tests/test_offline_render.cpp`:
```cpp
#include <doctest/doctest.h>
#include "core/OfflineRender.h"

using namespace oss;

// 120 bpm, 4/4 -> 2 s per bar.
static const double kSpb120 = 2.0;

TEST_CASE("renderFrameCount: exact for the listed frame rates") {
    RenderSettings s; s.startBar = 0.0; s.endBar = 8.0; s.fps = 60;
    CHECK(renderFrameCount(s, kSpb120) == 960);                 // 16 s * 60
    s.fps = 30; s.endBar = 1.0;
    CHECK(renderFrameCount(s, 2.4) == 72);                      // 1 bar @ 100 bpm = 2.4 s
    s.fps = 60; s.startBar = 0.0; s.endBar = 0.5;
    CHECK(renderFrameCount(s, kSpb120) == 60);                  // half a bar
    s.startBar = 2.0; s.endBar = 10.0; s.fps = 25;
    CHECK(renderFrameCount(s, kSpb120) == 400);                 // start offset does not matter
}

TEST_CASE("renderFrameCount: float noise does not add a frame, a tiny range still yields one") {
    RenderSettings s; s.startBar = 0.0; s.endBar = 0.1; s.fps = 60;
    CHECK(renderFrameCount(s, kSpb120) == 12);                  // 0.2 s * 60 = 12.000000000000002
    s.endBar = 0.0001;
    CHECK(renderFrameCount(s, kSpb120) == 1);                   // non-empty -> at least one frame
    s.endBar = 0.0;
    CHECK(renderFrameCount(s, kSpb120) == 0);                   // empty
    s.endBar = -1.0;
    CHECK(renderFrameCount(s, kSpb120) == 0);                   // inverted
}

TEST_CASE("prerollFrameCount: same rule over prerollBars") {
    RenderSettings s; s.prerollBars = 1.0; s.fps = 60;
    CHECK(prerollFrameCount(s, kSpb120) == 120);
    s.prerollBars = 0.5; s.fps = 30;
    CHECK(prerollFrameCount(s, kSpb120) == 30);
    s.prerollBars = 0.0;
    CHECK(prerollFrameCount(s, kSpb120) == 0);
}

TEST_CASE("renderFrameSeconds: frame k from the start bar, clamped at zero") {
    RenderSettings s; s.startBar = 4.0; s.fps = 60;
    CHECK(renderFrameSeconds(s, kSpb120, 0)   == doctest::Approx(8.0));
    CHECK(renderFrameSeconds(s, kSpb120, 30)  == doctest::Approx(8.5));
    CHECK(renderFrameSeconds(s, kSpb120, -1)  == doctest::Approx(8.0 - 1.0 / 60.0));
    s.startBar = 0.0;
    CHECK(renderFrameSeconds(s, kSpb120, -120) == doctest::Approx(0.0));   // pre-roll sits at the start
    CHECK(renderFrameSeconds(s, kSpb120, -1)   == doctest::Approx(0.0));
}

TEST_CASE("audioSamplesPerFrame: exact at 48 kHz for every listed rate") {
    for (int fps : kRenderFrameRates) {
        CHECK(isRenderFrameRate(fps));
        CHECK(48000 % fps == 0);
        CHECK(audioSamplesPerFrame(48000, fps) == 48000 / fps);
    }
    CHECK(audioSamplesPerFrame(48000, 60) == 800);
    CHECK(audioSamplesPerFrame(48000, 0) == 0);
    CHECK_FALSE(isRenderFrameRate(29));
    CHECK_FALSE(isRenderFrameRate(0));
}
```

- [ ] **Step 2: Register the test and run it to verify it fails**

In `CMakeLists.txt`, in the `add_executable(core_tests` list, add after `tests/test_bar_sync.cpp`:
```cmake
  tests/test_offline_render.cpp
```
Run: `cmake --build build -j 2>&1 | tail -5`
Expected: FAIL — `fatal error: 'core/OfflineRender.h' file not found`.

- [ ] **Step 3: Write the header**

Create `src/core/OfflineRender.h`:
```cpp
#pragma once
#include <cmath>
#include <string>
#include <vector>

namespace oss {

// Offline render settings + the GL-free math behind them (frame counts, the fixed clock,
// validation, command-line parsing). The OfflineRenderer (app/) drives the graph from these;
// the RenderDialog (ui/) edits them. Bars follow the Loop-field convention: 0 = the first bar,
// the finish bar is exclusive, fractions allowed.

constexpr int    kRenderFrameRates[]   = {24, 25, 30, 50, 60};   // every one divides 48 000
constexpr int    kRenderFrameRateCount = 5;
constexpr int    kRenderMinSize        = 16;
constexpr int    kRenderMaxSize        = 8192;
constexpr double kRenderLoadTimeoutSeconds = 30.0;   // give up waiting for a node's async load

struct RenderSettings {
    double startBar    = 0.0;    // first captured bar (>= 0)
    double endBar      = 8.0;    // exclusive
    double prerollBars = 1.0;    // bars evaluated but not captured before startBar (>= 0)
    int    fps         = 60;     // one of kRenderFrameRates
    int    width       = 1280;   // render size (own bounds, independent of the Preferences clamp)
    int    height      = 720;
    std::string outPath;         // the .mp4 to write
};

inline bool isRenderFrameRate(int fps) {
    for (int r : kRenderFrameRates) if (r == fps) return true;
    return false;
}

// Frames over `durationSeconds`: those whose start time lies in [0, duration) = ceil(dur*fps),
// with a small epsilon so float noise (0.2 s * 60 = 12.000000000000002) never adds a frame,
// and a floor of 1 so a non-empty range always yields a frame. 0 for an empty range.
inline long framesOver(double durationSeconds, int fps) {
    if (durationSeconds <= 0.0 || fps <= 0) return 0;
    long n = (long)std::ceil(durationSeconds * fps - 1e-6);
    return n < 1 ? 1 : n;
}

// Captured frames for [startBar, endBar).
inline long renderFrameCount(const RenderSettings& s, double secondsPerBar) {
    return framesOver((s.endBar - s.startBar) * secondsPerBar, s.fps);
}

// Discarded pre-roll frames before startBar.
inline long prerollFrameCount(const RenderSettings& s, double secondsPerBar) {
    return framesOver(s.prerollBars * secondsPerBar, s.fps);
}

// Transport position for frame k (k < 0 during pre-roll), clamped at 0 so a render that starts
// at bar 0 pre-rolls sitting at the start.
inline double renderFrameSeconds(const RenderSettings& s, double secondsPerBar, long k) {
    double t = s.startBar * secondsPerBar + (double)k / (double)s.fps;
    return t < 0.0 ? 0.0 : t;
}

// Interleaved-stereo FRAMES of audio per video frame (exact at 48 kHz for the listed rates).
inline int audioSamplesPerFrame(int sampleRate, int fps) {
    return fps > 0 ? sampleRate / fps : 0;
}

} // namespace oss
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build build -j 2>&1 | tail -3 && ./build/core_tests -tc="renderFrameCount*,prerollFrameCount*,renderFrameSeconds*,audioSamplesPerFrame*"`
Expected: `[doctest] test cases: 5 | 5 passed | 0 failed`.

- [ ] **Step 5: Commit**

```bash
git add src/core/OfflineRender.h tests/test_offline_render.cpp CMakeLists.txt
git commit -m "feat(core): offline render settings + frame-count / fixed-clock helpers

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 2: Validation and CLI argument parsing (`core/OfflineRender.h`)

**Files:**
- Modify: `src/core/OfflineRender.h`
- Modify: `tests/test_offline_render.cpp`

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_offline_render.cpp`:
```cpp
static RenderSettings validSettings() {
    RenderSettings s; s.startBar = 0.0; s.endBar = 4.0; s.prerollBars = 1.0;
    s.fps = 60; s.width = 1280; s.height = 720; s.outPath = "out.mp4";
    return s;
}

TEST_CASE("validateRenderSettings: a good set passes and each fault has its own message") {
    std::string err;
    CHECK(validateRenderSettings(validSettings(), true, err));
    CHECK(err.empty());

    RenderSettings s = validSettings();
    CHECK_FALSE(validateRenderSettings(s, false, err));
    CHECK(err == "add an Output node");

    s = validSettings(); s.endBar = s.startBar;
    CHECK_FALSE(validateRenderSettings(s, true, err));
    CHECK(err == "finish bar must be after start bar");

    s = validSettings(); s.startBar = -1.0; s.endBar = 2.0;
    CHECK_FALSE(validateRenderSettings(s, true, err));
    CHECK(err == "start bar must be 0 or later");

    s = validSettings(); s.prerollBars = -0.5;
    CHECK_FALSE(validateRenderSettings(s, true, err));
    CHECK(err == "pre-roll must be 0 or more bars");

    s = validSettings(); s.fps = 29;
    CHECK_FALSE(validateRenderSettings(s, true, err));
    CHECK(err == "frame rate must be 24, 25, 30, 50 or 60");

    s = validSettings(); s.width = 8;
    CHECK_FALSE(validateRenderSettings(s, true, err));
    CHECK(err == "width and height must be between 16 and 8192");

    s = validSettings(); s.height = 9000;
    CHECK_FALSE(validateRenderSettings(s, true, err));
    CHECK(err == "width and height must be between 16 and 8192");

    s = validSettings(); s.width = 641;
    CHECK_FALSE(validateRenderSettings(s, true, err));
    CHECK(err == "width and height must be even");

    s = validSettings(); s.outPath.clear();
    CHECK_FALSE(validateRenderSettings(s, true, err));
    CHECK(err == "choose an output file");
}

TEST_CASE("parseRenderArgs: full option set") {
    RenderCliArgs a; std::string err;
    REQUIRE(parseRenderArgs({"song.oss", "out.mp4", "--start", "2", "--end", "10.5",
                             "--fps", "30", "--size", "1920x1080", "--preroll", "0.25"}, a, err));
    CHECK(a.projectPath == "song.oss");
    CHECK(a.settings.outPath == "out.mp4");
    CHECK(a.settings.startBar == doctest::Approx(2.0));
    CHECK(a.settings.endBar == doctest::Approx(10.5));
    CHECK(a.settings.fps == 30);
    CHECK(a.settings.width == 1920);
    CHECK(a.settings.height == 1080);
    CHECK(a.settings.prerollBars == doctest::Approx(0.25));
}

TEST_CASE("parseRenderArgs: defaults leave the sentinels for the driver to fill") {
    RenderCliArgs a; std::string err;
    REQUIRE(parseRenderArgs({"song.oss", "out.mp4"}, a, err));
    CHECK(a.settings.startBar == doctest::Approx(0.0));
    CHECK(a.settings.endBar == doctest::Approx(-1.0));     // -> the project's song length
    CHECK(a.settings.width == 0);                          // -> the Preferences texture size
    CHECK(a.settings.height == 0);
    CHECK(a.settings.fps == 60);
    CHECK(a.settings.prerollBars == doctest::Approx(1.0));
}

TEST_CASE("parseRenderArgs: bad input is rejected with a message") {
    RenderCliArgs a; std::string err;
    CHECK_FALSE(parseRenderArgs({"song.oss"}, a, err));                       // missing output
    CHECK(err.rfind("usage:", 0) == 0);
    CHECK_FALSE(parseRenderArgs({"song.oss", "out.mp4", "--size", "12x"}, a, err));
    CHECK(err == "--size expects WxH, got 12x");
    CHECK_FALSE(parseRenderArgs({"song.oss", "out.mp4", "--size", "abc"}, a, err));
    CHECK(err == "--size expects WxH, got abc");
    CHECK_FALSE(parseRenderArgs({"song.oss", "out.mp4", "--fps", "sixty"}, a, err));
    CHECK(err == "bad value for --fps: sixty");
    CHECK_FALSE(parseRenderArgs({"song.oss", "out.mp4", "--end"}, a, err));
    CHECK(err == "--end needs a value");
    CHECK_FALSE(parseRenderArgs({"song.oss", "out.mp4", "--bogus", "1"}, a, err));
    CHECK(err == "unknown option --bogus");
}
```

- [ ] **Step 2: Run to verify they fail**

Run: `cmake --build build -j 2>&1 | grep -m1 error`
Expected: `error: use of undeclared identifier 'validateRenderSettings'` (or `RenderCliArgs`).

- [ ] **Step 3: Implement validation + parsing**

Append to `src/core/OfflineRender.h`, inside `namespace oss` (before the closing brace), and add `#include <cstdlib>` to the includes:
```cpp
// One reason a render cannot start, or true with `err` cleared. `hasOutputNode` is supplied
// by the caller (core knows no node types). Even dimensions: the H.264 yuv420p encode needs them.
inline bool validateRenderSettings(const RenderSettings& s, bool hasOutputNode, std::string& err) {
    if (!hasOutputNode)            { err = "add an Output node"; return false; }
    if (!(s.endBar > s.startBar))  { err = "finish bar must be after start bar"; return false; }
    if (s.startBar < 0.0)          { err = "start bar must be 0 or later"; return false; }
    if (s.prerollBars < 0.0)       { err = "pre-roll must be 0 or more bars"; return false; }
    if (!isRenderFrameRate(s.fps)) { err = "frame rate must be 24, 25, 30, 50 or 60"; return false; }
    if (s.width  < kRenderMinSize || s.width  > kRenderMaxSize ||
        s.height < kRenderMinSize || s.height > kRenderMaxSize) {
        err = "width and height must be between 16 and 8192"; return false;
    }
    if ((s.width % 2) || (s.height % 2)) { err = "width and height must be even"; return false; }
    if (s.outPath.empty())         { err = "choose an output file"; return false; }
    err.clear();
    return true;
}

// `--render <project.oss> <out.mp4> [--start B] [--end B] [--fps N] [--size WxH] [--preroll B]`
// (`args` = everything after `--render`). Fields not given are left as SENTINELS for the
// driver to fill once the project is loaded: endBar = -1 (-> the project's Automation song
// length) and width = height = 0 (-> the Preferences texture size).
struct RenderCliArgs {
    std::string    projectPath;
    RenderSettings settings;
};

inline bool parseRenderArgs(const std::vector<std::string>& args, RenderCliArgs& out, std::string& err) {
    out = RenderCliArgs{};
    out.settings.endBar = -1.0;
    out.settings.width  = 0;
    out.settings.height = 0;
    std::vector<std::string> positional;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a.size() < 2 || a[0] != '-' || a[1] != '-') { positional.push_back(a); continue; }
        if (a != "--start" && a != "--end" && a != "--fps" && a != "--size" && a != "--preroll") {
            err = "unknown option " + a; return false;
        }
        if (i + 1 >= args.size()) { err = a + " needs a value"; return false; }
        const std::string& v = args[++i];
        const char* c = v.c_str();
        char* end = nullptr;
        if (a == "--size") {
            long w = std::strtol(c, &end, 10);
            if (end == c || *end != 'x') { err = "--size expects WxH, got " + v; return false; }
            const char* hs = end + 1;
            long h = std::strtol(hs, &end, 10);
            if (end == hs || *end != '\0') { err = "--size expects WxH, got " + v; return false; }
            out.settings.width = (int)w; out.settings.height = (int)h;
            continue;
        }
        double d = std::strtod(c, &end);
        if (end == c || *end != '\0') { err = "bad value for " + a + ": " + v; return false; }
        if      (a == "--start")   out.settings.startBar    = d;
        else if (a == "--end")     out.settings.endBar      = d;
        else if (a == "--preroll") out.settings.prerollBars = d;
        else /* --fps */           out.settings.fps         = (int)d;
    }
    if (positional.size() != 2) {
        err = "usage: --render <project.oss> <out.mp4> [--start B] [--end B] [--fps N] [--size WxH] [--preroll B]";
        return false;
    }
    out.projectPath      = positional[0];
    out.settings.outPath = positional[1];
    return true;
}
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build build -j 2>&1 | tail -3 && ./build/core_tests -tc="validateRenderSettings*,parseRenderArgs*"`
Expected: `test cases: 4 | 4 passed`.

- [ ] **Step 5: Commit**

```bash
git add src/core/OfflineRender.h tests/test_offline_render.cpp
git commit -m "feat(core): offline render validation + --render argument parsing

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 3: `Node::loading()` hook and `AsyncLoader::pending()`

**Files:**
- Modify: `src/core/AsyncLoader.h`
- Modify: `src/core/Node.h` (after `loadState`)
- Modify: `src/modules/AudioPlayerNode.h`, `src/modules/DrumMachineNode.h`, `src/modules/MeshLoaderNode.h`, `src/modules/ImageSequencerNode.h`
- Create: `tests/test_async_loader.cpp`
- Modify: `CMakeLists.txt` (core_tests sources + Threads)

- [ ] **Step 1: Write the failing test**

Create `tests/test_async_loader.cpp`:
```cpp
#include <doctest/doctest.h>
#include "core/AsyncLoader.h"
#include "core/Node.h"
#include <chrono>
#include <future>
#include <thread>

using namespace oss;

TEST_CASE("AsyncLoader::pending is true only while the worker has not finished") {
    AsyncLoader<int> loader;
    CHECK_FALSE(loader.pending());                              // nothing requested

    std::promise<void> gate;
    std::shared_future<void> open = gate.get_future().share();
    REQUIRE(loader.request("a", [open]{ open.wait(); return 42; }));
    CHECK(loader.pending());                                    // worker blocked on the gate

    gate.set_value();
    for (int i = 0; i < 1000 && loader.pending(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    CHECK_FALSE(loader.pending());                              // finished but NOT yet polled -> not pending

    int out = 0;
    CHECK(loader.poll(out));
    CHECK(out == 42);
    CHECK_FALSE(loader.pending());

    loader.request("", []{ return 0; });                        // cleared
    CHECK_FALSE(loader.pending());
}

namespace {
struct Plain : Node { Plain() : Node("plain") {} void evaluate(EvalContext&) override {} };
}

TEST_CASE("Node::loading defaults to false") {
    Plain n;
    CHECK_FALSE(n.loading());
}
```

- [ ] **Step 2: Register + run to verify it fails**

In `CMakeLists.txt` add `tests/test_async_loader.cpp` right after `tests/test_offline_render.cpp` in the `core_tests` list. Then, just before `target_link_libraries(core_tests ...)`, add `find_package(Threads REQUIRED)` and append `Threads::Threads` to that `target_link_libraries(core_tests PRIVATE ...)` line (std::async needs pthread on older glibc).

Run: `cmake -S . -B build > /dev/null && cmake --build build -j 2>&1 | grep -m1 error`
Expected: `error: no member named 'pending' in 'oss::AsyncLoader<int>'`.

- [ ] **Step 3: Add `pending()` and `loading()`**

In `src/core/AsyncLoader.h`, after `poll()`:
```cpp
    // True while a load is in flight AND not yet finished. A finished-but-unpolled future does
    // NOT count: the offline renderer waits on this between frames, and the node's next
    // evaluate() consumes the result via poll() -- counting it would wait for an evaluate that
    // only happens after the wait ends.
    bool pending() const {
        return future_.valid() &&
               future_.wait_for(std::chrono::seconds(0)) != std::future_status::ready;
    }
```

In `src/core/Node.h`, after the `loadState` declaration:
```cpp
    // True while an asynchronous load (worker-thread decode/parse) is in flight and NOT yet
    // finished. The offline renderer polls this between frames and waits before advancing, so
    // it must be answerable without evaluate(). Default false (synchronous nodes).
    virtual bool loading() const { return false; }
```

- [ ] **Step 4: Run the tests**

Run: `cmake --build build -j 2>&1 | tail -3 && ./build/core_tests -tc="AsyncLoader::pending*,Node::loading*"`
Expected: `test cases: 2 | 2 passed`.

- [ ] **Step 5: Override `loading()` in the four async nodes**

`src/modules/AudioPlayerNode.h`, after `statusLine()`:
```cpp
    bool loading() const override { return loader_.pending(); }
```

`src/modules/MeshLoaderNode.h`, after `statusLine()`:
```cpp
    bool loading() const override { return loader_.pending(); }
```

`src/modules/DrumMachineNode.h`, after `statusLine()`:
```cpp
    bool loading() const override {
        for (const auto& l : loaders_) if (l.pending()) return true;
        return false;
    }
```

`src/modules/ImageSequencerNode.h`, after `statusLine()` (line ~162):
```cpp
    bool loading() const override {   // a prefetch in flight and not yet decoded
        return fetch_.valid() && fetch_.wait_for(std::chrono::seconds(0)) != std::future_status::ready;
    }
```

- [ ] **Step 6: Build everything and run all unit tests**

Run: `cmake --build build -j 2>&1 | tail -3 && ctest --test-dir build -R core_tests --output-on-failure | tail -3`
Expected: build OK, `100% tests passed`.

- [ ] **Step 7: Commit**

```bash
git add src/core/AsyncLoader.h src/core/Node.h src/modules/AudioPlayerNode.h src/modules/DrumMachineNode.h src/modules/MeshLoaderNode.h src/modules/ImageSequencerNode.h tests/test_async_loader.cpp CMakeLists.txt
git commit -m "feat(core): Node::loading() hook backed by AsyncLoader::pending() for the async loaders

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 4: `EvalContext::offline` + `Graph::setOffline`

**Files:**
- Modify: `src/core/Node.h` (`EvalContext`)
- Modify: `src/core/Graph.h`, `src/core/Graph.cpp`
- Modify: `tests/test_offline_render.cpp`

- [ ] **Step 1: Write the failing test**

Append to `tests/test_offline_render.cpp` (add `#include "core/Graph.h"`, `#include "core/Node.h"`, `#include <memory>` at the top of the file):
```cpp
namespace {
struct OfflineProbe : Node {
    bool seen = false;
    OfflineProbe() : Node("probe") {}
    void evaluate(EvalContext& ctx) override { seen = ctx.offline; }
};
}

TEST_CASE("Graph::setOffline reaches every node through EvalContext::offline") {
    Graph g;
    auto p = std::make_unique<OfflineProbe>();
    OfflineProbe* probe = p.get();
    g.addNode(std::move(p));
    CHECK_FALSE(g.offline());
    g.evaluate(1.0f / 60.0f);
    CHECK_FALSE(probe->seen);
    g.setOffline(true);
    CHECK(g.offline());
    g.evaluate(1.0f / 60.0f);
    CHECK(probe->seen);
    g.setOffline(false);
    g.evaluate(1.0f / 60.0f);
    CHECK_FALSE(probe->seen);
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build build -j 2>&1 | grep -m1 error`
Expected: `error: no member named 'offline' in 'oss::EvalContext'`.

- [ ] **Step 3: Implement**

`src/core/Node.h`, in `struct EvalContext` after `prefs`:
```cpp
    bool                      offline   = false;    // an offline render drives the graph (set by Graph::evaluate):
                                                    // real-time sinks (Audio Out, MIDI Out, Recorder) must stay quiet
```

`src/core/Graph.h`, after `setPreferences`:
```cpp
    // Offline-render mode: every EvalContext carries `offline = true` so the real-time sinks
    // (Audio Out, MIDI Out, Recorder) stay quiet while the OfflineRenderer drives the graph
    // faster (or slower) than real time. Set/cleared by the renderer.
    void setOffline(bool on) { offline_ = on; }
    bool offline() const { return offline_; }
```
and a member next to `prefs_`:
```cpp
    bool offline_ = false;
```

`src/core/Graph.cpp`, in `evaluate()` replace the `EvalContext ctx{...}` line with:
```cpp
        EvalContext ctx{inputs, outs, dt, &transport_, prefs_, offline_};
```

- [ ] **Step 4: Run the test**

Run: `cmake --build build -j 2>&1 | tail -3 && ./build/core_tests -tc="Graph::setOffline*"`
Expected: `test cases: 1 | 1 passed`.

- [ ] **Step 5: Commit**

```bash
git add src/core/Node.h src/core/Graph.h src/core/Graph.cpp tests/test_offline_render.cpp
git commit -m "feat(core): Graph::setOffline flows EvalContext::offline to every node

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 5: Offline-aware sinks (Audio Out tap, MIDI Out, Recorder)

**Files:**
- Modify: `src/modules/AudioOutputNode.h`, `src/modules/AudioOutputNode.cpp`
- Modify: `src/modules/MidiOutputNode.cpp`
- Modify: `src/modules/RecorderNode.cpp`
- Modify: `CMakeLists.txt` (gl_smoke gains `AudioOutputNode.cpp` + libsoundio)
- Modify: `tests/gl_smoke.cpp`

- [ ] **Step 1: Add Audio Out to the gl_smoke build**

In `CMakeLists.txt`, `add_executable(gl_smoke` list: add `src/modules/AudioOutputNode.cpp` after `src/modules/AudioPlayerNode.cpp`. Add `${libsoundio_SOURCE_DIR}` to `target_include_directories(gl_smoke ...)` and `libsoundio_static` to `target_link_libraries(gl_smoke PRIVATE ...)`.

- [ ] **Step 2: Write the failing gl_smoke scenario**

In `tests/gl_smoke.cpp` add includes near the other module includes:
```cpp
#include "modules/AudioOutputNode.h"
#include "core/Preferences.h"
```
Insert this scenario just before the final `glfwDestroyWindow(win);` at the end of `main()`:
```cpp
    // --- Scenario: offline mode -- Audio Out taps its block without a device, Recorder stays idle ---
    {
        Graph g;
        auto sine = std::make_unique<SineWaveNode>();
        auto aout = std::make_unique<AudioOutputNode>();
        auto col  = std::make_unique<ColourNode>(); col->initGL();
        auto rec  = std::make_unique<RecorderNode>();
        rec->inputDefault(3) = true;                                            // record on
        rec->inputDefault(4) = std::string("build/_offline_should_not_exist.mp4");
        auto out  = std::make_unique<OutputNode>(); out->initGL();
        int sId = g.addNode(std::move(sine)); int aId = g.addNode(std::move(aout));
        int cId = g.addNode(std::move(col));  int rId = g.addNode(std::move(rec));
        int oId = g.addNode(std::move(out));
        if (!g.connect(sId, 0, aId, 0) || !g.connect(cId, 0, rId, 0) || !g.connect(rId, 0, oId, 0)) {
            glfwTerminate(); return fail("offline sinks: connect");
        }
        std::remove("build/_offline_should_not_exist.mp4");
        auto* an = dynamic_cast<AudioOutputNode*>(g.findNode(aId));
        auto* rn = dynamic_cast<RecorderNode*>(g.findNode(rId));

        g.setOffline(true);
        for (int f = 0; f < 3; ++f) g.evaluate(1.0f / 60.0f);
        // Audio Out: a lone left wire mirrors to both channels; 800 frames at 48 kHz / 60 fps.
        if (an->lastSampleRate() != 48000) { glfwTerminate(); return fail("offline sinks: Audio Out sample rate not tapped"); }
        if (an->lastBlock().size() != 800 * 2) { glfwTerminate(); return fail("offline sinks: Audio Out block should be 800 stereo frames"); }
        bool mirrored = true;
        for (std::size_t i = 0; i < an->lastBlock().size(); i += 2)
            if (an->lastBlock()[i] != an->lastBlock()[i + 1]) { mirrored = false; break; }
        if (!mirrored) { glfwTerminate(); return fail("offline sinks: lone mono wire should mirror to both channels"); }
        // Recorder: `record` is ignored while offline -> no file, status still idle.
        if (rn->statusLine() != "idle") { glfwTerminate(); return fail("offline sinks: Recorder should stay idle while offline"); }
        if (std::ifstream("build/_offline_should_not_exist.mp4").good()) { glfwTerminate(); return fail("offline sinks: Recorder wrote a file while offline"); }
        // Nothing connected -> empty block, rate 0.
        g.disconnect(aId, 0);
        g.evaluate(1.0f / 60.0f);
        if (!an->lastBlock().empty() || an->lastSampleRate() != 0) { glfwTerminate(); return fail("offline sinks: disconnected Audio Out should tap an empty block"); }
        g.setOffline(false);
        std::fprintf(stderr, "gl_smoke OK: offline mode taps the Audio Out block and keeps the Recorder idle\n");
    }
```

- [ ] **Step 3: Run to verify it fails**

Run: `cmake -S . -B build > /dev/null && cmake --build build -j 2>&1 | grep -m1 error`
Expected: `error: no member named 'lastSampleRate' in 'oss::AudioOutputNode'`.

- [ ] **Step 4: Implement the Audio Out tap**

`src/modules/AudioOutputNode.h` — add to the public section after `evaluate`:
```cpp
    // The interleaved-stereo block (L,R,L,R,...) built by the last evaluate(), empty when nothing
    // was connected, and its sample rate (0 when empty). Built on EVERY evaluate -- with or
    // without a device, live or offline -- so the offline renderer can tap "what you hear".
    const std::vector<float>& lastBlock() const { return stereoScratch_; }
    int lastSampleRate() const { return lastSampleRate_; }
```
and a private member after `sampleRate_`:
```cpp
    int  lastSampleRate_ = 0;           // rate of the block in stereoScratch_ (0 = empty)
```

`src/modules/AudioOutputNode.cpp` — replace the whole `evaluate` with:
```cpp
void AudioOutputNode::evaluate(EvalContext& ctx) {
    AudioRef l = ctx.in<AudioRef>(0);
    AudioRef r = ctx.in<AudioRef>(1);
    // Symmetric mirror: a single connected side feeds both speakers, so a lone
    // mono wire just works. The ring carries interleaved stereo (L,R,L,R). The
    // block is built BEFORE any device work so it exists with or without a device
    // and while offline (the offline renderer reads it via lastBlock()).
    const AudioRef& effL = (l.samples && l.count > 0) ? l : r;
    const AudioRef& effR = (r.samples && r.count > 0) ? r : l;
    std::size_t nL = effL.samples ? effL.count : 0;
    std::size_t nR = effR.samples ? effR.count : 0;
    std::size_t n  = std::max(nL, nR);
    stereoScratch_.resize(n * 2);
    for (std::size_t i = 0; i < n; ++i) {
        stereoScratch_[i * 2]     = (i < nL) ? effL.samples[i] : 0.0f;
        stereoScratch_[i * 2 + 1] = (i < nR) ? effR.samples[i] : 0.0f;
    }
    lastSampleRate_ = n > 0 ? effL.sampleRate : 0;
    if (ctx.offline) return;               // offline render: tapped, never touches the device or ring

    std::string want = ctx.prefs ? ctx.prefs->audioOutputDeviceId : std::string();
    int wantMs       = ctx.prefs ? ctx.prefs->audioBufferMs : 150;
    if (!ensureDevice(want, wantMs)) return;   // no device -> silent no-op
    soundio_flush_events(soundio_);        // pump device events (non-blocking)
    if (n == 0) return;                                  // nothing connected -> silence
    if (ring_) ring_->push(stereoScratch_.data(), n * 2);   // overflow dropped, never blocks
}
```

- [ ] **Step 5: MIDI Out and Recorder honour `offline`**

`src/modules/MidiOutputNode.cpp`, in `evaluate` right after `syncPorts(ctx.prefs);`:
```cpp
    if (ctx.offline) return;   // offline render: never spray events at hardware off the real-time clock
```

`src/modules/RecorderNode.cpp`, replace `bool     rec   = ctx.in<bool>(3);` with:
```cpp
    bool     rec   = ctx.in<bool>(3) && !ctx.offline;   // an offline render owns the encoder: a live recording stops + saves
```

- [ ] **Step 6: Build and run gl_smoke**

Run: `cmake --build build -j 2>&1 | tail -3 && ctest --test-dir build -R gl_smoke --output-on-failure 2>&1 | grep -E "offline mode|FAIL|tests passed"`
Expected: `gl_smoke OK: offline mode taps the Audio Out block and keeps the Recorder idle` and `100% tests passed`.

- [ ] **Step 7: Commit**

```bash
git add src/modules/AudioOutputNode.h src/modules/AudioOutputNode.cpp src/modules/MidiOutputNode.cpp src/modules/RecorderNode.cpp CMakeLists.txt tests/gl_smoke.cpp
git commit -m "feat(modules): offline-aware sinks -- Audio Out taps its block, MIDI Out and Recorder stay quiet

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 6: `OfflineRenderer` skeleton — start, finish, cancel, state restore

**Files:**
- Create: `src/app/OfflineRenderer.h`, `src/app/OfflineRenderer.cpp`
- Modify: `CMakeLists.txt` (APP_SOURCES + gl_smoke)
- Modify: `tests/gl_smoke.cpp`

- [ ] **Step 1: Write the failing gl_smoke scenario**

Add includes to `tests/gl_smoke.cpp`:
```cpp
#include "core/OfflineRender.h"
#include "app/OfflineRenderer.h"
```
Insert before the final `glfwDestroyWindow(win);`:
```cpp
    // --- Scenario: OfflineRenderer start/cancel -- validation, prefs swap, transport snapshot + restore ---
    {
        Graph g;
        auto col = std::make_unique<ColourNode>(); col->initGL();
        int cId = g.addNode(std::move(col));
        Preferences live; live.textureWidth = 320; live.textureHeight = 240;
        g.setPreferences(&live);
        g.transport().bpm = 120.0; g.transport().seconds = 5.0; g.transport().looping = true;

        RenderSettings s; s.startBar = 0.0; s.endBar = 1.0; s.prerollBars = 0.5; s.fps = 30;
        s.width = 160; s.height = 120; s.outPath = "build/_offline_cancel.mp4";
        OfflineRenderer r; std::string err;
        if (r.start(g, &live, s, err)) { glfwTerminate(); return fail("offline start: should refuse a graph with no Output node"); }
        if (err != "add an Output node") { glfwTerminate(); return fail("offline start: wrong error for a missing Output node"); }

        auto out = std::make_unique<OutputNode>(); out->initGL();
        int oId = g.addNode(std::move(out));
        if (!g.connect(cId, 0, oId, 0)) { glfwTerminate(); return fail("offline start: connect"); }
        if (!r.start(g, &live, s, err)) { glfwTerminate(); return fail(("offline start: " + err).c_str()); }
        if (!r.active()) { glfwTerminate(); return fail("offline start: not active"); }
        if (r.progress().framesTotal != 60 || r.progress().prerollTotal != 30) { glfwTerminate(); return fail("offline start: expected 60 frames + 30 pre-roll (0.5 bar = 1 s at 30 fps)"); }
        if (!g.offline()) { glfwTerminate(); return fail("offline start: graph should be offline"); }
        const Transport& t = g.transport();
        if (!(t.externalClock && t.playing && !t.looping)) { glfwTerminate(); return fail("offline start: transport should be an external, playing, non-looping clock"); }

        r.cancel();
        if (r.active()) { glfwTerminate(); return fail("offline cancel: still active"); }
        if (r.progress().phase != OfflineRenderer::Phase::Cancelled) { glfwTerminate(); return fail("offline cancel: phase should be Cancelled"); }
        if (g.offline()) { glfwTerminate(); return fail("offline cancel: graph still offline"); }
        if (!(t.seconds == 5.0 && t.looping && !t.playing && !t.externalClock && t.bpm == 120.0)) { glfwTerminate(); return fail("offline cancel: transport not restored"); }
        g.evaluate(1.0f / 60.0f);
        auto* on = dynamic_cast<OutputNode*>(g.findNode(oId));
        if (on->current().w != 320 || on->current().h != 240) { glfwTerminate(); return fail("offline cancel: live prefs (texture size) not restored"); }
        r.cancel();                                              // idempotent
        std::fprintf(stderr, "gl_smoke OK: OfflineRenderer start validates, swaps prefs + clock, and cancel restores them\n");
    }
```

- [ ] **Step 2: Register the sources and run to verify it fails**

In `CMakeLists.txt`: add `src/app/OfflineRenderer.cpp` to `APP_SOURCES` (after `src/app/MidiSyncEngine.cpp`) and to the `gl_smoke` source list (after `src/modules/AudioOutputNode.cpp`).

Run: `cmake -S . -B build > /dev/null && cmake --build build -j 2>&1 | grep -m1 error`
Expected: `fatal error: 'app/OfflineRenderer.h' file not found`.

- [ ] **Step 3: Write the header**

Create `src/app/OfflineRenderer.h`:
```cpp
#pragma once
#include <glad/gl.h>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "core/OfflineRender.h"
#include "core/Preferences.h"
#include "core/Transport.h"
#include "gfx/Framebuffer.h"
#include "gfx/FullscreenPass.h"
#include "gfx/VideoEncoder.h"

namespace oss {

class Graph;
class OutputNode;
class AudioOutputNode;

// The offline render job. It poses as the transport's EXTERNAL CLOCK (the mechanism MIDI sync
// uses): every frame it places the transport at startBar*secondsPerBar + k/fps, evaluates the
// graph with a fixed dt = 1/fps, and -- after the discarded pre-roll -- blits the first Output
// node's texture through its own render-sized FBO into a VideoEncoder together with the first
// Audio Out node's stereo block (padded/trimmed to exactly sampleRate/fps frames, so audio can
// never drift from video). Between frames it waits while any node reports loading().
//
// start() snapshots the Transport and the graph's Preferences pointer, swaps in a copy with the
// render size (ShaderNodes recreate their FBOs from it), and sets the graph offline; finish()
// (success, failure, cancel, destructor -- exactly once per job) restores all three and closes
// the encoder. Node-internal state is NOT restored: the graph is left where the render ended.
//
// Driven incrementally: Application::frame calls step(budget) while active() so ImGui can draw
// progress; the --render CLI calls step() in a plain loop. Runs on the graph thread with the
// editor GL context current (its FBO/VAO/program live there).
class OfflineRenderer {
public:
    enum class Phase { Idle, Preroll, Rendering, Done, Failed, Cancelled };

    struct Progress {
        Phase  phase = Phase::Idle;
        long   prerollDone = 0, prerollTotal = 0;
        long   framesDone  = 0, framesTotal  = 0;      // captured frames
        long   blackFrames = 0;                        // frames with no Output texture (captured black)
        long   resizedAudioFrames = 0;                 // frames whose audio block was padded/trimmed
        bool   audio = false;                          // the file has an audio track
        double elapsedSeconds = 0.0;                   // wall time since start()
        double speed = 0.0;                            // captured frames per wall second
        std::string outPath;
        std::string status;                            // "waiting for X", the outcome line, or the error
    };

    OfflineRenderer() = default;
    ~OfflineRenderer();                                // cancel() if active (a valid partial file remains)
    OfflineRenderer(const OfflineRenderer&) = delete;
    OfflineRenderer& operator=(const OfflineRenderer&) = delete;

    // Validate, find the Output / Audio Out nodes, create the blit FBO at the render size,
    // snapshot state, arm the clock. False + `err` on a bad setting, no Output node, or an FBO
    // the driver refuses. `livePrefs` may be null (nodes then fall back to their canvas size).
    bool start(Graph& g, const Preferences* livePrefs, const RenderSettings& s, std::string& err);

    // Render frames until `budgetSeconds` of wall time have elapsed. Renders at least one frame
    // per call (so progress is guaranteed, even with budget 0) UNLESS a node reports loading(),
    // in which case it yields without rendering and re-checks on the next call. Returns
    // active(); the call that renders the last frame performs finish().
    bool step(double budgetSeconds);

    void cancel();                                     // close the encoder (partial file plays), restore state
    bool active() const { return active_; }
    const Progress& progress() const { return progress_; }   // valid after the job ends too

    // Test seam: how long to wait on loading() nodes before failing (default kRenderLoadTimeoutSeconds).
    void setLoadTimeoutSeconds(double s) { loadTimeout_ = s; }

private:
    bool anyNodeLoading(std::string& who) const;
    void evaluateFrame(long k);
    bool capture(long k);                              // false after finish(Failed)
    bool openEncoder();
    void finish(Phase outcome, const std::string& status);
    static double now();

    Graph*             graph_     = nullptr;
    const Preferences* livePrefs_ = nullptr;
    Preferences        renderPrefs_;
    Transport          savedTransport_;
    RenderSettings     settings_;
    OutputNode*        output_    = nullptr;
    AudioOutputNode*   audioOut_  = nullptr;

    std::unique_ptr<VideoEncoder> enc_;
    Framebuffer        fbo_;                           // the render-sized blit target
    FullscreenPass     fsq_;
    GLuint             blitProg_ = 0;
    std::vector<std::uint8_t> pixels_;
    std::vector<float>        audioScratch_;
    int                audioRate_ = 0;

    long   k_ = 0;                                     // next frame index (negative = pre-roll)
    long   prerollFrames_ = 0, totalFrames_ = 0;
    double secondsPerBar_ = 2.0;
    double startTime_ = 0.0, captureStartTime_ = 0.0;
    double loadWaitStart_ = -1.0;                      // wall time the current loader wait began (-1 = none)
    double loadTimeout_ = kRenderLoadTimeoutSeconds;
    bool   active_ = false;
    Progress progress_;
};

} // namespace oss
```

- [ ] **Step 4: Write the skeleton implementation (no capture yet)**

Create `src/app/OfflineRenderer.cpp`:
```cpp
#include "app/OfflineRenderer.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include "core/Graph.h"
#include "core/PathUtil.h"
#include "gfx/GLUtil.h"
#include "modules/AudioOutputNode.h"
#include "modules/OutputNode.h"

namespace oss {

// Fullscreen-triangle blit into the render FBO. NO vertical flip: FBO textures are bottom-up
// and VideoEncoder::addVideoFrame expects bottom-up rows (it flips for encoding), exactly like
// the Recorder's direct read-back. (The Output window's blit flips because it draws to a screen.)
static const char* kBlitVS = R"(#version 410 core
out vec2 vUV;
void main() {
    vec2 p = vec2(gl_VertexID == 1 ? 3.0 : -1.0, gl_VertexID == 2 ? 3.0 : -1.0);
    vUV = (p + 1.0) * 0.5;
    gl_Position = vec4(p, 0.0, 1.0);
}
)";
static const char* kBlitFS = R"(#version 410 core
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uTex;
void main() { FragColor = texture(uTex, vUV); }
)";

double OfflineRenderer::now() {
    using clk = std::chrono::steady_clock;
    return std::chrono::duration<double>(clk::now().time_since_epoch()).count();
}

OfflineRenderer::~OfflineRenderer() {
    cancel();
    if (blitProg_) glDeleteProgram(blitProg_);
}

bool OfflineRenderer::start(Graph& g, const Preferences* livePrefs, const RenderSettings& s, std::string& err) {
    if (active_) { err = "a render is already running"; return false; }
    OutputNode* out = nullptr; AudioOutputNode* aout = nullptr;
    for (const auto& n : g.nodes()) {
        if (!out)  out  = dynamic_cast<OutputNode*>(n.get());
        if (!aout) aout = dynamic_cast<AudioOutputNode*>(n.get());
    }
    if (!validateRenderSettings(s, out != nullptr, err)) return false;

    // GL objects live in the editor context (current on the graph thread). The FBO is
    // re-created per job at the render size; Framebuffer::create only prints on failure,
    // so check completeness ourselves.
    if (!blitProg_) { blitProg_ = linkProgram(kBlitVS, kBlitFS); fsq_.create(); }
    fbo_.create(s.width, s.height);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_.id());
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        err = "could not create a " + std::to_string(s.width) + "x" + std::to_string(s.height)
            + " framebuffer (GL status " + std::to_string(status) + ")";
        return false;
    }

    graph_ = &g; livePrefs_ = livePrefs; settings_ = s; output_ = out; audioOut_ = aout;
    savedTransport_ = g.transport();
    renderPrefs_ = livePrefs ? *livePrefs : Preferences{};
    renderPrefs_.textureWidth  = s.width;
    renderPrefs_.textureHeight = s.height;
    g.setPreferences(&renderPrefs_);
    g.setOffline(true);

    Transport& t = g.transport();
    t.externalClock = true;          // advance() becomes a no-op; we place the position ourselves
    t.playing       = true;          // synced nodes run
    t.looping       = false;         // linear start -> finish
    secondsPerBar_  = t.secondsPerBar();
    prerollFrames_  = prerollFrameCount(s, secondsPerBar_);
    totalFrames_    = renderFrameCount(s, secondsPerBar_);
    k_              = -prerollFrames_;
    enc_.reset();
    audioRate_ = 0;

    progress_ = Progress{};
    progress_.phase        = prerollFrames_ > 0 ? Phase::Preroll : Phase::Rendering;
    progress_.prerollTotal = prerollFrames_;
    progress_.framesTotal  = totalFrames_;
    progress_.outPath      = s.outPath;
    startTime_ = captureStartTime_ = now();
    loadWaitStart_ = -1.0;
    active_ = true;
    std::fprintf(stderr, "[Render] %s: bars %.2f-%.2f, %ld frames at %d fps, %dx%d, pre-roll %ld\n",
                 s.outPath.c_str(), s.startBar, s.endBar, totalFrames_, s.fps, s.width, s.height, prerollFrames_);
    return true;
}

void OfflineRenderer::finish(Phase outcome, const std::string& status) {
    if (!active_) return;
    if (enc_) { std::string e; enc_->close(e); enc_.reset(); }
    graph_->setOffline(false);
    graph_->setPreferences(livePrefs_);
    graph_->transport() = savedTransport_;
    active_ = false;
    progress_.phase          = outcome;
    progress_.status         = status;
    progress_.elapsedSeconds = now() - startTime_;
    std::fprintf(stderr, "[Render] %s\n", status.c_str());
}

void OfflineRenderer::cancel() {
    if (!active_) return;
    finish(Phase::Cancelled, "cancelled after " + std::to_string(progress_.framesDone) + " frames");
}

bool OfflineRenderer::anyNodeLoading(std::string& who) const {
    for (const auto& n : graph_->nodes())
        if (n->loading()) { who = n->name(); return true; }
    return false;
}

void OfflineRenderer::evaluateFrame(long k) {
    Transport& t = graph_->transport();
    t.externalClock = true;   // re-assert: nothing else should touch the clock mid-render
    t.playing       = true;
    t.seconds       = renderFrameSeconds(settings_, secondsPerBar_, k);
    graph_->evaluate(1.0f / (float)settings_.fps);
}

bool OfflineRenderer::openEncoder() { return false; }   // Task 7
bool OfflineRenderer::capture(long) { return false; }   // Task 7
bool OfflineRenderer::step(double)  { return false; }   // Task 7

} // namespace oss
```

- [ ] **Step 5: Build and run gl_smoke**

Run: `cmake --build build -j 2>&1 | tail -3 && ctest --test-dir build -R gl_smoke --output-on-failure 2>&1 | grep -E "OfflineRenderer start|FAIL|tests passed"`
Expected: `gl_smoke OK: OfflineRenderer start validates, swaps prefs + clock, and cancel restores them` and `100% tests passed`. (Unused-private-warnings are fine; the app target must also link — `shader_streamer` is in the default build.)

- [ ] **Step 6: Commit**

```bash
git add src/app/OfflineRenderer.h src/app/OfflineRenderer.cpp CMakeLists.txt tests/gl_smoke.cpp
git commit -m "feat(app): OfflineRenderer skeleton -- start validates + snapshots, finish/cancel restore

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 7: `OfflineRenderer::step` — evaluate, capture, encode (end to end)

**Files:**
- Modify: `src/app/OfflineRenderer.cpp`
- Modify: `tests/gl_smoke.cpp`

- [ ] **Step 1: Write the failing end-to-end scenario**

Insert before the final `glfwDestroyWindow(win);` (after the Task 6 scenario):
```cpp
    // --- Scenario: offline render writes every frame of a bar range, sample-locked, and restores state ---
    {
        Graph g;
        auto col  = std::make_unique<ColourNode>(); col->initGL();
        auto rec  = std::make_unique<RecorderNode>();
        rec->inputDefault(3) = true;                                            // a LIVE recording in progress
        rec->inputDefault(4) = std::string("build/_offline_live_rec.mp4");
        auto out  = std::make_unique<OutputNode>(); out->initGL();
        auto sine = std::make_unique<SineWaveNode>();
        auto aout = std::make_unique<AudioOutputNode>();
        int cId = g.addNode(std::move(col));  int rId = g.addNode(std::move(rec));
        int oId = g.addNode(std::move(out));  int sId = g.addNode(std::move(sine));
        int aId = g.addNode(std::move(aout));
        if (!g.connect(cId, 0, rId, 0) || !g.connect(rId, 0, oId, 0) || !g.connect(sId, 0, aId, 0)) {
            glfwTerminate(); return fail("offline e2e: connect");
        }
        Preferences live; live.textureWidth = 320; live.textureHeight = 240;
        g.setPreferences(&live);
        g.transport().bpm = 120.0; g.transport().seconds = 5.0; g.transport().looping = true;
        g.evaluate(1.0f / 60.0f);                         // one live frame: 320x240, recorder starts
        auto* on = dynamic_cast<OutputNode*>(g.findNode(oId));
        auto* rn = dynamic_cast<RecorderNode*>(g.findNode(rId));
        if (on->current().w != 320) { glfwTerminate(); return fail("offline e2e: live size not applied"); }
        if (rn->statusLine().rfind("REC", 0) != 0) { glfwTerminate(); return fail("offline e2e: live recorder should be recording"); }

        RenderSettings s; s.startBar = 0.0; s.endBar = 1.0; s.prerollBars = 0.5; s.fps = 30;
        s.width = 160; s.height = 120; s.outPath = "build/_offline.mp4";
        std::remove("build/_offline.mp4");
        OfflineRenderer r; std::string err;
        if (!r.start(g, &live, s, err)) { glfwTerminate(); return fail(("offline e2e: start: " + err).c_str()); }
        int steps = 0;
        while (r.step(0.02)) { if (++steps > 100000) { glfwTerminate(); return fail("offline e2e: never finished"); } }
        const OfflineRenderer::Progress& p = r.progress();
        if (p.phase != OfflineRenderer::Phase::Done) { glfwTerminate(); return fail(("offline e2e: " + p.status).c_str()); }
        if (p.framesDone != 60 || p.prerollDone != 30) { glfwTerminate(); return fail("offline e2e: progress counts (want 60 + 30 pre-roll)"); }
        if (!p.audio) { glfwTerminate(); return fail("offline e2e: expected an audio track (Sine -> Audio Out)"); }
        if (p.blackFrames != 0 || p.resizedAudioFrames != 0) { glfwTerminate(); return fail("offline e2e: unexpected black frames or resized audio blocks"); }
        if (p.status.rfind("rendered _offline.mp4 (60 frames, 2.0 s)", 0) != 0) { glfwTerminate(); return fail(("offline e2e: status line: " + p.status).c_str()); }
        if (rn->statusLine() != "saved build/_offline_live_rec.mp4") { glfwTerminate(); return fail("offline e2e: the live recording should have stopped + saved during the render"); }

        // The file: exactly 60 frames, 160x120, 2-channel non-silent audio, the Colour at the centre.
        VideoDecoder dec; std::string derr;
        if (!dec.open("build/_offline.mp4", derr)) { glfwTerminate(); return fail(("offline e2e: open output: " + derr).c_str()); }
        if (dec.width() != 160 || dec.height() != 120) { glfwTerminate(); return fail("offline e2e: output size should be 160x120"); }
        if (!dec.hasAudio() || dec.audioChannels() != 2) { glfwTerminate(); return fail("offline e2e: output should have 2-channel audio"); }
        VideoFrame vf; std::vector<float> au; double aS = 0; bool aV = false;
        int frames = 0; bool nz = false; bool centreOk = false;
        while (dec.decodeFrame(vf, au, aS, aV)) {
            if (frames == 0) {
                std::size_t i = ((std::size_t)(vf.height / 2) * vf.width + vf.width / 2) * 4;
                // Colour default (255,128,25) through a lossy yuv420p round-trip: check loosely.
                centreOk = vf.rgba[i] > 200 && vf.rgba[i + 1] > 90 && vf.rgba[i + 1] < 170 && vf.rgba[i + 2] < 80;
            }
            ++frames;
            for (float v : au) if (v > 0.01f || v < -0.01f) { nz = true; break; }
            au.clear();
        }
        if (frames != 60) { std::fprintf(stderr, "got %d frames\n", frames); glfwTerminate(); return fail("offline e2e: expected exactly 60 frames in the file"); }
        if (!nz) { glfwTerminate(); return fail("offline e2e: encoded audio is silent"); }
        if (!centreOk) { glfwTerminate(); return fail("offline e2e: centre pixel is not the Colour"); }

        // State restored; the next live frame is back at the live size.
        const Transport& t = g.transport();
        if (!(t.seconds == 5.0 && t.looping && !t.playing && !t.externalClock)) { glfwTerminate(); return fail("offline e2e: transport not restored"); }
        if (g.offline()) { glfwTerminate(); return fail("offline e2e: graph still offline"); }
        g.evaluate(1.0f / 60.0f);
        if (on->current().w != 320 || on->current().h != 240) { glfwTerminate(); return fail("offline e2e: live texture size not restored"); }
        rn->inputDefault(3) = false; g.evaluate(1.0f / 60.0f);   // stop the live recorder cleanly
        std::fprintf(stderr, "gl_smoke OK: offline render wrote 60 sample-locked 160x120 frames with stereo audio and restored state\n");
    }
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build build -j 2>&1 | tail -1 && ctest --test-dir build -R gl_smoke --output-on-failure 2>&1 | grep -E "offline e2e|FAIL" | head -3`
Expected: `gl_smoke FAIL: offline e2e: cancelled after 0 frames` (the stub `step` returns false and `phase` is not Done) — any `offline e2e` failure is the expected red.

- [ ] **Step 3: Implement step / capture / openEncoder**

In `src/app/OfflineRenderer.cpp`, replace the three stub lines at the bottom with:
```cpp
bool OfflineRenderer::openEncoder() {
    // Audio is recorded only if it is connected at the first captured frame (the Recorder's rule).
    audioRate_ = (audioOut_ && !audioOut_->lastBlock().empty()) ? audioOut_->lastSampleRate() : 0;
    enc_ = std::make_unique<VideoEncoder>();
    std::string err;
    if (!enc_->open(settings_.outPath, settings_.width, settings_.height, settings_.fps,
                    audioRate_, audioRate_ > 0 ? 2 : 0, err)) {
        enc_.reset();
        finish(Phase::Failed, "could not open " + settings_.outPath + ": " + err);
        return false;
    }
    progress_.audio = audioRate_ > 0;
    return true;
}

bool OfflineRenderer::capture(long k) {
    // 1. Blit the Output node's texture into the render-sized FBO (stretched, like the Output
    //    window). No texture -> black, counted, never skipped.
    TexRef src = output_->current();
    fbo_.bind();                                          // FBO + viewport
    glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST); glDisable(GL_SCISSOR_TEST);
    if (src.id) {
        glUseProgram(blitProg_);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, src.id);
        glUniform1i(glGetUniformLocation(blitProg_, "uTex"), 0);
        fsq_.draw();
        glBindTexture(GL_TEXTURE_2D, 0);
        glUseProgram(0);
    } else {
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ++progress_.blackFrames;
    }
    // 2. Read back (bottom-up rows, what the encoder wants).
    pixels_.resize((std::size_t)settings_.width * settings_.height * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, settings_.width, settings_.height, GL_RGBA, GL_UNSIGNED_BYTE, pixels_.data());
    Framebuffer::unbind();

    // 3. Encode: pts is exactly k (the encoder rounds t*fps).
    if (!enc_ && !openEncoder()) return false;
    enc_->addVideoFrame(pixels_.data(), (double)k / (double)settings_.fps);

    // 4. Audio: pad with silence / trim to exactly sampleRate/fps frames so the audio clock
    //    (sample count) can never drift from the video clock.
    if (progress_.audio) {
        const std::vector<float>& blk = audioOut_->lastBlock();
        const std::size_t want = (std::size_t)audioSamplesPerFrame(audioRate_, settings_.fps);
        const std::size_t have = blk.size() / 2;
        if (have != want) ++progress_.resizedAudioFrames;
        audioScratch_.assign(want * 2, 0.0f);
        const std::size_t n = std::min(have, want);
        std::copy(blk.begin(), blk.begin() + (std::ptrdiff_t)(n * 2), audioScratch_.begin());
        enc_->addAudio(audioScratch_.data(), (int)(want * 2));
    }
    return true;
}

bool OfflineRenderer::step(double budgetSeconds) {
    if (!active_) return false;
    const double t0 = now();
    while (active_) {
        // Loader gate: yield while any node is still loading (re-checked on the next call).
        std::string who;
        if (anyNodeLoading(who)) {
            const double n = now();
            if (loadWaitStart_ < 0.0) loadWaitStart_ = n;
            if (n - loadWaitStart_ > loadTimeout_) {
                finish(Phase::Failed, "timed out waiting for " + who + " to load");
                return false;
            }
            progress_.status = "waiting for " + who;
            progress_.elapsedSeconds = n - startTime_;
            return true;
        }
        loadWaitStart_ = -1.0;
        progress_.status.clear();

        if (k_ == 0) captureStartTime_ = now();
        evaluateFrame(k_);
        if (k_ >= 0 && !capture(k_)) return false;    // capture() already finished(Failed)
        if (k_ < 0) progress_.prerollDone = k_ + prerollFrames_ + 1;
        else        progress_.framesDone  = k_ + 1;
        ++k_;

        const double n = now();
        progress_.phase = k_ < 0 ? Phase::Preroll : Phase::Rendering;
        progress_.elapsedSeconds = n - startTime_;
        if (progress_.framesDone > 0 && n > captureStartTime_)
            progress_.speed = (double)progress_.framesDone / (n - captureStartTime_);

        if (k_ >= totalFrames_) {
            char buf[256];
            std::snprintf(buf, sizeof(buf), "rendered %s (%ld frames, %.1f s%s)",
                          fileBaseName(settings_.outPath).c_str(), totalFrames_,
                          (double)totalFrames_ / settings_.fps, progress_.audio ? "" : ", video only");
            std::string msg = buf;
            if (progress_.blackFrames)        msg += ", " + std::to_string(progress_.blackFrames) + " black frames";
            if (progress_.resizedAudioFrames) msg += ", " + std::to_string(progress_.resizedAudioFrames) + " audio blocks resized";
            finish(Phase::Done, msg);
            return false;
        }
        if (n - t0 >= budgetSeconds) break;             // budget spent; at least one frame was rendered
    }
    return active_;
}
```

- [ ] **Step 4: Build and run gl_smoke**

Run: `cmake --build build -j 2>&1 | tail -3 && ctest --test-dir build -R gl_smoke --output-on-failure 2>&1 | grep -E "offline|FAIL|tests passed"`
Expected: the three `offline` OK lines and `100% tests passed`.

- [ ] **Step 5: Commit**

```bash
git add src/app/OfflineRenderer.cpp tests/gl_smoke.cpp
git commit -m "feat(app): OfflineRenderer step -- fixed-clock evaluate, FBO blit capture, sample-locked encode

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 8: Loader gate, timeout, and cancel-mid-render checks

**Files:**
- Modify: `tests/gl_smoke.cpp`

(The gate and timeout logic already exist in `step()` from Task 7; this task proves them with a fake node and covers the partial file on cancel.)

- [ ] **Step 1: Write the scenario**

Insert before the final `glfwDestroyWindow(win);`:
```cpp
    // --- Scenario: offline loader gate yields without losing frames; timeout fails; cancel keeps a partial file ---
    {
        struct SlowLoader : Node {
            mutable int polls = 0;
            int holdPolls;
            explicit SlowLoader(int hold) : Node("Slow Loader"), holdPolls(hold) {}
            void evaluate(EvalContext&) override {}
            bool loading() const override { return ++polls <= holdPolls; }   // "loading" for the first N polls
        };
        auto build = [](Graph& g, int hold) {
            auto col = std::make_unique<ColourNode>(); col->initGL();
            auto out = std::make_unique<OutputNode>(); out->initGL();
            int cId = g.addNode(std::move(col)); int oId = g.addNode(std::move(out));
            g.addNode(std::make_unique<SlowLoader>(hold));
            g.connect(cId, 0, oId, 0);
            g.transport().bpm = 120.0;
        };
        RenderSettings s; s.startBar = 0.0; s.endBar = 1.0; s.prerollBars = 0.0; s.fps = 30;
        s.width = 160; s.height = 120;

        // (a) Gate: 5 polls of "loading" -> 5 yielding steps with nothing rendered, then a full
        //     60-frame file (1 bar at 120 bpm = 2 s at 30 fps).
        {
            Graph g; build(g, 5);
            s.outPath = "build/_offline_gate.mp4"; std::remove(s.outPath.c_str());
            OfflineRenderer r; std::string err;
            if (!r.start(g, nullptr, s, err)) { glfwTerminate(); return fail(("offline gate: start: " + err).c_str()); }
            for (int i = 0; i < 5; ++i) {
                if (!r.step(0.0)) { glfwTerminate(); return fail("offline gate: step should stay active while waiting"); }
                if (r.progress().framesDone != 0) { glfwTerminate(); return fail("offline gate: rendered a frame while a node was loading"); }
                if (r.progress().status != "waiting for Slow Loader") { glfwTerminate(); return fail("offline gate: status should name the loading node"); }
            }
            int steps = 0;
            while (r.step(0.02)) { if (++steps > 100000) { glfwTerminate(); return fail("offline gate: never finished"); } }
            if (r.progress().phase != OfflineRenderer::Phase::Done || r.progress().framesDone != 60) { glfwTerminate(); return fail("offline gate: should finish with all 60 frames"); }
            VideoDecoder dec; std::string derr;
            if (!dec.open(s.outPath, derr)) { glfwTerminate(); return fail("offline gate: output did not open"); }
            VideoFrame vf; std::vector<float> au; double aS = 0; bool aV = false; int frames = 0;
            while (dec.decodeFrame(vf, au, aS, aV)) { ++frames; au.clear(); }
            if (frames != 60) { glfwTerminate(); return fail("offline gate: file should hold exactly 60 frames"); }
        }

        // (b) Timeout: a node that never finishes loading fails the job, naming the node.
        {
            Graph g; build(g, 1 << 30);
            s.outPath = "build/_offline_timeout.mp4"; std::remove(s.outPath.c_str());
            OfflineRenderer r; std::string err;
            r.setLoadTimeoutSeconds(0.05);
            if (!r.start(g, nullptr, s, err)) { glfwTerminate(); return fail(("offline timeout: start: " + err).c_str()); }
            int steps = 0;
            while (r.step(0.0)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                if (++steps > 1000) { glfwTerminate(); return fail("offline timeout: never gave up"); }
            }
            if (r.progress().phase != OfflineRenderer::Phase::Failed) { glfwTerminate(); return fail("offline timeout: phase should be Failed"); }
            if (r.progress().status != "timed out waiting for Slow Loader to load") { glfwTerminate(); return fail(("offline timeout: status: " + r.progress().status).c_str()); }
            if (g.offline() || g.transport().externalClock) { glfwTerminate(); return fail("offline timeout: state not restored"); }
        }

        // (c) Cancel after one frame: a playable partial file, state restored.
        {
            Graph g; build(g, 0);
            g.transport().seconds = 3.0;
            s.outPath = "build/_offline_cancel2.mp4"; std::remove(s.outPath.c_str());
            OfflineRenderer r; std::string err;
            if (!r.start(g, nullptr, s, err)) { glfwTerminate(); return fail(("offline cancel: start: " + err).c_str()); }
            r.step(0.0);                                                   // exactly one frame
            if (r.progress().framesDone != 1) { glfwTerminate(); return fail("offline cancel: step(0) should render exactly one frame"); }
            r.cancel();
            if (r.active() || r.progress().phase != OfflineRenderer::Phase::Cancelled) { glfwTerminate(); return fail("offline cancel: should be Cancelled"); }
            if (r.progress().status != "cancelled after 1 frames") { glfwTerminate(); return fail("offline cancel: status"); }
            VideoDecoder dec; std::string derr;
            if (!dec.open(s.outPath, derr)) { glfwTerminate(); return fail("offline cancel: partial file should open"); }
            VideoFrame vf; std::vector<float> au; double aS = 0; bool aV = false; int frames = 0;
            while (dec.decodeFrame(vf, au, aS, aV)) { ++frames; au.clear(); }
            if (frames < 1 || frames >= 60) { glfwTerminate(); return fail("offline cancel: partial file frame count"); }
            if (g.transport().seconds != 3.0 || g.transport().playing || g.transport().externalClock) { glfwTerminate(); return fail("offline cancel: transport not restored"); }
        }
        std::fprintf(stderr, "gl_smoke OK: offline loader gate yields without losing frames, times out by name, and cancel keeps a partial file\n");
    }
```

- [ ] **Step 2: Build and run gl_smoke**

Run: `cmake --build build -j 2>&1 | tail -3 && ctest --test-dir build -R gl_smoke --output-on-failure 2>&1 | grep -E "offline|FAIL|tests passed"`
Expected: four `offline` OK lines and `100% tests passed`.

- [ ] **Step 3: Commit**

```bash
git add tests/gl_smoke.cpp
git commit -m "test(gl_smoke): offline render loader gate, load timeout, and cancel partial file

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 9: Render dialog, File menu item, Application wiring

**Files:**
- Create: `src/ui/RenderDialog.h`, `src/ui/RenderDialog.cpp`
- Modify: `src/ui/TransportBar.h`, `src/ui/TransportBar.cpp`
- Modify: `src/app/Application.h`, `src/app/Application.cpp`
- Modify: `CMakeLists.txt` (APP_SOURCES)

No headless test exists for ImGui panels (same as Assets/Preferences); the check is a clean build plus `--screenshot`.

- [ ] **Step 1: Menu hook**

`src/ui/TransportBar.h`, in `ProjectBarIO` after `onSaveAs`:
```cpp
    std::function<void()> onRender;     // File > Render Video... (opens the offline render dialog)
```
`src/ui/TransportBar.cpp`, in the File menu after the `Save As...` item:
```cpp
            if (io->onRender) {
                ImGui::Separator();
                if (ImGui::MenuItem("Render Video...")) io->onRender();
            }
```

- [ ] **Step 2: The dialog header**

Create `src/ui/RenderDialog.h`:
```cpp
#pragma once
#include <string>
#include "core/OfflineRender.h"

namespace oss {

class Graph;
struct Preferences;
class OfflineRenderer;

// The offline render UI: a "Render Video" settings window (start / finish bar in the Loop-field
// convention, pre-roll, frame rate, size, output file) and, while a job runs, a modal
// "Rendering" popup with progress + Cancel. The modal blocks graph edits, so a render is
// deterministic. Settings live here for the session (not persisted).
class RenderDialog {
public:
    // Draws the settings window when *show, and the progress modal while `r` is active.
    // `projectPath` seeds the default file name; `status` receives the job's outcome line (for
    // the toolbar) when it ends.
    void draw(Graph& g, const Preferences& prefs, OfflineRenderer& r, bool* show,
              const std::string& projectPath, std::string& status);

private:
    void seed(Graph& g, const Preferences& prefs, const std::string& projectPath);

    RenderSettings settings_;
    bool           seeded_    = false;   // defaults filled on first open
    bool           wasActive_ = false;   // to notice the job ending between draws
    std::string    error_;               // start() failure, shown inline
    std::string    outcome_;             // last job's outcome line, shown inline
};

} // namespace oss
```

- [ ] **Step 3: The dialog implementation**

Create `src/ui/RenderDialog.cpp`:
```cpp
#include "ui/RenderDialog.h"
#include <algorithm>
#include <cstdio>
#include <imgui.h>
#include "app/OfflineRenderer.h"
#include "core/Graph.h"
#include "core/PathUtil.h"
#include "core/Preferences.h"
#include "modules/OutputNode.h"
#include "ui/FileDialog.h"

namespace oss {

void RenderDialog::seed(Graph& g, const Preferences& prefs, const std::string& projectPath) {
    settings_ = RenderSettings{};
    settings_.endBar = g.automation().lengthBars();          // the Automation song length
    settings_.width  = prefs.textureWidth;
    settings_.height = prefs.textureHeight;
    // Default file: next to the project as <basename>.mp4, else render.mp4 in the projects dir.
    std::string base = projectPath.empty() ? std::string("render") : fileBaseName(projectPath);
    if (base.size() > 4 && base.compare(base.size() - 4, 4, ".oss") == 0) base.erase(base.size() - 4);
    std::string dir = projectPath.empty() ? prefs.projectsDir : parentDir(projectPath);
    settings_.outPath = dir.empty() ? base + ".mp4" : dir + "/" + base + ".mp4";
}

void RenderDialog::draw(Graph& g, const Preferences& prefs, OfflineRenderer& r, bool* show,
                        const std::string& projectPath, std::string& status) {
    // The job ends inside Application::frame (between draws): hand its outcome to the toolbar.
    if (wasActive_ && !r.active()) { outcome_ = r.progress().status; status = outcome_; }
    wasActive_ = r.active();

    if (show && *show) {
        if (!seeded_) { seed(g, prefs, projectPath); seeded_ = true; }
        ImGui::SetNextWindowSize(ImVec2(480.0f, 0.0f), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Render Video", show)) {
            const Transport& t = g.transport();

            float sb = (float)settings_.startBar, eb = (float)settings_.endBar, pb = (float)settings_.prerollBars;
            ImGui::SetNextItemWidth(100.0f);
            if (ImGui::InputFloat("Start bar", &sb, 0.0f, 0.0f, "%.2f")) settings_.startBar = std::max(0.0f, sb);
            ImGui::SetNextItemWidth(100.0f);
            if (ImGui::InputFloat("Finish bar", &eb, 0.0f, 0.0f, "%.2f")) settings_.endBar = eb;
            ImGui::SameLine();
            if (ImGui::Button("Use loop range")) { settings_.startBar = t.loopStartBar; settings_.endBar = t.loopEndBar; }
            ImGui::SetNextItemWidth(100.0f);
            if (ImGui::InputFloat("Pre-roll (bars)", &pb, 0.0f, 0.0f, "%.2f")) settings_.prerollBars = std::max(0.0f, pb);

            ImGui::SetNextItemWidth(100.0f);
            std::string fpsLabel = std::to_string(settings_.fps) + " fps";
            if (ImGui::BeginCombo("Frame rate", fpsLabel.c_str())) {
                for (int i = 0; i < kRenderFrameRateCount; ++i) {
                    std::string lbl = std::to_string(kRenderFrameRates[i]) + " fps";
                    if (ImGui::Selectable(lbl.c_str(), kRenderFrameRates[i] == settings_.fps)) settings_.fps = kRenderFrameRates[i];
                }
                ImGui::EndCombo();
            }

            ImGui::SetNextItemWidth(100.0f); ImGui::InputInt("Width",  &settings_.width,  0, 0);
            ImGui::SetNextItemWidth(100.0f); ImGui::InputInt("Height", &settings_.height, 0, 0);
            ImGui::SameLine();
            if (ImGui::Button("Use live size")) { settings_.width = prefs.textureWidth; settings_.height = prefs.textureHeight; }

            char pathBuf[1024];
            std::snprintf(pathBuf, sizeof(pathBuf), "%s", settings_.outPath.c_str());
            ImGui::SetNextItemWidth(-100.0f);
            if (ImGui::InputText("##outpath", pathBuf, sizeof(pathBuf))) settings_.outPath = pathBuf;
            ImGui::SameLine();
            if (ImGui::Button("Browse...")) {
                std::string defName = fileBaseName(settings_.outPath);
                if (defName.empty()) defName = "render.mp4";
                std::string p = saveFileDialog("Render Video", "MP4", {"mp4"}, defName, prefs.projectsDir);
                if (!p.empty()) settings_.outPath = ensureExtension(p, "mp4");
            }

            const double spb = t.secondsPerBar();
            const long frames = renderFrameCount(settings_, spb);
            ImGui::Text("%.2f bars -> %ld frames (%.1f s at %.2f bpm)",
                        settings_.endBar - settings_.startBar, frames, (double)frames / settings_.fps, t.bpm);

            bool hasOutput = false;
            for (const auto& n : g.nodes()) if (dynamic_cast<OutputNode*>(n.get())) { hasOutput = true; break; }
            std::string why;
            const bool valid = validateRenderSettings(settings_, hasOutput, why);

            ImGui::BeginDisabled(!valid || r.active());
            if (ImGui::Button("Render")) {
                error_.clear(); outcome_.clear();
                if (!r.start(g, &prefs, settings_, error_)) status = "render failed: " + error_;
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Close")) *show = false;
            if (!valid)                 ImGui::TextDisabled("%s", why.c_str());
            if (!error_.empty())        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f), "%s", error_.c_str());
            else if (!outcome_.empty()) ImGui::TextUnformatted(outcome_.c_str());
        }
        ImGui::End();
    }

    // Progress modal while the job runs. Opened/closed from the root ID stack (not inside the
    // window above), and closed explicitly when the job ends so the popup never lingers.
    if (r.active() && !ImGui::IsPopupOpen("Rendering")) ImGui::OpenPopup("Rendering");
    if (ImGui::BeginPopupModal("Rendering", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (!r.active()) {
            ImGui::CloseCurrentPopup();
        } else {
            const OfflineRenderer::Progress& p = r.progress();
            if (p.phase == OfflineRenderer::Phase::Preroll) {
                ImGui::Text("Pre-roll %ld / %ld", p.prerollDone, p.prerollTotal);
                ImGui::ProgressBar(p.prerollTotal ? (float)p.prerollDone / (float)p.prerollTotal : 0.0f, ImVec2(380.0f, 0.0f));
            }
            ImGui::Text("Frame %ld / %ld", p.framesDone, p.framesTotal);
            ImGui::ProgressBar(p.framesTotal ? (float)p.framesDone / (float)p.framesTotal : 0.0f, ImVec2(380.0f, 0.0f));
            const double remaining = p.speed > 0.0 ? (double)(p.framesTotal - p.framesDone) / p.speed : 0.0;
            ImGui::Text("Elapsed %.0f s   remaining ~%.0f s   %.2fx real time",
                        p.elapsedSeconds, remaining, p.speed / (double)settings_.fps);
            if (!p.status.empty()) ImGui::TextDisabled("%s", p.status.c_str());
            ImGui::TextDisabled("%s", p.outPath.c_str());
            if (ImGui::Button("Cancel")) r.cancel();
        }
        ImGui::EndPopup();
    }
}

} // namespace oss
```

- [ ] **Step 4: Application wiring**

`src/app/Application.h`: add includes
```cpp
#include "app/OfflineRenderer.h"
#include "ui/RenderDialog.h"
```
public accessors after `Graph& graph()`:
```cpp
    OfflineRenderer&   renderer() { return renderer_; }             // the offline render job (also driven by --render)
    const Preferences& preferences() const { return prefs_; }
```
and, as the LAST private members (declared after `graph_`, so they are destroyed before it — the renderer restores the graph in its destructor):
```cpp
    OfflineRenderer  renderer_;
    RenderDialog     renderDialog_;
    bool             showRender_ = false;
```

`src/app/Application.cpp`: near the top (after the includes) add
```cpp
// Wall time the offline render may use per UI frame while active: the editor keeps ~10 fps.
static constexpr double kRenderStepBudget = 0.1;
```
In `frame()`, after `io.onSaveAs = ...;` add:
```cpp
    io.onRender = [this]{ showRender_ = true; };
```
and replace the last two lines of `frame()`:
```cpp
    syncEngine_.update(graph_.transport(), prefs_, dt);   // MIDI clock sync in/out
    graph_.evaluate(dt);                     // advances the transport by dt
```
with:
```cpp
    renderDialog_.draw(graph_, prefs_, renderer_, &showRender_, currentPath_, projectStatus_);
    if (renderer_.active()) {
        // The offline render owns the graph + transport; the sync engine is not updated (it
        // would fight the renderer for the clock) and the wall-clock evaluate is skipped.
        renderer_.step(kRenderStepBudget);
    } else {
        syncEngine_.update(graph_.transport(), prefs_, dt);   // MIDI clock sync in/out
        graph_.evaluate(dt);                                  // advances the transport by dt
    }
```

`CMakeLists.txt`: add `src/ui/RenderDialog.cpp` to `APP_SOURCES` after `src/ui/PropertiesPanel.cpp`.

- [ ] **Step 5: Build, run the unit tests, and take a screenshot**

Run: `cmake -S . -B build > /dev/null && cmake --build build -j 2>&1 | tail -3 && ctest --test-dir build --output-on-failure | tail -3 && ./build/shader_streamer --screenshot build/_ui.png`
Expected: build OK, `100% tests passed`, `wrote screenshot build/_ui.png (...)`.

- [ ] **Step 6: Commit**

```bash
git add src/ui/RenderDialog.h src/ui/RenderDialog.cpp src/ui/TransportBar.h src/ui/TransportBar.cpp src/app/Application.h src/app/Application.cpp CMakeLists.txt
git commit -m "feat(ui): File > Render Video... dialog with a modal progress popup driving the OfflineRenderer

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 10: `--render` command line, fixture, `render_cli` ctest, CI

**Files:**
- Modify: `src/main.cpp`
- Create: `tests/assets/render_smoke.oss`
- Modify: `CMakeLists.txt` (add_test)
- Modify: `.github/workflows/build-linux.yml`, `.github/workflows/build-macos.yml`, `.github/workflows/build-windows.yml`

- [ ] **Step 1: The fixture project**

Create `tests/assets/render_smoke.oss` (Colour → Output, Sine → Audio Out, song length 2 bars):
```
oss-project 1
transport 120.000000 4 0 0.000000 4.000000 2.000000
node 1 40.000000 40.000000
type Colour
inc 0 0.200000 0.600000 0.900000 1.000000
node 2 360.000000 40.000000
type Output
node 3 40.000000 200.000000
type Sine
inf 0 220.000000
inf 1 0.500000
node 4 360.000000 200.000000
type Audio Out
conn 1 0 2 0
conn 3 0 4 0
```

- [ ] **Step 2: Register the ctest and run it to verify it fails**

`CMakeLists.txt`, after `add_test(NAME gl_smoke ...)`:
```cmake
# Headless offline render via the app binary (best-effort in CI, like gl_smoke: needs a GL context).
add_test(NAME render_cli
  COMMAND shader_streamer --render tests/assets/render_smoke.oss ${CMAKE_BINARY_DIR}/_cli_render.mp4
          --end 1 --fps 24 --size 160x120 --preroll 0.25
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
```
Run: `cmake -S . -B build > /dev/null && ctest --test-dir build -R render_cli --output-on-failure | tail -5`
Expected: FAIL (the app ignores `--render` and opens its windows, or exits non-zero).

- [ ] **Step 3: Implement `runRender` in `main.cpp`**

Add includes after `#include "gfx/GLUtil.h"`:
```cpp
#include "app/OfflineRenderer.h"
#include "core/OfflineRender.h"
#include <chrono>
#include <thread>
```
Add this function before `int main(`:
```cpp
// Headless offline render: `--render <project.oss> <out.mp4> [--start B] [--end B] [--fps N]
// [--size WxH] [--preroll B]`. Loads preferences + the project into an Application on a hidden
// window (like --screenshot) and steps its OfflineRenderer to completion, printing progress
// once a second. Exit 0 when the render finished, 1 otherwise (the reason is on stderr).
static int runRender(const std::vector<std::string>& args) {
    oss::RenderCliArgs cli; std::string err;
    if (!oss::parseRenderArgs(args, cli, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }

    if (!glfwInit()) { std::fprintf(stderr, "glfwInit failed\n"); return 1; }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 1);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    GLFWwindow* win = glfwCreateWindow(640, 480, "shader-streamer-render", nullptr, nullptr);
    if (!win) { std::fprintf(stderr, "createWindow failed (no offscreen GL?)\n"); glfwTerminate(); return 1; }
    glfwMakeContextCurrent(win);
    if (!gladLoadGL((GLADloadfunc)glfwGetProcAddress)) { glfwDestroyWindow(win); glfwTerminate(); return 1; }

    IMGUI_CHECKVERSION();                     // the Application's panels need a context even unused
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui_ImplGlfw_InitForOpenGL(win, false);
    ImGui_ImplOpenGL3_Init("#version 410");

    int rc = 1;
    {
        oss::Application app(win);
        if (!app.loadProjectFromFile(cli.projectPath)) {
            std::fprintf(stderr, "could not load project %s\n", cli.projectPath.c_str());
        } else {
            oss::RenderSettings s = cli.settings;
            if (s.endBar < 0.0) s.endBar = app.graph().automation().lengthBars();      // sentinel -> song length
            if (s.width <= 0 || s.height <= 0) {                                       // sentinel -> live size
                s.width = app.preferences().textureWidth; s.height = app.preferences().textureHeight;
            }
            oss::OfflineRenderer& r = app.renderer();
            if (!r.start(app.graph(), &app.preferences(), s, err)) {
                std::fprintf(stderr, "render failed: %s\n", err.c_str());
            } else {
                double lastPrint = glfwGetTime();
                while (r.step(1.0)) {
                    glfwPollEvents();
                    if (r.progress().status.rfind("waiting", 0) == 0)                 // loader wait: don't spin
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    double t = glfwGetTime();
                    if (t - lastPrint >= 1.0) {
                        lastPrint = t;
                        const oss::OfflineRenderer::Progress& p = r.progress();
                        std::fprintf(stderr, "  %ld / %ld frames (%.1fx real time)\n",
                                     p.framesDone, p.framesTotal, p.speed / (double)s.fps);
                    }
                }
                rc = (r.progress().phase == oss::OfflineRenderer::Phase::Done) ? 0 : 1;
            }
        }
    }   // app destroyed here (its context is current)

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(win);
    glfwTerminate();
    return rc;
}
```
In `main()`, inside the existing `for (int i = 1; i < argc; ++i)` loop, add before the `--screenshot` check:
```cpp
        if (std::strcmp(argv[i], "--render") == 0)
            return runRender(std::vector<std::string>(argv + i + 1, argv + argc));
```

- [ ] **Step 4: Build and run the CLI test**

Run: `cmake --build build -j 2>&1 | tail -3 && ctest --test-dir build -R render_cli --output-on-failure | tail -6`
Expected: `[Render] rendered _cli_render.mp4 (48 frames, 2.0 s)` in the output and `100% tests passed` (1 bar at 120 bpm = 2 s × 24 fps = 48 frames).

Then verify the file by hand:
```bash
./build/shader_streamer --render tests/assets/render_smoke.oss build/_cli_manual.mp4 --end 2 --fps 30 --size 320x240 && ls -la build/_cli_manual.mp4
```
Expected: `[Render] rendered _cli_manual.mp4 (120 frames, 4.0 s)` and a non-empty file. Also `./build/shader_streamer --render nope.oss out.mp4; echo rc=$?` → `could not load project nope.oss` and `rc=1`.

- [ ] **Step 5: CI best-effort step**

In each of `.github/workflows/build-linux.yml`, `build-macos.yml`, `build-windows.yml`, change the best-effort test step's filter from `-R gl_smoke` to `-R "gl_smoke|render_cli"` (Linux keeps its `xvfb-run -a` prefix; Windows keeps `shell: pwsh`). Rename the step to `Test (gl_smoke + render_cli, best-effort)`.

- [ ] **Step 6: Commit**

```bash
git add src/main.cpp tests/assets/render_smoke.oss CMakeLists.txt .github/workflows/build-linux.yml .github/workflows/build-macos.yml .github/workflows/build-windows.yml
git commit -m "feat(app): --render headless offline render + render_cli ctest (best-effort in CI)

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 11: Documentation

**Files:**
- Modify: `README.md`
- Modify: `CLAUDE.md`
- Modify: `docs/superpowers/specs/2026-09-22-offline-render-design.md` (status + the even-size rule)

- [ ] **Step 1: README**

In `README.md`, under **Build & run**, after the `--screenshot` code block, add:
```markdown
To render a project's output to a movie without opening the windows (see
[Render Video](#render-video-offline)):

```bash
./build/shader_streamer --render project.oss out.mp4 --start 0 --end 8 --fps 30 --size 1920x1080
```
```
After the **### Save / Load** section (before **### Preferences**), add:
```markdown
### Render Video (offline)

**File → Render Video…** writes the graph's output between a **start bar** and a **finish bar** to
an H.264/AAC `.mp4`, taking as long as it needs so **no frame is ever dropped**: the graph runs on a
fixed clock (`dt = 1/fps`) instead of the wall clock, and every frame carries exactly
`48000 / fps` audio samples, so picture and sound stay sample-locked. Video is what the first
**Output** node shows; audio is what feeds the first **Audio Out** (none → a video-only file).
Bars follow the **Loop** fields (start 0 = the first bar, finish exclusive; **Use loop range**
copies them). Frame rate is 24 / 25 / 30 / 50 / 60; the size defaults to the Preferences texture
size and can be overridden (up to 8192², even dimensions) — every shader node re-renders at the
render size. A **pre-roll** (default 1 bar) runs before the start bar, uncaptured, so envelopes,
MIDI notes, and file loaders settle; the render waits for any file still loading. A modal progress
popup shows frames, speed, and **Cancel** (a cancelled render leaves a playable partial file); the
Output window shows the frames as they render. The same render runs headlessly with
`--render <project.oss> <out.mp4> [--start B] [--end B] [--fps N] [--size WxH] [--preroll B]`
(finish defaults to the Automation song length; size to the Preferences).

Two things to know: nodes are left where the render ended (as if the range had been played —
LFO phases, free-running players, and synth envelopes are not rewound), and real-time inputs
(**Audio In**, **MIDI In**) just capture whatever arrives while the render runs. MIDI sync is not
updated during a render (sync-out carries on at its pre-render state). Audio Out, MIDI Out and the
**Recorder** are muted while rendering; a live recording in progress stops and saves.
```

- [ ] **Step 2: CLAUDE.md**

In the **Architecture** list, after the **Audio block sizing** bullet, add:
```markdown
- **Offline render** — `app/OfflineRenderer.{h,cpp}` renders the graph between a start and a
  finish bar (Loop-field convention, finish exclusive) to an mp4 with **no dropped frames**: it
  poses as the transport's **external clock** (`externalClock = true`, `playing = true`, position
  set to `startBar·spb + k/fps` every frame, so `advance()` is a no-op and there is no loop wrap or
  drift) and evaluates with a fixed `dt = 1/fps`; the listed frame rates (24/25/30/50/60) divide
  48 kHz so every frame carries exactly `sampleRate/fps` samples (the encoder block is padded/
  trimmed to that count regardless). A **pre-roll** (frames `-P..-1`, position clamped at 0) is
  evaluated but not captured. Between frames it waits while any node reports the new
  `Node::loading()` hook (Audio Player / Drum Machine / Mesh Loader via `AsyncLoader::pending()`
  = in flight AND not finished; Image Sequencer via its prefetch future), failing after 30 s with
  the node's name. `Graph::setOffline(true)` flows `EvalContext::offline` to every node: **Audio
  Out** builds its stereo block before any device work (so it exists with no device) and exposes
  `lastBlock()`/`lastSampleRate()` but never touches the device/ring while offline; **MIDI Out**
  sends nothing; **Recorder** treats `record` as off. Capture = blit the first `OutputNode`'s
  texture through the renderer's own render-sized FBO (stretch, NO V-flip — the encoder wants
  bottom-up rows), `glReadPixels`, `VideoEncoder::addVideoFrame(k/fps)` + the Audio Out block;
  the encoder opens lazily on the first captured frame (audio only if connected then). The
  resolution override is a temporary `Preferences` copy pointed at by `Graph::setPreferences`
  (ShaderNode/Wireframe/Shaded Render recreate their FBOs from it); `finish()` restores the
  pointer, the `Transport` snapshot and the offline flag exactly once per job (success / failure /
  cancel / destructor). Node-internal state is NOT restored. The GL-free settings, frame-count /
  clock math, validation and `--render` argument parsing live in `core/OfflineRender.h`
  (unit-tested). Drivers: `ui/RenderDialog` (File → Render Video…; `Application::frame` calls
  `step(0.1 s)` while active instead of the sync update + wall-clock evaluate, behind a modal
  progress popup with Cancel) and `main.cpp --render` (hidden window, same class, `step(1.0)`
  loop). `gl_smoke` covers the sinks, the end-to-end decode (frame count, size, stereo audio,
  pixels), the loader gate/timeout, cancel, and state restore; `render_cli` is a best-effort
  ctest over `tests/assets/render_smoke.oss`.
```
Also, in the **Tests** section's `gl_smoke` bullet, no change is needed; in **Adding a node**, add after step 3:
```markdown
4. If the node loads anything asynchronously, override `Node::loading()` (see `AsyncLoader::pending`)
   so the offline render waits for it; if it is a real-time sink, honour `EvalContext::offline`.
```

- [ ] **Step 3: Spec status**

In `docs/superpowers/specs/2026-09-22-offline-render-design.md`: change `**Status:** Approved (brainstorm)` to `**Status:** Implemented`, and in the `validateRenderSettings` bullet of the Error handling table add `, odd size` to the first row's condition list (the H.264 yuv420p encode needs even dimensions — decided during implementation).

- [ ] **Step 4: Final full check**

Run: `cmake --build build -j 2>&1 | tail -2 && ctest --test-dir build --output-on-failure | tail -4`
Expected: `100% tests passed, 0 tests failed out of 3`.

- [ ] **Step 5: Commit**

```bash
git add README.md CLAUDE.md docs/superpowers/specs/2026-09-22-offline-render-design.md
git commit -m "docs: offline render (File > Render Video..., --render CLI, clock model, loader gate, offline sinks)

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

## Plan self-review

**Spec coverage.** Clock model → Tasks 6–7 (`start` arms the external clock; `evaluateFrame` places the position; fixed `dt`). Pre-roll → Tasks 1, 7 (`prerollFrameCount`, clamp at 0, uncaptured `k < 0`). Loader gate + 30 s timeout naming the node → Tasks 3, 7, 8. Offline flag + the three sinks (Audio Out block-before-device + tap, MIDI Out, Recorder) → Tasks 4–5. Capture path (blit without V-flip, black frames counted, lazy encoder open, audio pad/trim) → Task 7. Resolution override via a Preferences copy + own bounds → Tasks 2, 6, 7. State restore exactly once (success / failure / cancel / destructor), node state not restored → Task 6 + README. `core/OfflineRender.h` API incl. sentinels → Tasks 1–2. `OfflineRenderer` API incl. `Progress` fields and `step` semantics → Tasks 6–7. Dialog (all fields, Use loop range, Use live size, Browse, read-out, disabled Render with reason, outcome to the toolbar; modal with pre-roll bar, frames, elapsed/remaining/speed, Cancel) → Task 9. Application wiring (member order, sync engine skipped, accessors) → Task 9. CLI (hidden window, sentinels filled, progress once a second, exit codes) → Task 10. Error table → Tasks 2, 6, 7, 8. Tests: `core_tests` (frame counts incl. float noise + min 1, pre-roll, frameSeconds, samples/frame, validation, parsing, `pending()`, offline flag) → Tasks 1–4; `gl_smoke` (sinks, start/cancel restore, end-to-end decode incl. Recorder stop, gate, timeout, cancel partial) → Tasks 5–8; `render_cli` + CI filter → Task 10. Docs → Task 11.

**Placeholder scan.** No TBD/TODO; every code step shows the code; every run step has a command and an expected result.

**Type consistency.** `RenderSettings` fields (`startBar, endBar, prerollBars, fps, width, height, outPath`), `renderFrameCount/prerollFrameCount(const RenderSettings&, double spb)`, `renderFrameSeconds(s, spb, k)`, `audioSamplesPerFrame(rate, fps)`, `validateRenderSettings(s, hasOutputNode, err)`, `RenderCliArgs{projectPath, settings}`, `parseRenderArgs(args, out, err)`, `AsyncLoader::pending()`, `Node::loading()`, `EvalContext::offline`, `Graph::setOffline/offline`, `AudioOutputNode::lastBlock()/lastSampleRate()`, `OfflineRenderer::{start, step, cancel, active, progress, setLoadTimeoutSeconds}`, `Progress::{phase, prerollDone, prerollTotal, framesDone, framesTotal, blackFrames, resizedAudioFrames, audio, elapsedSeconds, speed, outPath, status}`, `Phase::{Idle, Preroll, Rendering, Done, Failed, Cancelled}`, `ProjectBarIO::onRender`, `Application::{renderer(), preferences()}` are used with the same names and signatures in every task.
