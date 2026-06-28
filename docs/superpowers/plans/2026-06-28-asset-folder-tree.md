# Collapsible Assets folder tree — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Render each Assets tab's flat row list as a collapsible folder tree grouped by each asset's path, while preserving inline editing, the multi-select, and broadcast tagging.

**Architecture:** A new GL-free header `core/AssetTree.h` turns the filtered `std::vector<const Asset*>` into a nested `AssetTreeNode` (folders + leaf files, single-child chains collapsed). `ui/AssetsPanel` renders that tree recursively inside the existing `BeginTable`/`EndTable` with `ImGui::TreeNodeEx`: folders are `SpanFullWidth` nodes, files are `Leaf | NoTreePushOnOpen` nodes that carry selection (plain/Ctrl/Cmd/Shift), double-click rename, and the unchanged editable Path/Tags/✕ columns.

**Tech Stack:** C++17, Dear ImGui (tables + tree nodes), doctest (`core_tests`).

---

## Reference — design spec

`docs/superpowers/specs/2026-06-28-asset-folder-tree-design.md`. Key decisions: nest folders by path segment **and collapse single-child chains**; ungrouped/blank-path files are root-level leaves; leaf node text = label → filename → `(unnamed)`; selection on the leaf node (plain=select-only, Ctrl/Cmd=toggle, Shift=range over DFS leaf order); double-click a leaf node to rename; folders not selectable; broadcast tagging preserved; root folders collapsed unless there is exactly one root folder (then opened).

## File Structure

| File | Responsibility |
|---|---|
| `src/core/AssetTree.h` (new, header-only, GL-free) | `AssetTreeNode` + `buildAssetTree(rows)` — pure path-grouping logic. |
| `tests/test_asset_tree.cpp` (new) | doctest unit tests for `buildAssetTree`, compiled into `core_tests`. |
| `CMakeLists.txt` (modify line ~308) | add `tests/test_asset_tree.cpp` to the `core_tests` source list. |
| `src/ui/AssetsPanel.h` (modify) | include `core/AssetTree.h`; add `drawTreeNode`/`drawAssetLeaf` decls + `renamingId_`/`renameBuf_`/`renameFocus_`. |
| `src/ui/AssetsPanel.cpp` (modify) | rewrite `drawTab` to build + render the tree; add the two helpers + two anon-namespace statics. |
| `CLAUDE.md`, `README.md` (modify) | docs. |

---

### Task 1: `core/AssetTree.h` + unit tests

**Files:**
- Create: `src/core/AssetTree.h`
- Create: `tests/test_asset_tree.cpp`
- Modify: `CMakeLists.txt` (the `core_tests` source list, after `tests/test_path_util.cpp` on line ~308)

- [ ] **Step 1: Write the failing tests**

Create `tests/test_asset_tree.cpp`:

