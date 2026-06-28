# Asset dropdowns for media controls — design (Phase 2)

**Date:** 2026-06-27
**Status:** Approved (brainstorm)
**Branch:** `feat/asset-dropdowns` (off `develop`)
**Builds on:** Phase 1 — the Assets / media library (`docs/superpowers/specs/2026-06-27-assets-media-library-design.md`)

## Goal

Let a node's media `file` control pick from the project's Assets library instead of only
typing a path: an asset-backed `String` input shows the existing editable text field **plus**
a small **▾** picker button that opens a dropdown of the library's assets of that media type;
selecting one **copies its path** into the field.

This is **Phase 2** of two. Per the approved brainstorm, the binding model is **copy-path**
(not a live id reference): picking an asset copies its `path` string into the node's existing
`String` input. There is therefore **no live link** — re-pointing an asset later does not update
nodes that already picked it — and, deliberately, **no node, evaluation, or `.oss` format
change**. The library simply becomes a convenient picker for the path that nodes already consume.

## Scope — the five media inputs

Exactly these `String` `file` inputs become asset-backed, each mapped to one `AssetType`:

| Node | Input(s) | `AssetType` |
|---|---|---|
| Audio Player (`AudioPlayerNode`) | `file` | `Audio` |
| Video Player (`VideoPlayerNode`) | `file` | `Video` |
| Mesh Loader (`MeshLoaderNode`) | `file` | `Mesh` |
| MIDI File (`MidiFilePlayerNode`) | `file` | `Midi` |
| Drum Machine (`DrumMachineNode`) | `file 0`…`file 3` | `Audio` |

**Not** asset-backed (out of scope): the Recorder's `file` (an *output* destination, not an
existing asset), Text 2D/3D `font` (no font asset type), and Text `text` (not a file).

## Architecture (no node / eval / persistence changes)

| File | Change |
|---|---|
| `core/Port.h` | + `bool assetBacked = false;` + `AssetType assetType = AssetType::Audio;` (a `String`-input flag, the way `choices` flags a `Float`). Include `core/AssetLibrary.h` for `AssetType`. |
| `core/Node.h` | + `addAssetInput(name, AssetType, default="")` — builds a `String` input with `assetBacked = true` + `assetType` set (mirrors `addChoiceInput`). |
| `src/modules/AudioPlayerNode.cpp` | `file` input: `addInput(...String...)` → `addAssetInput("file", AssetType::Audio)` |
| `src/modules/VideoPlayerNode.cpp` | `file` → `addAssetInput("file", AssetType::Video)` |
| `src/modules/MeshLoaderNode.cpp` | `file` → `addAssetInput("file", AssetType::Mesh)` |
| `src/modules/MidiFilePlayerNode.h` | `file` → `addAssetInput("file", AssetType::Midi)` |
| `src/modules/DrumMachineNode.h` | the 4 voice loop: `addInput("file " + s, …)` → `addAssetInput("file " + s, AssetType::Audio)` |
| `src/ui/PortWidgets.cpp` | an asset-backed `String` renders the editable text field **+** a `▾` picker button; clicking the button signals the deferred popup (returns the existing `popupClicked`). |
| `src/ui/NodeEditorPanel.cpp` | the deferred `NodePopup` gains an asset case: list `graph.assets().byType(port.assetType)` by label; selecting writes that asset's `path` into the input's `String` default. |
| `tests/`, `CLAUDE.md`, `README.md` | unit test + docs (below). |

`addAssetInput` is an inline method in `Node.h` (like `addChoiceInput`), so there is **no
`Node.cpp` change**.

`AssetType` lives in `core/AssetLibrary.h` (GL-free, only `<string>`/`<vector>`). Including it in
`Port.h` is safe (no cycle: `AssetLibrary` does not include `Port`). `Node.h` includes `Port.h`,
so the five node files see `AssetType` through `Node.h`.

### `Node::addAssetInput` (mirrors `addChoiceInput`)

```cpp
void addAssetInput(std::string n, AssetType type, std::string def = "") {
    Port p;
    p.name        = std::move(n);
    p.direction   = Direction::Input;
    p.type        = PortType::String;
    p.defaultValue = Value(std::move(def));
    p.assetBacked = true;
    p.assetType   = type;
    inputs_.push_back(std::move(p));
}
```

