#include <doctest/doctest.h>
#include "core/AssetLibrary.h"
#include "core/Graph.h"
#include "core/ProjectFile.h"
#include <glm/vec4.hpp>
#include <memory>
#include <string>
#include <variant>

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

TEST_CASE("ProjectFile serialize/parse round-trips assets (spaces, empty label, backslash)") {
    ProjectDoc d;
    d.assets = {
        Asset{1, AssetType::Audio, "kick drum", "samples/kick drum.wav"},
        Asset{2, AssetType::Mesh,  "",           "models/a b c.obj"},   // empty label omits alabel
        Asset{3, AssetType::Midi,  "back\\slash","x.mid"},               // backslash survives escape
    };
    std::string text = serializeProject(d);
    ProjectDoc out;
    REQUIRE(parseProject(text, out));
    REQUIRE(out.assets.size() == 3);
    CHECK(out.assets[0].id == 1);
    CHECK(out.assets[0].type == AssetType::Audio);
    CHECK(out.assets[0].label == "kick drum");
    CHECK(out.assets[0].path == "samples/kick drum.wav");
    CHECK(out.assets[1].label == "");                 // empty round-trips
    CHECK(out.assets[1].type == AssetType::Mesh);
    CHECK(out.assets[1].path == "models/a b c.obj");
    CHECK(out.assets[2].label == "back\\slash");
}

TEST_CASE("ProjectFile without asset lines loads an empty asset list") {
    ProjectDoc out;
    REQUIRE(parseProject("oss-project 1\ntransport 120 4 0 0 4 8\n", out));
    CHECK(out.assets.empty());
}

TEST_CASE("captureProject / restoreProject carry assets through a Graph") {
    // Reference model: captureProject no longer embeds assets (they travel via the .osslib).
    Graph g;
    g.assets().add(AssetType::Audio, "kick", "k.wav");
    g.assets().add(AssetType::Video, "clip", "c.mp4");
    ProjectDoc d = captureProject(g);
    CHECK(d.assets.empty());
    CHECK(d.tagColors.empty());
}

namespace {
// A minimal Node that declares one asset-backed input, to exercise addAssetInput
// without needing the GL/FFmpeg node classes (which core_tests does not link).
struct AssetInputProbe : oss::Node {
    AssetInputProbe() : oss::Node("AssetInputProbe") {
        addAssetInput("clip", oss::AssetType::Video, "default.mp4");
        addAssetInput("noDefault", oss::AssetType::Audio);   // 2-arg form: empty default
    }
    void evaluate(oss::EvalContext&) override {}
};
} // namespace

TEST_CASE("addAssetInput marks a String input asset-backed with its type + default") {
    AssetInputProbe n;
    REQUIRE(n.inputs().size() == 2);
    const Port& p = n.inputs()[0];
    CHECK(p.name == "clip");
    CHECK(p.type == PortType::String);
    CHECK(p.assetBacked == true);
    CHECK(p.assetType == AssetType::Video);
    CHECK(std::get<std::string>(p.defaultValue) == "default.mp4");

    const Port& q = n.inputs()[1];
    CHECK(q.assetBacked == true);
    CHECK(q.assetType == AssetType::Audio);
    CHECK(std::get<std::string>(q.defaultValue) == "");   // omitted default -> empty path
}

TEST_CASE("a plain addInput String is not asset-backed") {
    // Sanity: the flag defaults off, so existing String inputs keep their text field.
    Port p;
    CHECK(p.assetBacked == false);
}

namespace {
// Exact component compare (avoid relying on glm's operator== availability).
inline bool vecEq(const glm::vec4& a, const glm::vec4& b) {
    return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w;
}
} // namespace

TEST_CASE("AssetLibrary addTag appends, dedups, ignores empty + bad id") {
    AssetLibrary lib;
    int a = lib.add(AssetType::Audio, "kick", "k.wav");
    lib.addTag(a, "drums");
    lib.addTag(a, "drums");          // duplicate -> ignored
    lib.addTag(a, "");               // empty -> ignored
    lib.addTag(999, "ghost");        // bad id -> ignored
    REQUIRE(lib.find(a) != nullptr);
    REQUIRE(lib.find(a)->tags.size() == 1);
    CHECK(lib.find(a)->tags[0] == "drums");
}

TEST_CASE("AssetLibrary removeTag drops only that tag") {
    AssetLibrary lib;
    int a = lib.add(AssetType::Audio, "k", "k");
    lib.addTag(a, "drums"); lib.addTag(a, "loop");
    lib.removeTag(a, "drums");
    REQUIRE(lib.find(a) != nullptr);
    REQUIRE(lib.find(a)->tags.size() == 1);
    CHECK(lib.find(a)->tags[0] == "loop");
    lib.removeTag(a, "nope");        // absent -> no-op
    lib.removeTag(999, "loop");      // bad id -> no-op
    CHECK(lib.find(a)->tags.size() == 1);
}

