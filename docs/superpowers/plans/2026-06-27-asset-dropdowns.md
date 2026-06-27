# Asset Dropdowns for Media Controls — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A node's media `file` control can pick a path from the Assets library via a ▾ dropdown that copies the chosen asset's path into the existing field.

**Architecture:** A `String` input is flagged "asset-backed" with an `AssetType` (`Port.assetBacked`/`assetType`, set by a new `Node::addAssetInput`). The editor renders such an input as the existing text field plus an `ImGui::ArrowButton` that opens the existing deferred node-popup, now listing `graph.assets().byType(type)`; selecting one copies its `path` into the field. **Copy-path model — no node `evaluate`, no `EvalContext`/`Graph`, and no `.oss` format change.**

**Tech Stack:** C++17, Dear ImGui, doctest (`core_tests`), the headless `gl_smoke` harness.

**Spec:** `docs/superpowers/specs/2026-06-27-asset-dropdowns-design.md`

---

## File Structure

| File | Responsibility |
|---|---|
| `src/core/Port.h` | **modify** — add `bool assetBacked` + `AssetType assetType`; include `core/AssetLibrary.h`. |
| `src/core/Node.h` | **modify** — add the inline `addAssetInput(name, AssetType, default)` helper. |
| `tests/test_assets.cpp` | **modify** — `core_tests` unit test for `addAssetInput` via a minimal probe `Node`. |
| `src/modules/AudioPlayerNode.cpp`, `VideoPlayerNode.cpp`, `MeshLoaderNode.cpp`, `MidiFilePlayerNode.h`, `DrumMachineNode.h` | **modify** — the `file` input(s) switch `addInput(...)` → `addAssetInput(...)`. |
| `tests/gl_smoke.cpp` | **modify** — construct the 5 node types, assert their `file` ports are asset-backed with the right type. |
| `CMakeLists.txt` | **modify** — add `src/core/MidiFile.cpp` to the `gl_smoke` target (so `MidiFilePlayerNode` links there). |
| `src/ui/PortWidgets.cpp` | **modify** — asset-backed `String` → text field + a ▾ `ArrowButton` that requests the picker popup. |
| `src/ui/NodeEditorPanel.cpp` | **modify** — the deferred `NodePopup` gains an asset branch listing `graph.assets().byType(type)`. |
| `CLAUDE.md`, `README.md` | **modify** — docs. |

---

## Task 1: `Port` asset flag + `Node::addAssetInput` + unit test

**Files:**
- Modify: `src/core/Port.h`, `src/core/Node.h`
- Test: `tests/test_assets.cpp`

- [ ] **Step 1: Write the failing test**

Append to `tests/test_assets.cpp`. First add `#include <variant>` near the top with the other includes (the others — `core/AssetLibrary.h`, `core/Graph.h`, `core/ProjectFile.h` — are already present; `core/Graph.h` transitively provides `Node`/`Port`/`EvalContext`). Then add a file-scope probe node and the test:

```cpp
namespace {
// A minimal Node that declares one asset-backed input, to exercise addAssetInput
// without needing the GL/FFmpeg node classes (which core_tests does not link).
struct AssetInputProbe : oss::Node {
    AssetInputProbe() : oss::Node("AssetInputProbe") {
        addAssetInput("clip", oss::AssetType::Video, "default.mp4");
    }
    void evaluate(oss::EvalContext&) override {}
};
} // namespace

TEST_CASE("addAssetInput marks a String input asset-backed with its type + default") {
    AssetInputProbe n;
    REQUIRE(n.inputs().size() == 1);
    const Port& p = n.inputs()[0];
    CHECK(p.name == "clip");
    CHECK(p.type == PortType::String);
    CHECK(p.assetBacked == true);
    CHECK(p.assetType == AssetType::Video);
    CHECK(std::get<std::string>(p.defaultValue) == "default.mp4");
}

TEST_CASE("a plain addInput String is not asset-backed") {
    // Sanity: the flag defaults off, so existing String inputs keep their text field.
    Port p;
    CHECK(p.assetBacked == false);
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build build -j --target core_tests`
Expected: FAIL — `'struct oss::Port' has no member named 'assetBacked'` and `'addAssetInput' was not declared`.

- [ ] **Step 3: Add the flag to `Port`**

In `src/core/Port.h`, add the include after `#include "core/Value.h"`:

```cpp
#include "core/AssetLibrary.h"
```

Add two fields to `struct Port`, after the `choices` vector:

```cpp
    // A String input marked asset-backed renders a library-picker dropdown (of this
    // AssetType) in the editor; picking copies the asset's path into this input.
    // Set via Node::addAssetInput. Defaults off, so ordinary String inputs are unaffected.
    bool      assetBacked = false;
    AssetType assetType   = AssetType::Audio;
```

