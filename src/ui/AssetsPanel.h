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
