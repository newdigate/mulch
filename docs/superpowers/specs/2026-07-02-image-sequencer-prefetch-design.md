# Image Sequencer — separate inputs + async prefetch — design

**Date:** 2026-07-02
**Status:** Approved (brainstorm)
**Branch:** `feat/image-sequencer-prefetch` (off `develop`)

## Goal

Two refinements to the just-shipped **Image Sequencer** node:
1. Split the overloaded `duration` into two inputs — a **seconds** `duration` (free-running) and
   an integer **`beat length`** (≥ 1, transport-synced).
2. Decode the upcoming image **ahead of the switch on a background thread** so transitions are
   smooth (no decode/upload frame-hitch), replacing the current decode-on-advance.

## Decisions (from brainstorm)

- **Two timing inputs + `sync` selects which applies.** New port order:
  `folder` (0), `duration` seconds (1, Float), `beat length` (2, int ≥ 1 via `addIntInput`,
  default 1 — beats per image), `sync` (3, Bool). `sync` off → `duration` seconds; `sync` on →
  `beat length` beats. `syncedImageIndex(beats, beatLength, count)` is unchanged; the node passes
  the integer beat length. The port-index shift is fine — the node is brand-new and no released
  project depends on its layout.
- **Async prefetch, double-buffered.** The node keeps **two GL textures** (shown + next) and a
  single in-flight `std::future<ImageData>`. It decodes image *(i+1)* on a worker thread while
  showing *i*; on the switch it just swaps shown↔next (instant). Memory stays bounded to ~2
  textures + one in-flight CPU buffer, regardless of folder size.
- **Only start a decode when the future is idle.** Re-keying a live `std::async` future would
  block the graph thread in the future's destructor — so the node never replaces an in-flight
  future; it starts a new decode only when the previous one has been polled. (This is why the
  node uses a raw `std::future<ImageData>` with idle-gating rather than the shared `AsyncLoader`,
  whose `request()` reassigns the future and would block on re-key.)
- **First image loads synchronously** (once, on folder change) for an immediate first frame; the
  rest are async. **On a miss** (sync-mode jump > 1, or a decode slower than the interval) the
  node keeps showing the current image and async-fetches the wanted one, switching when ready —
  a brief hold-on-previous, never a hitch.
- Decoding (`loadImage`: file read + stb decode) is already GL-free and runs on the worker
  thread; only `glTexImage2D` runs on the graph thread — honoring the app's "GL uploads on the
  main thread; threads bridge via futures" rule.

## Architecture

| File | Change |
|---|---|
| `src/modules/ImageSequencerNode.h` | **Rewrite.** New ports; two GL textures; `std::future<ImageData>` prefetch with idle-gating; adopt-on-ready / hold-on-miss. |
| `tests/gl_smoke.cpp` | Update the Image Sequencer scenario: new port indices, and **poll `evaluate()` until the shown texture reaches the expected colour** (async is nondeterministic) — reusing the Mesh-loader poll-until-ready idiom. |
| `tests/test_image_sequence.cpp` | Add a `beat length > 1` case to the `syncedImageIndex` tests (math is unchanged; the case documents beats-per-image). |
| `CLAUDE.md`, `README.md` | Update the node's description (two timing inputs; async prefetch). |

No new files, no CMake changes, no `core/` changes (`syncedImageIndex` keeps its signature). `<future>` is standard C++17.

## Component detail — `ImageSequencerNode`

**Ports:** `folder` (image-folder picker), `duration` (Float 1.0, [0.05, 60]), `beat length`
(`addIntInput("beat length", 1, 1, 16)`), `sync` (Bool false); output `image` (Texture).

**State:**
- `std::vector<std::string> files_`, `std::string folder_`, `std::string status_`
- `GLuint texShown_=0, texNext_=0`; `int wShown_,hShown_, wNext_,hNext_`
- `int shownIndex_=-1` (index in `texShown_`), `int cur_=0` (free-run counter), `float elapsed_=0`
- `std::future<ImageData> fetch_`; `int fetchIndex_=-1` (index being decoded; -1 = idle)
- `bool nextReady_=false`; `int nextIndex_=-1` (index held in `texNext_`)