- [ ] **Step 4: Add `addAssetInput` to `Node`**

In `src/core/Node.h`, add this inline method in the same `protected:` section as `addChoiceInput` (right after it):

```cpp
    void addAssetInput(std::string n, AssetType type, std::string def = std::string()) {
        Port p;
        p.name         = std::move(n);
        p.direction    = Direction::Input;
        p.type         = PortType::String;
        p.defaultValue = Value(std::move(def));
        p.assetBacked  = true;
        p.assetType    = type;
        inputs_.push_back(std::move(p));
    }
```

(`AssetType` is visible here: `Node.h` includes `Port.h`, which now includes `core/AssetLibrary.h`.)

- [ ] **Step 5: Run the test to verify it passes**

Run: `cmake --build build -j --target core_tests && ./build/core_tests -tc="*asset-backed*"`
Expected: PASS (2 cases). Then `ctest --test-dir build --output-on-failure -R core_tests` — all green.

- [ ] **Step 6: Commit**

```bash
git add src/core/Port.h src/core/Node.h tests/test_assets.cpp
git commit -m "feat(core): Port.assetBacked + Node::addAssetInput for media-picker inputs"
```

---

## Task 2: Five nodes adopt `addAssetInput` + gl_smoke construction check

**Files:**
- Modify: `src/modules/AudioPlayerNode.cpp`, `VideoPlayerNode.cpp`, `MeshLoaderNode.cpp`, `MidiFilePlayerNode.h`, `DrumMachineNode.h`
- Modify: `tests/gl_smoke.cpp`, `CMakeLists.txt`

TDD order: write the gl_smoke check first (it compiles against Task 1's flag but fails at runtime because the nodes aren't asset-backed yet), then flip the five inputs.

- [ ] **Step 1: Add `MidiFilePlayerNode` to gl_smoke + the construction check**

In `tests/gl_smoke.cpp`, add the include beside the other module includes (after `#include "modules/DrumMachineNode.h"`):

```cpp
#include "modules/MidiFilePlayerNode.h"
```

Add this block near the **top of `int main()`**, right after the GL window/context is created (the node constructors do not use GL — they only declare ports):

```cpp
    // Phase 2: the five media file inputs are asset-backed with the matching AssetType.
    {
        auto bad = [](const Node& n, int port, AssetType want) {
            if (port < 0 || port >= (int)n.inputs().size()) return true;
            const Port& p = n.inputs()[(std::size_t)port];
            return p.type != PortType::String || !p.assetBacked || p.assetType != want;
        };
        AudioPlayerNode    ap; if (bad(ap, 0, AssetType::Audio)) return fail("AudioPlayer.file not asset-backed Audio");
        VideoPlayerNode    vp; if (bad(vp, 0, AssetType::Video)) return fail("VideoPlayer.file not asset-backed Video");
        MeshLoaderNode     ml; if (bad(ml, 0, AssetType::Mesh))  return fail("MeshLoader.file not asset-backed Mesh");
        MidiFilePlayerNode mf; if (bad(mf, 0, AssetType::Midi))  return fail("MidiFile.file not asset-backed Midi");
        DrumMachineNode    dm;
        for (int v = 0; v < 4; ++v)
            if (bad(dm, 4 * v, AssetType::Audio)) return fail("DrumMachine.file voice not asset-backed Audio");
        std::fprintf(stderr, "gl_smoke OK: 5 media nodes expose asset-backed file inputs\n");
    }
```

(`file` is input 0 on Audio/Video/Mesh/MIDI; on Drum Machine the four voice files are at ports 0, 4, 8, 12.)

- [ ] **Step 2: Link `MidiFile.cpp` into gl_smoke**

In `CMakeLists.txt`, inside the `add_executable(gl_smoke ...)` source list, add (e.g. after `src/core/AssetLibrary.cpp`):

```cmake
  src/core/MidiFile.cpp
```

- [ ] **Step 3: Build + run gl_smoke to verify it fails**

Run: `cmake --build build -j --target gl_smoke && ./build/gl_smoke`
Expected: FAIL — `gl_smoke FAIL: AudioPlayer.file not asset-backed Audio` (the nodes still use plain `addInput`).

- [ ] **Step 4: Flip the five inputs to `addAssetInput`**

`src/modules/AudioPlayerNode.cpp` line 11 — change:
```cpp
    addInput("file", PortType::String, std::string(""));
```
to:
```cpp
    addAssetInput("file", AssetType::Audio);
```

