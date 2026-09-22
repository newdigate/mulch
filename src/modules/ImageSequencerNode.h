#pragma once
#include <glad/gl.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <future>
#include <string>
#include <utility>
#include <vector>
#include "core/Node.h"
#include "core/PathUtil.h"
#include "core/ImageSequence.h"
#include "core/Preferences.h"
#include "gfx/ImageLoader.h"
#include "gfx/Framebuffer.h"
#include "gfx/FullscreenPass.h"
#include "gfx/GLUtil.h"
#include "gfx/Canvas.h"

namespace oss {

// Plays a folder of images in sequence, one every `duration` seconds (free-running) or `beat
// length` beats (transport-synced), cross-dissolving over `fade duration` seconds at each change
// (0 = instant). Decodes the upcoming image on a worker thread into one of two GL textures and
// renders mix(from, to, m) into its own FBO (the ShaderNode pattern, inline). The worker only runs
// the GL-free loadImage; the graph thread does GL uploads + the blend pass.
class ImageSequencerNode : public Node {
public:
    ImageSequencerNode() : Node("Image Sequencer") {
        addImageFolderInput("folder");
        addInput("duration", PortType::Float, 1.0f, 0.05f, 60.0f);   // seconds (free-running)
        addIntInput("beat length", 1, 1, 16);                        // beats per image (synced)
        addInput("sync", PortType::Bool, false);
        addInput("fade duration", PortType::Float, 0.0f, 0.0f, 10.0f);  // seconds; 0 = instant
        addOutput("image", PortType::Texture);
    }
    ~ImageSequencerNode() override {
        // fbo_/fsq_ free themselves; the fetch_ member dtor joins any in-flight decode. The worker
        // never touches these GL objects, so freeing them in the dtor body first is safe.
        if (fadeProg_) glDeleteProgram(fadeProg_);
        if (texShown_) glDeleteTextures(1, &texShown_);
        if (texNext_)  glDeleteTextures(1, &texNext_);
    }

    void initGL() override {
        fadeProg_ = linkProgram(kFadeVS, readFile("shaders/crossfade.frag"));
        fbo_.create(kCanvasW, kCanvasH);
        fsq_.create();
    }