```cpp
#include <doctest/doctest.h>
#include "core/AssetTree.h"
#include <string>
#include <vector>

using namespace oss;

// Pointers into a stable backing vector (valid for the lifetime of `v` in each TEST_CASE).
static std::vector<const Asset*> ptrs(const std::vector<Asset>& v) {
    std::vector<const Asset*> out;
    for (const Asset& a : v) out.push_back(&a);
    return out;
}

TEST_CASE("buildAssetTree groups files under their folder; ungrouped at root") {
    std::vector<Asset> v = {
        {1, AssetType::Audio, "Kick",  "drums/kick.wav",  {}},
        {2, AssetType::Audio, "Snare", "drums/snare.wav", {}},
        {3, AssetType::Audio, "Pad",   "pad.wav",         {}},   // no directory -> ungrouped
    };
    AssetTreeNode root = buildAssetTree(ptrs(v));
    REQUIRE(root.folders.size() == 1);
    CHECK(root.folders[0].name == "drums");
    REQUIRE(root.folders[0].files.size() == 2);
    CHECK(root.folders[0].files[0]->id == 1);   // input order preserved
    CHECK(root.folders[0].files[1]->id == 2);
    REQUIRE(root.files.size() == 1);            // ungrouped sits at the root
    CHECK(root.files[0]->id == 3);
}

TEST_CASE("buildAssetTree nests subfolders, sorted ascending") {
    std::vector<Asset> v = {
        {1, AssetType::Audio, "", "a/c/y.wav", {}},   // inserted c first...
        {2, AssetType::Audio, "", "a/b/x.wav", {}},   // ...b second
    };
    AssetTreeNode root = buildAssetTree(ptrs(v));
    REQUIRE(root.folders.size() == 1);
    CHECK(root.folders[0].name == "a");
    REQUIRE(root.folders[0].folders.size() == 2);
    CHECK(root.folders[0].folders[0].name == "b");   // sorted: b before c
    CHECK(root.folders[0].folders[0].files[0]->id == 2);
    CHECK(root.folders[0].folders[1].name == "c");
    CHECK(root.folders[0].folders[1].files[0]->id == 1);
}

TEST_CASE("buildAssetTree collapses single-child chains") {
    std::vector<Asset> v = {
        {1, AssetType::Audio, "", "Users/me/Development/audio/kick.wav", {}},
    };
    AssetTreeNode root = buildAssetTree(ptrs(v));
    REQUIRE(root.folders.size() == 1);
    CHECK(root.folders[0].name == "Users/me/Development/audio");   // chain merged into one node
    CHECK(root.folders[0].folders.empty());
    REQUIRE(root.folders[0].files.size() == 1);
    CHECK(root.folders[0].files[0]->id == 1);
}

TEST_CASE("buildAssetTree keeps a folder that has both a file and a subfolder") {
    std::vector<Asset> v = {
        {1, AssetType::Audio, "", "a/own.wav",   {}},
        {2, AssetType::Audio, "", "a/b/deep.wav", {}},
    };
    AssetTreeNode root = buildAssetTree(ptrs(v));
    REQUIRE(root.folders.size() == 1);
    CHECK(root.folders[0].name == "a");           // NOT collapsed: it has a file of its own
    REQUIRE(root.folders[0].files.size() == 1);
    CHECK(root.folders[0].files[0]->id == 1);
    REQUIRE(root.folders[0].folders.size() == 1);
    CHECK(root.folders[0].folders[0].name == "b");
    CHECK(root.folders[0].folders[0].files[0]->id == 2);
}

TEST_CASE("buildAssetTree handles separators, redundancy, and empty paths") {
    std::vector<Asset> v = {
        {1, AssetType::Audio, "", "a\\b\\w.wav", {}},   // backslash separators
        {2, AssetType::Audio, "", "/x//y/z.wav", {}},   // leading + doubled '/'
        {3, AssetType::Audio, "", "",            {}},   // empty path -> ungrouped
        {4, AssetType::Audio, "", "bare.wav",    {}},   // bare filename -> ungrouped
    };
    AssetTreeNode root = buildAssetTree(ptrs(v));
    REQUIRE(root.files.size() == 2);                    // ids 3 and 4 ungrouped, input order
    CHECK(root.files[0]->id == 3);
    CHECK(root.files[1]->id == 4);
    bool foundAB = false, foundXY = false;
    for (const AssetTreeNode& f : root.folders) {
        if (f.name == "a/b") { foundAB = true; REQUIRE(f.files.size() == 1); CHECK(f.files[0]->id == 1); }
        if (f.name == "x/y") { foundXY = true; REQUIRE(f.files.size() == 1); CHECK(f.files[0]->id == 2); }
    }
    CHECK(foundAB);
    CHECK(foundXY);
}

TEST_CASE("buildAssetTree on empty input yields an empty root") {
    AssetTreeNode root = buildAssetTree({});
    CHECK(root.name.empty());
    CHECK(root.folders.empty());
    CHECK(root.files.empty());
}
```

- [ ] **Step 2: Register the test in CMake**

In `CMakeLists.txt`, inside the `add_executable(core_tests …)` source list, add the new line right after `tests/test_path_util.cpp` (currently line ~308):

```cmake
  tests/test_assets.cpp
  tests/test_path_util.cpp
  tests/test_asset_tree.cpp
```

- [ ] **Step 3: Run the tests to verify they fail to compile (header missing)**

Run: `cmake --build build -j --target core_tests`
Expected: FAIL — `fatal error: 'core/AssetTree.h' file not found`.

- [ ] **Step 4: Implement `core/AssetTree.h`**

Create `src/core/AssetTree.h`:

