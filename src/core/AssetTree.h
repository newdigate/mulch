#pragma once
#include <algorithm>
#include <string>
#include <utility>
#include <vector>
#include "core/AssetLibrary.h"   // Asset (GL-free)
#include "core/PathUtil.h"

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

// The distinct parent directories of `rows` (each asset's parentDir), dropping assets with
// no directory part, sorted ascending and de-duplicated. Drives the Image Sequencer's folder
// picker (the "image-containing folders").
inline std::vector<std::string> uniqueAssetFolders(const std::vector<const Asset*>& rows) {
    std::vector<std::string> out;
    for (const Asset* a : rows) {
        if (!a) continue;
        std::string dir = parentDir(a->path);
        if (!dir.empty()) out.push_back(std::move(dir));
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

} // namespace oss
