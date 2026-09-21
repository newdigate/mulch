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

TEST_CASE("AssetType::Image is the fifth type and round-trips the codec") {
    CHECK((int)AssetType::Image == 4);

    AssetLibrary lib;
    int i = lib.add(AssetType::Image, "Logo", "/m/img/logo.png");

    std::string text = serializeLibrary(lib);
    AssetLibrary out;
    REQUIRE(parseLibrary(text, out));
    const Asset* a = out.find(i);
    REQUIRE(a != nullptr);
    CHECK(a->type == AssetType::Image);
    CHECK(a->label == "Logo");
    CHECK(a->path == "/m/img/logo.png");
}

TEST_CASE("AssetType::Preset is the sixth type and round-trips the codec") {
    CHECK(kAssetTypeCount == 6);
    CHECK((int)AssetType::Preset == 5);       // appended, so older files' type ints are unchanged

    AssetLibrary lib;
    int i = lib.add(AssetType::Preset, "Cosmic Dust", "/m/presets/Geiss - Cosmic Dust.milk");

    AssetLibrary out;
    REQUIRE(parseLibrary(serializeLibrary(lib), out));
    const Asset* a = out.find(i);
    REQUIRE(a != nullptr);
    CHECK(a->type == AssetType::Preset);
    CHECK(a->path == "/m/presets/Geiss - Cosmic Dust.milk");
    CHECK(out.byType(AssetType::Preset).size() == 1);
    CHECK(out.byType(AssetType::Image).empty());
}

TEST_CASE("An unknown (future) asset type int is preserved verbatim; a negative one clamps to 0") {
    AssetLibrary out;
    REQUIRE(parseLibrary("oss-assetlib 1\n"
                         "asset 7 99\napath /m/x.future\n"
                         "asset 8 -3\napath /m/y.neg\n", out));
    const Asset* future = out.find(7);
    const Asset* neg    = out.find(8);
    REQUIRE(future != nullptr);
    REQUIRE(neg != nullptr);
    CHECK(future->path == "/m/x.future");             // the asset survives
    CHECK((int)future->type == 99);                   // not clamped onto a known type
    CHECK(neg->type == AssetType::Audio);             // negative clamps to 0
    for (int t = 0; t < kAssetTypeCount; ++t)         // it shows up in no tab
        for (const Asset* a : out.byType((AssetType)t)) CHECK(a->id != 7);
    CHECK(serializeLibrary(out).find("asset 7 99") != std::string::npos);   // a save writes it back
}