```cpp
#pragma once
#include <algorithm>
#include <string>
#include <utility>
#include <vector>
#include "core/AssetLibrary.h"   // Asset (GL-free)

namespace oss {

// One node of the Assets folder tree. A node holds child folders and/or leaf files.
struct AssetTreeNode {
    std::string                name;     // folder segment(s); "" for the synthetic root
    std::vector<AssetTreeNode> folders;  // child folders, sorted ascending by name (after build)
    std::vector<const Asset*>  files;    // leaf files in this folder, in input (byType) order
};

namespace detail {

// Split `path` into non-empty segments on '/' or '\\'. Leading / trailing / doubled separators
// produce no empty segments (so "/x//y/" -> {"x","y"}).
inline std::vector<std::string> splitPathSegments(const std::string& path) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : path) {
        if (c == '/' || c == '\\') {
            if (!cur.empty()) { out.push_back(cur); cur.clear(); }
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

// The immediate child folder named `seg` under `parent`, creating it (in insertion order) if absent.
inline AssetTreeNode& childFolder(AssetTreeNode& parent, const std::string& seg) {
    for (AssetTreeNode& f : parent.folders) if (f.name == seg) return f;
    parent.folders.push_back(AssetTreeNode{seg, {}, {}});
    return parent.folders.back();
}

// Collapse `node` in place: while it has exactly one child folder and no files of its own, absorb
// that child (join names with '/'). Leaves a node that has files, or 0/2+ child folders.
inline void collapseChain(AssetTreeNode& node) {
    while (node.files.empty() && node.folders.size() == 1) {
        AssetTreeNode only = std::move(node.folders.front());
        node.name += "/" + only.name;
        node.files   = std::move(only.files);
        node.folders = std::move(only.folders);
    }
}

// Recursively normalize: collapse this node's chain (unless it's the root), sort its child folders
// ascending by name, then recurse. Files keep their order.
inline void normalize(AssetTreeNode& node, bool isRoot) {
    if (!isRoot) collapseChain(node);
    std::sort(node.folders.begin(), node.folders.end(),
              [](const AssetTreeNode& a, const AssetTreeNode& b) { return a.name < b.name; });
    for (AssetTreeNode& f : node.folders) normalize(f, false);
}

} // namespace detail

// Group `rows` into a folder tree by the directory portion of each asset's `path`:
//  - split on '/' or '\\'; directory segments are folders, the last segment is the leaf;
//  - a file whose path has no directory part (empty / bare filename) becomes a file of the root;
//  - single-child folder chains collapse (name joined with '/');
//  - child folders are sorted ascending; files keep their order in `rows`.
inline AssetTreeNode buildAssetTree(const std::vector<const Asset*>& rows) {
    AssetTreeNode root;
    for (const Asset* a : rows) {
        if (!a) continue;
        std::vector<std::string> segs = detail::splitPathSegments(a->path);
        if (segs.size() <= 1) {                 // no directory part -> ungrouped, root-level leaf
            root.files.push_back(a);
            continue;
        }
        AssetTreeNode* node = &root;
        for (std::size_t i = 0; i + 1 < segs.size(); ++i)   // every segment but the basename
            node = &detail::childFolder(*node, segs[i]);
        node->files.push_back(a);
    }
    detail::normalize(root, /*isRoot*/true);
    return root;
}

} // namespace oss
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `cmake --build build -j --target core_tests && ctest --test-dir build -R core_tests --output-on-failure`
Expected: PASS — all `core_tests` cases green, including the new `buildAssetTree …` cases.

- [ ] **Step 6: Commit**

```bash
git add src/core/AssetTree.h tests/test_asset_tree.cpp CMakeLists.txt
git commit -m "feat(core): AssetTree path-grouping with single-child collapse + tests"
```

---

### Task 2: Render the Assets grid as a folder tree

**Files:**
- Modify: `src/ui/AssetsPanel.h`
- Modify: `src/ui/AssetsPanel.cpp`

This task is UI (ImGui tree + table) and is not headlessly testable — it's verified by a clean build plus the manual smoke check in Step 6. The grouping logic it depends on is already unit-tested in Task 1.

- [ ] **Step 1: Extend `AssetsPanel.h`**

In `src/ui/AssetsPanel.h`: add the `core/AssetTree.h` include, declare the two render helpers, and add the rename state. The full file after editing:

```cpp
#pragma once
#include <map>
#include <set>
#include <string>
#include <vector>
#include "core/AssetLibrary.h"   // AssetType / kAssetTypeCount / Asset
#include "core/AssetTree.h"      // AssetTreeNode (used in helper signatures)

