#include "core/AssetLibrary.h"
#include "core/ColorHsv.h"
#include <algorithm>
#include <functional>
#include <set>
#include <utility>

namespace oss {

int AssetLibrary::add(AssetType type, std::string label, std::string path) {
    int id = nextId_++;
    assets_.push_back(Asset{id, type, std::move(label), std::move(path)});
    return id;
}

void AssetLibrary::remove(int id) {
    assets_.erase(std::remove_if(assets_.begin(), assets_.end(),
                                 [id](const Asset& a) { return a.id == id; }),
                  assets_.end());
}

Asset* AssetLibrary::find(int id) {
    for (Asset& a : assets_) if (a.id == id) return &a;
    return nullptr;
}
const Asset* AssetLibrary::find(int id) const {
    for (const Asset& a : assets_) if (a.id == id) return &a;
    return nullptr;
}

void AssetLibrary::setLabel(int id, std::string label) {
    if (Asset* a = find(id)) a->label = std::move(label);
}
void AssetLibrary::setPath(int id, std::string path) {
    if (Asset* a = find(id)) a->path = std::move(path);
}

std::vector<const Asset*> AssetLibrary::byType(AssetType type) const {
    std::vector<const Asset*> out;
    for (const Asset& a : assets_) if (a.type == type) out.push_back(&a);
    return out;
}

void AssetLibrary::clear() {
    assets_.clear();
    nextId_ = 1;
    tagColors_.clear();
}

void AssetLibrary::load(std::vector<Asset> assets) {
    assets_ = std::move(assets);
    int maxId = 0;
    for (const Asset& a : assets_) maxId = std::max(maxId, a.id);
    nextId_ = maxId + 1;
}

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

} // namespace oss
