# Asset Library files + path preferences — design

**Date:** 2026-06-28
**Status:** Approved (brainstorm)
**Branch:** `feat/asset-library-files` (off `develop`)
**Builds on:** the Assets / media library (`core/AssetLibrary`, `ui/AssetsPanel`, `ProjectFile`,
`core/Preferences`, `ui/FileDialog`, `ui/TransportBar`).

## Goal

Make the Asset Library a **standalone, referenced document**. A new top-level **Asset Library** menu
opens / saves / saves-as a portable `.osslib` file and remaps a directory prefix across its assets.
A project stores a *reference* to its library (no longer embedding assets) and loads it alongside.
Two new preferences (projects dir, asset-library dir) seed the file dialogs.

## Decisions (from brainstorm)

- **Referenced model:** a project `.oss` stores a path to an external `.osslib` (no embedded assets);
  loading the project also loads the referenced library.
- **Save lifecycle (option A):** saving a project also writes the bound `.osslib` (kept in sync); if
  the in-memory library is non-empty but **unbound**, the project save first prompts a library
  *Save As*. **Backward compat:** an old project with embedded asset lines still loads (assets read
  into the in-memory library, unbound); its next project save prompts a library Save As.
- **Remap** is a **prefix** swap (base-path), exact-match, across **all** assets.
- Stored library reference is an **absolute** path; missing-on-load → status warning + leave the
  library **unbound** (user can Open/locate one), no crash.
- Per-asset media **Browse** (the "…" / "Add files…" in the Assets grid) also defaults to the
  asset-library pref dir.

## Architecture

Three coordinated parts. Core stays GL-free **and** file-I/O-free (the `core/` codecs return/accept
strings; the **Application** layer does all `ifstream`/`ofstream` work, as it already does for
projects and preferences).

| File | Change |
|---|---|
| `src/core/AssetLibraryFile.{h,cpp}` | **New, GL-free.** `serializeLibrary` / `parseLibrary` (the `.osslib` text codec) + shared asset-block helpers reused by `ProjectFile`. |
| `src/core/ProjectFile.{h,cpp}` | `ProjectDoc` gains `assetLibraryPath`; serialize writes `assetlib <path>` and **stops embedding** assets; parse reads `assetlib` and **still parses legacy** asset lines; `captureProject` stops populating `assets`/`tagColors`. |
| `src/core/AssetLibrary.{h,cpp}` | new `remapPathPrefix(from, to)` → rewrites matching `path` prefixes, returns count. |
| `src/core/Preferences.{h,cpp}` | `projectsDir` + `assetLibraryDir` fields + codec lines. |
| `src/ui/FileDialog.{h,cpp}` | `defaultPath` arg on open/save; new `pickFolderDialog`. |
| `src/ui/TransportBar.{h,cpp}` | new **Asset Library** menu + `ProjectBarIO` callbacks. |
| `src/ui/AssetsPanel.{h,cpp}` | a **Remap Directory** modal; per-asset Browse takes a default dir. |
| `src/ui/PreferencesPanel.cpp` | a **Locations** section (two folder pickers). |
| `src/app/Application.{h,cpp}` | `currentLibraryPath_` + library Open/Save/SaveAs/Remap + the save/load lifecycle + dialog-default wiring. |
| tests | `core_tests`: library round-trip, ProjectFile reference + legacy parse, `remapPathPrefix`, Preferences round-trip. Update existing ProjectFile/gl_smoke asset round-trip tests to the reference model. |
| `CLAUDE.md`, `README.md` | docs. |

## 1. Library file codec — `core/AssetLibraryFile.h/.cpp`

Factor the existing asset block out of `ProjectFile.cpp` so the project and the library file share
one implementation and can't drift.

```cpp
// AssetLibraryFile.h (GL-free)
namespace oss {
// Append the asset + tag-color block (asset/alabel/apath/atag/tagcolor lines) for `assets`/`colors`
// to `out`. Shared by ProjectFile (legacy read) and the .osslib codec.
void appendAssetBlock(std::string& out, const std::vector<Asset>& assets,
                      const std::map<std::string, glm::vec4>& colors);
// Handle one asset-block keyword line during a line-by-line parse. Returns true if `kw` was an
// asset-block keyword (and updated `assets`/`colors`/`curAsset`), false otherwise. `curAsset`
// tracks the asset most recent `asset` line opened.
bool parseAssetBlockLine(const std::string& kw, const std::string& rest,
                         std::vector<Asset>& assets, std::map<std::string,glm::vec4>& colors,
                         Asset*& curAsset);

// The standalone library file: header `oss-assetlib 1` then the asset block.
std::string serializeLibrary(const AssetLibrary& lib);
bool        parseLibrary(const std::string& text, AssetLibrary& out);   // false on bad header
}
```

- `serializeLibrary` writes `oss-assetlib 1\n` then `appendAssetBlock(out, lib.all(), lib.tagColors())`.
- `parseLibrary` checks the header, then for each line splits keyword/rest and feeds
  `parseAssetBlockLine`; on success it `out.load(assets)` + `out.loadTagColors(colors)`. A bad header
  returns false (caller leaves the current library untouched).
