#include "core/AssetLibraryFile.h"
#include "core/TextCodec.h"

namespace oss {

void appendAssetBlock(std::string& out, const std::vector<Asset>& assets,
                      const std::map<std::string, glm::vec4>& colors) {
    for (const Asset& a : assets) {
        out += "asset " + std::to_string(a.id) + " " + std::to_string((int)a.type) + "\n";
        if (!a.label.empty()) out += "alabel " + escape(a.label) + "\n";
        if (!a.path.empty())  out += "apath "  + escape(a.path)  + "\n";
        for (const std::string& t : a.tags) out += "atag " + escape(t) + "\n";
    }
    for (const auto& kv : colors) {
        const glm::vec4& c = kv.second;
        out += "tagcolor " + std::to_string(c.x) + " " + std::to_string(c.y) + " "
             + std::to_string(c.z) + " " + std::to_string(c.w) + " " + escape(kv.first) + "\n";
    }
}

bool parseAssetBlockLine(const std::string& kw, std::istringstream& ls,
                         std::vector<Asset>& assets, std::map<std::string, glm::vec4>& colors,
                         Asset*& curAsset) {
    if (kw == "asset") {
        int id, typeInt; ls >> id >> typeInt;
        if (ls.fail()) return true;                        // malformed asset line -> consumed, skipped
        if (typeInt < 0) typeInt = 0;
        if (typeInt >= kAssetTypeCount) typeInt = kAssetTypeCount - 1;
        assets.push_back(Asset{id, (AssetType)typeInt, "", ""});
        curAsset = &assets.back();
        return true;
    } else if (kw == "alabel") { if (curAsset) curAsset->label = unescape(restOfLine(ls)); return true; }
    else if (kw == "apath")    { if (curAsset) curAsset->path  = unescape(restOfLine(ls)); return true; }
    else if (kw == "atag")     { if (curAsset) curAsset->tags.push_back(unescape(restOfLine(ls))); return true; }
    else if (kw == "tagcolor") {
        float r, g, b, a; ls >> r >> g >> b >> a;
        if (ls.fail()) return true;                        // malformed -> consumed, skipped
        colors[unescape(restOfLine(ls))] = glm::vec4(r, g, b, a);
        return true;
    }
    return false;   // not an asset-block keyword
}

std::string serializeLibrary(const AssetLibrary& lib) {
    std::string out = "oss-assetlib 1\n";
    appendAssetBlock(out, lib.all(), lib.tagColors());
    return out;
}

bool parseLibrary(const std::string& text, AssetLibrary& out) {
    std::istringstream in(text);
    std::string line;
    if (!std::getline(in, line) || line.rfind("oss-assetlib", 0) != 0) return false;
    std::vector<Asset> assets;
    std::map<std::string, glm::vec4> colors;
    Asset* curAsset = nullptr;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        std::istringstream ls(line);
        std::string kw; ls >> kw;
        parseAssetBlockLine(kw, ls, assets, colors, curAsset);   // ignore non-asset keywords
    }
    out.load(std::move(assets));              // adopt: replaces assets, resets nextId_
    out.loadTagColors(std::move(colors));     // adopt: replaces the registry
    return true;
}

} // namespace oss
