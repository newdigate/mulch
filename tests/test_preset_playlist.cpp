#include <doctest/doctest.h>
#include "core/PresetPlaylist.h"
#include "core/PathUtil.h"
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

using namespace oss;
namespace fs = std::filesystem;

// A fixed three-preset folder for the selector tests (no disk access).
static std::vector<std::string> fakeLister(const std::string& dir) {
    if (dir == "/p") return {"/p/a.milk", "/p/b.milk", "/p/c.milk"};
    return {};
}

TEST_CASE("listPresetsInDir: only .milk, case-insensitive, sorted, no subfolders") {
    fs::path dir = fs::temp_directory_path() / "oss_preset_playlist_test";
    fs::remove_all(dir);
    fs::create_directories(dir / "sub");
    for (const char* n : {"b.milk", "A.MILK", "notes.txt"}) std::ofstream(dir / n) << "x";
    std::ofstream(dir / "sub" / "x.milk") << "x";

    std::vector<std::string> files = listPresetsInDir(dir.string());
    REQUIRE(files.size() == 2);
    CHECK(fileBaseName(files[0]) == "A.MILK");
    CHECK(fileBaseName(files[1]) == "b.milk");
    CHECK(files[0] == dir.string() + "/A.MILK");   // paths are dir + "/" + name

    CHECK(listPresetsInDir((dir / "missing").string()).empty());
    CHECK(listPresetsInDir("").empty());
    fs::remove_all(dir);
}

TEST_CASE("indexOfPreset matches by file name; stepPresetIndex wraps") {
    std::vector<std::string> f = {"/p/a.milk", "/p/b.milk", "/p/c.milk"};
    CHECK(indexOfPreset(f, "/p/b.milk") == 1);
    CHECK(indexOfPreset(f, "C:\\p\\c.milk") == 2);   // separators differ, name matches
    CHECK(indexOfPreset(f, "/p/zzz.milk") == -1);
    CHECK(indexOfPreset(f, "") == -1);

    CHECK(stepPresetIndex(0, -1, 3) == 2);
    CHECK(stepPresetIndex(2, +1, 3) == 0);
    CHECK(stepPresetIndex(1, +1, 3) == 2);
    CHECK(stepPresetIndex(0, +1, 0) == -1);   // empty list
}

TEST_CASE("syncedPresetStep advances every N bars and is stateless") {
    CHECK(syncedPresetStep(0.0, 4) == 0);
    CHECK(syncedPresetStep(3.99, 4) == 0);
    CHECK(syncedPresetStep(4.0, 4) == 1);
    CHECK(syncedPresetStep(9.0, 4) == 2);
    CHECK(syncedPresetStep(-0.5, 4) == -1);
    CHECK(syncedPresetStep(2.5, 0) == 2);      // N < 1 is treated as 1
    CHECK(syncedPresetStep(4.0, 4) == syncedPresetStep(4.0, 4));
}

TEST_CASE("syncedPresetIndex: sequential is a positive modulo; shuffle is deterministic and in range") {
    CHECK(syncedPresetIndex(0, 3, false, kPresetShuffleSeed) == 0);
    CHECK(syncedPresetIndex(4, 3, false, kPresetShuffleSeed) == 1);
    CHECK(syncedPresetIndex(-1, 3, false, kPresetShuffleSeed) == 2);
    CHECK(syncedPresetIndex(7, 0, false, kPresetShuffleSeed) == 0);   // empty list

    std::set<int> seen;
    for (long long s = -50; s <= 50; ++s) {
        int i = syncedPresetIndex(s, 5, true, kPresetShuffleSeed);
        CHECK(i >= 0);
        CHECK(i < 5);
        CHECK(i == syncedPresetIndex(s, 5, true, kPresetShuffleSeed));   // same in, same out
        seen.insert(i);
    }
    CHECK(seen.size() >= 3);   // it actually shuffles
}

TEST_CASE("PresetSelector: an incoming change loads once") {
    PresetSelector sel(&fakeLister);
    PresetSelectorInput in;
    in.incoming = "/p/b.milk";
    CHECK(sel.update(in));
    CHECK(sel.current() == "/p/b.milk");
    CHECK(sel.count() == 3);
    CHECK(sel.index() == 1);
    CHECK_FALSE(sel.update(in));   // unchanged -> nothing to do
}

