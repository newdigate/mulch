#include "modules/VideoPlayerNode.h"
#include "core/Value.h"
#include "gfx/GLStateGuard.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace oss {

VideoPlayerNode::VideoPlayerNode() : Node("Video Player"), outBuf_(kAudioMaxBlock, 0.0f) {
    addAssetInput("file", AssetType::Video);   // .mp4/.mov/... path
    addInput("rate", PortType::Float,  1.0f, -2.0f, 2.0f); // signed: negative = reverse
    addInput("play", PortType::Bool,   true);
    addInput("loop", PortType::Bool,   true);
    addOutput("video", PortType::Texture);
    addOutput("audio", PortType::Audio);
}

VideoPlayerNode::~VideoPlayerNode() {
    VideoStream::retire(std::move(stream_));   // torn down on another thread: at 4K that takes 0.1-0.5 s
    freeGL();
}

void VideoPlayerNode::initGL() { /* textures are allocated once the stream knows the size */ }

void VideoPlayerNode::evaluate(EvalContext& ctx) {
    const std::string& path = ctx.in<std::string>(0);
    const float rate = ctx.in<float>(1);
    const bool  play = ctx.in<bool>(2);
    const bool  loop = ctx.in<bool>(3);
    const bool  wasOffline = offline_;
    offline_ = ctx.offline;
    if (!offline_) stalled_ = false;
    loop_ = loop;

    if (path != path_) { path_ = path; openPath(path); }
    if (!stream_) { publishEmpty(ctx); return; }

    const VideoStream::State st = stream_->state();
    if (st == VideoStream::State::Failed) {
        status_ = (opened_ ? "failed: " : "load failed: ") + stream_->error();
        if (!failLogged_) {
            std::fprintf(stderr, "[Video] %s (%s)\n", status_.c_str(), path_.c_str());
            failLogged_ = true;
        }
        publishEmpty(ctx);
        return;
    }
    if (st == VideoStream::State::Opening) {
        status_ = "opening...";
        publishEmpty(ctx);
        return;
    }
    if (needInfo_) {
        opened_ = true;
        const VideoStream::Info inf = stream_->info();
        duration_ = inf.duration;
        frameDur_ = inf.frameDur;
        ensureTextures(inf.width, inf.height);
        needInfo_ = false;
        std::fprintf(stderr, "[Video] loaded %s (%dx%d, %.1fs, %s)\n", path_.c_str(),
                     inf.width, inf.height, inf.duration, inf.hasAudio ? "audio" : "no audio");
    }

    // Hold the playhead at the start until the first frame is on screen: the worker's first frames
    // take a moment (open, decoder warm-up), and running the clock meanwhile would skip them.
    const bool advancing = play && shownSerial_ != 0;
    // Offline, dt is 1.0f / fps: snap it to the exact frame step so a long render stays on the grid.
    const double dt = offline_ ? videoFrameStep(ctx.dt) : (double)ctx.dt;
    const double prevU = ph_.u;
    ph_ = videoAdvance(ph_, advancing, rate, loop, dt, duration_);
    stream_->request(makeRequest(ph_, play, rate, loop));

    // Offline renders are exact: wait for the frame for u (normally already there -- see below). Not on a
    // render's first frame: it can need a whole reverse stretch decoded, and it is never captured -- the
    // renderer always runs a pre-roll frame first (Node::loading()) -- so the gate waits for the next one.
    if (offline_ && wasOffline && !stream_->frameReadyFor(ph_.u) &&
        !stream_->waitForFrame(ph_.u, kOfflineFrameWaitSeconds))
        stalled_ = true;                               // publish what we have; loading() fails the render

    VideoStream::FrameView fv;
    if (stream_->frameAt(ph_.u, fv) && fv.serial != shownSerial_) upload(fv);
    ctx.out<TexRef>(0, TexRef{ shownSerial_ ? tex_ : 0u, texW_, texH_ });

    // Audio for the slice of source time just played. The playhead is unwrapped, so a loop wrap
    // is a continuous slice too; a pause is silent.
    const int n = audioBlockFrames(outRate_, ctx.dt);
    if (!play || ph_.u == prevU) std::fill(outBuf_.begin(), outBuf_.begin() + n, 0.0f);
    else                         stream_->readAudio(prevU, ph_.u, outBuf_.data(), n);
    lastAudioN_ = n;
    ctx.out<AudioRef>(1, AudioRef{outBuf_.data(), (std::size_t)n, outRate_});

    // Offline, ask for the NEXT frame now: loading() reports it not ready, so the renderer's gate
    // waits between frames instead of this evaluate() blocking the UI.
    if (offline_) {
        const VideoPlayhead next = videoAdvance(ph_, play, rate, loop, dt, duration_);
        pendingU_ = next.u;
        stream_->request(makeRequest(next, play, rate, loop));
    }
    updateStatus(play, rate);
}

