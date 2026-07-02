#include <doctest/doctest.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#include "gfx/ImageLoader.h"
#include <cstdio>
#include <string>
#include <vector>

using namespace oss;

TEST_CASE("loadImage decodes RGBA and flips to bottom-up rows") {
    // Author a 4x2 image: top row red, bottom row green (as written, top-down).
    const int W = 4, H = 2;
    std::vector<unsigned char> src((std::size_t)W * H * 4);
    for (int x = 0; x < W; ++x) {
        std::size_t top = (std::size_t)(0 * W + x) * 4;   // row 0 = top
        std::size_t bot = (std::size_t)(1 * W + x) * 4;   // row 1 = bottom
        src[top+0]=255; src[top+1]=0;   src[top+2]=0; src[top+3]=255;   // red
        src[bot+0]=0;   src[bot+1]=255; src[bot+2]=0; src[bot+3]=255;   // green
    }
    std::string path = "test_image_loader_fixture.png";
    REQUIRE(stbi_write_png(path.c_str(), W, H, 4, src.data(), W * 4) != 0);

    std::string err;
    ImageData img = loadImage(path, err);
    std::remove(path.c_str());

    REQUIRE(img.ok());
    CHECK(img.width  == W);
    CHECK(img.height == H);
    // Vertical flip: returned row 0 is the file's BOTTOM row -> green.
    CHECK(img.rgba[0] == 0);    CHECK(img.rgba[1] == 255); CHECK(img.rgba[2] == 0);
    // Returned row 1 is the file's TOP row -> red.
    std::size_t r1 = (std::size_t)(1 * W + 0) * 4;
    CHECK(img.rgba[r1+0] == 255); CHECK(img.rgba[r1+1] == 0); CHECK(img.rgba[r1+2] == 0);
}

TEST_CASE("loadImage fails cleanly on an empty or missing path") {
    std::string err;
    CHECK_FALSE(loadImage("", err).ok());
    CHECK(err == "empty path");
    err.clear();
    CHECK_FALSE(loadImage("no_such_file_zzz.png", err).ok());
    CHECK_FALSE(err.empty());
}
