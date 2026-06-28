# Asset Tags Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Tag media files in the Assets window — per-asset tags + a project tag-color registry, a Tags column of colored chips, and a per-tab tag-filter toolbar (OR filter, right-click to recolor).

**Architecture:** `core/AssetLibrary` gains `Asset::tags` + a `tagColors_` registry with tag ops (default color from `core/ColorHsv.h`); `core/ProjectFile` persists both with additive `atag`/`tagcolor` lines; `ui/AssetsPanel` adds a Tags column + a per-tab filter toolbar, with filter selection + per-row add-box text held as panel state.

**Tech Stack:** C++17, glm (GL-free), Dear ImGui, doctest (`core_tests`).

**Spec:** `docs/superpowers/specs/2026-06-28-asset-tags-design.md`

---

## File Structure

| File | Change |
|---|---|
| `src/core/AssetLibrary.{h,cpp}` | `Asset::tags`; `tagColors_` registry; `addTag`/`removeTag`/`tagsForType`/`tagColor`/`setTagColor`/`tagColors`/`loadTagColors`; default color. |
| `src/core/ProjectFile.{h,cpp}` | `ProjectDoc::tagColors`; serialize/parse `atag` + `tagcolor`; capture/restore. |
| `src/ui/AssetsPanel.{h,cpp}` | Tags column (chips + add box), per-tab filter toolbar; `filter_` + `addText_` panel state; `drawTab` becomes a private member. |
| `tests/test_assets.cpp` | tag model + codec round-trip tests. |
| `CLAUDE.md`, `README.md` | docs. |

---

## Task 1: `AssetLibrary` tags + color registry

**Files:**
- Modify: `src/core/AssetLibrary.h`, `src/core/AssetLibrary.cpp`
- Test: `tests/test_assets.cpp`

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_assets.cpp`. Add `#include <glm/vec4.hpp>` near the top (the others — `core/AssetLibrary.h`, `core/Graph.h`, `core/ProjectFile.h`, `<variant>` — are already present). Then:

```cpp
namespace {
// Exact component compare (avoid relying on glm's operator== availability).
inline bool vecEq(const glm::vec4& a, const glm::vec4& b) {
    return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w;
}
} // namespace

TEST_CASE("AssetLibrary addTag appends, dedups, ignores empty + bad id") {
    AssetLibrary lib;
    int a = lib.add(AssetType::Audio, "kick", "k.wav");
    lib.addTag(a, "drums");
    lib.addTag(a, "drums");          // duplicate -> ignored
    lib.addTag(a, "");               // empty -> ignored
    lib.addTag(999, "ghost");        // bad id -> ignored
    REQUIRE(lib.find(a) != nullptr);
    REQUIRE(lib.find(a)->tags.size() == 1);
    CHECK(lib.find(a)->tags[0] == "drums");
}

TEST_CASE("AssetLibrary removeTag drops only that tag") {
    AssetLibrary lib;
    int a = lib.add(AssetType::Audio, "k", "k");
    lib.addTag(a, "drums"); lib.addTag(a, "loop");
    lib.removeTag(a, "drums");
    REQUIRE(lib.find(a) != nullptr);
    REQUIRE(lib.find(a)->tags.size() == 1);
    CHECK(lib.find(a)->tags[0] == "loop");
    lib.removeTag(a, "nope");        // absent -> no-op
    lib.removeTag(999, "loop");      // bad id -> no-op
    CHECK(lib.find(a)->tags.size() == 1);
}

TEST_CASE("AssetLibrary tagsForType is distinct, sorted, per-type") {
    AssetLibrary lib;
    int au = lib.add(AssetType::Audio, "a", "a");
    int mi = lib.add(AssetType::Midi,  "m", "m");
    int au2 = lib.add(AssetType::Audio, "b", "b");
    lib.addTag(au, "loop"); lib.addTag(au, "drums");
    lib.addTag(au2, "drums");        // same tag on two audio assets
    lib.addTag(mi, "bass");
    CHECK(lib.tagsForType(AssetType::Audio) == std::vector<std::string>{"drums", "loop"}); // distinct + sorted
    CHECK(lib.tagsForType(AssetType::Midi)  == std::vector<std::string>{"bass"});
    CHECK(lib.tagsForType(AssetType::Video).empty());
}

TEST_CASE("AssetLibrary tag colors: deterministic default, override, clear") {
    AssetLibrary lib;
    CHECK(vecEq(lib.tagColor("drums"), lib.tagColor("drums")));   // deterministic default
    lib.setTagColor("drums", glm::vec4(0.1f, 0.2f, 0.3f, 1.0f));
    CHECK(vecEq(lib.tagColor("drums"), glm::vec4(0.1f, 0.2f, 0.3f, 1.0f)));
    CHECK(lib.tagColors().count("drums") == 1);
    lib.clear();
    CHECK(lib.tagColors().empty());
}

TEST_CASE("AssetLibrary loadTagColors round-trips the registry") {
    AssetLibrary lib;
    std::map<std::string, glm::vec4> reg;
    reg["loop"] = glm::vec4(0.4f, 0.5f, 0.6f, 1.0f);
    lib.loadTagColors(reg);
    CHECK(vecEq(lib.tagColor("loop"), glm::vec4(0.4f, 0.5f, 0.6f, 1.0f)));
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build build -j --target core_tests`
Expected: FAIL — `'struct oss::Asset' has no member named 'tags'` / `'addTag' is not a member`.