## Editor rendering — reuse the deferred popup

A popup opened *inside* a node renders in canvas space (off-screen), which is why choice-ports
and the colour picker defer their popup to the editor's `Suspend`/`Resume` block via the existing
`pendingPopup*` / `NodePopup` machinery in `NodeEditorPanel`. The asset picker reuses that path:

- **`PortWidgets::drawInlineInputWidget`** — for a `String` port with `assetBacked`, draw the
  editable `InputText` (narrowed) so the actual path stays visible/typeable, then `SameLine()` a
  small **`▾`** button. Clicking `▾` returns `popupClicked = true` (the same signal the choice /
  colour buttons return), so `NodeEditorPanel` records the pending popup. A non-asset `String`
  keeps today's plain `InputText`.
- **`NodeEditorPanel` `NodePopup`** — dispatch gains an explicit asset branch (the popup already
  has `graph`, so `graph.assets()` is in reach):
  - `port.type == Colour` → colour picker (unchanged)
  - `port.type == String && port.assetBacked` → list `graph.assets().byType(port.assetType)`;
    each `Selectable(asset.label)` sets the input's `String` default to `asset.path` and closes.
    An empty list shows a disabled hint: `"No <Type> assets — add them in the Assets window"`.
  - else (`Float` with `choices`) → choice labels (unchanged)

The text field remains authoritative and free-text, so typing a raw path still works, and the
picker is purely additive.

## Data flow

The picker writes a path into the node's `String` input default — identical to the user typing it.
Nodes read that path exactly as before (`ctx.in<std::string>(filePort)` / their async loaders).
On save, the path persists through the existing `ins <port> <path>` line; on load it restores as a
plain string. The `assetBacked` flag is a static port property re-established by the node's
constructor, never serialized.

## What does NOT change

- No `EvalContext` / `Graph::evaluate` change; no new store; **no `.oss` format change**.
- Old projects load identically (their typed `file` paths remain, the field is unchanged).
- Connected `file` inputs (an edge) show no inline widget — and thus no picker — as today.

## Error / edge handling

- No assets of the port's type → popup shows the disabled hint; the text field still accepts a
  typed path.
- Selecting an asset with an empty `path` copies `""` (clears the field) — the user picked an
  empty asset; acceptable and non-fatal.
- A picked path that doesn't exist on disk → the consuming node reports its own load error, as
  today (Phase 2 does not validate paths).
- The library is `graph.assets()`; the popup reads it live each frame, so newly added assets
  appear without reopening the node.

## Testing

- **`core_tests`** (GL-free): a minimal test `Node` subclass calls `addAssetInput("f",
  AssetType::Video)` and asserts the resulting port has `assetBacked == true`,
  `assetType == AssetType::Video`, `type == PortType::String`, and the given default. Confirms the
  core hook without needing the GL/FFmpeg node classes (which `core_tests` doesn't link).
- **`gl_smoke`** (links the real nodes): construct each of the five node types and assert their
  `file` port(s) are `assetBacked` with the expected `AssetType` (e.g. Audio Player→`Audio`, Mesh
  Loader→`Mesh`, all four Drum Machine voices→`Audio`). This is a cheap construct-and-inspect check
  — no rendering. (If `MidiFilePlayerNode` is not already compiled into `gl_smoke`, add its
  header include for the construction check.)
- The dropdown UI itself is not headlessly testable (same as the Phase 1 Assets panel) — verified
  by build + a manual smoke check: open an Audio Player, click `▾`, pick a library asset, confirm
  the path field fills.

## Out of scope (YAGNI)

- A **live id binding** (re-point an asset → nodes update). We chose copy-path; the stable ids
  remain available if a future phase wants live binding.
- A font asset type / making the Recorder output path or Text `font` asset-backed.
- Reverse-matching a path back to its asset to show "which asset is selected" on the button.
- Adding a new asset to the library from inside the dropdown; multi-select; path validation.

## Decided defaults (flag to change)

- Binding model: **copy-path** (selecting copies the asset's `path` into the existing field).
- Inline widget: **editable text field + a `▾` picker button** (not a label-only dropdown), so the
  path stays visible/typeable and old projects render unchanged.
- Scope: the **five media-input nodes** only; Recorder/font/Text excluded.
- Picker lists assets by **label**; empty type → a disabled "add them in the Assets window" hint.
