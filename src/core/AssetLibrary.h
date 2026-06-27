#pragma once
#include <string>
#include <vector>

namespace oss {

// The four Assets-window tabs, in tab order. Persisted as the int 0..3.
enum class AssetType { Audio, Video, Midi, Mesh };
constexpr int kAssetTypeCount = 4;   // number of AssetType values (and Assets-window tabs)

struct Asset {
    int         id = 0;                 // unique within the library; monotonic; never reused
    AssetType   type = AssetType::Audio;
    std::string label;                  // human name (editable; may be empty / duplicated)
    std::string path;                   // file path (editable; may be empty / duplicated)
};

// A per-project media library: media files grouped by type, each with a stable unique id
// plus an editable label and path. GL-free. Owned by Graph and persisted via ProjectFile;
// Phase-2 node controls will reference an asset by id, so ids must be stable across edits
// and preserved verbatim across a save/load.
class AssetLibrary {
public:
    int  add(AssetType type, std::string label, std::string path);   // returns the fresh id
    void remove(int id);                                             // no-op if id is absent

    // Returned pointers are valid only until the next add/remove/load/clear, which may
    // reallocate the underlying vector -- don't cache them across those.
    Asset*       find(int id);                                        // nullptr if absent
    const Asset* find(int id) const;
    void setLabel(int id, std::string label);                        // no-op if id is absent
    void setPath(int id, std::string path);                          // no-op if id is absent

    // All assets of one type in insertion order (drives a tab now, Phase-2 dropdowns later).
    // Same pointer-lifetime caveat as find() -- valid only until the next add/remove/load/clear.
    std::vector<const Asset*> byType(AssetType type) const;

    const std::vector<Asset>& all() const { return assets_; }
    void clear();                                                    // empty; nextId_ -> 1
    void load(std::vector<Asset> assets);                            // adopt; nextId_ = max(id)+1

private:
    std::vector<Asset> assets_;
    int nextId_ = 1;                                                 // monotonic; never reused
};

} // namespace oss