- [ ] **Step 3: Extend `AssetLibrary.h`**

In `src/core/AssetLibrary.h`, change the includes at the top to:

```cpp
#pragma once
#include <map>
#include <string>
#include <vector>
#include <glm/vec4.hpp>
```

Add the `tags` field to `struct Asset` (after `path`):

```cpp
    std::vector<std::string> tags;      // tag names on this asset (insertion order)
```

Add the tag methods to the `public:` section (after `load`):

```cpp
    // --- tags ---
    void addTag(int id, const std::string& tag);     // append if absent; register a default color
                                                     // if the tag is new. No-op on bad id / empty / dup.
    void removeTag(int id, const std::string& tag);  // drop the tag from the asset; no-op if absent.

    // Distinct tags used by assets of `type`, sorted ascending (drives the per-tab toolbar).
    std::vector<std::string> tagsForType(AssetType type) const;

    glm::vec4 tagColor(const std::string& tag) const;            // registered color, else default-from-name
    void      setTagColor(const std::string& tag, glm::vec4 c);  // register/update

    const std::map<std::string, glm::vec4>& tagColors() const { return tagColors_; }
    void loadTagColors(std::map<std::string, glm::vec4> colors) { tagColors_ = std::move(colors); }
```

Add the registry to the `private:` section (after `nextId_`):

```cpp
    std::map<std::string, glm::vec4> tagColors_;   // tag name -> color
```

- [ ] **Step 4: Implement in `AssetLibrary.cpp`**

In `src/core/AssetLibrary.cpp`, change the includes to:

```cpp
#include "core/AssetLibrary.h"
#include "core/ColorHsv.h"
#include <algorithm>
#include <functional>
#include <set>
#include <utility>
```

Add `tagColors_.clear();` inside `clear()`:

```cpp
void AssetLibrary::clear() {
    assets_.clear();
    nextId_ = 1;
    tagColors_.clear();
}
```

Add the tag implementations (before the closing `} // namespace oss`):