- The exact line format (`asset <id> <type>`, `alabel`, `apath`, `atag`, `tagcolor <r> <g> <b> <a> <name>`)
  is unchanged — it is literally the code moved out of `ProjectFile.cpp`, so existing `.oss` files and
  the new `.osslib` share bytes for the asset section.

## 2. Project format — `ProjectFile` (referenced model)

- `ProjectDoc` gains `std::string assetLibraryPath;`.
- `serializeProject`: write `assetlib <escape(d.assetLibraryPath)>` when non-empty; **remove** the
  asset/tagcolor emission loop (the project no longer embeds assets). The `assets`/`tagColors` fields
  remain on `ProjectDoc` (for legacy reads), just unwritten.
- `parseProject`: handle `assetlib` (`out.assetLibraryPath = unescape(rest)`); **keep** handling the
  legacy `asset`/`alabel`/`apath`/`atag`/`tagcolor` lines via the shared `parseAssetBlockLine` (old
  projects load their embedded assets into `out.assets`/`out.tagColors`).
- `captureProject`: stop setting `d.assets` / `d.tagColors` (model C). `restoreProject`: keep loading
  `d.assets`/`d.tagColors` into `g.assets()` — this is now only exercised by **legacy** projects
  (a fresh capture leaves them empty, so a model-C restore is a no-op for assets, and the Application
  loads the external library afterwards).
- Header stays `oss-project 1` (the change is additive + a dropped section; old files still parse).

## 3. Remap — `core/AssetLibrary`

```cpp
// Replace the leading `from` of every asset path that starts with it with `to`. Exact, case-
// sensitive prefix match across all assets. Returns the number of assets changed. No-op if `from`
// is empty. (A trailing separator is the caller's responsibility; the UI supplies directory paths.)
int AssetLibrary::remapPathPrefix(const std::string& from, const std::string& to);
```

Unit-tested: matches a prefix, leaves non-matching paths, returns the count, empty-`from` is a no-op.

## 4. Application lifecycle

New member `std::string currentLibraryPath_;` (`""` = unbound) + `std::string libStatus_;`.

- **Open Asset Library** (`openLibraryDialog`): `openFileDialog("Open Asset Library", "Asset Library",
  {"osslib"}, prefs_.assetLibraryDir)` → read file → `parseLibrary` into `graph_.assets()`; on success
  set `currentLibraryPath_`, `libStatus_ = "opened <name>"`. Bad header / unreadable → `libStatus_ =
  "open failed"`, library untouched.
- **Save As Library** (`saveLibraryAs`): `saveFileDialog(... , "library.osslib", prefs_.assetLibraryDir)`
  → `ensureExtension(path,"osslib")` → write `serializeLibrary(graph_.assets())`; set
  `currentLibraryPath_`.
- **Save Library** (`saveLibraryOrPrompt`): if unbound → `saveLibraryAs`; else write to
  `currentLibraryPath_`.
- **Save Project** flow: in `saveProjectAs` / `saveCurrentOrPrompt`, **before** writing the project: if
  `graph_.assets()` is non-empty and `currentLibraryPath_` is empty → call `saveLibraryAs` (prompt); if
  the user cancels that, abort the project save. Then `saveProjectToFile`:
  ```
  ProjectDoc d = captureProject(graph_);
  d.assetLibraryPath = currentLibraryPath_;
  write serializeProject(d);
  if (!currentLibraryPath_.empty()) write serializeLibrary(graph_.assets()) to currentLibraryPath_;  // keep in sync
  ```
- **Load Project** flow (`loadProjectFromFile`): parse + `restoreProject` (loads legacy embedded
  assets if any). Then, using the parsed `assetLibraryPath`:
  - non-empty + file loads → `parseLibrary` into `graph_.assets()`, `currentLibraryPath_ = path`.
  - non-empty + missing/unreadable → `currentLibraryPath_ = ""` (unbound), `libStatus_ = "library not
    found: <name>"`; the graph keeps whatever `restoreProject` left (empty for a model-C project).
  - empty (legacy or no library) → `currentLibraryPath_ = ""`.
  (`loadProjectFromFile` must surface `assetLibraryPath`; the Application calls `parseProject` +
  `restoreProject` directly rather than the `loadProject` convenience wrapper, so it can read the path
  and orchestrate the external file. Same for save via `captureProject`/`serializeProject`.)
- **Remap Directory**: opens the Assets-panel remap modal (below); on Apply calls
  `graph_.assets().remapPathPrefix(from, to)`. Persists on the next library/project save.

## 5. Preferences

`Preferences` gains `std::string projectsDir;` and `std::string assetLibraryDir;` (both `""` = no
default → dialog opens wherever the OS chooses). `serializePreferences` writes `projectsdir <path>` /
`assetlibdir <path>` (rest-of-line escaped, like the existing string fields); `parsePreferences`
reads them; absent lines → empty (old prefs files load unchanged; header unchanged).
`PreferencesPanel` gains a **Locations** section (its own tab or appended to an existing one) with two
rows: a read-only path display + a **Browse** button calling `pickFolderDialog`, writing the field and
persisting via the existing on-change save callback.

