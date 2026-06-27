#include "core/AssetLibrary.h"
#include <algorithm>
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
}

void AssetLibrary::load(std::vector<Asset> assets) {
    assets_ = std::move(assets);
    int maxId = 0;
    for (const Asset& a : assets_) maxId = std::max(maxId, a.id);
    nextId_ = maxId + 1;
}

} // namespace oss
