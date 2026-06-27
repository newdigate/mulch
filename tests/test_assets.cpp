#include <doctest/doctest.h>
#include "core/AssetLibrary.h"
#include "core/Graph.h"

using namespace oss;

TEST_CASE("AssetLibrary add returns unique increasing ids") {
    AssetLibrary lib;
    int a = lib.add(AssetType::Audio, "kick", "k.wav");
    int b = lib.add(AssetType::Audio, "snare", "s.wav");
    CHECK(a == 1);
    CHECK(b == 2);
    CHECK(b > a);
}

TEST_CASE("AssetLibrary byType filters by type and preserves insertion order") {
    AssetLibrary lib;
    int aud0 = lib.add(AssetType::Audio, "a0", "a0");
    lib.add(AssetType::Mesh, "m0", "m0");
    int aud1 = lib.add(AssetType::Audio, "a1", "a1");
    auto audio = lib.byType(AssetType::Audio);
    REQUIRE(audio.size() == 2);
    CHECK(audio[0]->id == aud0);
    CHECK(audio[1]->id == aud1);
    CHECK(lib.byType(AssetType::Video).empty());
}

TEST_CASE("AssetLibrary find/setLabel/setPath mutate the right asset; no-op on a bad id") {
    AssetLibrary lib;
    int id = lib.add(AssetType::Midi, "old", "old.mid");
    lib.setLabel(id, "new");
    lib.setPath(id, "new.mid");
    REQUIRE(lib.find(id) != nullptr);
    CHECK(lib.find(id)->label == "new");
    CHECK(lib.find(id)->path == "new.mid");
    lib.setLabel(999, "x");          // absent id: must be a harmless no-op
    CHECK(lib.find(999) == nullptr);
}

TEST_CASE("AssetLibrary remove deletes only its id and never reuses ids") {
    AssetLibrary lib;
    int a = lib.add(AssetType::Audio, "a", "a");
    int b = lib.add(AssetType::Audio, "b", "b");
    lib.remove(a);
    CHECK(lib.find(a) == nullptr);
    CHECK(lib.find(b) != nullptr);
    int c = lib.add(AssetType::Audio, "c", "c");
    CHECK(c > b);                     // monotonic; the removed id is not reused
    CHECK(c != a);
    lib.remove(99999);                // absent id: harmless no-op, nothing changes
    CHECK(lib.find(b) != nullptr);
    CHECK(lib.byType(AssetType::Audio).size() == 2);  // a removed earlier; b + c remain
}

TEST_CASE("AssetLibrary clear empties the library") {
    AssetLibrary lib;
    lib.add(AssetType::Audio, "a", "a");
    lib.clear();
    CHECK(lib.all().empty());
}

TEST_CASE("AssetLibrary load adopts ids verbatim and advances nextId past the max") {
    AssetLibrary lib;
    std::vector<Asset> in = {
        Asset{5, AssetType::Audio, "five", "5.wav"},
        Asset{9, AssetType::Mesh,  "nine", "9.obj"},
    };
    lib.load(in);
    REQUIRE(lib.find(5) != nullptr);
    CHECK(lib.find(5)->label == "five");
    CHECK(lib.find(9)->type == AssetType::Mesh);
    int next = lib.add(AssetType::Video, "v", "v.mp4");
    CHECK(next == 10);                // max(id)+1; never collides with a loaded id
}

TEST_CASE("Graph owns an AssetLibrary and clear() empties it") {
    Graph g;
    g.assets().add(AssetType::Audio, "k", "k.wav");
    CHECK(g.assets().all().size() == 1);
    g.clear();
    CHECK(g.assets().all().empty());
}