## 6. FileDialog

- Add `const std::string& defaultPath = ""` to `openFileDialog` and `saveFileDialog` (NFD's
  `defaultPath`; `""` keeps current behavior). Update the existing 3 call sites to pass `""` or the
  relevant pref.
- Add `std::string pickFolderDialog(const char* title, const std::string& defaultPath = "");` (NFD
  `NFD_PickFolder`). Returns the chosen directory or `""`.
- Wiring of defaults: **project** Load/Save/SaveAs → `prefs_.projectsDir`; **library** Open/SaveAs →
  `prefs_.assetLibraryDir`; per-asset media **Browse** + the remap **From/To** Browse →
  `prefs_.assetLibraryDir`.

## 7. Menu — `TransportBar`

`ProjectBarIO` gains `onLibOpen`, `onLibSave`, `onLibSaveAs`, `onLibRemap` (`std::function<void()>`).
`drawTransportBar` adds a left-anchored **Asset Library** menu (after **File**, before **View**):
*Open Asset Library… · (sep) · Save · Save As… · (sep) · Remap Directory…*, each gated on its callback
being set. The existing right-side `status` line also shows `libStatus_` (the Application can fold the
library status into the same `status` string, or a second line — keep it in the one status string for
simplicity).

## 8. Remap modal — `AssetsPanel`

The panel exposes a `bool* showRemap` (toggled by the menu callback) and draws a modal when set, with
two text fields **From** and **To**, each with a **Browse** (folder) button (default
`assetLibraryDir`), an **Apply** and a **Cancel**. **From** is pre-filled on open with the longest
common directory prefix of the current library's non-empty asset paths (a GL-free helper, e.g.
`commonDirPrefix(paths)`), so the typical "swap the base dir" case is one Browse + Apply. Apply calls
`remapPathPrefix` and reports the count; the modal closes. The panel needs the `AssetLibrary&` (it
already has it) and the default dir.

## Error / edge handling

- Bad/short library header or unreadable file → operation fails, current library untouched, status set.
- Missing referenced library on project load → unbound + warn (per decisions); no crash; the user can
  Open one. A subsequent project save with a now-non-empty library prompts a Save As (option A).
- Legacy `.oss` with embedded assets → loads them (unbound); first project save prompts a library Save As.
- `remapPathPrefix` with empty `from`, or no matching paths → returns 0, no change.
- Library path with spaces round-trips (`escape`/rest-of-line, like labels/paths). Same for the two
  preference dirs.
- New project / no library → empty library, unbound; status neutral.
- `pickFolderDialog`/dialog cancel → `""`, no change.

## Testing

- **`core_tests`** (GL-free): `serializeLibrary`→`parseLibrary` round-trips assets incl. tags, a tag
  with a space, and custom tag colors; a bad header returns false. `ProjectFile`: a project with an
  `assetlib` path round-trips the path and embeds **no** asset lines; an **old** project text with
  embedded asset lines still parses into `ProjectDoc.assets`/`tagColors`. `remapPathPrefix`: prefix
  match / non-match / count / empty-from. `commonDirPrefix` helper. `Preferences` round-trip carries
  `projectsDir`/`assetLibraryDir`, and an old prefs file (without them) parses to empty.
- **Update** the existing ProjectFile and `gl_smoke` asset round-trip checks (SL2/SL5/test_assets/
  test_project_file) so they assert the **reference** model (project no longer carries assets; assets
  travel via the library file) — keeping a legacy-parse case for backward compat.
- The new UI (menu, dialogs, remap modal, Locations prefs) isn't headlessly testable → build + a manual
  smoke check: add assets → Save As library → see `assetlib` in the saved `.oss` → reload → assets
  return; Remap a prefix; set the two pref dirs and confirm dialogs open there; load an old embedded
  project and confirm assets load + the save-prompts-for-library flow.

## Out of scope (YAGNI — flag to pull in)

Relative-to-project library paths; auto-creating a `.osslib` next to the project on first save; a
"recent libraries" list; merging two libraries; per-asset (non-prefix) path fixups; watching the
`.osslib` for external changes; a separate media-files preference dir distinct from the library dir;
remap undo (re-run with swapped args).

## Decided defaults (flag to change)

- Referenced model; project stores an **absolute** `assetlib` path; missing-on-load → unbound + warn.
- Save lifecycle = auto-save the bound library on project save; prompt a library Save As when the
  library is unbound and non-empty; legacy embedded projects load unbound.
- Remap = exact prefix swap across all assets; From pre-filled with the common directory prefix.
- Two prefs (`projectsDir`, `assetLibraryDir`); project dialogs default to the former; library dialogs,
  per-asset Browse, and remap Browse to the latter.
- New files: `.osslib` (header `oss-assetlib 1`) + `core/AssetLibraryFile`; project header unchanged.
- Menu name **Asset Library** with Open / Save / Save As / Remap Directory.
