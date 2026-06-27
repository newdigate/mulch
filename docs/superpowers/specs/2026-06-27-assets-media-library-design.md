# Assets / Media Library — design (Phase 1)

**Date:** 2026-06-27
**Status:** Approved (brainstorm)
**Branch:** `feat/assets-library` (off `develop`)

## Goal

A dedicated **Assets** window: a per-project media library organised into four tabs —
**Audio / Video / MIDI / 3D**. Each tab is a table of media files of that type; files can be
added, edited (label + path, both plain strings), and removed. Each asset carries a hidden,
stable, unique id plus a human label and a file path.

This is **Phase 1**. **Phase 2** (a separate spec) rewires the nodes' `file` controls
(Audio File, Video Player, Mesh Loader, MIDI File, Drum Machine, …) into dropdowns that pick
from this library by asset id. Phase 1 builds the library + window and the seams Phase 2 needs
(stable ids, a `byType` accessor); it does not touch any node.

## Why project state (not app-global)

The library is **project state** — owned by `Graph`, saved/loaded with each `.oss` project via
`ProjectFile`, exactly like `AutomationStore`. Phase 2 node controls will store an asset **id**;
for those references to survive save/load the library must travel inside the project, and the ids
must be preserved verbatim across a round-trip. An app-global (Preferences-style) library would
let a project's references dangle whenever the shared list changed. So: Graph-owned, `.oss`-persisted.

## Architecture (three units + integration)

| File | Responsibility |
|---|---|
| `core/AssetLibrary.{h,cpp}` | **new** — `Asset{id,type,label,path}` + add/edit/remove/`byType`/`load`; GL-free, unit-tested |
| `ui/AssetsPanel.{h,cpp}` | **new** — the "Assets" window: a tab bar (Audio/Video/MIDI/3D), a table per tab |
| `ui/FileDialog.{h,cpp}` | **new** — `openFileDialog(title, filters)` → path; native dialog confined to the `.cpp` |
| `core/Graph.{h,cpp}` | + owns an `AssetLibrary`; `assets()` accessor; `clear()` empties it (modify) |
| `core/ProjectFile.{h,cpp}` | + `ProjectDoc::assets`; capture/restore + `asset` line codec (modify) |
| `src/app/Application.{h,cpp}` | + owns `AssetsPanel`; draws it; `showAssets_` toggle (modify) |
| `src/ui/TransportBar.{h,cpp}` | + an **Assets** toolbar button (modify) |
| `tests/test_assets.cpp` | **new** — `core_tests`: library ops + ProjectFile round-trip |
| `CLAUDE.md`, `README.md` | docs (modify) |

`AssetLibrary` is GL-free and independently testable; `AssetsPanel` and `FileDialog` are the only
GL/ImGui-adjacent pieces and stay in `ui/`.

## Data model — `core/AssetLibrary`

```cpp
enum class AssetType { Audio, Video, Midi, Mesh };   // the 4 tabs, in this order

struct Asset {
    int         id;       // unique within the library; monotonic; never reused
    AssetType   type;
    std::string label;    // human name (editable; may be empty / duplicated)
    std::string path;     // file path (editable; may be empty / duplicated)
};

class AssetLibrary {
public:
    int  add(AssetType type, std::string label, std::string path);  // returns the fresh id
    void remove(int id);                          // no-op if id absent
    Asset*       find(int id);                     // nullptr if absent
    const Asset* find(int id) const;
    void setLabel(int id, std::string label);      // no-op if id absent
    void setPath (int id, std::string path);       // no-op if id absent

    // All assets of one type in insertion order — drives a tab now and Phase-2 dropdowns later.
    std::vector<const Asset*> byType(AssetType type) const;

    const std::vector<Asset>& all() const { return assets_; }
    void clear();                                  // empties; resets nextId_ to 1

    // Restore: adopt `assets` verbatim (ids preserved) and set nextId_ = max(id)+1
    // so future add()s never collide with or reuse a loaded id.
    void load(std::vector<Asset> assets);

private:
    std::vector<Asset> assets_;
    int nextId_ = 1;                               // monotonic; never reused
};
```

**Invariant — ids are unique, monotonic, never reused.** `add` always hands out `nextId_++`;
`remove` never lowers `nextId_`. This is what lets a Phase-2 node control hold an asset id and
stay valid as the library is edited.

**Restore preserves ids verbatim.** Unlike node ids (which `restoreProject` *remaps* file-id →
fresh-id because nodes are recreated), assets are pure data, so `load()` keeps each asset's id
exactly. That is the whole point — Phase-2 references must still resolve after a load.

## UI — `ui/AssetsPanel`

