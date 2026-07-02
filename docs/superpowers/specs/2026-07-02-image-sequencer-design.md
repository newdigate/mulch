# Image Sequencer node — design

**Date:** 2026-07-02
**Status:** Approved (brainstorm)
**Branch:** `feat/image-sequencer` (off `develop`)

## Goal

A new **Image Sequencer** node (Texture category): like **Image Streamer**, but it plays a
*folder* of images in sequence, advancing one image every `duration` (a Float input). The
folder is chosen from a dropdown of the distinct image-containing folders in the Assets
library.

## Decisions (from brainstorm)

- **"Each image-containing folder" is a selectable folder.** The ▾ dropdown lists the distinct
  *immediate parent directory* of every Image asset in the library (e.g. `media/fire`,
  `media/rain`, `other`), deduplicated and sorted. Picking one copies that folder path into
  the node's `folder` field (copy-path model, exactly like the existing asset picker).
- **Disk is the source of truth.** The library only *populates the menu*. On folder change the
  node scans that directory on disk for image files (so it plays every image file physically in
  the folder, including ones never added to the library).
- **Both timing modes** via a `sync` toggle (mirrors Audio File / Step Seq):
  - `sync` **off** — free-running: `duration` is **seconds per image**; the node advances by
    accumulated frame `dt` and loops. Keeps advancing even when the transport is stopped.
  - `sync` **on** — transport-locked: `duration` is **beats per image**; the current index is
    derived *statelessly* from `transport.beats()`, so it starts/stops with playback, stays
    bar-aligned, and survives loops.
- **Decode-on-advance.** The node holds one GL texture (like Image Streamer) and decodes the
  next image only when the index actually changes — memory is bounded regardless of folder size.
- **New node, not a mode on Image Streamer** (their timing/state differ enough).

## Architecture

| File | Change |
|---|---|
| `src/core/PathUtil.h` | Add `parentDir(path)` — the directory portion of a path. |
| `src/core/AssetTree.h` | Add `uniqueAssetFolders(rows)` — distinct `parentDir`s of the assets, deduped + sorted. |
| `src/core/ImageSequence.h` | **New, header-only, GL-free.** `syncedImageIndex(beats, durationBeats, count)` — the transport-synced index math. |
| `src/gfx/ImageLoader.h` / `.cpp` | Add `listImagesInDir(dir)` — scan a directory for image files, sorted by filename (GL-free, `std::filesystem`). |
| `src/core/Port.h` | Add `bool folderPicker = false;`. |
| `src/core/Node.h` | Add `addImageFolderInput(name)` — a folder-picker String input. |
| `src/modules/ImageSequencerNode.h` | **New, header-only.** The node. |
| `src/ui/NodeEditorPanel.cpp` | `NodePopup`: a folder-picker branch listing `uniqueAssetFolders(byType(Image))`. |
| `src/app/Application.cpp` | Register `"Image Sequencer"` in `makeNode()` + the `"Texture"` category; add the include. |
| `tests/test_path_util.cpp` | `core_tests`: `parentDir`. |
| `tests/test_asset_tree.cpp` | `core_tests`: `uniqueAssetFolders`. |
| `tests/test_image_sequence.cpp` | **New.** `core_tests`: `listImagesInDir` (temp dir) + `syncedImageIndex`. |
| `tests/gl_smoke.cpp` | Scenario: folder of 3 PNGs → node → advance → colour changes 0→1→wrap. |
| `CMakeLists.txt` | Register `tests/test_image_sequence.cpp` in the `core_tests` source list. |
| `CLAUDE.md`, `README.md` | Document the node. |

## Component detail

### 1. Folder selection

- `Port::folderPicker` (new bool). `Node::addImageFolderInput(name)` builds a `String` input
  with `assetBacked = true`, `folderPicker = true`, `assetType = AssetType::Image`. Because
  `assetBacked` is set, `PortWidgets` already renders the editable path + ▾ arrow — **no
  `PortWidgets` change**.
- In `NodeEditorPanel`'s `NodePopup`, inside the existing `String && assetBacked` block, branch:
  if `port.folderPicker`, list `uniqueAssetFolders(graph.assets().byType(port.assetType))`
  instead of individual assets; selecting a folder does `v = Value(folder)` and closes. If the
  list is empty, show "No image folders — add images in the Assets window".
- `uniqueAssetFolders(rows)` (GL-free, in `AssetTree.h`): collect `parentDir(a->path)` for each
  non-null asset, drop empties, sort ascending, unique. `parentDir(path)` (in `PathUtil.h`):
  the substring before the last `/` or `\\`; `""` if there is no separator.

### 2. Directory scan

