# Native Save/Load dialogs for projects — design

**Date:** 2026-06-28
**Status:** Approved (brainstorm)
**Branch:** `feat/native-project-dialogs` (off `develop`)
**Builds on:** the NFD `ui/FileDialog` added in the Assets library Phase 1.

## Goal

Replace the toolbar's editable `project.oss` filename field + Save/Load with native OS file
dialogs: **Load** opens a native open dialog, **Save As…** opens a native save dialog, and **Save**
writes to the current file (or, when the project is untitled, opens Save As…). The app starts
**untitled** — the first Save prompts.

## Approach (decided in brainstorm)

- The filename text field is **removed**. The current file is implicit (the dialogs name it; the
  status line reports `saved <name>` / `loaded <name>`).
- **Save** writes to the current path silently; **Save As…** always prompts. A project starts
  untitled (no current path), so its first **Save** routes to Save As….

## Architecture (three small units)

| File | Change |
|---|---|
| `src/ui/FileDialog.{h,cpp}` | + `saveFileDialog(...)` (NFD_SaveDialog); generalize both functions to take a **filter display-name** so the dialog reads "Project (\*.oss)" not "Media". |
| `src/app/Application.{h,cpp}` | replace the `filename_[256]` buffer with `std::string currentPath_` (empty = untitled); wire three callbacks — Save / Save As… / Load — with the untitled branching, basename, and `.oss`-extension handling. |
| `src/ui/TransportBar.{h,cpp}` | drop the filename `InputText`; draw **Save · Save As… · Load**; `ProjectBarIO` loses `filename`/`filenameLen`, gains `onSaveAs`. |

`FileDialog` stays the single home for the native dialog (NFD confined to the `.cpp`); `Application`
owns the project-path state and the Save/Save-As/Load policy; `TransportBar` just renders buttons and
forwards clicks. No new file is needed.

## FileDialog API

```cpp
// Native open-file dialog. filterName is the label shown for the filter (e.g. "Project");
// filters are bare extensions ("oss"). Returns the chosen path, or "" on cancel/error.
std::string openFileDialog(const char* title, const char* filterName,
                           const std::vector<std::string>& filters);

// Native save-file dialog (NFD_SaveDialog). defaultName seeds the filename field
// (e.g. "project.oss"). Returns the chosen path, or "" on cancel/error.
std::string saveFileDialog(const char* title, const char* filterName,
                           const std::vector<std::string>& filters,
                           const std::string& defaultName);
```

- Both build one `nfdfilteritem_t{ filterName, "ext1,ext2,…" }` (empty filters → no filter).
- `saveFileDialog` calls `NFD_SaveDialogU8(&out, list, count, /*defaultPath*/ nullptr, defaultName.c_str())`.
- The lone existing `openFileDialog` caller — `AssetsPanel.cpp:54` — is updated to pass a filter
  name (`"Media"`): `openFileDialog(noun, "Media", filters)`. No behavior change there.

## Application — state + behavior

`std::string currentPath_;` (empty = **untitled**) replaces `char filename_[256]`. The app starts
untitled. `projectStatus_` is unchanged. A small static helper `baseName(path)` (filename only) is
used for status text, and the Save-As result is normalized so it ends in `.oss`.

- **Load** → `openFileDialog("Load Project", "Project", {"oss"})`. If non-empty:
  `loadProjectFromFile(path)`; on success `currentPath_ = path`, status `"loaded " + baseName(path)`;
  on failure status `"load failed"`. Cancel (empty) → no-op.
- **Save As…** → `saveFileDialog("Save Project", "Project", {"oss"}, defaultName)` where
  `defaultName = currentPath_.empty() ? "project.oss" : baseName(currentPath_)`. If non-empty:
  append `.oss` if the path doesn't already end with it; `saveProjectToFile(path)`; on success
  `currentPath_ = path`, status `"saved " + baseName(path)`; on failure `"save failed"`. Cancel → no-op.
- **Save** → if `currentPath_.empty()` (untitled), do exactly Save As…; otherwise
  `saveProjectToFile(currentPath_)`, status `"saved " + baseName(currentPath_)` (or `"save failed"`).

The three are passed to the toolbar as `io.onSave`, `io.onSaveAs`, `io.onLoad`. `io.status =
projectStatus_` as today.

## Toolbar

`ProjectBarIO` drops `char* filename` + `std::size_t filenameLen`; adds `std::function<void()> onSaveAs`.
The `if (io)` block replaces the `InputText` + Save/Load with three buttons (a `Separator` first, as
now): **Save**, **Save As…**, **Load** — each invoking its callback if set — then the `status` text.
The Preferences/Assets toggles remain in the **View** menu (unchanged).

## What does *not* change

The `.oss` format and `ProjectFile` save/load are untouched — only *how the path is chosen* changes.
`saveProjectToFile`/`loadProjectFromFile` keep their `const std::string&` signatures. Preferences
(`preferences.oss`, the Prefs window) are unaffected.

## Error / edge handling

- Dialog cancelled (`""` returned) → no-op; status unchanged (no spurious "failed").
- Save-As path missing the `.oss` extension → appended before saving (case-insensitive check on the
  trailing `.oss`).
- Untitled **Save** → routed to Save As… (prompts).
- `saveProjectToFile` / `loadProjectFromFile` returning false → `"save failed"` / `"load failed"`.
- Loading a malformed/incompatible file → `loadProjectFromFile` already returns false (status
  reflects it); no crash. The native dialog briefly pauses rendering while open — expected for a
  user-triggered action.

## Testing

UI-only (a native dialog + button wiring can't run headlessly), so verified by **build + a manual
smoke check** — the same call made for the Assets panel and the media picker:
1. Launch; status empty (untitled). **Save** → native save dialog suggesting `project.oss`; choose a
   name/location → status `saved <name>`; the file exists on disk.
2. **Save** again → writes silently to the same file (no dialog), status `saved <name>`.
3. **Save As…** → dialog with the current name pre-filled; save to a second file → `currentPath_`
   moves to it.
4. **Load** → open dialog filtered to `.oss`; pick the saved file → graph restores, status
   `loaded <name>`. Cancel any dialog → nothing changes.

The underlying `saveProjectToFile` → `loadProjectFromFile` → `ProjectFile` round-trip is already
covered by `core_tests` (`test_project_file.cpp`, `test_assets.cpp`); this change does not alter it.

## Out of scope (YAGNI)

A recent-files menu; auto-save; a modified/"unsaved changes" dirty indicator or save-on-quit prompt;
remembering the last directory across app restarts (NFD already remembers within a session on most
platforms); a keyboard shortcut for Save (could be added later).

## Decided defaults (flag to change)

- Filename field **removed**; current file is implicit + shown via the status line.
- **Save** = write current (prompt if untitled); **Save As…** = always prompt; **Load** = open dialog.
- App starts **untitled**; first Save suggests `project.oss`.
- Save-As normalizes the path to end in `.oss`.
- Filter shown as **"Project" (\*.oss)**.
