# Asset tags — design

**Date:** 2026-06-28
**Status:** Approved (brainstorm)
**Branch:** `feat/asset-tags` (off `develop`)
**Builds on:** the Assets / media library (`core/AssetLibrary`, `ui/AssetsPanel`, `ProjectFile`).

## Goal

Tag media files in the Assets window. Each asset carries a list of tags; a per-tab toolbar lists
the current media type's tags as colored buttons; clicking tags filters the grid (match **any** —
OR; no tags selected shows all). A **Tags** column shows each row's tags as colored chips with an
add box. Tags share one namespace + color across the four tabs; the user can recolor a tag.

## Decisions (from brainstorm)

- **Filter:** OR — an asset shows if it has **at least one** selected tag; empty filter shows all.
- **Toolbar scope:** per-tab — the toolbar lists the tags used by the **currently-shown** media
  type (`tagsForType`). Tags are one shared namespace (a tag name = the same tag + color anywhere).
- **Tags column:** colored chips + an add box.
- **Recolor:** right-click a toolbar tag → a color-picker popup.

## Architecture (extends the three existing units)

| File | Change |
|---|---|
| `src/core/AssetLibrary.{h,cpp}` | `Asset` gains `tags`; library gains a `tagColors_` registry + tag ops; GL-free. |
| `src/core/ProjectFile.{h,cpp}` | persist asset tags (`atag` lines) + the tag-color registry (`tagcolor` lines). |
| `src/ui/AssetsPanel.{h,cpp}` | a **Tags** column (chips + add box) + a per-tab tag-filter toolbar; filter is panel state. |
| `tests/test_assets.cpp` | tag model + codec round-trip tests (`core_tests`). |
| `CLAUDE.md`, `README.md` | docs. |

## Data model — `AssetLibrary`

`Asset` gains `std::vector<std::string> tags;` (insertion order). The library gains a registry
`std::map<std::string, glm::vec4> tagColors_;` (RGBA, GL-free `glm`). New includes: `<map>`,
`<glm/vec4.hpp>` in the header; `core/ColorHsv.h` in the `.cpp` for the default color.

```cpp
struct Asset {
    int id; AssetType type; std::string label, path;
    std::vector<std::string> tags;                  // NEW — tag names on this asset
};

class AssetLibrary {
    // ... existing add/remove/find/setLabel/setPath/byType/all/clear/load ...

    void addTag(int id, const std::string& tag);     // append if absent; register a default
                                                     // color if the tag is new. No-op on a bad
                                                     // id, an empty tag, or a duplicate.
    void removeTag(int id, const std::string& tag);  // drop the tag from the asset; no-op if absent.

    // The distinct tags used by assets of `type`, sorted ascending. Drives the per-tab toolbar.
    std::vector<std::string> tagsForType(AssetType type) const;

    glm::vec4 tagColor(const std::string& tag) const;            // registered color, else default-from-name
    void      setTagColor(const std::string& tag, glm::vec4 c);  // registers/updates the color

    const std::map<std::string, glm::vec4>& tagColors() const;   // the registry (for capture)
    void loadTagColors(std::map<std::string, glm::vec4> colors); // adopt (for restore)
private:
    std::vector<Asset> assets_;
    int nextId_ = 1;
    std::map<std::string, glm::vec4> tagColors_;     // NEW
};
```

- **Default color** (GL-free, deterministic): `defaultTagColor(name)` = `glm::vec4(hsvToRgb(hue,
  0.55f, 0.95f), 1.0f)` where `hue = (std::hash<std::string>{}(name) % 1000) / 1000.0f`. Same name →
  same color every run, distinct-ish across names, and the user can override. `tagColor(name)`
  returns the registry entry if present, else `defaultTagColor(name)` (so even an unregistered tag
  renders consistently).
- `addTag` registers `tagColors_[tag] = defaultTagColor(tag)` only when the tag is new to the
  registry (so it never clobbers a user-set color). A tag keeps its registry color even after its
  last use is removed (re-adding keeps it).
- `clear()` also clears `tagColors_`. `load(assets)` adopts the assets (with their tags);
  `loadTagColors(map)` adopts the registry separately (capture/restore call both).

## Persistence (`.oss`, additive — old projects load unchanged)

`serializeProject`, in the existing asset loop, after the `apath` line, emits one line per tag:

```
atag <escaped-tag>
```

(rest-of-line via the existing `escape`, so tags may contain spaces). After the asset loop, the
registry:

```
tagcolor <r> <g> <b> <a> <escaped-name>
```

(four floats first as fixed tokens, then the name as the escaped rest-of-line, so names may contain
spaces). `parseProject` handles `atag` (`curAsset->tags.push_back(unescape(restOfLine))`) and
`tagcolor` (read 4 floats, then `out.tagColors[unescape(restOfLine)] = vec4`); a malformed line is
skipped, not fatal — matching the codec. The header stays `oss-project 1`.