Signature mirrors `PreferencesPanel`:

```cpp
class AssetsPanel {
public:
    void draw(AssetLibrary& lib, bool* open);
};
```

- `if (!open || !*open) return;` then `ImGui::Begin("Assets", open)`.
- `ImGui::BeginTabBar("assets_tabs")` with four tabs: **Audio**, **Video**, **MIDI**, **3D**.
- Each tab calls one helper:
  `drawTab(lib, AssetType, const char* noun, const std::vector<std::string>& filters)`.

`drawTab` renders an `ImGui::BeginTable(..., 3 columns: Label | Path | "")`:

- For each `lib.byType(type)` asset (capture `id` up front):
  - **Label**: `ImGui::InputText("##label{id}", buf, …)` → on edit `lib.setLabel(id, buf)`.
  - **Path**: `ImGui::InputText("##path{id}", buf, …)` → on edit `lib.setPath(id, buf)`;
    `ImGui::SameLine()` then a `…##browse{id}` button → `openFileDialog(title, filters)`; a
    non-empty result calls `lib.setPath(id, picked)` and, **if the label is empty**, seeds the
    label from the picked file's basename (sans extension).
  - **Remove**: an `x##del{id}` button → record `id` in a local `toRemove` (see below).
- A **`＋ Add {noun}`** button below the table → `lib.add(type, "", "")`.
- After the row loop, apply the deferred removal: `if (toRemove >= 0) lib.remove(toRemove);`
  (one removal per frame is fine; mutating the vector mid-iteration is the bug the
  connection-delete code already guards against). `ImGui::PushID(id)` scopes the per-row widgets.

Fixed `char` buffers (512) for the InputTexts, matching the rest of the app's inline text fields.
**No `onChange` callback** — assets are project state, written to disk only when the user saves the
project (unlike Preferences, which persist live). Edits mutate the in-memory library directly.

Filters per tab (used by Browse; not enforced on typed paths):

| Tab | `AssetType` | noun | filters |
|---|---|---|---|
| Audio | `Audio` | "audio file" | `mp3 wav flac m4a ogg aac aiff` |
| Video | `Video` | "video file" | `mp4 mov mkv avi webm` |
| MIDI  | `Midi`  | "MIDI file"  | `mid midi` |
| 3D    | `Mesh`  | "3D model"   | `obj gltf glb` |

## File dialog — `ui/FileDialog`

```cpp
namespace oss {
// Native open-file dialog. `filters` are bare extensions ("wav","mp3"); empty = all files.
// Returns the chosen absolute path, or "" if the user cancels. The library is confined to the .cpp.
std::string openFileDialog(const char* title, const std::vector<std::string>& filters);
}
```

**Backend: `nativefiledialog-extended` (NFD)** via CMake `FetchContent` — real Cocoa/GTK pickers,
modern CMake, matching the project's dependency convention; pinned like the other fetched deps,
relaxed with `CMAKE_POLICY_VERSION_MINIMUM 3.5` if its bundled CMake is too old for CMake 4.x.
A modal native dialog briefly pauses rendering while it is open — expected for a user-triggered
picker. Because the whole backend sits behind the single `openFileDialog()` function, swapping to a
vendored single-file fallback (e.g. tinyfiledialogs) later is a one-file change if NFD proves
awkward to build here.

## Persistence — `Graph` + `ProjectFile`

- **`Graph`**: add `AssetLibrary assets_;` + `AssetLibrary& assets()` / `const AssetLibrary& assets() const`.
  `Graph::clear()` calls `assets_.clear()` (a new/loaded project starts from the file's assets;
  `nextId_` resets — ids come from the loaded file, not from a prior session).
- **`ProjectDoc`**: add `std::vector<Asset> assets;`.
- **`captureProject`**: `d.assets = g.assets().all();`.
- **`restoreProject`**: after the existing `g.clear()`, `g.assets().load(d.assets);`
  (load after clear, ids preserved verbatim).
- **`serializeProject`** emits each asset as a small **multi-line record**, after the
  node/connection/automation blocks — mirroring the existing `node` → `type` → `ins`/`state`
  structure. This is required because the codec's `escape()` guards only `\` and `\n` (**not
  spaces**), so a free-text string must be the *rest of its line*; an asset has two free-text
  fields (label **and** path), so each gets its own line:

  ```
  asset <id> <typeInt>
  alabel <escaped-label>     # only emitted when the label is non-empty
  apath  <escaped-path>      # only emitted when the path is non-empty
  ```

  where `typeInt` is `0=Audio 1=Video 2=Midi 3=Mesh`. `alabel`/`apath` reuse `escape()` +
  `restOfLine`, so labels and paths may contain spaces (only a literal newline would need the
  `\n` escape). Empty fields are omitted, exactly like `state` — same convention.