`src/modules/VideoPlayerNode.cpp` line 11 — change:
```cpp
    addInput("file", PortType::String, std::string(""));   // .mp4/.mov/... path
```
to:
```cpp
    addAssetInput("file", AssetType::Video);   // .mp4/.mov/... path
```

`src/modules/MeshLoaderNode.cpp` line 11 — change:
```cpp
    addInput("file",  PortType::String, std::string(""));   // .obj / .gltf / .glb path
```
to:
```cpp
    addAssetInput("file", AssetType::Mesh);   // .obj / .gltf / .glb path
```

`src/modules/MidiFilePlayerNode.h` line 22 — change:
```cpp
        addInput("file",         PortType::String, std::string(""));
```
to:
```cpp
        addAssetInput("file",    AssetType::Midi);
```

`src/modules/DrumMachineNode.h` line 38 — change:
```cpp
            addInput("file " + s, PortType::String, std::string(""));
```
to:
```cpp
            addAssetInput("file " + s, AssetType::Audio);
```

(`AssetType` is visible in each: they include `core/Node.h`, which now reaches `core/AssetLibrary.h` via `Port.h`.)

- [ ] **Step 5: Build + run gl_smoke to verify it passes**

Run: `cmake --build build -j --target gl_smoke && ./build/gl_smoke`
Expected: PASS — prints `gl_smoke OK: 5 media nodes expose asset-backed file inputs` and exits 0.

- [ ] **Step 6: Full test sweep (nothing else regressed)**

Run: `cmake --build build -j && ctest --test-dir build --output-on-failure`
Expected: `core_tests` and `gl_smoke` both pass.

- [ ] **Step 7: Commit**

```bash
git add src/modules/AudioPlayerNode.cpp src/modules/VideoPlayerNode.cpp \
        src/modules/MeshLoaderNode.cpp src/modules/MidiFilePlayerNode.h \
        src/modules/DrumMachineNode.h tests/gl_smoke.cpp CMakeLists.txt
git commit -m "feat(modules): media file inputs are asset-backed (Audio/Video/Mesh/MIDI/Drum)"
```

---

## Task 3: Editor rendering — ▾ picker + asset popup

**Files:**
- Modify: `src/ui/PortWidgets.cpp`, `src/ui/NodeEditorPanel.cpp`

