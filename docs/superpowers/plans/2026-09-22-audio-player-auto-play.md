# Audio Player Auto Play Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add an `auto play` Bool input to the **Audio File** node so the clip plays when the global transport plays, holds on pause, and rewinds on stop.

**Architecture:** One new Bool input port appended at index 6. When it is on and a transport is present, the effective play flag becomes `transport->playing` and the `play` input is ignored. The node tracks the transport's position between frames and rewinds the clip to 0 whenever that position moves backwards, which is what Stop and a transport loop wrap look like. All of this is skipped while `sync` is on, where the playhead is already derived from the bar position every frame.

**Tech Stack:** C++17, the existing `Node` / `EvalContext` / `Transport` machinery, and the headless `gl_smoke` test (the node reaches FFmpeg through `audio/AudioFile`, so it cannot live in `core_tests`).

**Spec:** `docs/superpowers/specs/2026-09-22-audio-player-auto-play-design.md`

**Branch:** `feat/audio-player-auto-play` (already created from `develop`; the spec is committed on it).

---

## File map

| File | Change |
|---|---|
| `src/modules/AudioPlayerNode.h` | The two tracker members, the `updateStatus` signature, and the class doc comment. |
| `src/modules/AudioPlayerNode.cpp` | The new port, the effective-play rule, the rewind, and the status text. |
| `tests/gl_smoke.cpp` | One scenario covering all six behaviours. |

No other file changes. The port needs no UI work — `core/PanelSlot.h`'s `inputSlot` already routes every Bool input to the Properties panel as a checkbox — and no save/load work, because control defaults persist through the existing `ProjectFile` path.

## Conventions

- Build: `cmake --build build -j` from the repo root.
- Tests: `ctest --test-dir build --output-on-failure` (runs `core_tests`, `gl_smoke` and `render_cli`).
- `gl_smoke` is one large `main()` of scenarios in bare `{ ... }` blocks; every failure is `{ glfwTerminate(); return fail("..."); }`.
- Commit messages: Conventional Commits, ending `Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>`.
- A shell heredoc containing an apostrophe has repeatedly broken in this environment. Write commit messages to a file and use `git commit -F <file>`.

---

### Task 1: The failing test

**Files:**
- Modify: `tests/gl_smoke.cpp`

- [ ] **Step 1: Write the scenario**

Insert this immediately after the existing Audio File scenario — find the line

```cpp
        std::fprintf(stderr, "gl_smoke OK: Audio File played stereo, advanced %.2fs then reversed %.2f->%.2f\n",
```

and add the new block after that scenario's closing `}`, before the comment `// --- Scenario 14: a shared World Transform aligns two renderers ---`.

Do **not** put it near the end of the file: the three `RLIMIT_FSIZE` scenarios there lower a process-wide file-size limit, and anything inserted inside one of those windows fails confusingly.