bool VideoPlayerNode::loading() const {
    if (!stream_) return false;
    const VideoStream::State st = stream_->state();
    if (st == VideoStream::State::Opening) return true;
    if (!offline_) return false;
    // A stream that fails once open has no frames left to give: hold the render, so it fails naming this
    // node rather than finishing with the picture gone. (One that never opened renders without video.)
    if (st == VideoStream::State::Failed) return opened_;
    return stalled_ || !stream_->frameReadyFor(pendingU_);
}

void VideoPlayerNode::openPath(const std::string& path) {
    VideoStream::retire(std::move(stream_));            // never waits for the old worker
    ph_ = VideoPlayhead{};
    duration_ = 0.0;
    frameDur_ = 1.0 / 30.0;
    shownSerial_ = 0;
    shownT_ = -1.0;
    stalled_ = false;
    pendingU_ = 0.0;
    failLogged_ = false;
    opened_ = false;
    needInfo_ = !path.empty();
    status_.clear();
    if (path.empty()) return;
    stream_ = std::make_unique<VideoStream>(path);
    status_ = "opening...";
}

VideoRequest VideoPlayerNode::makeRequest(const VideoPlayhead& p, bool play, float rate, bool loop) const {
    VideoRequest r;
    r.u       = p.u;
    r.rate    = play ? rate : 0.0f;
    r.offline = offline_;
    r.loop    = loop && duration_ > 0.0;
    if (!r.loop) {
        r.lapLo = duration_ > 0.0 ? p.lapLo : 0.0;
        r.lapHi = duration_ > 0.0 ? p.lapLo + duration_ : std::numeric_limits<double>::infinity();
    }
    return r;
}

void VideoPlayerNode::ensureTextures(int w, int h) {
    if (tex_ && texW_ == w && texH_ == h) return;
    freeGL();
    GLStateGuard guard;                                 // restores bindings the setup disturbs
    auto makeTex = [&](GLuint& t) {
        glGenTextures(1, &t);
        glBindTexture(GL_TEXTURE_2D, t);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    };
    makeTex(stageTex_);
    makeTex(tex_);
    glGenFramebuffers(1, &readFbo_);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, readFbo_);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, stageTex_, 0);
    glGenFramebuffers(1, &drawFbo_);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, drawFbo_);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex_, 0);
    texW_ = w;
    texH_ = h;
    shownSerial_ = 0;
    shownT_ = -1.0;
}

void VideoPlayerNode::freeGL() {
    if (readFbo_)  glDeleteFramebuffers(1, &readFbo_);
    if (drawFbo_)  glDeleteFramebuffers(1, &drawFbo_);
    if (stageTex_) glDeleteTextures(1, &stageTex_);
    if (tex_)      glDeleteTextures(1, &tex_);
    readFbo_ = drawFbo_ = stageTex_ = tex_ = 0;
    texW_ = texH_ = 0;
}

// Upload the worker's top-down rows to the staging texture, then flip them into the published
// texture with one blit (source rows 0..H to destination rows H..0).
void VideoPlayerNode::upload(const VideoStream::FrameView& f) {
    GLStateGuard guard;
    glDisable(GL_SCISSOR_TEST);                         // the blit is clipped by the scissor box
    glBindTexture(GL_TEXTURE_2D, stageTex_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, texW_, texH_, GL_RGBA, GL_UNSIGNED_BYTE, f.rgba);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, readFbo_);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, drawFbo_);
    glBlitFramebuffer(0, 0, texW_, texH_, 0, texH_, texW_, 0, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    shownSerial_ = f.serial;
    shownT_ = f.t;
}

void VideoPlayerNode::publishEmpty(EvalContext& ctx) {
    ctx.out<TexRef>(0, TexRef{});
    ctx.out<AudioRef>(1, AudioRef{});
}

void VideoPlayerNode::updateStatus(bool play, float rate) {
    const bool buffering = play && rate > 0.0f && shownSerial_ != 0 &&
                           ph_.u - shownT_ > std::max(0.2, 2.0 * frameDur_);
    const char* tail = !play ? " (paused)" : (buffering ? " (buffering)" : "");
    char buf[128];
    if (duration_ > 0.0)
        std::snprintf(buf, sizeof(buf), "%.1f / %.1f s  x%.2f%s", playhead(), duration_, rate, tail);
    else
        std::snprintf(buf, sizeof(buf), "%.1f s  x%.2f%s", playhead(), rate, tail);
    status_ = buf;
}

} // namespace oss