namespace oss {

// The "Assets" window: a tab bar (Audio / Video / MIDI / 3D). Each tab is a tag-filter toolbar
// plus a collapsible folder tree of that type's media files (name, path + Browse, tags, remove)
// and an Add row. Edits mutate the library in place (project state, saved with the project).
class AssetsPanel {
public:
    AssetsPanel() { for (int& a : anchor_) a = -1; }   // anchors start at "none"

    void draw(AssetLibrary& lib, bool* open);

private:
    void drawTab(AssetLibrary& lib, AssetType type, const char* noun,
                 const std::vector<std::string>& filters);

    // Recursively render a tree node's child folders (collapsible) then its leaf files.
    void drawTreeNode(AssetLibrary& lib, const AssetTreeNode& node, bool rootLevel,
                      bool singleRoot, std::set<int>& selSet, int& anchor,
                      const std::vector<int>& leafOrder, const char* noun,
                      const std::vector<std::string>& filters, int& toRemove);

    // Render one file leaf row: the column-0 node (selection / rename) + the editable
    // Path / Tags / remove columns.
    void drawAssetLeaf(AssetLibrary& lib, const Asset* a, std::set<int>& selSet, int& anchor,
                       const std::vector<int>& leafOrder, const char* noun,
                       const std::vector<std::string>& filters, int& toRemove);

    std::set<std::string> filter_[kAssetTypeCount];   // selected filter tags per media tab (OR)
    std::map<int, std::string> addText_;              // per-asset "+ add tag" in-progress text
    std::set<int> selected_[kAssetTypeCount];   // selected asset ids per media tab (transient)
    int           anchor_[kAssetTypeCount];     // Shift-range pivot (last-clicked id); -1 = none (set in ctor)

    int  renamingId_ = -1;        // asset id being inline-renamed (one at a time); -1 = none
    char renameBuf_[512] = {0};   // rename text buffer
    bool renameFocus_ = false;    // request keyboard focus on the rename field's first frame
};

} // namespace oss
```

- [ ] **Step 2: Add the includes + anon-namespace statics in `AssetsPanel.cpp`**

In `src/ui/AssetsPanel.cpp`, add two includes near the existing ones:

```cpp
#include "core/AssetTree.h"
#include "core/PathUtil.h"
```

Then, inside the existing anonymous `namespace { … }` (which already holds `baseLabel` and `pushTagColors`), add these two helpers:

```cpp
// Depth-first leaf ids in render order (folders before files at each level) -> drives Shift-range.
void flattenLeaves(const AssetTreeNode& node, std::vector<int>& out) {
    for (const AssetTreeNode& f : node.folders) flattenLeaves(f, out);
    for (const Asset* a : node.files) out.push_back(a->id);
}