TEST_CASE("PresetSelector: next / prev wrap, and the written-back path does not reload") {
    PresetSelector sel(&fakeLister);
    PresetSelectorInput in;
    in.incoming = "/p/b.milk";
    sel.update(in);

    in.button = 1;                       // next
    CHECK(sel.update(in));
    CHECK(sel.current() == "/p/c.milk");

    in.button = -1;
    in.incoming = sel.current();         // the node wrote the path back into the field
    CHECK_FALSE(sel.update(in));

    in.button = 1;                       // next wraps to the first
    CHECK(sel.update(in));
    CHECK(sel.current() == "/p/a.milk");

    in.incoming = sel.current();
    in.button = 0;                       // prev wraps to the last
    CHECK(sel.update(in));
    CHECK(sel.current() == "/p/c.milk");
}

TEST_CASE("PresetSelector: a step holds against an unchanged edge value") {
    PresetSelector sel(&fakeLister);
    PresetSelectorInput in;
    in.incoming = "/p/a.milk";           // an edge keeps sending this
    CHECK(sel.update(in));
    in.button = 1;
    CHECK(sel.update(in));
    CHECK(sel.current() == "/p/b.milk");
    in.button = -1;
    CHECK_FALSE(sel.update(in));         // not snapped back to a.milk
    CHECK(sel.current() == "/p/b.milk");
}

TEST_CASE("PresetSelector: buttons and sync are no-ops without a folder") {
    PresetSelector sel(&fakeLister);
    PresetSelectorInput in;
    in.button = 1;
    in.sync = true; in.playing = true; in.bars = 8.0; in.everyNBars = 1;
    CHECK_FALSE(sel.update(in));
    in.bars = 9.0;
    CHECK_FALSE(sel.update(in));
    CHECK(sel.current().empty());
    CHECK(sel.index() == -1);
}

TEST_CASE("PresetSelector: random never repeats the current preset when there is a choice") {
    for (unsigned rv = 0; rv < 10; ++rv) {
        PresetSelector sel(&fakeLister);
        PresetSelectorInput in;
        in.incoming = "/p/a.milk";
        sel.update(in);
        in.button = 2;
        in.randomValue = rv;
        CHECK(sel.update(in));
        CHECK(sel.current() != "/p/a.milk");
    }
}

TEST_CASE("PresetSelector: sync primes, then switches on a bar boundary to an absolute position") {
    PresetSelector sel(&fakeLister);
    PresetSelectorInput in;
    in.incoming = "/p/c.milk";
    in.sync = true; in.playing = true; in.everyNBars = 1;

    in.bars = 0.5;
    CHECK(sel.update(in));               // the incoming load only; sync just primes
    CHECK(sel.current() == "/p/c.milk");

    in.bars = 0.9;
    CHECK_FALSE(sel.update(in));         // same step

    in.bars = 1.1;                       // step 1 -> files[1]
    CHECK(sel.update(in));
    CHECK(sel.current() == "/p/b.milk");

    in.button = 0; in.bars = 1.6;        // prev by hand inside the step
    CHECK(sel.update(in));
    CHECK(sel.current() == "/p/a.milk");
    in.button = -1; in.bars = 1.9;
    CHECK_FALSE(sel.update(in));         // holds until the next boundary
    CHECK(sel.current() == "/p/a.milk");

    in.bars = 2.0;                       // step 2 -> files[2]
    CHECK(sel.update(in));
    CHECK(sel.current() == "/p/c.milk");

    in.bars = 0.2;                       // seek back: step 0 -> files[0]
    CHECK(sel.update(in));
    CHECK(sel.current() == "/p/a.milk");
}

TEST_CASE("PresetSelector: sync ignores a stopped transport and re-primes on play") {
    PresetSelector sel(&fakeLister);
    PresetSelectorInput in;
    in.incoming = "/p/a.milk";
    in.sync = true; in.everyNBars = 1;
    in.playing = true;  in.bars = 0.1; sel.update(in);   // primes at step 0
    in.playing = false; in.bars = 5.3;
    CHECK_FALSE(sel.update(in));                          // stopped: no switch
    in.playing = true;
    CHECK_FALSE(sel.update(in));                          // re-primes at step 5
    in.bars = 6.0;                                        // step 6 -> files[0], already current
    CHECK_FALSE(sel.update(in));
    in.bars = 7.0;                                        // step 7 -> files[1]
    CHECK(sel.update(in));
    CHECK(sel.current() == "/p/b.milk");
}
