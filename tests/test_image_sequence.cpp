#include <doctest/doctest.h>
#include "core/ImageSequence.h"
#include "gfx/ImageLoader.h"
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace oss;

TEST_CASE("syncedImageIndex wraps over the beat position") {
    CHECK(syncedImageIndex(0.0, 1.0f, 3) == 0);
    CHECK(syncedImageIndex(0.9, 1.0f, 3) == 0);
    CHECK(syncedImageIndex(1.0, 1.0f, 3) == 1);
    CHECK(syncedImageIndex(2.0, 1.0f, 3) == 2);
    CHECK(syncedImageIndex(3.0, 1.0f, 3) == 0);   // wrap
    CHECK(syncedImageIndex(5.5, 1.0f, 3) == 2);   // floor(5.5)=5 -> 5 % 3 = 2
    CHECK(syncedImageIndex(10.0, 1.0f, 0) == 0);  // no images
    CHECK(syncedImageIndex(-1.0, 1.0f, 3) == 2);  // negative wraps into range
    CHECK(syncedImageIndex(0.0, 0.0f, 3) == 0);   // zero durationBeats -> floor guard, no crash
    CHECK(syncedImageIndex(0.0, -1.0f, 3) == 0);  // negative durationBeats -> floor guard
}

TEST_CASE("listImagesInDir returns sorted image files, ignoring non-images") {
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() / "oss_imgseq_test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    auto touch = [&](const char* n){ std::ofstream(dir / n) << "x"; };
    touch("b.png"); touch("a.png"); touch("c.jpg"); touch("note.txt"); touch("d.PNG");

    std::vector<std::string> got = listImagesInDir(dir.string());
    fs::remove_all(dir);

    REQUIRE(got.size() == 4);   // a.png, b.png, c.jpg, d.PNG  (note.txt excluded)
    CHECK(fs::path(got[0]).filename() == "a.png");
    CHECK(fs::path(got[1]).filename() == "b.png");
    CHECK(fs::path(got[2]).filename() == "c.jpg");
    CHECK(fs::path(got[3]).filename() == "d.PNG");
}

TEST_CASE("listImagesInDir on a missing or empty directory is empty, not an error") {
    CHECK(listImagesInDir("/no/such/dir/oss_zzz").empty());
    CHECK(listImagesInDir("").empty());
}
