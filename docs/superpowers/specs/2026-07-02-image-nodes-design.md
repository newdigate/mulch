# Image assets + Image Streamer + Kaleidoscope — design

**Date:** 2026-07-02
**Status:** Approved (brainstorm)
**Branch:** `feat/image-nodes` (off `develop`)

## Goal

Three related additions, all texture-oriented:

1. An **Image** section (fifth tab) in the Assets library, so image files are first-class
   project media alongside Audio / Video / MIDI / 3D.
2. An **Image Streamer** node that loads one still image and publishes it as a texture.
3. A **Kaleidoscope** node that folds an input texture into a mirrored kaleidoscopic
   pattern.

## Decisions (from brainstorm)

- **Image Streamer = single static image.** It loads one image file (from an Image asset
  field) and republishes it as a `TexRef`. It has no animation of its own — you animate it
  downstream (Kaleidoscope, World Transform, Compositor, LFO-driven params). No sequence
  player, no multi-slot cross-fader (YAGNI).
- **`AssetType::Image` is appended (enum value 4).** Existing persisted type ints `0..3`
  are unchanged and the asset-block codec already clamps unknown type ints, so the
  `.oss` / `.osslib` file formats need **no change** and old projects load untouched.
- **Synchronous image load.** Loading happens once, only when the file path changes (a rare
  user action, never per-frame), so no `AsyncLoader` is warranted — unlike Video/Mesh which
  decode continuously or slowly. `stb_image` decodes a still in milliseconds.
- **Kaleidoscope is a `ShaderNode`.** Same shape as the Compositor: texture-in → texture-out,
  parameters as Float/int input ports (so any of them — notably `rotation` — can be wired to
  an LFO or Automation for animation). No dedicated "spin" control.
- **No new third-party dependency.** `stb_image.h` already ships inside the `stb`
  FetchContent dep (currently used for `stb_truetype` in the Text nodes).

## Architecture

| File | Change |
|---|---|
| `src/core/AssetLibrary.h` | Append `Image` to `AssetType`; bump `kAssetTypeCount` 4 → 5. |
| `src/ui/AssetsPanel.cpp` | Add an **Image** tab (after Video) via `drawTab(...)` with image extensions. |
| `src/ui/NodeEditorPanel.cpp` | Add the `AssetType::Image → "Image"` case to `assetTypeName()`. |
| `src/gfx/ImageLoader.h` / `.cpp` | **New, GL-free** `stb_image` wrapper (mirrors `VideoDecoder`): file → RGBA8 buffer + dims. `STB_IMAGE_IMPLEMENTATION` lives here. |
| `src/modules/ImageStreamerNode.h` | **New, header-only.** Image asset input → `TexRef` output; loads on path change, owns a GL texture. |
| `shaders/kaleidoscope.frag` | **New.** Polar fold / mirror math. |
| `src/modules/KaleidoscopeNode.h` | **New, header-only `ShaderNode`.** Texture-in → texture-out with fold params. |
| `src/app/Application.cpp` | Register `"Image Streamer"` and `"Kaleidoscope"` in `makeNode()` + the `"Texture"` category; add the includes. |
| `tests/test_asset_library_file.cpp` | `core_tests`: `AssetType::Image` round-trips the asset-block codec; `kAssetTypeCount == 5`. |
| `tests/test_image_loader.cpp` | `core_tests`: `ImageLoader` round-trip on a fixture written at runtime via `stb_image_write`. |
| `tests/gl_smoke/*` | Image Streamer renders a non-blank texture; Kaleidoscope output is symmetric across the fold. |
| `tests/CMakeLists.txt` | Register the new test sources; link `ImageLoader.cpp`. |
| `CLAUDE.md`, `README.md` | Document the new asset type + two nodes. |

## Component detail

### 1. Asset library — Image type

- `enum class AssetType { Audio, Video, Midi, Mesh, Image };` and
  `constexpr int kAssetTypeCount = 5;`. Because the value is appended, `(int)AssetType::Image
  == 4`; the codec's `if (typeInt >= kAssetTypeCount) typeInt = kAssetTypeCount - 1;` clamp
  keeps forward/backward compatibility.
- `AssetsPanel::draw` gains a `BeginTabItem("Image")` **after the Video tab**, calling
  `drawTab(lib, AssetType::Image, "image file", {"png","jpg","jpeg","bmp","tga","gif","hdr","psd"})`
  — the formats `stb_image` decodes. The per-tab state arrays (`filter_`, `selected_`,
  `anchor_`) are already sized by `kAssetTypeCount`, so they grow with no other change.
- `assetTypeName(AssetType::Image)` returns `"Image"` so the node ▾ asset-picker labels the
  Image group. The picker body already enumerates `graph.assets().byType(type)` generically.
- **Tab display order vs enum value:** the Image tab is shown *after Video* for UX, while its
  enum value stays 4 (appended, for file-format compat). Display order and enum value are
  independent — the panel lists tabs explicitly and indexes state by `(int)type`.

