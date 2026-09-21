# projectM Node — Design

**Date:** 2026-09-21
**Status:** Approved

## Goal

A new texture module, **projectM**, that runs the Milkdrop-compatible
[projectM](https://github.com/projectM-visualizer/projectm) visualizer inside the graph:
**audio in → video out**. The library is **loaded at runtime** (`dlopen` / `LoadLibrary`),
never linked and never bundled, so the node exists only on machines where the user has
installed projectM themselves. Presets are picked from the asset library and can be stepped
through by hand or on the transport; another node's texture can be burned into projectM's
canvas so presets warp and feed back the graph's own visuals.

## Scope (v1)

- Runtime loader for **libprojectM 4.2 or later** (C API), with a version gate.
- `left`/`right` audio in, `texture` out at the Preferences texture resolution.
- One asset-backed `preset` file input; its parent folder is the playlist.
  **prev / next / random** buttons and **transport-synced** stepping every N bars.
- **Texture-in burn** via `projectm_opengl_burn_texture`.
- Parameter ports: `blend`, `blend time`, `beat sensitivity`, `mesh`.
- A sixth asset type, **Preset** (`.milk`), and two new Preferences (library path,
  textures folder).

## Version requirement (why 4.2+, and what that costs)

Verified against the projectM source on 2026-09-21:

- The latest **release** is **v4.1.7** (2026-07-14). `master` is versioned **4.2.0** and is
  **not yet tagged or released**.
- In 4.1.7, `ProjectM::RenderFrame` ends with `glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0)`:
  it always draws into the default framebuffer and **cannot render into an FBO we bind**.
- `projectm_opengl_render_frame_fbo`, `projectm_opengl_burn_texture`,
  `projectm_set_frame_time` and `projectm_create_with_opengl_load_proc` are all marked
  `@since 4.2.0` and do not exist in 4.1.7.

So the node requires a **from-source build of projectM master**. That adds no cost in
practice: Homebrew still ships 3.1.12 (a C++ class API that cannot be `dlopen`ed sanely) and
4.x releases are source-only, so a source build is needed regardless.

**Accepted risk — unreleased API.** The version gate reads only the version number. If a
function changes signature before 4.2.0 is tagged, resolving symbols by name will not catch
it. Mitigation: the README build instructions pin a known-good projectM commit; revisit when
4.2.0 is released. The accepted major version is exactly 4.

## License

libprojectM is **LGPL-2.1**. Its README states it may be used from closed-source applications
as a shared library. This design goes further than that allowance requires:

- **No linking.** The library is opened at runtime and we declare the function-pointer types
  ourselves; no projectM header is included.
- **No distribution.** We never ship libprojectM, so essentially no LGPL obligations attach to
  our packages. If it is ever bundled: ship the dylib unmodified, include the license text,
  and keep it user-replaceable (a separate dylib already is).
- **Presets are not bundled.** Preset packs live in separate repositories with mixed or
  unstated licensing. The node points at the user's own folder, and the test fixture is a
  preset we author ourselves.

This repository currently has no LICENSE file; LGPL-2.1 used this way is compatible with
MIT, Apache, GPL or proprietary terms, so it does not constrain that choice.

## Structure

Four small units plus two supporting changes.

### `core/DynLib.{h,cpp}` (GL-free)

RAII wrapper over `dlopen`/`dlsym` (`LoadLibrary`/`GetProcAddress` on Windows).
`open(path)`, `isOpen()`, `error()`, `symbol<T>(name)` (null when missing); move-only.
Reusable for any future optional library. It has a `.cpp` so the platform headers stay out of
every header: `<windows.h>` `#define`s `near`, which `tests/gl_smoke.cpp` uses as a helper name.

### `gfx/ProjectMApi.{h,cpp}`

Process-wide singleton holding a struct of function pointers for the ~20 projectM calls the
node uses. Contains no GL headers (GL handles cross as `uint32_t`).

- **Load:** resolve every symbol, call `projectm_get_version_components`, apply the version
  gate. The gate is a pure function, `isSupportedVersion(major, minor)` → `major == 4 &&
  minor >= 2`, so it is unit-testable.
- **Reports:** `available()` and `statusText()` — "projectM not found", "found 4.1.7, needs
  4.2+", or "missing symbol `<name>`".
- **Search order** (built by a pure, testable `candidatePaths(prefPath)`):
  1. `Preferences::projectMLibraryPath`, when set.
  2. The bare library name via the system loader: `libprojectM-4.dylib`,
     `libprojectM-4.so.4`, `projectM-4.dll`.
  3. `/usr/local/lib`, `/opt/homebrew/lib`, `~/.local/lib`.
- **Binding is all-or-nothing:** a candidate library is bound into a local function table that
  is adopted only when the version gate and every symbol pass, so a rejected (and then closed)
  library never leaves dangling pointers in the live table.
- **Lifetime:** loaded once at startup, after Preferences. Never unloaded: the singleton is
  deliberately leaked, because a static object's destructor would `dlclose` a GL-touching
  library during static destruction, after the GL contexts are gone (a classic crash-at-exit).
  If the preference is set later, the app retries only when nothing is loaded yet; otherwise
  the Preferences panel shows "restart to apply".

### `core/PresetPlaylist.h` (GL-free, header-only)

- `listPresetsInDir(dir)` — `.milk` files only (case-insensitive extension), sorted
  case-insensitively, subfolders ignored.
- `indexOf(files, path)`, `step(index, delta, count)` (wraps both ways; safe for `count == 0`).
- `syncedPresetStep(bars, everyNBars)` → `floor(bars / N)`, and
  `syncedPresetIndex(step, count, shuffle, seed)` — stateless from `transport.bars()`, like
  `syncedImageIndex`. Sequential mode is `step % count` (an **absolute** position in the
  sorted folder, not relative to the preset chosen by hand); shuffle mode hashes `step` with
  `seed`, so it replays identically after a loop or seek. In v1 the node passes a **fixed
  constant seed**, so the shuffled order is also the same in every session (node ids are
  remapped on load, so they are not a stable seed); a `seed` port is deferred.
- `PresetSelector` — the preset-selection state machine described under **Preset selection**
  (incoming change / buttons / primed, edge-triggered sync), with an injectable directory
  lister. Keeping it here rather than in the node means the logic is unit-tested in
  `core_tests`, which CI runs; the node only calls `update()` once per frame.

### `modules/ProjectMNode.{h,cpp}`

The node (see **Ports** and **Per-frame flow**). Registered as **projectM** in the
**Texture** category.

### Supporting changes

- `gfx/Framebuffer` gains an `id()` accessor (needed by `render_frame_fbo`).
- `gfx/GLStateGuard.h` — new RAII class. projectM changes GL state freely; the guard saves
  and restores: draw + read framebuffer bindings, viewport, program, vertex array, array
  buffer, active texture unit and its 2D binding, the blend / depth-test / cull / scissor
  enables, blend function, depth mask, and unpack alignment.
- `AssetType::Preset` appended as value **5** (`kAssetTypeCount` → 6), so the asset codec's
  type integer stays backward-compatible. The Assets window gains a **Presets** tab; its
  Browse dialog filters on `.milk`.
- `Preferences` gains `projectMLibraryPath` and `projectMTexturesDir`, both edited in the
  **Locations** tab and persisted in `preferences.oss`.

## Availability

- `nodeCategories()` lists **projectM** only when `ProjectMApi::available()` is true.
- `makeNode("projectM")` **always** constructs the node, so a project that uses it opens on
  any machine. Without the library the node is inert: it publishes a black texture of the
  correct size (downstream nodes keep working) and shows the status text.

## Ports

| Port | Type | Default | Notes |
|---|---|---|---|
| `left`, `right` | Audio | — | Mono edges. A lone connected side is mirrored to both, as the Recorder does. |
| `texture in` | Texture | — | Source image for the burn. |
| `burn` | Float 0..1 | 0 | A gate: above 0.5, `texture in` is stamped over the whole canvas every frame. Float (not Bool) so an LFO, Step Seq or automation channel can drive it. Held = projectM as an effect on your video; pulsed = stamp once, then it melts into the preset's feedback. |
| `preset` | String, asset-backed (`Preset`) | empty | Single source of truth for the playing preset. |
| `blend` | Bool | on | Smooth transition when on, hard cut when off. |
| `blend time` | Float 0..10 s | 3 | → `projectm_set_soft_cut_duration`. |
| `beat sensitivity` | Float 0..2 | 1 | → `projectm_set_beat_sensitivity`. |
| `mesh` | Int 8..128 | 48 | Mesh columns; rows = `max(2, round(cols · 0.75))`. → `projectm_set_mesh_size`. |
| `sync` | Bool | off | Step through the preset's folder on the transport. |
| `bars` | Int 1..64 | 4 | Change preset every N bars. |
| `shuffle` | Bool | off | Applies to the bar-synced step. |

- **Buttons** (existing button-bank hook): **prev**, **next**, **random**.
- **Output 0:** `texture` (`TexRef`).
- **Panels:** the existing `inputSlot` classifier routes the Float sliders to Controls and
  everything else (and the buttons) to Properties; no change needed.
- **Persistence:** every control is an input-port default, saved by `ProjectFile` as-is. The
  node needs no `saveState`.

## Preset selection

The `preset` input is the one source of truth; the playlist is `listPresetsInDir(
parentDir(preset))`, rescanned only when that folder changes.

- A **change in the incoming `preset` value** (field edit, asset pick, or an edge) loads it.
- A **button press**, or a **bar boundary** while `sync` is on and the transport is playing,
  picks a sibling file, loads it, and **writes its path back** through `inputDefault()` so the
  Properties field always shows — and the project saves — what is actually playing.
- Sync is **edge-triggered**: the node remembers the last `syncedPresetStep` and acts only
  when it changes (a bar boundary, a loop seam, or a seek). So a preset picked by hand or by
  button holds until the next boundary instead of being snapped back on the next frame.
- Sync is also **primed**: the first synced frame (turning `sync` on, pressing play, or loading
  a project) only records the current step and switches nothing, so a saved `preset` survives
  until the next boundary.
- **A manual action outranks sync in the same frame.** If a pick or a button lands on the frame
  a bar boundary falls on, the manual choice wins and the boundary is consumed, so a click is
  never silently swallowed.
- Sync **re-primes instead of switching** when `bars` changes: that moves the step number
  without a real boundary, and treating it as one would load a preset per slider tick
  (measured: 4 loads in a one-second drag). A pick in another folder needs no rule of its own:
  the folder can only change through a manual pick, which the rule above already covers.
- While `sync` is on, prev / next / random hold only until the next boundary, when absolute
  positioning reclaims the selection.
- **random** (the button) uses a node-local RNG and is not reproducible; only the bar-synced
  shuffle is. The shuffle is a hash, not a permutation, so consecutive steps can land on the
  same preset (about 1 in `count`); that is the price of replaying identically after a seek.
- projectM's own automatic switching stays off (`projectm_set_preset_locked(true)`); the node
  decides every change.

## Per-frame flow (`evaluate`)

1. **Resolution.** If `prefs->textureWidth/Height` changed, recreate the FBO and call
   `projectm_set_window_size`.
2. **Parameters.** Push `blend time`, `beat sensitivity`, `mesh` when their values change.
3. **Preset.** Resolve per **Preset selection**; on change,
   `projectm_load_preset_file(path, blend)`.
4. **Audio.** Interleave the two `AudioRef` blocks into a reusable LRLR scratch buffer →
   `projectm_pcm_add_float(buf, frames, PROJECTM_STEREO)`.
5. **Burn.** If `burn > 0.5` and `texture in` is valid →
   `projectm_opengl_burn_texture(tex, 0, 0, w, h)`.
6. **Render.** Inside a `GLStateGuard`: `projectm_opengl_render_frame_fbo(fbo_.id())`.
7. **Publish** `TexRef{fbo_.texture(), w, h}` on output 0.

**Timing.** The node accumulates `ctx.dt` and calls `projectm_set_frame_time` each frame, so
projectM runs on the app's clock rather than the wall clock: it pauses when the app stalls and
gives the Recorder repeatable output.

**Lifecycle.** The instance is created in `initGL()` with
`projectm_create_with_opengl_load_proc`, passing a wrapper around `glfwGetProcAddress`
(signature `void* (*)(const char* name, void* user_data)`). It is destroyed in the node's
destructor. Both run with the **editor GL context current**, per the existing rule that node
GL objects belong to that context. When the API is unavailable, `initGL()` creates only the
FBO.

**Texture search paths.** The current preset's folder, plus
`Preferences::projectMTexturesDir` when set (for separate Milkdrop texture packs). Updated
when either changes.

**Status line.** e.g. `projectM 4.2.0 · Geiss - Cosmic Dust (12/4188)`, or the last error.

## Error handling

| Situation | Behaviour |
|---|---|
| Library missing, older than 4.2, or a symbol missing | Inert; black texture; status from `ProjectMApi`; not offered in the Add menu. |
| `projectm_create…` returns NULL | Same inert state; status "projectM failed to initialise". |
| A preset fails to load (`preset_switch_failed` callback, fired on the graph thread during our call) | The previous preset keeps running; status `failed: <name>: <message>`. The failed path is remembered so a synced step does not retry it every frame; the next bar boundary moves on normally. |
| `preset` empty | The node loads `idle://` (projectM's built-in idle preset); buttons and sync are no-ops; status "no preset". |
| `preset` file missing or unreadable | Reported through the `preset_switch_failed` callback, as above. |
| `texture in` unconnected | Burn skipped. |
| No audio connected | Nothing is fed; presets animate without reacting. |

**Accepted risk — in-process crashes.** A crash inside projectM (a bad preset, a GL driver
fault) takes the app down. That is inherent to loading the library in-process.

## Unknowns to settle during implementation

- Whether `burn_texture` respects alpha. If it does, `burn` can later become an amount rather
  than a gate.
- Whether the burn needs a vertical flip (the API takes a negative height for that). A
  `gl_smoke` check pins the orientation either way.

## Tests

### `core_tests`

- **`test_dynlib.cpp`** — a nonexistent path fails cleanly (error text, no exception); opens
  the system C library, resolves `cos` and calls it; a missing symbol returns null; move
  semantics.
- **`test_preset_playlist.cpp`** — `PresetSelector`: an incoming change loads once; next / prev
  wrap and the written-back path does not reload; a step holds against an unchanged edge value;
  no folder → no-ops; random never repeats the current preset; sync primes, switches on a
  boundary to an absolute position, holds a hand-picked preset within a step, follows a seek,
  ignores a stopped transport and re-primes. Helpers: lists only `.milk` (case-insensitive), sorted, subfolders
  ignored; `step` wraps both ways; empty list is safe; `syncedPresetStep` advances every N
  bars and gives the same answer after seeking backwards; `syncedPresetIndex` is
  `step % count` sequentially, and in shuffle mode is deterministic per seed and never out of
  range.
- **`test_projectm_api.cpp`** — the gate rejects 4.1.7 and 5.0 and accepts 4.2.0;
  `candidatePaths` puts the preference path first; a bogus preference path leaves the API
  unavailable with a status text.
- Existing asset + preferences tests gain cases: `AssetType::Preset` (5) round-trips, older
  files still parse, the two new preferences round-trip.

### `gl_smoke` — always run

- The node builds, `initGL()` succeeds, and `evaluate` publishes a valid black texture of the
  right size with a non-empty status (this is the library-absent path, which is what CI sees).
- `preset` is asset-backed with type `Preset` (extends the existing asset-backed check).
- **Playlist write-back** (needs no library): with three presets written, an incoming `a.milk`
  is written back to the field, **next** moves `inputDefault(preset)` to `b.milk`, and the step
  holds against an unchanged incoming value.
- `GLStateGuard` restores state deliberately changed inside its scope.

### `gl_smoke` — only when the library is found (prints a skip notice otherwise)

- **Fixture:** the test writes a minimal `.milk` preset authored by us at runtime; no
  third-party preset ships in the repo.
- **Rendering:** sine audio in, ~10 frames evaluated → the texture is not black.
- **GL state:** framebuffer binding, viewport, program and vertex array are unchanged across
  `evaluate`.
- **Burn:** a red Colour node on `texture in` with the gate high → red-dominant output; a
  half-red / half-green fixture pins the orientation.
- **Status:** after **next**, the status line shows `(2/3)`.

**CI gap.** CI runners will not have projectM 4.2, so the render / burn / playlist checks run
locally only. Building projectM master in CI is possible later and is out of scope here.

## Adding-a-node checklist (from CLAUDE.md)

1. `src/modules/ProjectMNode.{h,cpp}` — ports in the constructor, GL in `initGL()`.
2. Register in `makeNode()` (always) and `nodeCategories()` (only when
   `ProjectMApi::available()`) in `src/app/Application.cpp`.
3. Tests as above.
4. Docs: a CLAUDE.md architecture bullet (+ the two new Preferences and the sixth asset
   type); a README "projectM (optional)" section — building the pinned projectM commit,
   setting the library path, presets not bundled, LGPL note.
5. No packaging change (nothing is bundled). No CMake dependency change beyond
   `${CMAKE_DL_LIBS}` for `dlopen` on Linux.

## Branch and integration notes

- Work happens on `feat/projectm-node`, cut from `develop`.
- When the audio-worker work on `main` lands in `develop`, this node must read its audio from
  the audio→visual **boundary snapshot**, as the Spectrograph does. The node code is the same;
  it only has to be classed as a visual consumer by the audio-subgraph compiler.

## Deferred (future extensions)

- **Preset text input** — a String edge carrying Milkdrop source, loaded with
  `projectm_load_preset_data`; groundwork for an in-app preset editor.
- **Asset library as playlist** — play every `Preset` asset, optionally filtered by tag, to
  curate sets across folders.
- **4.1.x fallback** — render in a hidden shared-context window and blit out; rejected for v1
  (extra context switch per frame, no burn, relies on a hidden backbuffer being readable).
- **Touch waveforms** (`projectm_touch*`) as `x` / `y` / trigger ports.
- **projectM's own playlist library** and beat-driven hard cuts (`hard_cut_*` parameters).
- **Burn rectangle / amount** ports, once the alpha behaviour is known.
- **Building projectM in CI** so the library-dependent `gl_smoke` checks run there.
