# Collapsible folder tree for the Assets grid — design

**Date:** 2026-06-28
**Status:** Approved (brainstorm)
**Branch:** `feat/asset-folder-tree` (off `develop`)
**Builds on:** the Assets / media library (`core/AssetLibrary`, `ui/AssetsPanel`) including the
multi-select + broadcast-tagging feature (`selected_`/`anchor_`, the `rows` filter list).

## Goal

Turn each Assets tab's flat row list into a **collapsible folder tree**, grouping files by the
directory portion of their `path`. Folders are collapsible `TreeNodeEx` nodes; files are leaf nodes
that keep their editable Path / Tags / remove columns, the multi-select, and broadcast tagging.

## Decisions (from brainstorm)

- **Tree shape:** nest by folder segment **and collapse single-child chains** — a folder whose only
  child is one folder (and which holds no files of its own) merges with that child, so a long
  absolute prefix like `/Users/me/Development/audio` shows as one node, not a chain of near-empty
  ones.
- **Leaf row model — "standard tree row":** a file leaf's node text is its **display name** (label,
  else filename, else `(unnamed)`); the leaf node is the **selection target** (plain-click selects
  only it, Ctrl/Cmd-click toggles, Shift-click ranges over the visible leaves); **double-click the
  node renames** inline. The Path(+Browse), Tags (chips + add box) and remove (✕) columns stay
  editable exactly as today. This replaces the previous hold-a-modifier "select mode" with
  always-available node selection, which the dedicated tree click target now makes clean.
- **Folders are not selectable** — clicking a folder only expands/collapses it.
- **Broadcast tagging is preserved** — adding/removing a tag on a *selected* leaf applies to the
  whole selection (unchanged from the multi-select feature).

## Architecture

A new GL-free core utility builds the tree (data only, unit-tested); `ui/AssetsPanel` renders it.
No `core/AssetLibrary`, persistence, or `.oss` change — the tree is derived each frame from the
existing `path` field.

| File | Change |
|---|---|
| `src/core/AssetTree.h` | **New, header-only, GL-free.** `AssetTreeNode` + `buildAssetTree(rows)`. |
| `src/ui/AssetsPanel.{h,cpp}` | `drawTab` builds the tree from the filtered `rows` and renders it recursively; new `renamingId_` + rename buffer state. |
| `tests/test_asset_tree.cpp` | `core_tests`: grouping, single-child collapse, ungrouped, sort, empty. |
| `CLAUDE.md`, `README.md` | docs. |

## Tree model — `core/AssetTree.h` (GL-free, header-only)

```cpp
#pragma once
#include <string>
#include <vector>
#include "core/AssetLibrary.h"   // Asset

namespace oss {

// One node of the Assets folder tree. A node holds child folders and/or leaf files.
struct AssetTreeNode {
    std::string name;                       // folder segment(s); "" for the synthetic root
    std::vector<AssetTreeNode> folders;     // child folders, sorted ascending by name
    std::vector<const Asset*> files;        // leaf files in this folder, in input (byType) order
};

// Group `rows` into a folder tree by the directory portion of each asset's `path`.
// - Split path on '/' or '\\'; directory segments form the folder path, basename is the leaf.
// - Files whose path has no directory part (empty path, or a bare filename) become files of the
//   returned root node (rendered as top-level leaves).
// - Single-child chains collapse: a folder with exactly one child folder and no files of its own
//   merges into that child (name joined with '/').
// - Folders are sorted ascending; files keep their position in `rows`.
AssetTreeNode buildAssetTree(const std::vector<const Asset*>& rows);

} // namespace oss
```

**Algorithm.** (1) For each asset, split `path` into segments on `/`/`\`. The last segment is the
basename (the leaf); the preceding segments are the directory path. Drop empty segments produced by
leading, trailing, or doubled separators (so `/a//b/` → `a`,`b`). If there are no directory
segments, the file is appended to the root's `files`. (2) Walk/create the folder chain from the
root, appending the asset to the final folder's `files`. (3) Post-process the whole tree bottom-up,
sorting each node's `folders` ascending by `name` and **collapsing single-child chains**: while a
node has exactly one child folder and zero files, replace it with that child, joining the names as
`parent + "/" + child`. The synthetic root is never collapsed away (it may legitimately hold both
top-level folders and ungrouped files).

This is pure `std::vector`/`std::string` logic — unit-testable with no GL.

## Rendering — `AssetsPanel::drawTab`

Build `rows` exactly as today (`byType(type)` minus the tag-filtered-out assets), then
`buildAssetTree(rows)`, then render recursively inside the existing `BeginTable`/`EndTable`.

- **Table flags:** keep `SizingStretchProp | BordersInnerH`; add `ImGuiTableFlags_RowBg` so the
  selection tint and tree rows read cleanly. Columns are unchanged (**Label/Tree · Path · Tags ·
  ✕**) except column 0 now hosts the tree node instead of the Label editor.
- **A flattened visible-leaf list** is produced alongside rendering (a `std::vector<int>` of leaf
  ids in depth-first tree order) to drive Shift-range. It is built once per frame from the tree
  (DFS), independent of which folders are open, so a range covers the leaves in tree order. (Old
  behaviour ranged over the filtered `rows`; this preserves that, just reordered by the tree.)