```cpp
namespace {
// Deterministic default color for a tag: a hue from the name's hash, fed through hsvToRgb.
// Same name -> same color every run; distinct-ish across names; the user can override.
glm::vec4 defaultTagColor(const std::string& name) {
    float hue = (float)(std::hash<std::string>{}(name) % 1000) / 1000.0f;
    glm::vec3 rgb = hsvToRgb(hue, 0.55f, 0.95f);
    return glm::vec4(rgb, 1.0f);
}
} // namespace

void AssetLibrary::addTag(int id, const std::string& tag) {
    if (tag.empty()) return;
    Asset* a = find(id);
    if (!a) return;
    if (std::find(a->tags.begin(), a->tags.end(), tag) != a->tags.end()) return;   // dedup
    a->tags.push_back(tag);
    if (tagColors_.find(tag) == tagColors_.end()) tagColors_[tag] = defaultTagColor(tag);
}

void AssetLibrary::removeTag(int id, const std::string& tag) {
    Asset* a = find(id);
    if (!a) return;
    a->tags.erase(std::remove(a->tags.begin(), a->tags.end(), tag), a->tags.end());
}

std::vector<std::string> AssetLibrary::tagsForType(AssetType type) const {
    std::set<std::string> uniq;     // sorted + distinct
    for (const Asset& a : assets_)
        if (a.type == type)
            for (const std::string& t : a.tags) uniq.insert(t);
    return std::vector<std::string>(uniq.begin(), uniq.end());
}

glm::vec4 AssetLibrary::tagColor(const std::string& tag) const {
    auto it = tagColors_.find(tag);
    return it != tagColors_.end() ? it->second : defaultTagColor(tag);
}

void AssetLibrary::setTagColor(const std::string& tag, glm::vec4 c) {
    tagColors_[tag] = c;
}
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `cmake --build build -j --target core_tests && ./build/core_tests -tc="AssetLibrary*Tag*,AssetLibrary*tag*"`
Expected: PASS (5 cases). Then `ctest --test-dir build --output-on-failure -R core_tests` — all green (existing AssetLibrary tests still pass; `Asset{...}` 4-field aggregate init still works because `tags` value-initializes).

- [ ] **Step 6: Commit**

```bash
git add src/core/AssetLibrary.h src/core/AssetLibrary.cpp tests/test_assets.cpp
git commit -m "feat(core): AssetLibrary tags + per-tag color registry"
```

---

## Task 2: ProjectFile persistence for tags + colors

**Files:**
- Modify: `src/core/ProjectFile.h`, `src/core/ProjectFile.cpp`
- Test: `tests/test_assets.cpp`

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_assets.cpp` (`<map>` comes via `core/AssetLibrary.h`; `<glm/vec4.hpp>` was added in Task 1):

```cpp
TEST_CASE("ProjectFile round-trips asset tags + tag colors (incl. spaces)") {
    ProjectDoc d;
    Asset a{1, AssetType::Audio, "kick", "k.wav"};
    a.tags = {"drums", "tight loop"};          // a tag with a space
    d.assets = { a };
    d.tagColors["drums"]      = glm::vec4(0.2f, 0.4f, 0.6f, 1.0f);
    d.tagColors["tight loop"] = glm::vec4(0.9f, 0.1f, 0.3f, 1.0f);

    std::string text = serializeProject(d);
    ProjectDoc out;
    REQUIRE(parseProject(text, out));
    REQUIRE(out.assets.size() == 1);
    CHECK(out.assets[0].tags == std::vector<std::string>{"drums", "tight loop"});
    REQUIRE(out.tagColors.count("drums") == 1);
    REQUIRE(out.tagColors.count("tight loop") == 1);
    CHECK(out.tagColors["drums"].x == doctest::Approx(0.2f));
    CHECK(out.tagColors["drums"].z == doctest::Approx(0.6f));
    CHECK(out.tagColors["tight loop"].x == doctest::Approx(0.9f));
}

TEST_CASE("ProjectFile without tag lines loads empty tags + colors") {
    ProjectDoc out;
    REQUIRE(parseProject("oss-project 1\ntransport 120 4 0 0 4 8\nasset 1 0\n", out));
    REQUIRE(out.assets.size() == 1);
    CHECK(out.assets[0].tags.empty());
    CHECK(out.tagColors.empty());
}

TEST_CASE("captureProject / restoreProject carry tags + colors through a Graph") {
    Graph g;
    int a = g.assets().add(AssetType::Audio, "kick", "k.wav");
    g.assets().addTag(a, "drums");
    g.assets().setTagColor("drums", glm::vec4(0.3f, 0.3f, 0.3f, 1.0f));
    ProjectDoc d = captureProject(g);

    Graph g2;
    auto factory = [](const std::string&) -> std::unique_ptr<Node> { return nullptr; };
    auto init    = [](Node&) {};
    restoreProject(d, g2, factory, init);
    REQUIRE(g2.assets().all().size() == 1);
    CHECK(g2.assets().all()[0].tags == std::vector<std::string>{"drums"});
    CHECK(g2.assets().tagColor("drums").x == doctest::Approx(0.3f));
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build build -j --target core_tests`
Expected: FAIL — `'struct oss::ProjectDoc' has no member named 'tagColors'`.