// Index of `id` in the flattened leaf order, or -1 if absent.
int indexOfLeaf(const std::vector<int>& order, int id) {
    for (std::size_t k = 0; k < order.size(); ++k) if (order[k] == id) return (int)k;
    return -1;
}
```

- [ ] **Step 3: Replace `drawTab` with the tree-building version**

Replace the entire existing `AssetsPanel::drawTab(…)` function body with:

```cpp
void AssetsPanel::drawTab(AssetLibrary& lib, AssetType type, const char* noun,
                          const std::vector<std::string>& filters) {
    std::set<std::string>& filterSet = filter_[(int)type];
    std::set<int>&         selSet    = selected_[(int)type];
    int&                   anchor    = anchor_[(int)type];

    // --- Tag-filter toolbar: this tab's tags as colored toggle buttons (left-click filter,
    //     right-click recolor). ---
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
            if (ImGui::ColorPicker4("##pick", &c.x, ImGuiColorEditFlags_NoAlpha))
                lib.setTagColor(tag, c);   // tag colors are RGB-only; alpha is unused
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    if (!filterSet.empty()) {
        if (!tabTags.empty()) ImGui::SameLine();
        if (ImGui::SmallButton("clear filter")) filterSet.clear();
    }

    // --- Selection status line: count + clear + a hint. ---
    if (!selSet.empty()) {
        ImGui::Text("%d selected", (int)selSet.size());
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear selection")) { selSet.clear(); anchor = -1; }
        ImGui::SameLine();
        ImGui::TextDisabled("(click=select, Ctrl/Cmd=toggle, Shift=range; double-click a name to rename)");
    }

    // Filtered rows -> folder tree -> flattened leaf order (drives Shift-range).
    std::vector<const Asset*> rows;
    for (const Asset* a : lib.byType(type)) {
        if (!filterSet.empty()) {
            bool match = false;
            for (const std::string& t : a->tags) if (filterSet.count(t)) { match = true; break; }
            if (!match) continue;
        }
        rows.push_back(a);
    }
    AssetTreeNode tree = buildAssetTree(rows);
    std::vector<int> leafOrder;
    flattenLeaves(tree, leafOrder);
    const bool singleRoot = tree.folders.size() == 1;

    int toRemove = -1;
    if (ImGui::BeginTable("##assettbl", 4,
            ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Path", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Tags", ImGuiTableColumnFlags_WidthFixed, 220.0f);
        ImGui::TableSetupColumn("##act", ImGuiTableColumnFlags_WidthFixed, 30.0f);
        ImGui::TableHeadersRow();

        drawTreeNode(lib, tree, /*rootLevel*/true, singleRoot, selSet, anchor, leafOrder,
                     noun, filters, toRemove);

        ImGui::EndTable();
    }

    std::string addLabel = std::string("+ Add ") + noun;
    if (ImGui::Button(addLabel.c_str())) lib.add(type, "", "");
    ImGui::SameLine();
    if (ImGui::Button("Add files...")) {
        for (const std::string& path : openMultipleFileDialog(noun, "Media", filters))
            lib.add(type, baseLabel(path), path);
    }
    if (toRemove >= 0) {
        lib.remove(toRemove);
        addText_.erase(toRemove);
        selSet.erase(toRemove);
        if (anchor == toRemove)       anchor = -1;        // anchor's row is gone
        if (renamingId_ == toRemove)  renamingId_ = -1;   // stop renaming a removed row
    }
}
```

- [ ] **Step 4: Add the `drawTreeNode` recursion**

Add this new method to `src/ui/AssetsPanel.cpp` (after `drawTab`):

```cpp
void AssetsPanel::drawTreeNode(AssetLibrary& lib, const AssetTreeNode& node, bool rootLevel,
                               bool singleRoot, std::set<int>& selSet, int& anchor,
                               const std::vector<int>& leafOrder, const char* noun,
                               const std::vector<std::string>& filters, int& toRemove) {
    for (const AssetTreeNode& f : node.folders) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanFullWidth;
        if (rootLevel && singleRoot) flags |= ImGuiTreeNodeFlags_DefaultOpen;   // open the lone root
        // The label IS the node id: the joined path is unique per tree position and stable across
        // the per-frame rebuild, so ImGui remembers each folder's open/closed state.
        bool open = ImGui::TreeNodeEx(f.name.c_str(), flags);                   // cols 1-3 stay empty
        if (open) {
            drawTreeNode(lib, f, /*rootLevel*/false, singleRoot, selSet, anchor, leafOrder,
                         noun, filters, toRemove);
            ImGui::TreePop();
        }
    }
    for (const Asset* a : node.files)
        drawAssetLeaf(lib, a, selSet, anchor, leafOrder, noun, filters, toRemove);
}
```

- [ ] **Step 5: Add the `drawAssetLeaf` row renderer**

Add this new method to `src/ui/AssetsPanel.cpp` (after `drawTreeNode`). The Path/Tags/✕ columns are copied verbatim from the old editable row (including the broadcast-tagging); only column 0 is new (leaf node + selection + rename):

```cpp
void AssetsPanel::drawAssetLeaf(AssetLibrary& lib, const Asset* a, std::set<int>& selSet,
                                int& anchor, const std::vector<int>& leafOrder, const char* noun,
                                const std::vector<std::string>& filters, int& toRemove) {
    const ImGuiIO& io = ImGui::GetIO();
    int id = a->id;
    ImGui::PushID(id);
    ImGui::TableNextRow();
    const bool isSel = selSet.count(id) > 0;
    if (isSel)
        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                               ImGui::GetColorU32(ImVec4(0.26f, 0.45f, 0.62f, 0.45f)));

    // --- Column 0: the leaf node (selection + double-click rename), or the inline rename field. ---
    ImGui::TableSetColumnIndex(0);
    if (renamingId_ == id) {
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (renameFocus_) { ImGui::SetKeyboardFocusHere(); renameFocus_ = false; }
        bool enter = ImGui::InputText("##rename", renameBuf_, sizeof(renameBuf_),
                                      ImGuiInputTextFlags_EnterReturnsTrue);
        if (enter) {                                          // Enter commits
            lib.setLabel(id, renameBuf_); renamingId_ = -1;
        } else if (ImGui::IsItemDeactivated()) {              // focus left the field
            if (!ImGui::IsKeyPressed(ImGuiKey_Escape)) lib.setLabel(id, renameBuf_);  // click-away commits; Esc cancels
            renamingId_ = -1;
        }
    } else {
        std::string base = fileBaseName(a->path);
        std::string disp = !a->label.empty() ? a->label
                         : (!base.empty() ? base : std::string("(unnamed)"));
        ImGui::TreeNodeEx(disp.c_str(),
            ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen |
            (isSel ? ImGuiTreeNodeFlags_Selected : 0));
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            renamingId_ = id;
            std::snprintf(renameBuf_, sizeof(renameBuf_), "%s", a->label.c_str());
            renameFocus_ = true;
        } else if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
            if (io.KeyShift && anchor != -1) {                // range over the visible leaves
                int ai = indexOfLeaf(leafOrder, anchor), yi = indexOfLeaf(leafOrder, id);
                if (ai >= 0 && yi >= 0) {
                    int lo = ai < yi ? ai : yi, hi = ai < yi ? yi : ai;
                    for (int k = lo; k <= hi; ++k) selSet.insert(leafOrder[(std::size_t)k]);
                } else { selSet.clear(); selSet.insert(id); }                       // stale anchor -> single
            } else if (io.KeyCtrl || io.KeySuper) {           // toggle one
                if (isSel) selSet.erase(id); else selSet.insert(id);
            } else {                                          // plain click -> select only this
                selSet.clear(); selSet.insert(id);
            }
            anchor = id;
        }
    }

    // --- Column 1: path + Browse. ---
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

    // --- Column 2: tag chips (+ remove) and an add box; both broadcast to a selection. ---
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
        if (selSet.count(id)) { for (int sid : selSet) lib.addTag(sid, addbuf); }  // broadcast to selection
        else                    lib.addTag(id, addbuf);
        s.clear();
    } else {
        s = addbuf;                 // persist in-progress typing across frames
    }
    if (tagToRemove >= 0) {
        std::string tg = a->tags[(std::size_t)tagToRemove];   // copy before any mutation
        if (selSet.count(id)) { for (int sid : selSet) lib.removeTag(sid, tg); }   // broadcast
        else                    lib.removeTag(id, tg);
    }

    // --- Column 3: remove. ---
    ImGui::TableSetColumnIndex(3);
    if (ImGui::Button("x")) toRemove = id;

    ImGui::PopID();
}
```

- [ ] **Step 6: Build and smoke-test**

Run: `cmake --build build -j`
Expected: clean build of `shader_streamer` (and all targets), no warnings about the changed file.

Then `./build/shader_streamer` and check (View → Assets):
- Add several files via "Add files..." in nested folders → they appear grouped under collapsible folders; a long shared absolute prefix shows as one collapsed node; "+ Add audio file" (blank path) appears as a row at the root.
- Root folders are collapsed, except when there's exactly one root folder (then it's open). Expand/collapse persists as you interact.
- Plain-click a file selects only it; Ctrl/Cmd-click toggles; Shift-click selects the range; selected rows are tinted and "N selected · Clear selection" shows.
- Type a tag in a selected row's add box → all selected rows get it; remove a chip on a selected row → all lose it.
- Double-click a file's name → it becomes an editable field; Enter or click-away commits the rename, Esc cancels.
- The ✕ removes a row and drops it from the selection.

- [ ] **Step 7: Commit**

```bash
git add src/ui/AssetsPanel.h src/ui/AssetsPanel.cpp
git commit -m "feat(ui): render the Assets grid as a collapsible folder tree"
```

---

### Task 3: Documentation

**Files:**
- Modify: `CLAUDE.md`
- Modify: `README.md`

- [ ] **Step 1: Update the CLAUDE.md Assets bullet**

In `CLAUDE.md`, find the **Assets / media library** bullet. Its last sentence (added by the multi-select feature) currently reads:

```
  Grid rows are multi-selectable — holding Ctrl/Cmd/Shift renders the rows as read-only span-all-columns
  `Selectable`s (Ctrl/Cmd-click toggles, Shift-click ranges from an `anchor_`); adding/removing a tag on a
  selected row then broadcasts to the whole selection. `selected_`/`anchor_` are transient panel state.