**Per-frame `evaluate`:**
1. Read `folder`, `duration` (clamp ≥ 0.01), `beatLen` (round, ≥ 1), `sync`.
2. **Folder changed:** rescan `files_ = listImagesInDir(folder)`; reset `shownIndex_=-1, cur_=0,
   elapsed_=0, nextReady_=false, fetchIndex_=-1`, `fetch_={}`. If `files_` non-empty, **synchronously**
   load index 0 into `texShown_` (immediate first frame), set `shownIndex_=0`.
3. `n = files_.size()`. If `n==0`: `status_="no images in "+folder_` (or empty), output empty
   `TexRef`, return.
4. **Poll the future:** if `fetch_` valid & ready → `ImageData img = fetch_.get()`; if `img.ok()`
   upload into `texNext_`, set `nextIndex_=fetchIndex_, nextReady_=true`; set `fetchIndex_=-1` (idle).
5. **Compute `target`:** `sync` → `syncedImageIndex(transport?beats:0, beatLen, n)` and keep
   `cur_=target` (so a later sync→free handoff resumes from the shown image); else free-run:
   `elapsed_+=dt; while (elapsed_>=duration){ elapsed_-=duration; cur_=(cur_+1)%n; }`, `target=cur_`.
6. **Switch if needed:** if `target != shownIndex_` **and** `nextReady_ && nextIndex_==target`:
   swap `texShown_`↔`texNext_` (and dims), `shownIndex_=target`, `nextReady_=false`. Otherwise
   keep showing the current image (no hitch).
7. **Drive the prefetch (idle-gated):** `want = (shownIndex_==target) ? (target+1)%n : target`.
   If not already satisfied (`nextReady_ && nextIndex_==want`) **and** the future is idle
   (`fetchIndex_==-1`), start `fetch_ = std::async(std::launch::async, [p=files_[want]]{ std::string e; return loadImage(p, e); })`, `fetchIndex_=want`.
8. `status_ = (shownIndex_+1)/N  basename` (or "load failed …" if the sync first-load failed).
9. Output `TexRef{texShown_, wShown_, hShown_}` when `shownIndex_>=0 && texShown_`, else empty.

**Teardown:** the destructor lets `fetch_`'s destructor join any in-flight decode (brief), then
deletes both textures (editor context current, per the two-context rule).

## Data flow / behavior

Steady state at interval *T*: while image *i* shows, *(i+1)* decodes in the background and is
uploaded to `texNext_`; at the next boundary the node swaps to it instantly and begins decoding
*(i+2)*. A decode that overruns *T* simply lands a frame or two late — the node holds *i* until
*(i+1)* is ready, then catches up. Bounded to two textures + one in-flight buffer.

## Error handling

- **Undecodable image:** the worker returns `!ok()`; step 4 marks the future idle and skips the
  upload (keeps the current shown image), so a corrupt file never blanks or hitches the output.
  The synchronous first-image load failing sets a "load failed" status and leaves `shownIndex_=-1`
  → empty output.
- **Missing/empty folder:** `listImagesInDir` empty → "no images" status, empty `TexRef`.
- **`duration` ≤ 0 / `beat length` < 1:** clamped (≥ 0.01 s; `beat length` port min is 1 and the
  value is rounded and floored to 1).
- **Folder changed mid-decode:** reassigning `fetch_={}` may briefly join the in-flight worker —
  acceptable, as a folder switch is a rare setup action, not a playback transition.

## Testing

- **`core_tests`** — `syncedImageIndex` tests stay; add a `beat length = 2` case
  (`syncedImageIndex(beats, 2, 3)`: beats 0,1.9 → 0; 2 → 1; 4 → 2; 6 → 0).
- **`gl_smoke`** — rewrite the Sequencer scenario for the new ports and the async path: write 3
  solid-colour PNGs; set `duration=1`, `sync` off; **poll `evaluate(1/60)` up to N frames until the
  centre is red** (image 0 loads synchronously, so this is immediate), then advance with a
  `dt≈1.1` step and **poll until the centre becomes green**, then blue, then wraps to red — each
  with a frame-count timeout (reusing the Mesh-loader poll-until-ready idiom). Add a synced check:
  set `sync` on with `beat length=1`, drive `transport().seconds`, poll until the expected colour.
  Keep the port-flag probe (folder input is a folder picker). Clean up the temp dir on every path.

## Out of scope (YAGNI)

- Pre-loading the whole folder to GPU (the rejected alternative); a decode thread pool
  (one in-flight decode is enough for a single node); cross-fade/dissolve between images;
  recursive scan; per-image durations.