- **`parseProject`** handles three keywords: `asset` pushes a new `Asset{id, clamp(typeInt,0,3),
  "", ""}` onto `out.assets` and makes it the **current asset**; `alabel` / `apath` set
  `unescape(restOfLine())` on the current asset (no-op if none is open). Malformed lines are
  skipped, never thrown — matching the codec's forgiving style. The format header stays
  `oss-project 1` (additive; older projects with no `asset` lines load with an empty library).

## Application / toolbar integration

- **`Application`**: add `AssetsPanel assets_;` and `bool showAssets_ = false;`. In `frame()`:
  `assets_.draw(graph_.assets(), &showAssets_);` (next to the existing `preferences_.draw(...)`).
  The library lives in `graph_`, so save/load already carry it — no extra Application wiring.
- **`TransportBar`**: its IO struct gains `bool* showAssets`. Draw an **Assets** button (beside
  **Prefs**) that toggles `*io.showAssets`. `Application::frame` sets `io.showAssets = &showAssets_`.

## Data flow

**Edit (per frame):** `AssetsPanel::draw` reads `graph.assets()`, renders tables, and writes edits
(`add`/`setLabel`/`setPath`/`remove`) straight back into the library. Nothing else reads it in
Phase 1.

**Save:** `captureProject` snapshots `assets().all()` into `ProjectDoc`; `serializeProject` writes
`asset` lines.

**Load:** `parseProject` fills `ProjectDoc::assets`; `restoreProject` → `assets().load(...)`,
preserving ids so any (future) references resolve.

## Error handling

- Add with empty label/path → allowed; the user fills it in. (`byType` still lists it.)
- Browse cancelled → `openFileDialog` returns `""`; no change.
- Duplicate labels or paths → allowed (only `id` is unique).
- Remove of an absent id, `setLabel`/`setPath` on an absent id → no-op.
- Very long paths → 512-char InputText buffers (matches existing fields); longer input is truncated
  in the field, not crashed.
- Malformed `asset` line on load → skipped; clamp `typeInt`; never throws.
- A path that doesn't exist on disk → **not** validated in Phase 1 (the node that consumes it
  already reports its own load error, as today).

## Testing (`core_tests` — `tests/test_assets.cpp`)

- **AssetLibrary**: `add` returns strictly increasing ids; `byType` filters by type and preserves
  insertion order; `find`/`setLabel`/`setPath` mutate the right asset and no-op on a bad id;
  `remove` deletes only its id and **does not** lower `nextId_` (a subsequent `add` gets a fresh,
  higher id); `clear` empties and resets; `load` adopts ids verbatim and sets `nextId_` past the max
  (a post-`load` `add` never collides with a loaded id).
- **ProjectFile round-trip**: a `ProjectDoc` with several assets — including labels and paths that
  contain **internal and trailing** spaces (a backslash too) — survives `serializeProject` →
  `parseProject` with ids, types, labels, and paths intact; an empty-label/empty-path asset
  round-trips (its `alabel`/`apath` lines are omitted and restored as `""`); an older project text
  with no `asset` lines parses to an empty asset list. (A *leading* space is intentionally not
  tested — the codec's shared `restOfLine` trims leading whitespace, same as `ins` strings.)

No `gl_smoke` scenario: the panel is ImGui-only (can't be exercised headlessly) and the library is
fully covered by `core_tests` — the same call made for the Drum Machine.

## Out of scope (Phase 2 / YAGNI)

- Rewiring node `file` controls into asset dropdowns (**Phase 2**, its own spec).
- An image/texture tab (only the four tabs named: Audio/Video/MIDI/3D).
- Thumbnails / waveform / preview, drag-and-drop import, multi-select.
- Per-asset metadata beyond label + path (duration, channels, tags).
- Validating that a path exists on disk, or de-duplicating paths.
- A "reveal in Finder" / open-externally action.

## Decided defaults (flag to change)

- Window title **"Assets"**, toolbar button **"Assets"** beside **Prefs**.
- Four tabs in order **Audio / Video / MIDI / 3D** → `AssetType{Audio,Video,Midi,Mesh}` →
  `typeInt 0..3`.
- Table layout (Label | Path + Browse | Remove), not a card grid.
- Add → blank asset (empty label + path); Browse seeds an empty label from the filename.
- File dialog backend: **NFD** via FetchContent, behind a swappable `openFileDialog()`.
- Library is **project state** (`.oss`), owned by `Graph`; ids preserved verbatim on load.
