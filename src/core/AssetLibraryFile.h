#pragma once
#include <map>
#include <sstream>
#include <string>
#include <vector>
#include <glm/vec4.hpp>
#include "core/AssetLibrary.h"   // Asset / AssetLibrary (GL-free)

namespace oss {

// Append the asset + tag-color block (asset/alabel/apath/atag/tagcolor lines) for `assets`/`colors`
// to `out`. Shared by ProjectFile (legacy embedded read/write) and the .osslib codec.
void appendAssetBlock(std::string& out, const std::vector<Asset>& assets,
                      const std::map<std::string, glm::vec4>& colors);

// Handle one asset-block keyword line during a line-by-line parse: `ls` is positioned just after
// `kw`. Returns true and consumes `ls` if `kw` is an asset-block keyword (updating assets/colors/
// curAsset, where curAsset tracks the asset the most recent `asset` line opened); false otherwise.
bool parseAssetBlockLine(const std::string& kw, std::istringstream& ls,
                         std::vector<Asset>& assets, std::map<std::string, glm::vec4>& colors,
                         Asset*& curAsset);

// The standalone library file: header `oss-assetlib 1` then the asset block.
std::string serializeLibrary(const AssetLibrary& lib);
bool        parseLibrary(const std::string& text, AssetLibrary& out);   // false on bad header

} // namespace oss