- **Folder row:** `TreeNodeEx(name, ImGuiTreeNodeFlags_SpanFullWidth)` in column 0; columns 1–3 are
  left empty. Recurse into child folders, then render this folder's leaf files, then `TreePop()`.
  **Default-open:** root-level folders are **collapsed**, *unless there is exactly one root folder*,
  in which case it is opened (`ImGuiTreeNodeFlags_DefaultOpen` applied only to that single root).
  ImGui stores open/closed state keyed by the node id, so it survives the per-frame rebuild as long
  as ids are stable (they derive from the folder's joined path).
- **Leaf (file) row:** column 0 is
  `TreeNodeEx(displayName, ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen |
  (isSelected ? ImGuiTreeNodeFlags_Selected : 0))`. `displayName` is the asset's label, else its
  basename, else `(unnamed)`. The node deliberately stays in column 0 (no `SpanFullWidth`) so the
  editable widgets in columns 1–3 cleanly receive their own clicks — the column-0 node is the
  selection/rename click target. Then columns 1–3 render the **Path (InputText + Browse) · Tags
  (chips + add box) · ✕** exactly as today, including the broadcast tagging. `PushID(id)`/`PopID()`
  per leaf keeps ids unique. Because `NoTreePushOnOpen` is set, a leaf needs no matching `TreePop`.

### Selection (reuses `selected_` / `anchor_`)

On a leaf node click (`ImGui::IsItemClicked()` after the `TreeNodeEx`):
- **Shift** + a valid `anchor` (found in the flattened leaf list) → select the inclusive range from
  the anchor to this id in DFS leaf order (add to the selection); `anchor = id`.
- **Ctrl/Cmd** → toggle this id; `anchor = id`.
- **Plain click** → replace the selection with just this id; `anchor = id`.

Selected leaves are tinted via the existing `TableSetBgColor(ImGuiTableBgTarget_RowBg0, …)`. The
"N selected · Clear selection" status line above the table is kept. Removing an asset still prunes
it from `selected_`/`anchor_`.

### Rename (double-click a leaf node)

A new panel field `int renamingId_ = -1` (one rename at a time across all tabs) plus a small text
buffer. When a leaf node receives a double-click (`ImGui::IsMouseDoubleClicked` while the item is
hovered), set `renamingId_ = id` and seed the buffer from the current label. While
`renamingId_ == id`, column 0 renders an `InputText` (auto-focused) **instead of** the `TreeNodeEx`;
**Enter or focus-loss** commits `lib.setLabel(id, buf)` and clears `renamingId_`, **Escape** cancels.
Removing the renamed asset (or selecting another) clears `renamingId_`.

## Error / edge handling

- **Blank / empty-path assets** (e.g. a freshly-added row, or one whose path is a bare filename)
  become **root-level leaves**, so a new blank asset is always visible and immediately editable —
  it is never hidden inside a collapsed folder.
- **Tag filter** is applied before the tree is built (`rows` already excludes filtered-out assets),
  so filtered-out leaves and any folders left empty simply don't appear.
- **Anchor / rename lifecycle:** the flattened leaf list is rebuilt each frame; a Shift-click whose
  anchor isn't in the current leaves falls back to a plain single-select. Asset removal prunes
  `selected_`, resets `anchor_` to -1 if it pointed at the removed id, and clears `renamingId_` if
  it was renaming that id. (`addText_` pruning is unchanged.)
- **Stable open/close + ids:** node ids derive from the folder's joined path / the asset id, so
  collapse state and per-row widget ids stay stable across the per-frame rebuild.
- **Duplicate paths / names** are fine — leaves are keyed by asset id, not by name; two files in the
  same folder with the same name render as two distinct leaves.
- **Separators:** both `/` and `\\` split; empty segments from leading/trailing/doubled separators
  are dropped.

## Testing

- **`core_tests`** (`tests/test_asset_tree.cpp`, GL-free): `buildAssetTree` over a handful of
  `Asset`s verifies — files group under the right folder; nested folders nest; a single-child chain
  collapses to one joined node; ungrouped (empty / bare-filename) files land in the root's `files`;
  folders come out sorted ascending while files keep input order; an empty input yields an empty
  root; `/` and `\\` both split and redundant separators are ignored. (Build the `rows` vector from
  a local `std::vector<Asset>` and pass pointers — the tree only reads `path`.)
- **UI** (tree rendering, expand/collapse, node selection, double-click rename) can't run headlessly
  → build + a manual smoke check: add files in nested folders, expand/collapse, plain/Ctrl/Shift
  select across folders, tag a selection, rename a leaf, confirm a blank-path add shows at the root.

## Out of scope (YAGNI — flag to pull in)

Folder-level selection / bulk-tag-a-whole-folder; drag-and-drop to move files between folders;
persisting expand/collapse state in the `.oss`; remembering the selection across project load;
restricting Shift-range to only the currently-expanded (visible-on-screen) leaves; a flat-view
toggle; folder child counts.

## Decided defaults (flag to change)

- Tree = nested folders with single-child-chain collapsing; ungrouped files at the root.
- Leaf node text = label → filename → `(unnamed)`; double-click renames.
- Selection: plain-click = select-only, Ctrl/Cmd = toggle, Shift = range over DFS leaf order;
  folders not selectable; broadcast tagging preserved.
- Default-open: roots collapsed unless exactly one root folder (then opened); collapse state via
  ImGui id storage (not persisted).
- New table flag `RowBg`; no `core`/persistence/`.oss` change; one new GL-free `core/AssetTree.h`.
