#pragma once
#include <string>
#include <vector>

namespace oss {

// Decoded image pixels. GL-free (the caller uploads to a texture). Rows are stored
// bottom-up to match the app's existing texture convention (VideoDecoder frames are
// bottom-up and display upright through the same shaders / Output blit).
struct ImageData {
    std::vector<unsigned char> rgba;   // width*height*4, row-major, bottom-up
    int width  = 0;
    int height = 0;
    bool ok() const { return width > 0 && height > 0 && !rgba.empty(); }
};

// Decode `path` to RGBA8. On failure returns an ImageData with ok()==false and fills
// `err` with a reason. An empty path is a (silent) failure with err = "empty path".
ImageData loadImage(const std::string& path, std::string& err);

// List the image files directly in `dir` (non-recursive), sorted ascending by path.
// Filters to the extensions loadImage decodes. Returns empty on a missing/unreadable dir
// (never throws).
std::vector<std::string> listImagesInDir(const std::string& dir);

} // namespace oss
