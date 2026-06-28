# Multi-select + broadcast tagging in the Assets grid — design

**Date:** 2026-06-28
**Status:** Approved (brainstorm)
**Branch:** `feat/asset-multiselect-tag` (off `develop`)
**Builds on:** the asset-tags feature (`ui/AssetsPanel` Tags column + `AssetLibrary::addTag`/`removeTag`).

## Goal

Select multiple rows in the Assets grid (per tab) and tag/untag them all at once: adding a tag
(type + Enter in the add-box) **or** removing one (chip ×) on a *selected* row applies the change to
**every selected row**. Selection is by **Ctrl/Cmd-click** on a row.

## Decisions (from brainstorm)

- **Select** rows by **Ctrl/Cmd-click** (toggle one) and **Shift-click** (select the range from the
  anchor to the clicked row) — not a checkbox column.
- **Both** adding and removing a tag broadcast to the whole selection.
- The interaction uses **hold-modifier = select mode** (below), so "Ctrl/Cmd/Shift-click anywhere on
  a row" works despite the row's editable fields.

## Architecture

Entirely in `ui/AssetsPanel` — no core, persistence, or `.oss` change (it reuses `addTag`/`removeTag`).
The panel gains one member: a per-tab selection set.

```cpp
// AssetsPanel.h (private)
std::set<int> selected_[kAssetTypeCount];   // selected asset ids per media tab (transient)
int           anchor_[kAssetTypeCount] = {-1, -1, -1, -1};   // last-clicked row id (Shift-range pivot)
```

`<set>` is already included. Selection + anchor are transient view state (never saved), like `filter_`.

## Selection model + interaction

The rows are full of editable widgets (Label/Path inputs, the add-box, chips), so a plain click
can't both edit a field and select the row. Resolution — **hold-modifier select mode**:

- `const bool selecting = io.KeyCtrl || io.KeySuper || io.KeyShift;` (Ctrl, Cmd, **or** Shift).
- **While `selecting`** (a modifier is held), each row renders as a **read-only selection row**: a
  full-width `ImGui::Selectable(label, sel, ImGuiSelectableFlags_SpanAllColumns)` in column 0
  (showing the asset's label, or its path / `(unnamed)` when blank), and the Path + Tags columns
  shown as read-only text (tags as small colored text via `tagColor`). Clicking anywhere on the row
  acts on `selected_[type]` per the modifier (below). This makes "click anywhere on the row" work
  because no editable widget is present to capture the click.
- **Modifier behavior** on a row click (id `Y`), with `anchor_[type]` = the last-clicked row id:
  - **Shift** + a valid anchor → select the inclusive range from the anchor to `Y` in *display order*
    (**add** to the selection, never deselect); then `anchor_[type] = Y`.
  - otherwise (**Ctrl/Cmd**, or Shift with no anchor) → toggle `Y` in `selected_[type]`; `anchor_[type] = Y`.
- **Display order for the range:** the row loop first builds the **filtered, ordered** visible list
  once — `std::vector<const Asset*> rows` = `lib.byType(type)` minus the tag-filtered-out rows — and
  iterates *that* for rendering. A Shift-click resolves the anchor's and `Y`'s indices in `rows` and
  selects every id in `[lo, hi]`. So the range spans exactly the rows the user currently sees (the
  active tag filter is respected).
- **While not `selecting`** (normal), rows render exactly as today (editable Label/Path/Browse,
  Tags chips + add-box, remove-×) — plus the broadcast behavior below.
- **Selected rows are highlighted** in *both* modes: `ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
  <tint>)` for rows whose id is in `selected_[type]`, so you can see the selection while typing the
  tag after releasing the modifier.
- Above the table, when `!selected_[type].empty()`, a line shows **"N selected"** + a **Clear**
  button (`selected_[type].clear()`).

So the flow is: Ctrl/Cmd-click (toggle) and/or Shift-click (range) rows to build a selection (release
the modifier — the selection persists, highlighted) → type a tag in any selected row's add-box → it's
applied to all selected.

## Broadcast tagging (normal/edit mode)

In the editable row, when committing a tag add or a chip removal, broadcast if the row is part of a
non-empty selection:

- **Add** (add-box Enter): `if (selected_[type].count(id)) for (int sid : selected_[type]) lib.addTag(sid, tag); else lib.addTag(id, tag);`
- **Remove** (chip ×): `if (selected_[type].count(id)) for (int sid : selected_[type]) lib.removeTag(sid, tagName); else lib.removeTag(id, tagName);`

`addTag` already dedups and `removeTag` is a no-op on a row that lacks the tag, so broadcasting is
safe across a heterogeneous selection. A row that isn't selected behaves exactly as today
(single-row).

## Error / edge handling

- Removing an asset (the ✕ in the actions column) also prunes it from `selected_[type]` (and the
  existing `addText_`), and resets `anchor_[type]` to `-1` if it pointed at the removed id. Stale
  selected ids that somehow remain just don't render and are harmless.
- A Shift-click whose anchor row was filtered out / removed (anchor not found in `rows`) falls back
  to a plain toggle of the clicked row.
- An empty selection (or a not-selected row) → single-row add/remove, today's behavior.
- Selection is per media type; switching tabs keeps each tab's selection. It is **not** persisted, so
  loading/clearing a project leaves a stale set that simply highlights/affects nothing in the new
  project until re-selected — acceptable transient-state behavior (consistent with `filter_`).
- The selection-mode rows are read-only by design; you cannot edit a field while a modifier is held
  (release it to edit).

## Testing

UI-only (modifier-driven select mode + ImGui table can't run headlessly), so verified by build + a
manual smoke check: Ctrl/Cmd-click several rows (they highlight, "N selected" shows), release the
modifier, type a tag in one selected row → all selected rows gain the tag; remove a chip on a
selected row → all lose it; the ✕ on a row drops it from the selection; an unselected row tags only
itself. `AssetLibrary::addTag`/`removeTag` are already unit-tested.

## Out of scope (YAGNI)

A checkbox column; select-all; "invert selection"; a Shift-click that *replaces* the prior range
(ours only adds); persisting the selection; broadcasting label/path edits or the Browse pick; a
separate "bulk tag" dialog; cross-tab selection.

## Decided defaults (flag to change)

- Modifier = **Ctrl, Cmd, or Shift**; hold-modifier switches the grid to read-only select mode.
  Ctrl/Cmd-click toggles one row; Shift-click adds the inclusive range from the anchor (in display
  order); both update the anchor.
- Selected rows tinted via `TableSetBgColor`; an "N selected · Clear" line above the table.
- Add **and** remove broadcast to the selection; unselected rows act single-row.
- Selection is per-tab and transient (not saved).
