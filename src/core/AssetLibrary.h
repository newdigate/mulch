#pragma once
#include <map>
#include <string>
#include <vector>
#include <glm/vec4.hpp>

namespace oss {

// The four Assets-window tabs, in tab order. Persisted as the int 0..3.
enum class AssetType { Audio, Video, Midi, Mesh };
constexpr int kAssetTypeCount = 4;   // number of AssetType values (and Assets-window tabs)

struct Asset {
    int         id = 0;                 // unique within the library; monotonic; never reused
    AssetType   type = AssetType::Audio;
    std::string label;                  // human name (editable; may be empty / duplicated)
    std::string path;                   // file path (editable; may be empty / duplicated)
    std::vector<std::string> tags;      // tag names on this asset (insertion order)
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

    // Replace the leading `from` of every asset path that starts with it with `to`. Exact, case-
    // sensitive prefix match across all assets; returns the number changed. No-op if `from` is empty.
    int remapPathPrefix(const std::string& from, const std::string& to);

    // All assets of one type in insertion order (drives a tab now, Phase-2 dropdowns later).
    // Same pointer-lifetime caveat as find() -- valid only until the next add/remove/load/clear.
    std::vector<const Asset*> byType(AssetType type) const;

    const std::vector<Asset>& all() const { return assets_; }
    void clear();                                                    // empty; nextId_ -> 1
    void load(std::vector<Asset> assets);                            // adopt; nextId_ = max(id)+1

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

private:
    std::vector<Asset> assets_;
    int nextId_ = 1;                                                 // monotonic; never reused
    std::map<std::string, glm::vec4> tagColors_;   // tag name -> color
};

} // namespace oss