`listImagesInDir(const std::string& dir)` in `gfx/ImageLoader.{h,cpp}` (GL-free):
`std::filesystem::directory_iterator` over `dir`, keep regular files whose lower-cased
extension is one of `png/jpg/jpeg/bmp/tga/gif/hdr/psd`, return their full paths **sorted
ascending by filename** (so `01.png,02.png,…` order). Returns an empty vector on a missing /
unreadable directory (wrapped in a `try/catch` so a bad path can't throw out of `evaluate`).

### 3. The node (`ImageSequencerNode.h`, header-only)

Ports:
- `folder` — `addImageFolderInput("folder")`.
- `duration` — `addInput("duration", PortType::Float, 1.0f, 0.05f, 60.0f)`. Seconds (free) / beats (synced).
- `sync` — `addInput("sync", PortType::Bool, false)`.
- Output `image` — Texture.

Per `evaluate`:
1. If the folder string changed: `files_ = listImagesInDir(folder)`, reset `elapsed_ = 0`,
   `index_ = -1` (force a load), clear the texture cache key.
2. Compute the target index:
   - `sync` on: `target = syncedImageIndex(transport ? transport->beats() : 0.0, duration, files_.size())`.
   - `sync` off: `elapsed_ += dt`; while `elapsed_ >= duration` and non-empty: `elapsed_ -= duration`, `cur_ = (cur_+1) % N`; `target = cur_`.
   (Guard `duration` to a small minimum; guard `N==0` → publish empty `TexRef`.)
3. If `target != index_`: decode `files_[target]` via `loadImage`, upload to the single texture
   (Image Streamer's `ensureTexture`/`glTexImage2D` path), set `index_ = target`.
4. Publish `TexRef{ tex_, w_, h_ }` (or empty when no images / load failed).

`syncedImageIndex(double beats, float durationBeats, int count)` (GL-free, `ImageSequence.h`):
`count<=0 → 0`; `d = max(durationBeats, 1e-4f)`; `step = (long)floor(beats / d)`; return
`((step % count) + count) % count` (loop-robust for negative beats too).

`statusLine()` → `"<i+1>/<N>  <basename>"`, or `"no images in <folder>"`, or `"load failed: …"`.

### 4. Persistence

`folder` / `duration` / `sync` are control-type input defaults → already saved in `.oss` like
every node. The scanned `files_` list is transient, rebuilt from `folder` on load. No
`saveState`.

## Data flow

`Image Sequencer` is a pure texture *source* (its only meaningful input is the folder path;
`sync` reads the transport). Typical wiring: `Image Sequencer → Kaleidoscope → Output`, or
straight into `Output` / `Compositor`.

## Error handling

- **Missing / empty folder:** `listImagesInDir` returns empty (its `try/catch` swallows a bad
  path); the node publishes an empty `TexRef` and shows "no images in <folder>".
- **Undecodable image at the current index:** `loadImage` fails → keep the previous texture,
  status "load failed: …"; the sequence still advances on the next step.
- **`duration` ≤ 0:** clamped to a small minimum so stepping can't divide by zero or spin.
- **Folder with one image:** behaves like Image Streamer (always shows that image).

## Testing

- **`core_tests`**
  - `parentDir`: `"a/b/c.png" → "a/b"`, `"c.png" → ""`, backslash paths, trailing slash.
  - `uniqueAssetFolders`: assets across `media/fire`, `media/fire`, `media/rain`, `other`,
    and a bare `x.png` → `{"media/fire","media/rain","other"}` (deduped, sorted, bare dropped).
  - `listImagesInDir`: create a temp dir with `b.png`, `a.png`, `note.txt`, `c.jpg`; assert it
    returns `{".../a.png",".../b.png",".../c.jpg"}` (sorted, non-image excluded). Clean up.
  - `syncedImageIndex`: `count=3` → beats 0,0.9 → 0; 1.0→1; 2.0→2; 3.0→0 (wrap) at
    `durationBeats=1`; `count=0 → 0`; negative beats wrap into range.
- **`gl_smoke`**: write a temp folder with 3 solid-colour PNGs (red/green/blue, distinct
  filenames), point the node at the folder with `duration=1`, `sync` off; evaluate one frame
  (shows image 0 = red), then evaluate with `dt=1.1` (advance to image 1 = green), then enough
  to wrap back to red; read back the output texture centre each time and assert the colour
  matches the expected image. Proves scan + sequence + decode + upload. Remove the temp folder.

## Out of scope (YAGNI)

- Recursive folder scan, per-image durations, cross-fade between images, shuffle/ping-pong,
  a `play` toggle (freeze by not advancing is not needed; sync-off always advances by design).
- Watching the folder for new files at runtime (rescans only on folder-path change).
- Live binding to the library (copy-path model, like every other media input).