- [ ] **Step 3: Add `tagColors` to `ProjectDoc`**

In `src/core/ProjectFile.h`, add the include near the top (beside the existing `#include "core/AssetLibrary.h"`):

```cpp
#include <map>
```

Add the field to `struct ProjectDoc` (after `std::vector<Asset> assets;`):

```cpp
    std::map<std::string, glm::vec4> tagColors;
```

(`glm::vec4` is already available via `core/AssetLibrary.h`. The asset tags travel inside the `Asset`s.)

- [ ] **Step 4: Serialize `atag` + `tagcolor`**

In `src/core/ProjectFile.cpp`, in the asset serialize loop, add an `atag` line per tag after the `apath` line; then, after the loop, emit the registry. The current loop is:

```cpp
    for (const Asset& a : d.assets) {
        out += "asset " + std::to_string(a.id) + " " + std::to_string((int)a.type) + "\n";
        if (!a.label.empty()) out += "alabel " + escape(a.label) + "\n";
        if (!a.path.empty())  out += "apath "  + escape(a.path)  + "\n";
    }
```

Replace it with:

```cpp
    for (const Asset& a : d.assets) {
        out += "asset " + std::to_string(a.id) + " " + std::to_string((int)a.type) + "\n";
        if (!a.label.empty()) out += "alabel " + escape(a.label) + "\n";
        if (!a.path.empty())  out += "apath "  + escape(a.path)  + "\n";
        for (const std::string& t : a.tags) out += "atag " + escape(t) + "\n";
    }
    for (const auto& kv : d.tagColors) {
        const glm::vec4& c = kv.second;
        out += "tagcolor " + std::to_string(c.x) + " " + std::to_string(c.y) + " "
             + std::to_string(c.z) + " " + std::to_string(c.w) + " " + escape(kv.first) + "\n";
    }
```

- [ ] **Step 5: Parse `atag` + `tagcolor`**

In `parseProject`, the asset branches currently are:

```cpp
        } else if (kw == "alabel") {
            if (curAsset) curAsset->label = unescape(restOfLine(ls));
        } else if (kw == "apath") {
            if (curAsset) curAsset->path = unescape(restOfLine(ls));
```

Add two branches right after the `apath` branch (still inside the same if/else-if chain):

```cpp
        } else if (kw == "atag") {
            if (curAsset) curAsset->tags.push_back(unescape(restOfLine(ls)));
        } else if (kw == "tagcolor") {
            float r, g, b, a; ls >> r >> g >> b >> a;
            if (ls.fail()) continue;                       // skip malformed; not fatal
            out.tagColors[unescape(restOfLine(ls))] = glm::vec4(r, g, b, a);
```

- [ ] **Step 6: Capture + restore the registry**

In `captureProject`, after `d.assets = g.assets().all();` add:

```cpp
    d.tagColors = g.assets().tagColors();
```

In `restoreProject`, after `g.assets().load(d.assets);` add:

```cpp
    g.assets().loadTagColors(d.tagColors);
```

- [ ] **Step 7: Run the tests to verify they pass**