### 2. `gfx/ImageLoader` (GL-free)

Mirrors the `VideoDecoder` boundary (an external decoder confined to a `.cpp`, producing CPU
RGBA that a node uploads):

```cpp
// gfx/ImageLoader.h  — GL-free
namespace oss {
struct ImageData {
    std::vector<unsigned char> rgba;   // width*height*4, row-major
    int width = 0, height = 0;
    bool ok() const { return width > 0 && height > 0 && !rgba.empty(); }
};
// Decode `path` to RGBA8. Returns ok()==false and fills `err` on failure.
ImageData loadImage(const std::string& path, std::string& err);
}
```

`ImageLoader.cpp` defines `STB_IMAGE_IMPLEMENTATION`, calls
`stbi_set_flip_vertically_on_load(true)` and `stbi_load(..., 4)` (force RGBA), so the buffer
matches the app's existing **bottom-up** texture orientation (Video frames are bottom-up and
display upright, so images use the same convention → they render right-side-up through the
same shaders/output blit). GL stays out of this file.

### 3. `modules/ImageStreamerNode.h` (header-only)

- Ports: `addAssetInput("file", AssetType::Image)` (String, Image-backed) → `addOutput("image", PortType::Texture)`.
- On `evaluate`: if the path changed, call `loadImage`, (re)allocate/upload a GL texture using
  the Video Player's `ensureTexture` + `glTexSubImage2D` pattern, and cache dims. Every frame
  it republishes the stable `TexRef{ tex_, w_, h_ }` (or an empty `TexRef{}` when unloaded /
  load failed).
- `statusLine()` shows `"WxH"` on success or `"load failed: …"`. GL objects freed in the dtor
  with the editor context current (the node owns them), per the two-context rule.
- Registered in `makeNode()` and the `"Texture"` category of `nodeCategories()`.

### 4. `modules/KaleidoscopeNode.h` + `shaders/kaleidoscope.frag`

- Derives from `ShaderNode` exactly like `CompositorNode`. Inputs:
  - `image` — Texture
  - `segments` — int slider 2–32, default 6 (`addIntInput`)
  - `rotation` — Float, turns 0–1, default 0 (wireable to an LFO for spin)
  - `zoom` — Float 0.1–4, default 1
  - `center x` — Float 0–1, default 0.5
  - `center y` — Float 0–1, default 0.5
  - Output: `out` — Texture.
- `setUniforms` binds the input texture to unit 0 and sets `uSegments`, `uRotation`
  (× 2π), `uZoom`, `uCenter`.
- Shader math: `p = (uv − center)`; convert to polar `(r, θ)`; fold θ into one wedge of size
  `2π / segments` with a mirror (reflect the second half of each wedge → triangle wave), so
  adjacent wedges meet without a seam; add `uRotation`; scale `r` by `1/uZoom`; reconstruct a
  sample UV = `center + r·(cos, sin)`; sample the input with `GL_CLAMP_TO_EDGE`.

## Data flow

Unchanged evaluation model. `Image Streamer` is a pure texture *source* (no inputs but the
path); `Kaleidoscope` is a pure texture *transform*. Typical wiring:

```
Image Streamer ──image──▶ Kaleidoscope ──out──▶ Output
                                 ▲
                    LFO ──▶ rotation (spin)
```

## Error handling

- **Missing / undecodable image:** `loadImage` returns `ok()==false`; the node publishes an
  empty `TexRef{}` and shows the error in `statusLine()`. No crash, no per-frame retry (only
  re-attempts on the next path change).
- **Empty path:** treated as unloaded — empty `TexRef`, blank status.
- **Kaleidoscope with no input** (`TexRef{}` id 0): binding texture 0 samples black; output is
  black. Acceptable, matches Compositor's behavior with an unconnected input.

## Testing

- **`core_tests`**
  - Assert `kAssetTypeCount == 5` and that an `Asset{ …, AssetType::Image, … }` survives
    `appendAssetBlock` → `parseAssetBlockLine` (type int 4 preserved).
  - `ImageLoader` round-trip: the test writes a small known RGBA image to a temp file with
    `stbi_write_png` (defining `STB_IMAGE_WRITE_IMPLEMENTATION` in the test TU), loads it back
    with `loadImage`, and checks dims + a couple of pixels (accounting for the vertical flip).
- **`gl_smoke`** (needs a GL context; runs with the repo root as CWD)
  - **Image Streamer:** generate a fixture PNG, point the node at it, evaluate, read back its
    output texture, assert it is non-blank and the right dims.
  - **Kaleidoscope:** feed a known asymmetric input texture, set `segments = 6`, render, read
    back, and assert the output is symmetric across a fold boundary (a pixel and its mirror
    within a wedge match within tolerance) — proving the shader actually folds.

## Out of scope (YAGNI)

- Image sequence / animated-GIF playback, multi-image cross-fading, drag-and-drop import.
- Saving/exporting images (the Recorder already covers video/frame capture).
- Signing/bundling changes — these nodes ship inside the existing binary and shaders dir.
