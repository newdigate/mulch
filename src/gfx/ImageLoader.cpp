#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#include "gfx/ImageLoader.h"
#include <algorithm>
#include <cctype>
#include <filesystem>

namespace oss {

ImageData loadImage(const std::string& path, std::string& err) {
    ImageData out;
    if (path.empty()) { err = "empty path"; return out; }

    stbi_set_flip_vertically_on_load(1);   // -> bottom-up rows, matching VideoDecoder
    int w = 0, h = 0, comps = 0;
    unsigned char* data = stbi_load(path.c_str(), &w, &h, &comps, 4);   // force RGBA
    if (!data) {
        const char* why = stbi_failure_reason();
        err = why ? why : "decode failed";
        return out;
    }
    out.width  = w;
    out.height = h;
    out.rgba.assign(data, data + (std::size_t)w * h * 4);
    stbi_image_free(data);
    return out;
}

static bool isImageExt(std::string ext) {
    for (char& c : ext) c = (char)std::tolower((unsigned char)c);
    return ext == "png" || ext == "jpg" || ext == "jpeg" || ext == "bmp" ||
           ext == "tga" || ext == "gif" || ext == "hdr" || ext == "psd";
}

std::vector<std::string> listImagesInDir(const std::string& dir) {
    std::vector<std::string> out;
    if (dir.empty()) return out;
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::directory_iterator it(dir, ec), end;
    if (ec) return out;                                  // missing / unreadable
    for (; it != end; it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec) || ec) continue;
        std::string ext = it->path().extension().string();   // e.g. ".PNG"
        if (!ext.empty() && ext[0] == '.') ext.erase(0, 1);
        if (isImageExt(ext)) out.push_back(it->path().string());
    }
    std::sort(out.begin(), out.end());   // all paths share the same parent -> path sort == filename sort
    return out;
}

} // namespace oss