Run: `cmake --build build -j --target core_tests && ./build/core_tests -tc="*tags + tag colors*,*empty tags*,*carry tags*"`
Expected: PASS (3 cases). Then `ctest --test-dir build --output-on-failure -R core_tests` — all green.

- [ ] **Step 8: Commit**

```bash
git add src/core/ProjectFile.h src/core/ProjectFile.cpp tests/test_assets.cpp
git commit -m "feat(core): persist asset tags + tag colors in .oss (atag/tagcolor)"
```

---

## Task 3: AssetsPanel — Tags column + filter toolbar

**Files:**
- Modify: `src/ui/AssetsPanel.h`, `src/ui/AssetsPanel.cpp`

No automated test (ImGui). Verified by build + a manual smoke check.

- [ ] **Step 1: Add panel state to `AssetsPanel.h`**

Replace the body of `src/ui/AssetsPanel.h` with:

```cpp
#pragma once
#include <map>
#include <set>
#include <string>
#include <vector>
#include "core/AssetLibrary.h"   // AssetType / kAssetTypeCount / Asset

namespace oss {

// The "Assets" window: a tab bar (Audio / Video / MIDI / 3D). Each tab is a tag-filter toolbar
// plus a table of that type's media files (label, path + Browse, tags, remove) and an Add row.
// Edits mutate the library in place (project state, saved with the project).
class AssetsPanel {
public:
    void draw(AssetLibrary& lib, bool* open);

private:
    void drawTab(AssetLibrary& lib, AssetType type, const char* noun,
                 const std::vector<std::string>& filters);

    std::set<std::string> filter_[kAssetTypeCount];   // selected filter tags per media tab (OR)
    std::map<int, std::string> addText_;              // per-asset "+ add tag" in-progress text
};

} // namespace oss
```

- [ ] **Step 2: Rewrite `AssetsPanel.cpp`**

Replace the whole of `src/ui/AssetsPanel.cpp` with:

```cpp
#include "ui/AssetsPanel.h"
#include "ui/FileDialog.h"
#include "core/AssetLibrary.h"
#include <imgui.h>
#include <cfloat>
#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>

namespace oss {

namespace {

// Filename basename, without directory or extension -> a default label seed.
std::string baseLabel(const std::string& path) {
    std::size_t slash = path.find_last_of("/\\");
    std::string name = (slash == std::string::npos) ? path : path.substr(slash + 1);
    std::size_t dot = name.find_last_of('.');
    if (dot != std::string::npos && dot != 0) name = name.substr(0, dot);  // keep dotfiles like ".vimrc" intact
    return name;
}

// Push the 3 button-state colors tinted with `c` (alpha distinguishes selected/unselected).
void pushTagColors(const glm::vec4& c, float baseAlpha) {
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(c.x, c.y, c.z, baseAlpha));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(c.x, c.y, c.z, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(c.x, c.y, c.z, 1.0f));
}

} // namespace

void AssetsPanel::drawTab(AssetLibrary& lib, AssetType type, const char* noun,
                          const std::vector<std::string>& filters) {
    std::set<std::string>& filterSet = filter_[(int)type];

    // --- Tag-filter toolbar: this tab's tags as colored toggle buttons (left-click filter,
    //     right-click recolor). A selected tag is opaque; unselected is dimmed. ---
    std::vector<std::string> tabTags = lib.tagsForType(type);
    for (std::size_t i = 0; i < tabTags.size(); ++i) {
        const std::string& tag = tabTags[i];
        if (i) ImGui::SameLine();
        ImGui::PushID(tag.c_str());
        const bool selected = filterSet.count(tag) > 0;
        pushTagColors(lib.tagColor(tag), selected ? 1.0f : 0.45f);
        if (ImGui::SmallButton(tag.c_str())) {
            if (selected) filterSet.erase(tag); else filterSet.insert(tag);
        }
        ImGui::PopStyleColor(3);
        if (ImGui::BeginPopupContextItem("##recolor")) {     // right-click a tag -> color picker
            glm::vec4 c = lib.tagColor(tag);
            if (ImGui::ColorPicker4("##pick", &c.x)) lib.setTagColor(tag, c);
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    if (!filterSet.empty()) {
        if (!tabTags.empty()) ImGui::SameLine();
        if (ImGui::SmallButton("clear filter")) filterSet.clear();
    }

    int toRemove = -1;
    if (ImGui::BeginTable("##assettbl", 4,
            ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerH)) {
        ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 120.0f);
        ImGui::TableSetupColumn("Path");
        ImGui::TableSetupColumn("Tags",  ImGuiTableColumnFlags_WidthFixed, 220.0f);
        ImGui::TableSetupColumn("##act", ImGuiTableColumnFlags_WidthFixed, 30.0f);
        ImGui::TableHeadersRow();

        for (const Asset* a : lib.byType(type)) {
            // OR filter: skip assets that share no tag with the selection (empty -> show all).
            if (!filterSet.empty()) {
                bool match = false;
                for (const std::string& t : a->tags) if (filterSet.count(t)) { match = true; break; }
                if (!match) continue;
            }
            int id = a->id;
            ImGui::PushID(id);
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            char lbl[512];
            std::snprintf(lbl, sizeof(lbl), "%s", a->label.c_str());
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::InputText("##label", lbl, sizeof(lbl))) lib.setLabel(id, lbl);

            ImGui::TableSetColumnIndex(1);
            char pth[512];
            std::snprintf(pth, sizeof(pth), "%s", a->path.c_str());
            ImGui::SetNextItemWidth(-30.0f);
            if (ImGui::InputText("##path", pth, sizeof(pth))) lib.setPath(id, pth);
            ImGui::SameLine();
            if (ImGui::Button("...")) {
                std::string picked = openFileDialog(noun, "Media", filters);
                if (!picked.empty()) {
                    lib.setPath(id, picked);
                    const Asset* cur = lib.find(id);
                    if (cur && cur->label.empty()) lib.setLabel(id, baseLabel(picked));
                }
            }

            // --- Tags column: colored chips ("tag x" removes) + a "+ add" box. ---
            ImGui::TableSetColumnIndex(2);
            int tagToRemove = -1;
            for (std::size_t ti = 0; ti < a->tags.size(); ++ti) {
                const std::string& tag = a->tags[ti];
                ImGui::PushID((int)ti);
                pushTagColors(lib.tagColor(tag), 0.8f);
                if (ImGui::SmallButton((tag + " x").c_str())) tagToRemove = (int)ti;
                ImGui::PopStyleColor(3);
                ImGui::PopID();
                ImGui::SameLine();
            }
            std::string& s = addText_[id];
            char addbuf[64];
            std::snprintf(addbuf, sizeof(addbuf), "%s", s.c_str());
            ImGui::SetNextItemWidth(70.0f);
            if (ImGui::InputText("##addtag", addbuf, sizeof(addbuf),
                                 ImGuiInputTextFlags_EnterReturnsTrue)) {
                lib.addTag(id, addbuf);     // commit on Enter
                s.clear();
            } else {
                s = addbuf;                 // persist in-progress typing across frames
            }
            if (tagToRemove >= 0) lib.removeTag(id, a->tags[(std::size_t)tagToRemove]);

            ImGui::TableSetColumnIndex(3);
            if (ImGui::Button("x")) toRemove = id;

            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    std::string addLabel = std::string("+ Add ") + noun;
    if (ImGui::Button(addLabel.c_str())) lib.add(type, "", "");
    ImGui::SameLine();
    if (ImGui::Button("Add files...")) {
        for (const std::string& path : openMultipleFileDialog(noun, "Media", filters))
            lib.add(type, baseLabel(path), path);
    }
    if (toRemove >= 0) { lib.remove(toRemove); addText_.erase(toRemove); }
}

void AssetsPanel::draw(AssetLibrary& lib, bool* open) {
    if (!open || !*open) return;
    if (!ImGui::Begin("Assets", open)) { ImGui::End(); return; }

    if (ImGui::BeginTabBar("assets_tabs")) {
        if (ImGui::BeginTabItem("Audio")) {
            drawTab(lib, AssetType::Audio, "audio file",
                    {"mp3", "wav", "flac", "m4a", "ogg", "aac", "aiff"});
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Video")) {
            drawTab(lib, AssetType::Video, "video file",
                    {"mp4", "mov", "mkv", "avi", "webm"});
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("MIDI")) {
            drawTab(lib, AssetType::Midi, "MIDI file", {"mid", "midi"});
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("3D")) {
            drawTab(lib, AssetType::Mesh, "3D model", {"obj", "gltf", "glb"});
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

} // namespace oss
```