`ProjectDoc` gains `std::map<std::string, glm::vec4> tagColors;` (the asset tags live inside its
`Asset`s). `captureProject`: `d.tagColors = g.assets().tagColors();` (the `Asset`s already carry
tags via `all()`). `restoreProject`: after `g.assets().load(d.assets)`, call
`g.assets().loadTagColors(d.tagColors)`. `ProjectFile.h` includes `<map>` (glm + `Asset` come via
the existing `core/AssetLibrary.h` include).

## UI — `AssetsPanel`

`AssetsPanel` gains per-type filter state (transient, not persisted). It now includes
`core/AssetLibrary.h` (for `AssetType`/`kAssetTypeCount`) instead of forward-declaring:

```cpp
class AssetsPanel {
public:
    void draw(AssetLibrary& lib, bool* open);
private:
    std::set<std::string> filter_[kAssetTypeCount];   // selected tags per media tab (OR filter)
};
```

`drawTab` gains the filter set for its type (`draw` passes `filter_[(int)type]`):

- **Tag-filter toolbar** (top of the tab, before the table): for each `lib.tagsForType(type)`, a
  button tinted with `lib.tagColor(tag)` and labelled with the tag. **Left-click toggles** the tag
  in the filter set; a selected tag is drawn pressed (e.g. a border / brighter). **Right-click**
  opens a per-tag color-picker popup (`ImGui::ColorPicker4` bound to a local `vec4` re-read from
  `tagColor` each frame; on change → `setTagColor`). A "clear filter" affordance appears when the
  set is non-empty (clicking a selected tag again also clears it). When the type has no tags, the
  toolbar row is omitted.
- **Tags column** (the table grows to 4 columns: Label · Path · **Tags** · `✕`): each tag in
  `a->tags` renders as a small chip tinted with `tagColor` with an `✕` to remove it
  (`removeTag`, deferred to after the chip loop to avoid mutating the tags vector mid-iteration); a
  short `+ add…` `InputText` at the end commits a new tag on Enter (`addTag`, then clears the box).
  Tag edits are safe inside the row loop — they mutate an asset's own `tags` (and possibly the
  `tagColors_` map), never the `assets_` vector, so the `byType()` pointers stay valid (the existing
  asset add/remove stay deferred to after `EndTable`).
- **Filtering:** skip an asset whose tags don't intersect the filter set (OR); an empty filter
  shows everything. Applied in the `byType` row loop.

ImGui popups/buttons get scoped ids (`PushID(tag)` per toolbar tag and `PushID(id)` per row already)
so duplicate labels don't collide.

## Error / edge handling

- `addTag` ignores an empty tag, a duplicate on the same asset, and a bad id.
- `removeTag` of an absent tag/id is a no-op; the tag's registry color lingers harmlessly.
- Tags and tag names with internal/trailing spaces round-trip (escaped rest-of-line); a leading
  space is trimmed (the codec's `restOfLine` convention, as for labels/paths).
- A malformed `atag`/`tagcolor` line on load is skipped; an old project (no tag lines) loads with
  empty tag lists + an empty registry, and every tag still renders via `defaultTagColor`.
- A tag used only by another media type does not appear in this tab's toolbar (`tagsForType`).

## Testing

- **`core_tests`** (GL-free): `addTag` appends + dedups + ignores empty/bad-id; `removeTag`;
  `tagsForType` returns distinct, sorted tags and excludes other types' tags; `tagColor` is a stable
  default for an unregistered tag and reflects `setTagColor`; `clear` empties tags + colors; `load` /
  `loadTagColors` round-trip. A `ProjectFile` round-trip: assets with tags (incl. a tag with a
  space) + a custom tag color survive `serializeProject` → `parseProject` with everything intact,
  and an old project text without tag lines parses to empty tags.
- The panel UI (chips, toolbar toggles, color-picker) is not headlessly testable → build + a manual
  smoke check (tag a row, toggle filters, recolor a tag, save/load), as with the rest of the Assets
  window.

## Out of scope (YAGNI)

Renaming a tag across all assets at once; bulk tag/untag of multiple rows; a global (cross-tab) tag
view; AND filtering; drag-to-tag; tag-name autocomplete; deleting an unused tag from the registry.

## Decided defaults (flag to change)

- OR filter; per-tab toolbar (`tagsForType`); colored chips + add box; right-click to recolor.
- Default tag color = deterministic hue (hash of name) via `core/ColorHsv.h`, sat 0.55 / val 0.95.
- Tags are a shared namespace across the four media types; colors persist in the registry.
- Filter selection is transient (not saved in `.oss`); tags + colors are saved.
- New `.oss` lines: `atag` (per asset tag) + `tagcolor` (registry); format header unchanged.