No automated test (ImGui can't run headlessly). Verified by build + a manual smoke check.

- [ ] **Step 1: Render the ▾ button on an asset-backed String input**

In `src/ui/PortWidgets.cpp`, replace the existing `case PortType::String:` block:

```cpp
        case PortType::String: {
            auto& s = std::get<std::string>(v);
            char buf[256];
            std::snprintf(buf, sizeof(buf), "%s", s.c_str());
            if (ImGui::InputText("##s", buf, sizeof(buf))) s = buf;
            break;
        }
```

with:

```cpp
        case PortType::String: {
            auto& s = std::get<std::string>(v);
            char buf[256];
            std::snprintf(buf, sizeof(buf), "%s", s.c_str());
            if (port.assetBacked) {
                // Editable path + a down-arrow that opens the library picker. The popup
                // must be opened in the editor's Suspend block (screen space), so just
                // signal the click here -- like the choice/colour buttons do.
                ImGui::SetNextItemWidth(94.0f);
                if (ImGui::InputText("##s", buf, sizeof(buf))) s = buf;
                ImGui::SameLine(0.0f, 2.0f);
                if (ImGui::ArrowButton("##assetpick", ImGuiDir_Down)) popupClicked = true;
            } else {
                if (ImGui::InputText("##s", buf, sizeof(buf))) s = buf;
            }
            break;
        }
```

- [ ] **Step 2: List the library in the deferred popup**

In `src/ui/NodeEditorPanel.cpp`, add the include near the top (after the existing includes):

```cpp
#include "core/AssetLibrary.h"
```

Add a small file-scope helper near the top of the `oss` namespace (after the `decodePin` helper):

```cpp
static const char* assetTypeName(AssetType t) {
    switch (t) {
        case AssetType::Audio: return "Audio";
        case AssetType::Video: return "Video";
        case AssetType::Midi:  return "MIDI";
        case AssetType::Mesh:  return "3D";
    }
    return "media";
}
```

In the `NodePopup` body, the dispatch currently is:

```cpp
            if (port.type == PortType::Colour) {
                auto& c = std::get<glm::vec4>(v);
                ImGui::ColorPicker4("##picker", &c.x, ImGuiColorEditFlags_AlphaBar);
            } else {   // a Float choice port: a list of its labels
                int idx = std::clamp((int)std::lround(std::get<float>(v)),
                                     0, (int)port.choices.size() - 1);
                for (int k = 0; k < (int)port.choices.size(); ++k) {
                    if (ImGui::Selectable(port.choices[k].c_str(), k == idx)) {
                        v = Value((float)k);
                        ImGui::CloseCurrentPopup();
                    }
                }
            }
```

Insert an asset branch between the Colour and the Float-choice branches (so the `else` stays the Float-choice case):

```cpp
            if (port.type == PortType::Colour) {
                auto& c = std::get<glm::vec4>(v);
                ImGui::ColorPicker4("##picker", &c.x, ImGuiColorEditFlags_AlphaBar);
            } else if (port.type == PortType::String && port.assetBacked) {
                // Pick a library asset of this type; copy its path into the field.
                std::vector<const Asset*> assets = graph.assets().byType(port.assetType);
                if (assets.empty()) {
                    ImGui::TextDisabled("No %s assets -- add them in the Assets window",
                                        assetTypeName(port.assetType));
                } else {
                    const std::string cur = std::get<std::string>(v);
                    for (const Asset* a : assets) {
                        std::string label = a->label.empty() ? a->path : a->label;
                        if (label.empty()) label = "(unnamed)";
                        if (ImGui::Selectable(label.c_str(), a->path == cur)) {
                            v = Value(a->path);
                            ImGui::CloseCurrentPopup();
                        }
                    }
                }
            } else {   // a Float choice port: a list of its labels
                int idx = std::clamp((int)std::lround(std::get<float>(v)),
                                     0, (int)port.choices.size() - 1);
                for (int k = 0; k < (int)port.choices.size(); ++k) {
                    if (ImGui::Selectable(port.choices[k].c_str(), k == idx)) {
                        v = Value((float)k);
                        ImGui::CloseCurrentPopup();
                    }
                }
            }
```

(`graph` is the `Graph&` parameter of `NodeEditorPanel::draw`, in scope here; `graph.assets()` is the Phase-1 library.)

- [ ] **Step 3: Build the app**

Run: `cmake --build build -j --target shader_streamer`
Expected: links cleanly.

- [ ] **Step 4: Manual smoke check**

Run: `./build/shader_streamer`
Verify: add an **Audio Player** node — its `file` input shows a text field + a small **▾** arrow. Add a couple of audio assets in the **View → Assets** window. Back on the node, click **▾** → a popup lists the audio assets by label; pick one → the `file` field fills with that asset's path. A media node whose Assets tab is empty shows the "No _Audio_ assets…" hint. Typing a path directly still works. Close the app.

- [ ] **Step 5: Commit**

```bash
git add src/ui/PortWidgets.cpp src/ui/NodeEditorPanel.cpp
git commit -m "feat(ui): media file inputs get a library-picker dropdown (copies the path)"
```

---

## Task 4: Documentation

**Files:**
- Modify: `CLAUDE.md`, `README.md`

- [ ] **Step 1: Update the CLAUDE.md Assets bullet**

In `CLAUDE.md`, find the **Assets / media library** bullet. Replace its final sentence:

```
  Phase 1 of two — Phase 2 will rewire node `file` controls into asset-id dropdowns. The
```

with:

```
  A `String` input flagged `assetBacked` (an `AssetType`, set by `Node::addAssetInput`) renders
  in the editor as the text field plus a ▾ picker (the deferred `NodePopup`) listing
  `graph.assets().byType(type)`; selecting copies the asset's `path` into the field (copy-path,
  no live binding — no node/eval/`.oss` change). The five media inputs use it: Audio Player,
  Video Player, Mesh Loader, MIDI File, and the Drum Machine's four voices. The
```

- [ ] **Step 2: Update the README Assets subsection**

In `README.md`, in the **### Assets** subsection, append this sentence to the end of the paragraph:

```markdown
Nodes that load media — **Audio Player**, **Video Player**, **Mesh Loader**, **MIDI File**, and the
**Drum Machine**'s voices — show a **▾** next to their `file` field that picks a path from the
matching Assets tab (it copies the asset's path in; you can still type a path directly).
```

- [ ] **Step 3: Verify only docs changed**

Run: `git diff --stat`
Expected: only `CLAUDE.md` and `README.md` changed.

- [ ] **Step 4: Commit**

```bash
git add CLAUDE.md README.md
git commit -m "docs: media controls can pick a path from the Assets library"
```

---

## Final verification (after all tasks)

- [ ] `cmake --build build -j && ctest --test-dir build --output-on-failure` — `core_tests` (incl. the new `addAssetInput` cases) and `gl_smoke` (incl. the 5-node asset-backed check) pass.
- [ ] Manual: `./build/shader_streamer` — an Audio Player's `file` shows the ▾ picker; picking a library asset fills the path; empty tab shows the hint; typing still works.
