#include <doctest/doctest.h>
#include "core/AssetLibraryFile.h"
#include "core/AssetLibrary.h"

using namespace oss;

TEST_CASE("serializeLibrary -> parseLibrary round-trips assets, tags, and colors") {
    AssetLibrary lib;
    int a = lib.add(AssetType::Audio, "Kick", "/m/drums/kick.wav");
    lib.addTag(a, "drums");
    lib.addTag(a, "to keep");                 // tag with a space
    int b = lib.add(AssetType::Mesh, "Cube", "/m/meshes/cube.obj");
    lib.setTagColor("drums", glm::vec4(0.1f, 0.2f, 0.3f, 1.0f));

    std::string text = serializeLibrary(lib);
    CHECK(text.rfind("oss-assetlib", 0) == 0);   // header

    AssetLibrary out;
    REQUIRE(parseLibrary(text, out));
    REQUIRE(out.all().size() == 2);
    const Asset* ka = out.find(a);
    REQUIRE(ka != nullptr);
    CHECK(ka->label == "Kick");
    CHECK(ka->path  == "/m/drums/kick.wav");
    REQUIRE(ka->tags.size() == 2);
    CHECK(ka->tags[0] == "drums");
    CHECK(ka->tags[1] == "to keep");
    CHECK(out.find(b)->type == AssetType::Mesh);
    glm::vec4 c = out.tagColor("drums");
    CHECK(c.x == doctest::Approx(0.1f));
    CHECK(c.z == doctest::Approx(0.3f));
}

TEST_CASE("parseLibrary rejects a bad header and leaves the library untouched") {
    AssetLibrary out;
    out.add(AssetType::Audio, "keep", "k.wav");
    CHECK_FALSE(parseLibrary("not-a-lib\nasset 1 0\n", out));
    CHECK(out.all().size() == 1);   // unchanged
}