```cpp
    // --- Scenario: Audio File `auto play` follows the transport ---
    // With `auto play` on, the transport's play state drives the clip and the `play` input is
    // ignored. Pause holds the position; Stop zeroes the transport, which the node sees as a
    // BACKWARDS move and rewinds the clip, so the next Play starts the file from the beginning.
    // A forward scrub is deliberately NOT followed -- strict position locking is what `sync` is
    // for, and auto play is about free-running playback the transport starts and stops.
    {
        Graph g;
        auto ap = std::make_unique<AudioPlayerNode>();
        ap->inputDefault(0) = std::string("tests/assets/tone.mp3");
        ap->inputDefault(3) = false;                 // loop off, so the playhead is monotonic
        ap->inputDefault(6) = true;                  // auto play on
        int aId = g.addNode(std::move(ap));
        auto* an = dynamic_cast<AudioPlayerNode*>(g.findNode(aId));
        g.transport().bpm = 120.0;
        g.transport().playing = false;
        g.transport().seconds = 0.0;

        auto pump = [&](int n) { for (int i = 0; i < n; ++i) g.evaluate(1.0f / 60.0f); };
        // The decode STARTS inside evaluate() (loader_.request), so loading() is false until one
        // frame has run -- drive a frame first, then wait, then drive one more so poll() adopts it.
        pump(1);
        for (int f = 0; f < 500 && an->loading(); ++f) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            pump(1);
        }
        pump(1);
        if (an->loading()) { glfwTerminate(); return fail("auto play: clip never finished loading"); }

        auto blockSilent = [&]() {
            AudioRef l = an->leftOut();
            if (l.count == 0) return true;
            for (std::size_t i = 0; i < l.count; ++i) if (std::fabs(l.samples[i]) > 0.01f) return false;
            return true;
        };

        // (1) Transport stopped -> silent, and the playhead does not move.
        pump(5);
        if (!blockSilent()) { glfwTerminate(); return fail("auto play: clip sounded while the transport was stopped"); }
        if (an->playhead() != 0.0) { glfwTerminate(); return fail("auto play: playhead advanced while the transport was stopped"); }

        // (2) Transport playing -> audible, and the playhead advances. (Graph::evaluate advances
        // the transport itself, so nothing here moves `seconds` by hand.)
        g.transport().playing = true;
        bool heard = false;
        for (int f = 0; f < 20 && !heard; ++f) { pump(1); if (!blockSilent()) heard = true; }
        if (!heard) { glfwTerminate(); return fail("auto play: clip stayed silent while the transport played"); }
        double playedTo = an->playhead();
        if (!(playedTo > 0.0)) { glfwTerminate(); return fail("auto play: playhead did not advance while the transport played"); }

        // (3) Pause -> position holds (the transport stops moving; it does not move backwards).
        g.transport().playing = false;
        pump(5);
        if (an->playhead() != playedTo) { glfwTerminate(); return fail("auto play: a pause did not hold the playhead"); }
        if (!blockSilent()) { glfwTerminate(); return fail("auto play: clip sounded while paused"); }

        // (4) Stop -> playing false AND seconds 0, a backwards move, so the clip rewinds.
        g.transport().stop();
        pump(1);
        if (an->playhead() != 0.0) { glfwTerminate(); return fail("auto play: a stop did not rewind the clip"); }

        // (5) auto play OFF -> the `play` toggle governs again, transport still stopped.
        an->inputDefault(6) = false;
        an->inputDefault(2) = true;
        bool heardManual = false;
        for (int f = 0; f < 20 && !heardManual; ++f) { pump(1); if (!blockSilent()) heardManual = true; }
        if (!heardManual) { glfwTerminate(); return fail("auto play off: the play toggle should still work with the transport stopped"); }

        // (6) A FORWARD scrub is not followed: the clip keeps playing at its own rate.
        an->inputDefault(6) = true;
        g.transport().playing = true;
        pump(3);
        double beforeScrub = an->playhead();
        g.transport().seconds += 5.0;
        pump(1);
        if (an->playhead() < beforeScrub) { glfwTerminate(); return fail("auto play: a forward scrub must not rewind the clip"); }

        std::fprintf(stderr, "gl_smoke OK: Audio File auto play follows the transport (stop rewinds, pause holds, forward scrub ignored)\n");
    }
```

- [ ] **Step 2: Run it and confirm the red**

Run: `cmake --build build -j 2>&1 | grep -m1 error`

Expected: a compile error, because port 6 does not exist yet. The message names `inputDefault` or an out-of-range access — `AudioPlayerNode` declares only 6 inputs (indices 0-5), so `inputDefault(6)` is undefined behaviour rather than a compile error in some builds. If it compiles, run `ctest --test-dir build -R gl_smoke --output-on-failure 2>&1 | tail -5` instead and expect a failure or a crash in the new scenario.

Record which of the two you saw — the test must be seen failing before the implementation lands.

---

### Task 2: The port and the behaviour

**Files:**
- Modify: `src/modules/AudioPlayerNode.h`
- Modify: `src/modules/AudioPlayerNode.cpp`

- [ ] **Step 1: Add the port**

In `src/modules/AudioPlayerNode.cpp`, in the constructor, after the `length` line and before the outputs:

```cpp
    addIntInput("length", 4, 1, 64);                         // bars to fit the clip into (whole)
    addInput("auto play", PortType::Bool, false);            // transport drives playback (overrides `play`)
    addOutput("left",  PortType::Audio);
```

**Append it here, do not insert it earlier.** `ProjectFile` stores control defaults by port index (`inb <port> <value>`), so a port added anywhere but the end would silently reassign every saved value after it — an existing project's `length` would be read back as `sync`.

- [ ] **Step 2: Add the tracker members**

In `src/modules/AudioPlayerNode.h`, in the private section, after `bool wasSync_`:

```cpp
    bool        wasSync_ = false;              // sync state last frame (silence the switch block)
    // Transport position last frame, for `auto play`: a BACKWARDS move is Stop (which zeroes the
    // transport) or a loop wrap, and rewinds the clip. Both are updated on EVERY evaluate that has
    // a transport -- not only while auto play is on -- so switching auto play or sync on mid-song
    // compares against the previous frame rather than a stale position from minutes ago.
    double      prevTransportSeconds_ = 0.0;
    bool        hadTransport_         = false; // false on the first frame, where prev has no meaning
```

- [ ] **Step 3: Update the `updateStatus` declaration**

In the same header, change:

```cpp
    void updateStatus(bool play, float rate, bool synced, int lengthBars);
```

to:

```cpp
    void updateStatus(bool play, float rate, bool synced, int lengthBars, bool autoPlay);
```

- [ ] **Step 4: Read the input and track the transport**

In `src/modules/AudioPlayerNode.cpp`, in `evaluate`, replace:

```cpp
    int   lengthBars = std::max(1, (int)std::lround(ctx.in<float>(5)));
```

with:

```cpp
    int   lengthBars = std::max(1, (int)std::lround(ctx.in<float>(5)));
    bool  autoPlay   = ctx.in<bool>(6) && ctx.transport != nullptr;

    // Snapshot last frame's transport position, then update the tracker for this one. This runs
    // before the not-yet-loaded early return below, so a Stop during the decode is not missed.
    const double prevTs = prevTransportSeconds_;
    const bool   hadTs  = hadTransport_;
    if (ctx.transport) { prevTransportSeconds_ = ctx.transport->seconds; hadTransport_ = true; }
    else                 hadTransport_ = false;
```

- [ ] **Step 5: Apply the effective play flag and the rewind**

Still in `evaluate`, replace:

```cpp
    double prev = playhead_;
    bool wrapped = false;
    bool syncActive = sync && ctx.transport && duration_ > 0.0;

    // Switching between sync and free playback jumps the playhead to an unrelated
    // position; treat it like a seam so that block is silenced, not swept.
    if (syncActive != wasSync_) wrapped = true;
    wasSync_ = syncActive;
```

with:

```cpp
    double prev = playhead_;
    bool wrapped = false;
    bool syncActive = sync && ctx.transport && duration_ > 0.0;
    // `auto play` hands playback to the transport and overrides the manual toggle.
    bool effPlay = autoPlay ? ctx.transport->playing : play;

    // Switching between sync and free playback jumps the playhead to an unrelated
    // position; treat it like a seam so that block is silenced, not swept.
    if (syncActive != wasSync_) wrapped = true;
    wasSync_ = syncActive;

    // Auto play follows the transport's own position: a backwards move is Stop (which zeroes the
    // transport) or a loop wrap, and rewinds the clip so the next Play starts from the beginning.
    // A forward scrub is deliberately left alone -- strict position locking is what `sync` is for.
    // Skipped while synced, where the playhead is derived from the bar position every frame.
    if (autoPlay && !syncActive && hadTs && ctx.transport->seconds < prevTs) {
        playhead_ = 0.0;
        wrapped   = true;                 // reuse the seam path so a Stop is a clean cut, not a click
    }
```

Then replace the three remaining uses of `play` in this function. The free-run advance:

```cpp
        if (play) playhead_ += (double)rate * (double)ctx.dt;
```
becomes
```cpp
        if (effPlay) playhead_ += (double)rate * (double)ctx.dt;
```

The silence gate:

```cpp
    if (wrapped || !play) a1 = a0;
```
becomes
```cpp
    if (wrapped || !effPlay) a1 = a0;
```

And the status call:

```cpp
    updateStatus(play, rate, syncActive, lengthBars);
```
becomes
```cpp
    updateStatus(effPlay, rate, syncActive, lengthBars, autoPlay);
```

Note `prev` is captured *before* the rewind, so on a rewinding frame `prev` holds the old position while `playhead_` is 0. `wrapped` is true, so the silence gate collapses the slice to `(prev, prev)` and that block is silent — which is the intended clean cut.

- [ ] **Step 6: Say which control is in charge, in the status line**

Replace the whole of `updateStatus`:

```cpp
void AudioPlayerNode::updateStatus(bool play, float rate, bool synced, int lengthBars, bool autoPlay) {
    char buf[96];
    // Name the control that is actually deciding, so a clip silent because the transport is
    // stopped reads as stopped rather than broken.
    const char* state = play ? (autoPlay ? " (auto)" : "")
                             : (autoPlay ? " (auto, stopped)" : " (paused)");
    if (synced)
        std::snprintf(buf, sizeof(buf), "%.1f / %.1f s  sync %d bar%s%s",
                      playhead_, duration_, lengthBars, lengthBars == 1 ? "" : "s", state);
    else
        std::snprintf(buf, sizeof(buf), "%.1f / %.1f s  x%.2f%s",
                      playhead_, duration_, rate, state);
    status_ = buf;
}
```

- [ ] **Step 7: Update the class doc comment**

In `src/modules/AudioPlayerNode.h`, in the comment above the class, change the sentence

```
// negative, reverse playback (it reads the clip backwards). `play` pauses; `loop`
// wraps at the ends. When `sync` is on, the clip is instead time-warped to span exactly
```

to

```
// negative, reverse playback (it reads the clip backwards). `play` pauses; `loop`
// wraps at the ends. `auto play` hands playback to the global transport instead, overriding
// `play`: the clip sounds while the transport plays, holds on pause, and rewinds when the
// transport moves backwards (Stop, or a loop wrap). When `sync` is on, the clip is instead time-warped to span exactly
```

- [ ] **Step 8: Build and run the test**

Run: `cmake --build build -j 2>&1 | tail -3 && ctest --test-dir build -R gl_smoke --output-on-failure 2>&1 | tail -4`

Expected: build clean, `100% tests passed`. Confirm the new line appears:

Run: `./build/gl_smoke 2>&1 | grep "auto play"`
Expected: `gl_smoke OK: Audio File auto play follows the transport (stop rewinds, pause holds, forward scrub ignored)`

- [ ] **Step 9: Mutation-check your own work**

A test that does not bite is worse than no test, because it occupies the line a real assertion would. Apply each mutation, rebuild, run `gl_smoke`, and record which assertion fails. Restore the file after each one and confirm `git diff src/` is empty at the end.

| Mutation | Must be caught by |
|---|---|
| `bool effPlay = play;` (ignore auto play entirely) | (1) or (2) |
| Drop the rewind block | (4) |
| `ctx.transport->seconds > prevTs` (rewind on a forward move) | (6) |
| Drop `&& !syncActive` from the rewind guard | nothing — `sync` is off in this scenario. **Expected to survive; say so.** |
| `bool autoPlay = ctx.in<bool>(6);` (drop the null-transport guard) | nothing — the scenario always has a transport. **Expected to survive; say so.** |

Report the two expected survivors honestly rather than implying coverage you do not have.

- [ ] **Step 10: Run the full suite**

Run: `ctest --test-dir build --output-on-failure 2>&1 | tail -4`
Expected: `100% tests passed out of 3`.

- [ ] **Step 11: Commit**

Write the message to a file and use `-F` (a heredoc with an apostrophe has repeatedly broken in this environment):

```bash
git add src/modules/AudioPlayerNode.h src/modules/AudioPlayerNode.cpp tests/gl_smoke.cpp
git commit -F <message-file>
```

Message:

