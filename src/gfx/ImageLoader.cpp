#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#include "gfx/ImageLoader.h"

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

} // namespace oss