    void evaluate(EvalContext& ctx) override {
        const std::string& folder = ctx.in<std::string>(0);
        float duration = ctx.in<float>(1);
        int   beatLen  = std::max(1, (int)std::lround(ctx.in<float>(2)));
        bool  sync     = ctx.in<bool>(3);
        float fadeDur  = ctx.in<float>(4);
        if (duration < 0.01f) duration = 0.01f;

        if (folder != folder_) {
            folder_ = folder;
            files_  = listImagesInDir(folder);
            shownIndex_ = -1; cur_ = 0; elapsed_ = 0.0f;
            nextReady_ = false; nextIndex_ = -1; failedIndex_ = -1;
            fading_ = false; fadeElapsed_ = 0.0f;
            fetch_ = std::future<ImageData>{}; fetchIndex_ = -1;   // (joins any in-flight decode)
            if (!files_.empty()) syncLoadShown(0);                 // immediate first frame
        }

        int n = (int)files_.size();
        if (n == 0) {
            status_ = folder_.empty() ? std::string() : ("no images in " + folder_);
            ctx.out<TexRef>(0, TexRef{});
            return;
        }

        // Poll the background decode; upload its result into the "next" texture.
        if (fetch_.valid() &&
            fetch_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            ImageData img = fetch_.get();
            if (img.ok()) { upload(texNext_, wNext_, hNext_, img); nextIndex_ = fetchIndex_; nextReady_ = true; }
            else          { failedIndex_ = fetchIndex_; }   // don't hammer a bad/missing file every frame
            fetchIndex_ = -1;   // idle
        }

        // Where should we be now, and how long is each image shown (seconds)?
        int target;
        if (sync) {
            double beats = ctx.transport ? ctx.transport->beats() : 0.0;
            target = syncedImageIndex(beats, (float)beatLen, n);
            cur_ = target;   // keep the free-run counter aligned for a later sync->free handoff
        } else {
            elapsed_ += ctx.dt;
            while (elapsed_ >= duration) { elapsed_ -= duration; cur_ = (cur_ + 1) % n; }
            target = cur_;
        }
        if (target >= n) target = n - 1;   // defensive: never index past files_
        float interval = sync
            ? (float)beatLen * (ctx.transport ? (float)ctx.transport->secondsPerBeat() : 0.5f)
            : duration;

        // Fade progression / start.
        float m = 0.0f;
        if (fading_) {
            fadeElapsed_ += ctx.dt;
            m = crossfadeMix(fadeElapsed_, fadeDur, interval);
            if (m >= 1.0f) {   // fade complete -> adopt the incoming image
                std::swap(texShown_, texNext_); std::swap(wShown_, wNext_); std::swap(hShown_, hNext_);
                shownIndex_ = fadeToIndex_; nextReady_ = false; fading_ = false; m = 0.0f;
            }
        } else if (target != shownIndex_ && nextReady_ && nextIndex_ == target) {
            float eff = fadeDur < interval ? fadeDur : interval;
            if (eff > 0.0f) { fading_ = true; fadeElapsed_ = 0.0f; fadeToIndex_ = target; }
            else {   // instant swap (no fade)
                std::swap(texShown_, texNext_); std::swap(wShown_, wNext_); std::swap(hShown_, hNext_);
                shownIndex_ = target; nextReady_ = false;
            }
        }

        // Prefetch (only when not fading -- both textures are in use during a fade).
        if (!fading_) {
            int want = (shownIndex_ == target) ? (target + 1) % n : target;
            if (want != failedIndex_) failedIndex_ = -1;   // moved off the bad index -> allow a retry
            bool haveWant = nextReady_ && nextIndex_ == want;
            if (!haveWant && fetchIndex_ == -1 && want != failedIndex_) {
                std::string p = files_[(std::size_t)want];
                fetch_ = std::async(std::launch::async, [p]() { std::string e; return loadImage(p, e); });
                fetchIndex_ = want;
            }
        }

        if (shownIndex_ >= 0)
            status_ = std::to_string(shownIndex_ + 1) + "/" + std::to_string(n)
                    + "  " + fileBaseName(files_[(std::size_t)shownIndex_]);

        // Publish the frame. When not fading, output the current image directly (no FBO pass) --
        // zero-cost for instant cuts and dwell, matching the pre-crossfade behavior. The blend
        // pass runs only during an active fade.
        if (!fading_) {
            ctx.out<TexRef>(0, (shownIndex_ >= 0 && texShown_) ? TexRef{ texShown_, wShown_, hShown_ }
                                                               : TexRef{});
            return;
        }

        int fw = ctx.prefs ? ctx.prefs->textureWidth  : kCanvasW;
        int fh = ctx.prefs ? ctx.prefs->textureHeight : kCanvasH;
        if (fbo_.width() != fw || fbo_.height() != fh) fbo_.create(fw, fh);
        fbo_.bind();
        glUseProgram(fadeProg_);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, texShown_);
        glUniform1i(glGetUniformLocation(fadeProg_, "uFrom"), 0);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, texNext_);
        glUniform1i(glGetUniformLocation(fadeProg_, "uTo"), 1);
        glUniform1f(glGetUniformLocation(fadeProg_, "uMix"), m);
        glActiveTexture(GL_TEXTURE0);   // restore default unit
        fsq_.draw();
        Framebuffer::unbind();
        ctx.out<TexRef>(0, TexRef{ fbo_.texture(), fbo_.width(), fbo_.height() });
    }

    std::string statusLine() const override { return status_; }
    bool loading() const override {   // a prefetch in flight and not yet decoded
        return fetch_.valid() && fetch_.wait_for(std::chrono::seconds(0)) != std::future_status::ready;
    }

private:
    // Synchronous decode+upload into the shown texture (the first image on folder load).
    void syncLoadShown(int i) {
        std::string err;
        ImageData img = loadImage(files_[(std::size_t)i], err);
        if (!img.ok()) { status_ = "load failed: " + err; shownIndex_ = -1; return; }
        upload(texShown_, wShown_, hShown_, img);
        shownIndex_ = i; cur_ = i;
    }

    static void upload(GLuint& tex, int& w, int& h, const ImageData& img) {
        if (!tex) glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, img.width, img.height, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, img.rgba.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);
        w = img.width; h = img.height;
    }

    static constexpr const char* kFadeVS = R"(#version 410 core
out vec2 vUV;
void main() {
    vec2 p = vec2(gl_VertexID == 1 ? 3.0 : -1.0, gl_VertexID == 2 ? 3.0 : -1.0);
    vUV = (p + 1.0) * 0.5;
    gl_Position = vec4(p, 0.0, 1.0);
}
)";

    std::vector<std::string> files_;
    std::string folder_, status_;
    GLuint texShown_ = 0, texNext_ = 0;
    int    wShown_ = 0, hShown_ = 0, wNext_ = 0, hNext_ = 0;
    int    shownIndex_ = -1;    // index in texShown_ (-1 = none)
    int    cur_ = 0;            // free-run position
    float  elapsed_ = 0.0f;     // free-run seconds accumulator
    std::future<ImageData> fetch_;
    int    fetchIndex_ = -1;    // index being decoded; -1 = idle
    bool   nextReady_ = false;  // texNext_ holds a decoded image
    int    nextIndex_ = -1;     // its index
    int    failedIndex_ = -1;   // an index whose decode failed; don't re-launch it until we move on
    Framebuffer    fbo_;        // the node renders its (blended) output here
    FullscreenPass fsq_;
    GLuint fadeProg_ = 0;       // crossfade shader program
    bool   fading_ = false;     // a cross-fade is in progress
    float  fadeElapsed_ = 0.0f; // seconds into the current fade
    int    fadeToIndex_ = -1;   // the index we're fading to (held in texNext_)
};

} // namespace oss