```

Replace those three lines with (the selection model changed — the tree gives each file a dedicated click target):

```
  The grid is a **collapsible folder tree**: the GL-free `core/AssetTree.h` `buildAssetTree` groups the
  filtered rows by each asset's `path` directory (nesting folders, collapsing single-child chains;
  blank/no-folder files sit at the root), and `AssetsPanel` renders it recursively in the table — folders
  are `TreeNodeEx(SpanFullWidth)` nodes (root folders collapsed unless there's exactly one), files are
  `Leaf | NoTreePushOnOpen` nodes. A file node is the multi-select click target (plain-click selects only
  it, Ctrl/Cmd toggles, Shift ranges over the DFS leaf order) and double-click renames it inline; the
  Path/Tags/✕ columns stay editable, and adding/removing a tag on a selected file still broadcasts to the
  whole selection. `buildAssetTree` is unit-tested in `core_tests`; `selected_`/`anchor_`/`renamingId_`
  are transient panel state.
```

- [ ] **Step 2: Update the README Assets subsection**

In `README.md`, find the **### Assets** subsection. Its last sentence (added by the multi-select feature) currently reads:

```markdown
Hold **Ctrl/Cmd** and click rows (or **Shift**-click a range) to select several at once — then typing a
tag in any selected row (or removing one) applies it to every selected row.
```

Replace those two lines with:

```markdown
Files are shown in a **collapsible folder tree** grouped by their path — expand or collapse folders, and
**double-click** a file's name to rename it. Click a file to select it, **Ctrl/Cmd**-click to toggle, or
**Shift**-click a range; then typing a tag in any selected row (or removing one) applies it to every
selected row.
```

- [ ] **Step 3: Verify only docs changed**

Run: `git diff --stat`
Expected: only `CLAUDE.md` and `README.md` changed.

- [ ] **Step 4: Commit**

```bash
git add CLAUDE.md README.md
git commit -m "docs: collapsible folder tree for the Assets grid"
```

---

## Self-Review (plan vs spec)

**Spec coverage:**
- Tree model + single-child collapse + ungrouped-at-root → Task 1 (`buildAssetTree`) + tests.
- `TreeNodeEx` folders (`SpanFullWidth`) + `Leaf | NoTreePushOnOpen` files inside `BeginTable`/`EndTable`, `RowBg` added → Task 2 Steps 3–5.
- Default-open: roots collapsed unless a single root → Task 2 Step 4 (`rootLevel && singleRoot`).
- Selection (plain/Ctrl/Cmd/Shift over DFS leaf order) reusing `selected_`/`anchor_`; broadcast tagging preserved → Task 2 Step 5.
- Double-click rename (`renamingId_`/`renameBuf_`/`renameFocus_`) → Task 2 Steps 1, 5.
- Edge cases: blank assets visible at root; filter before tree-build; prune selection/anchor/rename on removal → Task 1 (ungrouped) + Task 2 Steps 3, 5.
- Tests: `core_tests` for `buildAssetTree`; UI by build + manual smoke → Task 1 + Task 2 Step 6.
- Docs → Task 3.

**Placeholder scan:** none — every code step shows complete code; every command has an expected result.

**Type consistency:** `AssetTreeNode{name, folders, files}`, `buildAssetTree(const std::vector<const Asset*>&)`, `flattenLeaves`/`indexOfLeaf`, `drawTreeNode`/`drawAssetLeaf` signatures, and `renamingId_`/`renameBuf_`/`renameFocus_` are used identically across the header, the `.cpp`, and the tests.