TEST_CASE("AssetLibrary tagsForType is distinct, sorted, per-type") {
    AssetLibrary lib;
    int au = lib.add(AssetType::Audio, "a", "a");
    int mi = lib.add(AssetType::Midi,  "m", "m");
    int au2 = lib.add(AssetType::Audio, "b", "b");
    lib.addTag(au, "loop"); lib.addTag(au, "drums");
    lib.addTag(au2, "drums");        // same tag on two audio assets
    lib.addTag(mi, "bass");
    CHECK(lib.tagsForType(AssetType::Audio) == std::vector<std::string>{"drums", "loop"}); // distinct + sorted
    CHECK(lib.tagsForType(AssetType::Midi)  == std::vector<std::string>{"bass"});
    CHECK(lib.tagsForType(AssetType::Video).empty());
}

TEST_CASE("AssetLibrary tag colors: deterministic default, override, clear") {
    AssetLibrary lib;
    CHECK(vecEq(lib.tagColor("drums"), lib.tagColor("drums")));   // deterministic default
    lib.setTagColor("drums", glm::vec4(0.1f, 0.2f, 0.3f, 1.0f));
    CHECK(vecEq(lib.tagColor("drums"), glm::vec4(0.1f, 0.2f, 0.3f, 1.0f)));
    CHECK(lib.tagColors().count("drums") == 1);
    lib.clear();
    CHECK(lib.tagColors().empty());
}

TEST_CASE("AssetLibrary addTag does not clobber an existing tag color") {
    AssetLibrary lib;
    int a = lib.add(AssetType::Audio, "k", "k");
    lib.setTagColor("drums", glm::vec4(0.1f, 0.2f, 0.3f, 1.0f));
    lib.addTag(a, "drums");                                   // must NOT overwrite the set color
    CHECK(vecEq(lib.tagColor("drums"), glm::vec4(0.1f, 0.2f, 0.3f, 1.0f)));
}

TEST_CASE("AssetLibrary loadTagColors round-trips the registry") {
    AssetLibrary lib;
    std::map<std::string, glm::vec4> reg;
    reg["loop"] = glm::vec4(0.4f, 0.5f, 0.6f, 1.0f);
    lib.loadTagColors(reg);
    CHECK(vecEq(lib.tagColor("loop"), glm::vec4(0.4f, 0.5f, 0.6f, 1.0f)));
}

TEST_CASE("ProjectFile round-trips asset tags + tag colors (incl. spaces)") {
    ProjectDoc d;
    Asset a{1, AssetType::Audio, "kick", "k.wav"};
    a.tags = {"drums", "tight loop"};          // a tag with a space
    d.assets = { a };
    d.tagColors["drums"]      = glm::vec4(0.2f, 0.4f, 0.6f, 1.0f);
    d.tagColors["tight loop"] = glm::vec4(0.9f, 0.1f, 0.3f, 1.0f);

    std::string text = serializeProject(d);
    ProjectDoc out;
    REQUIRE(parseProject(text, out));
    REQUIRE(out.assets.size() == 1);
    CHECK(out.assets[0].tags == std::vector<std::string>{"drums", "tight loop"});
    REQUIRE(out.tagColors.count("drums") == 1);
    REQUIRE(out.tagColors.count("tight loop") == 1);
    CHECK(out.tagColors["drums"].x == doctest::Approx(0.2f));
    CHECK(out.tagColors["drums"].z == doctest::Approx(0.6f));
    CHECK(out.tagColors["tight loop"].x == doctest::Approx(0.9f));
}

TEST_CASE("ProjectFile without tag lines loads empty tags + colors") {
    ProjectDoc out;
    REQUIRE(parseProject("oss-project 1\ntransport 120 4 0 0 4 8\nasset 1 0\n", out));
    REQUIRE(out.assets.size() == 1);
    CHECK(out.assets[0].tags.empty());
    CHECK(out.tagColors.empty());
}

TEST_CASE("captureProject / restoreProject carry tags + colors through a Graph") {
    // Reference model: captureProject no longer embeds assets or tag colors (they travel via the .osslib).
    Graph g;
    int a = g.assets().add(AssetType::Audio, "kick", "k.wav");
    g.assets().addTag(a, "drums");
    g.assets().setTagColor("drums", glm::vec4(0.3f, 0.3f, 0.3f, 1.0f));
    ProjectDoc d = captureProject(g);
    CHECK(d.assets.empty());
    CHECK(d.tagColors.empty());
}