(Notes: `<set>`/`<map>` come via `AssetsPanel.h`; `glm::vec4`/`ImVec4` via `core/AssetLibrary.h` + `imgui.h`. The Tags edits mutate an asset's own `tags`/`tagColors_`, never the `assets_` vector, so the `byType()` pointers stay valid through the loop; asset add/remove stay deferred to after `EndTable`. `addText_.erase` on removal prunes the stale add-box buffer.)

- [ ] **Step 3: Build the app**

Run: `cmake --build build -j --target shader_streamer`
Expected: links cleanly.

- [ ] **Step 4: Manual smoke check (DEFERRED — do not launch the GUI as a subagent)**

The interactive check is performed by the coordinator/user: open **View → Assets**, type a tag in a row's Tags column (Enter to commit) → a colored chip appears + the tag shows in the toolbar; click toolbar tags to filter the grid (OR; multiple); right-click a toolbar tag → recolor it; click a chip's `x` to remove; save + load and confirm tags + colors persist. Your verification is the clean build (Step 3) + re-reading the code.

- [ ] **Step 5: Commit**

```bash
git add src/ui/AssetsPanel.h src/ui/AssetsPanel.cpp
git commit -m "feat(ui): Assets tags column + per-tab tag-filter toolbar"
```

---

## Task 4: Documentation

**Files:**
- Modify: `CLAUDE.md`, `README.md`

- [ ] **Step 1: Extend the CLAUDE.md Assets bullet**

In `CLAUDE.md`, find the **Assets / media library** bullet. Append this sentence at the end of the bullet (before the next `-` bullet):

```
  Each `Asset` also carries `tags`; the library holds a `tagColors_` registry (tag → `glm::vec4`,
  default hue from the tag-name hash via `core/ColorHsv.h`). `ui/AssetsPanel` renders a Tags column
  of colored chips + a per-tab tag-filter toolbar (`tagsForType`; left-click toggles an OR filter,
  right-click recolors). Tags + colors persist via `ProjectFile` (`atag` per asset, `tagcolor` for
  the registry); the filter selection is transient panel state.
```

- [ ] **Step 2: Extend the README Assets subsection**

In `README.md`, in the **### Assets** subsection, append this sentence to the end of the paragraph:

```markdown
A **Tags** column tags each file (colored chips + an add box); each tab's tag bar filters the grid
(click tags — match any; none = show all) and right-clicking a tag recolors it. Tags and their
colors are saved with the project.
```

- [ ] **Step 3: Verify only docs changed**

Run: `git diff --stat`
Expected: only `CLAUDE.md` and `README.md` changed.

- [ ] **Step 4: Commit**

```bash
git add CLAUDE.md README.md
git commit -m "docs: asset tags (Tags column + tag-filter toolbar)"
```

---

## Final verification (after all tasks)

- [ ] `cmake --build build -j && ctest --test-dir build --output-on-failure` — `core_tests` (incl. the new tag + codec cases) and `gl_smoke` pass.
- [ ] Manual: launch `./build/shader_streamer` — tag rows, filter via the toolbar (OR), recolor a tag, remove tags, and save/load to confirm tags + colors persist.
