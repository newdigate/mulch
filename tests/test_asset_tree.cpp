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

TEST_CASE("uniqueAssetFolders lists distinct parent dirs, sorted, bare files dropped") {
    std::vector<Asset> v = {
        {1, AssetType::Image, "a", "media/fire/a.png", {}},
        {2, AssetType::Image, "b", "media/fire/b.png", {}},
        {3, AssetType::Image, "c", "media/rain/c.png", {}},
        {4, AssetType::Image, "x", "other/x.png",      {}},
        {5, AssetType::Image, "y", "y.png",            {}},   // no dir -> dropped
    };
    std::vector<std::string> f = uniqueAssetFolders(ptrs(v));
    REQUIRE(f.size() == 3);
    CHECK(f[0] == "media/fire");
    CHECK(f[1] == "media/rain");
    CHECK(f[2] == "other");
}
