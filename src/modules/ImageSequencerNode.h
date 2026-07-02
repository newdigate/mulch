#pragma once
#include <glad/gl.h>
#include <string>
#include <vector>
#include "core/Node.h"
#include "core/PathUtil.h"
#include "core/ImageSequence.h"
#include "gfx/ImageLoader.h"

namespace oss {

// Plays a folder of images in sequence, one every `duration` (seconds when free-running,
// beats when synced). The folder is chosen from the Assets library's image folders; the node
// scans it on disk. Holds one GL texture and decodes the next image only when the index
// changes (bounded memory). Mirrors ImageStreamerNode.
class ImageSequencerNode : public Node {
public:
    ImageSequencerNode() : Node("Image Sequencer") {
        addImageFolderInput("folder");
        addInput("duration", PortType::Float, 1.0f, 0.05f, 60.0f);   // seconds or beats
        addInput("sync", PortType::Bool, false);
        addOutput("image", PortType::Texture);
    }
    ~ImageSequencerNode() override { if (tex_) glDeleteTextures(1, &tex_); }

    void initGL() override {}   // texture allocated lazily on first load

    void evaluate(EvalContext& ctx) override {
        const std::string& folder = ctx.in<std::string>(0);
        float duration = ctx.in<float>(1);
        bool  sync     = ctx.in<bool>(2);
        if (duration < 0.01f) duration = 0.01f;

        if (folder != folder_) {
            folder_  = folder;
            files_   = listImagesInDir(folder);
            index_   = -1;          // force a (re)load
            cur_     = 0;
            elapsed_ = 0.0f;
        }

        int n = (int)files_.size();
        if (n == 0) {
            status_ = folder_.empty() ? std::string() : ("no images in " + folder_);
            haveTex_ = false;
            ctx.out<TexRef>(0, TexRef{});
            return;
        }

        int target;
        if (sync) {
            double beats = ctx.transport ? ctx.transport->beats() : 0.0;
            target = syncedImageIndex(beats, duration, n);
        } else {
            elapsed_ += ctx.dt;
            while (elapsed_ >= duration) { elapsed_ -= duration; cur_ = (cur_ + 1) % n; }
            target = cur_;
        }
        if (target >= n) target = n - 1;   // folder shrank between frames

        if (target != index_) load(target);
        ctx.out<TexRef>(0, haveTex_ ? TexRef{ tex_, w_, h_ } : TexRef{});
    }

    std::string statusLine() const override { return status_; }

private:
    void load(int i) {
        index_ = i;
        std::string err;
        ImageData img = loadImage(files_[(std::size_t)i], err);
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
        status_ = std::to_string(i + 1) + "/" + std::to_string(files_.size())
                + "  " + fileBaseName(files_[(std::size_t)i]);
    }

    GLuint tex_ = 0;
    int    w_ = 0, h_ = 0;
    bool   haveTex_ = false;
    std::vector<std::string> files_;
    std::string folder_;
    std::string status_;
    int   index_   = -1;    // currently-loaded image index (-1 = none)
    int   cur_     = 0;     // free-running position
    float elapsed_ = 0.0f;  // free-running seconds accumulator
};

} // namespace oss