```
feat(modules): auto play on the Audio File node

A new auto play Bool input hands playback to the global transport: the clip
sounds while the transport plays, holds on pause, and rewinds when the transport
moves backwards, which is what Stop and a transport loop wrap look like. It
overrides the manual play toggle while on, and defaults off so existing projects
are unchanged. Appended as the last port, since ProjectFile stores control
defaults by port index.

A forward scrub is deliberately not followed: strict position locking is what the
existing sync mode is for.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
```

---

### Task 3: Documentation

**Files:**
- Modify: `README.md`
- Modify: `CLAUDE.md`
- Modify: `docs/superpowers/specs/2026-09-22-audio-player-auto-play-design.md`

- [ ] **Step 1: README**

Find the **Audio File** row in the module table (search for `| **Audio File**`) and extend its description with the new input. Read the neighbouring rows first and match their voice — they are terse, pipe-separated phrases, not sentences. The row must mention that `auto play` overrides `play`, and that Stop rewinds.

- [ ] **Step 2: CLAUDE.md**

Find the `**Audio File**` / `AudioPlayerNode` mention in the **Hard rules** section (search for `audio/BarSync.h`) and add a clause for `auto play` alongside the existing `sync` description. Match that file's dense, specific style: name the real behaviour (effective play becomes `transport->playing`, a backwards move rewinds, skipped while synced) rather than describing it loosely.

- [ ] **Step 3: Mark the spec implemented**

In `docs/superpowers/specs/2026-09-22-audio-player-auto-play-design.md`, change `**Status:** Approved (brainstorm)` to `**Status:** Implemented`. If anything in the built code differs from the design, correct the spec to match and note what changed and why — do not rewrite it into a description of the code.

- [ ] **Step 4: Verify the docs are true**

Run: `cmake --build build -j 2>&1 | tail -2 && ctest --test-dir build --output-on-failure 2>&1 | tail -3`
Expected: build clean, `100% tests passed out of 3`.

Then check every claim you wrote actually holds — in particular, open the app's Properties panel path in your head: the port is a Bool, so `inputSlot` routes it to Properties as a checkbox. Confirm that by reading `src/core/PanelSlot.h` rather than assuming.

- [ ] **Step 5: Commit**

```bash
git add README.md CLAUDE.md docs/superpowers/specs/2026-09-22-audio-player-auto-play-design.md
git commit -F <message-file>
```

Message:

```
docs(audio): auto play on the Audio File node

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
```

---

## Plan self-review

**Spec coverage.** The port, its default and its position at index 6 → Task 2 Step 1, with the reason appending matters. The effective-play rule and the null-transport fallback → Step 4 and Step 5. The backwards-move rewind and its table of transport actions → Step 5. Skipping the rewind while synced → the `!syncActive` guard in Step 5. The tracker updating on every frame regardless of mode, and the first-frame guard → Step 4, placed above the not-loaded early return. The seam reuse → the `wrapped = true` in Step 5, with the note on why `prev` is captured first. The status line naming the controlling input → Step 6. All six test behaviours from the spec → Task 1, in the same order. The out-of-scope list needs no task.

**Placeholder scan.** No TBD or TODO. Every code step shows the code; every run step gives the command and what to expect. Task 3's doc steps describe what must be said rather than quoting final prose, which is deliberate — the wording depends on the neighbouring rows, and both steps name the file, the search anchor and the required content.

**Type consistency.** `autoPlay`, `effPlay`, `prevTs`, `hadTs`, `prevTransportSeconds_`, `hadTransport_` and the five-argument `updateStatus` are spelled identically in the header change, the declaration change, the implementation and the mutation table. The test uses `inputDefault(6)` for the new port, matching the constructor order in Step 1, and `inputDefault(2)` / `inputDefault(3)` for `play` / `loop`, matching the existing constructor.

**One risk worth flagging to the implementer.** Task 1 Step 2 cannot promise a compile error, because `inputDefault(6)` on a node with six ports is an out-of-range `std::vector` access rather than a type error. The step says so and gives the fallback. That is honest rather than tidy, and the alternative — writing the test against a port that does not exist and pretending the compiler will catch it — would be worse.
