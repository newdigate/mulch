#include <doctest/doctest.h>
#include "core/PresetPlaylist.h"
#include "core/PathUtil.h"
#include <filesystem>
#include <fstream>
#include <limits>
#include <set>
#include <string>
#include <vector>

using namespace oss;
namespace fs = std::filesystem;

// Fixed folders for the selector tests (no disk access).
static std::vector<std::string> fakeLister(const std::string& dir) {
    if (dir == "/p")   return {"/p/a.milk", "/p/b.milk", "/p/c.milk"};
    if (dir == "/q")   return {"/q/x.milk", "/q/y.milk", "/q/z.milk"};
    if (dir == "/one") return {"/one/solo.milk"};
    return {};
}

TEST_CASE("listPresetsInDir: only .milk, case-insensitive, sorted, no subfolders") {
    fs::path dir = fs::temp_directory_path() / "oss_preset_playlist_test";
    fs::remove_all(dir);
    fs::create_directories(dir / "sub");
    for (const char* n : {"b.milk", "A.MILK", "notes.txt"}) std::ofstream(dir / n) << "x";
    std::ofstream(dir / "sub" / "x.milk") << "x";

    const std::string base = dir.string();
    std::vector<std::string> files   = listPresetsInDir(base);
    std::vector<std::string> missing = listPresetsInDir((dir / "missing").string());
    fs::remove_all(dir);                           // clean up before asserting: a failure leaks nothing

    REQUIRE(files.size() == 2);
    CHECK(fileBaseName(files[0]) == "A.MILK");
    CHECK(fileBaseName(files[1]) == "b.milk");
    CHECK(files[0] == base + "/A.MILK");           // paths are dir + "/" + name
    CHECK(missing.empty());
    CHECK(listPresetsInDir("").empty());
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

    // Non-finite / out-of-range positions must not hit an undefined double -> long long cast.
    CHECK(syncedPresetStep(std::numeric_limits<double>::infinity(), 4) == 0);
    CHECK(syncedPresetStep(-std::numeric_limits<double>::infinity(), 4) == 0);
    CHECK(syncedPresetStep(std::numeric_limits<double>::quiet_NaN(), 4) == 0);
    CHECK(syncedPresetStep(1e300, 4) == 0);
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

TEST_CASE("PresetSelector: an out-of-range button is ignored") {
    PresetSelector sel(&fakeLister);
    PresetSelectorInput in;
    in.incoming = "/p/a.milk";
    sel.update(in);
    in.button = 7;
    CHECK_FALSE(sel.update(in));
    CHECK(sel.current() == "/p/a.milk");
}

TEST_CASE("PresetSelector: a single-preset folder never reports a change from buttons or sync") {
    PresetSelector sel(&fakeLister);
    PresetSelectorInput in;
    in.incoming = "/one/solo.milk";
    CHECK(sel.update(in));
    CHECK(sel.count() == 1);
    CHECK(sel.index() == 0);
    for (int b = 0; b <= 2; ++b) { in.button = b; in.randomValue = 5; CHECK_FALSE(sel.update(in)); }
    in.button = -1;
    in.sync = true; in.playing = true; in.everyNBars = 1;
    in.bars = 0.5; CHECK_FALSE(sel.update(in));
    in.bars = 1.5; CHECK_FALSE(sel.update(in));
    CHECK(sel.current() == "/one/solo.milk");
}

TEST_CASE("PresetSelector: changing `bars` re-primes instead of switching") {
    PresetSelector sel(&fakeLister);
    PresetSelectorInput in;
    in.incoming = "/p/a.milk";
    in.sync = true; in.playing = true; in.everyNBars = 4;
    in.bars = 10.0;
    sel.update(in);                              // primes at step 2 (10 / 4)
    in.everyNBars = 1;                           // the step number would jump 2 -> 10, i.e. files[1]
    CHECK_FALSE(sel.update(in));                 // re-primed: no load storm while dragging the slider
    CHECK(sel.current() == "/p/a.milk");
    in.bars = 11.0;                              // the next real boundary: step 11 -> files[2]
    CHECK(sel.update(in));
    CHECK(sel.current() == "/p/c.milk");
}

TEST_CASE("PresetSelector: a button on a boundary frame wins, and the boundary is consumed") {
    PresetSelector sel(&fakeLister);
    PresetSelectorInput in;
    in.incoming = "/p/a.milk";
    in.sync = true; in.playing = true; in.everyNBars = 1;
    in.bars = 0.5;
    sel.update(in);                              // primes at step 0
    in.button = 0; in.bars = 1.0;                // prev (a -> c) on the frame sync would pick b
    CHECK(sel.update(in));
    CHECK(sel.current() == "/p/c.milk");         // the click is not swallowed
    in.button = -1; in.bars = 1.5;
    CHECK_FALSE(sel.update(in));                 // and step 1 is not reclaimed afterwards
    CHECK(sel.current() == "/p/c.milk");
    in.bars = 2.0;                               // step 2 -> files[2] = c, already current
    CHECK_FALSE(sel.update(in));
    in.bars = 3.0;                               // step 3 -> files[0]
    CHECK(sel.update(in));
    CHECK(sel.current() == "/p/a.milk");
}

TEST_CASE("PresetSelector: a pick in another folder on a boundary frame is not overridden") {
    PresetSelector sel(&fakeLister);
    PresetSelectorInput in;
    in.incoming = "/p/a.milk";
    in.sync = true; in.playing = true; in.everyNBars = 1;
    in.bars = 0.5;
    sel.update(in);
    in.incoming = "/q/x.milk"; in.bars = 1.0;    // a pick on a boundary frame; sync alone would choose files[1]
    CHECK(sel.update(in));
    CHECK(sel.current() == "/q/x.milk");         // the pick wins
    CHECK(sel.count() == 3);
    CHECK(sel.index() == 0);
    in.bars = 1.5;
    CHECK_FALSE(sel.update(in));                 // and step 1 is not reclaimed afterwards
    CHECK(sel.current() == "/q/x.milk");
    in.bars = 2.0;                               // step 2 -> /q files[2]
    CHECK(sel.update(in));
    CHECK(sel.current() == "/q/z.milk");
    CHECK(sel.index() == 2);
}

TEST_CASE("PresetSelector: shuffle replays the same sequence after a seek and in a fresh selector") {
    auto run = [](std::vector<std::string>& out) {
        PresetSelector sel(&fakeLister);
        PresetSelectorInput in;
        in.incoming = "/p/a.milk";
        in.sync = true; in.playing = true; in.everyNBars = 1; in.shuffle = true;
        for (int pass = 0; pass < 2; ++pass) {
            in.bars = 0.5;                       // (re)start; the second pass is a seek back
            sel.update(in);
            for (int b = 1; b <= 12; ++b) { in.bars = b + 0.5; sel.update(in); out.push_back(sel.current()); }
        }
    };
    std::vector<std::string> seq;
    run(seq);
    REQUIRE(seq.size() == 24);
    for (int i = 0; i < 12; ++i) CHECK(seq[(std::size_t)i] == seq[(std::size_t)i + 12]);
    std::vector<std::string> again;
    run(again);
    CHECK(seq == again);                         // fixed seed: identical in every session
}

TEST_CASE("PresetSelector: sub-1 `bars` values all mean 1 and do not re-prime each other") {
    PresetSelector sel(&fakeLister);
    PresetSelectorInput in;
    in.incoming = "/p/a.milk";
    in.sync = true; in.playing = true;
    in.everyNBars = 0;  in.bars = 0.5; sel.update(in);      // primes at step 0 with N = 1
    in.everyNBars = -3; in.bars = 1.0;                      // still N = 1: a real boundary, not a re-prime
    CHECK(sel.update(in));
    CHECK(sel.current() == "/p/b.milk");
}

TEST_CASE("PresetSelector: the written-back path after a sync switch does not reload") {
    PresetSelector sel(&fakeLister);
    PresetSelectorInput in;
    in.incoming = "/p/a.milk";
    in.sync = true; in.playing = true; in.everyNBars = 1;
    in.bars = 0.5;
    sel.update(in);
    in.bars = 1.0;
    CHECK(sel.update(in));
    CHECK(sel.current() == "/p/b.milk");
    in.incoming = sel.current();                 // the node wrote it back into the unconnected field
    in.bars = 1.2;
    CHECK_FALSE(sel.update(in));
    CHECK(sel.current() == "/p/b.milk");
}

TEST_CASE("shortenForStatus keeps short text, truncates long text with ASCII dots, flattens newlines") {
    CHECK(shortenForStatus("abc", 10) == "abc");
    CHECK(shortenForStatus("abcdefghij", 10) == "abcdefghij");
    CHECK(shortenForStatus("abcdefghijk", 10) == "abcdefg...");
    CHECK(shortenForStatus("abcdefghijk", 10).size() == 10);
    CHECK(shortenForStatus("line1\nline2\r\nline3", 40) == "line1 line2  line3");
    CHECK(shortenForStatus("abcdef", 2) == "ab");                 // max below 3: a plain cut, no dots
}

TEST_CASE("presetDisplayName strips only a real .milk extension and shortens long names") {
    CHECK(presetDisplayName("/p/Geiss - Cosmic Dust.milk") == "Geiss - Cosmic Dust");
    CHECK(presetDisplayName("/p/UPPER.MILK") == "UPPER");
    CHECK(presetDisplayName("/p/notes.txt") == "notes.txt");      // not mangled
    CHECK(presetDisplayName("/p/milk") == "milk");
    CHECK(presetDisplayName("") == "");
    std::string longName(120, 'x');
    std::string shown = presetDisplayName("/p/" + longName + ".milk");
    CHECK(shown.size() == kPresetStatusNameMax);
    CHECK(shown.substr(shown.size() - 3) == "...");
}
