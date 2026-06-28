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
    AssetsPanel() { for (int& a : anchor_) a = -1; }   // anchors start at "none"

    void draw(AssetLibrary& lib, bool* open);

private:
    void drawTab(AssetLibrary& lib, AssetType type, const char* noun,
                 const std::vector<std::string>& filters);

    std::set<std::string> filter_[kAssetTypeCount];   // selected filter tags per media tab (OR)
    std::map<int, std::string> addText_;              // per-asset "+ add tag" in-progress text
    std::set<int> selected_[kAssetTypeCount];   // selected asset ids per media tab (transient)
    int           anchor_[kAssetTypeCount];     // Shift-range pivot (last-clicked id); -1 = none (set in ctor)
};

} // namespace oss
