#pragma once
#include <glad/gl.h>
#include <string>
#include "core/Node.h"
#include "gfx/ImageLoader.h"

namespace oss {

// Loads one still image (from an Image asset path) and publishes it as a TexRef.
// The load happens once, only when the path changes; every frame it republishes the
// same texture. Animate it downstream (Kaleidoscope, World Transform, Compositor).
class ImageStreamerNode : public Node {
public:
    ImageStreamerNode() : Node("Image Streamer") {
        addAssetInput("file", AssetType::Image);
        addOutput("image", PortType::Texture);
    }
    ~ImageStreamerNode() override { if (tex_) glDeleteTextures(1, &tex_); }

    void initGL() override {}   // texture is allocated lazily on first load

    void evaluate(EvalContext& ctx) override {
        const std::string& path = ctx.in<std::string>(0);
        if (path != path_) { path_ = path; load(path); }
        ctx.out<TexRef>(0, haveTex_ ? TexRef{ tex_, w_, h_ } : TexRef{});
    }

    std::string statusLine() const override { return status_; }

private:
    void load(const std::string& path) {
        haveTex_ = false;
        if (path.empty()) { status_.clear(); return; }
        std::string err;
        ImageData img = loadImage(path, err);
        if (!img.ok()) { status_ = "load failed: " + err; return; }

        if (!tex_) glGenTextures(1, &tex_);
        glBindTexture(GL_TEXTURE_2D, tex_);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, img.width, img.height, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, img.rgba.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);

        w_ = img.width; h_ = img.height; haveTex_ = true;
        status_ = std::to_string(w_) + "x" + std::to_string(h_);
    }

    GLuint tex_ = 0;
    int    w_ = 0, h_ = 0;
    bool   haveTex_ = false;
    std::string path_;
    std::string status_;
};

} // namespace oss
