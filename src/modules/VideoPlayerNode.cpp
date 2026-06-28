#include "modules/VideoPlayerNode.h"
#include "core/Value.h"
#include "audio/AudioBlock.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

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
    if (tex_) glDeleteTextures(1, &tex_);
}

void VideoPlayerNode::initGL() { /* texture is allocated lazily once dims are known */ }

void VideoPlayerNode::evaluate(EvalContext& ctx) {
    const std::string& path = ctx.in<std::string>(0);
    float rate = ctx.in<float>(1);
    bool  play = ctx.in<bool>(2);
    bool  loop = ctx.in<bool>(3);

    if (path != path_) { path_ = path; openPath(path); }

    if (!opened_) {
        ctx.out<TexRef>(0, TexRef{});
        ctx.out<AudioRef>(1, AudioRef{});
        return;
    }

    double prev = playhead_;
    if (play) playhead_ += (double)rate * (double)ctx.dt;

    // Clamp or wrap to the clip bounds.
    bool wrapped = false;
    if (duration_ > 0.0) {
        if (loop) {
            if (playhead_ >= duration_ || playhead_ < 0.0) {
                playhead_ = std::fmod(playhead_, duration_);
                if (playhead_ < 0.0) playhead_ += duration_;
                wrapped = true;
            }
        } else {
            playhead_ = std::clamp(playhead_, 0.0, duration_);
        }
    } else if (playhead_ < 0.0) {
        playhead_ = 0.0;
    }

    // Keep the sliding window covering the playhead, then show the nearest frame.
    ensureCache(playhead_, rate >= 0.0f);
    if (const Frame* f = nearestFrame(playhead_)) uploadFrame(*f);
    ctx.out<TexRef>(0, TexRef{ haveFrame_ ? tex_ : 0u, texW_, texH_ });

    // Audio for the slice of source time just played. A loop wrap (or pause)
    // makes that slice discontinuous, so emit silence for that one frame rather
    // than a swept glitch.
    double a0 = prev, a1 = playhead_;
    if (wrapped || !play) a1 = a0;
    emitAudio(ctx, a0, a1);

    updateStatus(play, rate);
}

void VideoPlayerNode::openPath(const std::string& path) {
    reset();
    if (path.empty()) { status_.clear(); return; }
    dec_ = std::make_unique<VideoDecoder>();
    std::string err;
    if (dec_->open(path, err)) {
        opened_   = true;
        duration_ = dec_->duration();
        playhead_ = 0.0;
        ensureTexture(dec_->width(), dec_->height());
        status_ = "loaded";
        std::fprintf(stderr, "[Video] loaded %s (%dx%d, %.1fs, %s)\n",
                     path.c_str(), dec_->width(), dec_->height(), duration_,
                     dec_->hasAudio() ? "audio" : "no audio");
    } else {
        opened_ = false;
        dec_.reset();
        status_ = "load failed: " + err;
        std::fprintf(stderr, "[Video] load failed: %s (%s)\n", err.c_str(), path.c_str());
    }
}

void VideoPlayerNode::reset() {
    opened_ = false;
    dec_.reset();
    frames_.clear();
    audio_.clear();
    audioValid_   = false;
    audioStartT_  = 0.0;
    cacheStartT_  = cacheEndT_ = 0.0;
    playhead_     = 0.0;
    duration_     = 0.0;
    haveFrame_    = false;
    lastUploadedT_ = -1.0;
    eofReached_   = false;
}

void VideoPlayerNode::ensureTexture(int w, int h) {
    if (tex_ && texW_ == w && texH_ == h) return;
    if (!tex_) glGenTextures(1, &tex_);
    glBindTexture(GL_TEXTURE_2D, tex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    texW_ = w; texH_ = h;
    haveFrame_ = false; lastUploadedT_ = -1.0;
}

void VideoPlayerNode::ensureCache(double t, bool forward) {
    bool covered = !frames_.empty() && t >= cacheStartT_ - 1e-6 && t <= cacheEndT_ + 1e-6;
    if (!covered) {
        // Rebuild around the playhead, biased in the playback direction. A small
        // margin on the trailing side keeps the current frame inside the window.
        double from = forward ? t - 0.1        : t - kLookahead;
        double to   = forward ? t + kLookahead : t + 0.1;
        rebuildCache(from, to);
        return;
    }
    // Covered: top up when running low in the playback direction.
    if (forward) {
        if (!eofReached_ && cacheEndT_ - t < kLookahead * 0.5) extendForward(t + kLookahead);
    } else {
        if (cacheStartT_ > 1e-6 && t - cacheStartT_ < kLookahead * 0.5)
            rebuildCache(t - kLookahead, t + 0.1);
    }
}

void VideoPlayerNode::rebuildCache(double from, double to) {
    if (!dec_) return;
    if (from < 0.0) from = 0.0;
    frames_.clear();
    audio_.clear();
    audioValid_  = false;
    audioStartT_ = 0.0;
    eofReached_  = false;

    dec_->seek(from);
    VideoFrame vf;
    while ((int)frames_.size() < kMaxFrames) {
        if (!dec_->decodeFrame(vf, audio_, audioStartT_, audioValid_)) { eofReached_ = true; break; }
        Frame f;
        f.t = vf.t;
        f.rgba.assign(vf.rgba, vf.rgba + (std::size_t)vf.width * vf.height * 4);
        frames_.push_back(std::move(f));
        if (vf.t >= to) break;
    }
    if (!frames_.empty()) { cacheStartT_ = frames_.front().t; cacheEndT_ = frames_.back().t; }
    else                  { cacheStartT_ = cacheEndT_ = 0.0; }
}

void VideoPlayerNode::extendForward(double toT) {
    if (!dec_) return;
    VideoFrame vf;
    while ((int)frames_.size() < kMaxFrames * 2 && cacheEndT_ < toT) {
        if (!dec_->decodeFrame(vf, audio_, audioStartT_, audioValid_)) { eofReached_ = true; break; }
        Frame f;
        f.t = vf.t;
        f.rgba.assign(vf.rgba, vf.rgba + (std::size_t)vf.width * vf.height * 4);
        frames_.push_back(std::move(f));
        cacheEndT_ = vf.t;
    }
    trimFront();
}

void VideoPlayerNode::trimFront() {
    if ((int)frames_.size() <= kMaxFrames) return;
    int drop = (int)frames_.size() - kMaxFrames;
    double newStartT = frames_[drop].t;
    frames_.erase(frames_.begin(), frames_.begin() + drop);
    cacheStartT_ = frames_.front().t;
    // Drop the audio that precedes the new window start to bound memory.
    if (audioValid_) {
        int s = (int)((newStartT - audioStartT_) * outRate_);
        if (s > 0 && s < (int)audio_.size()) {
            audio_.erase(audio_.begin(), audio_.begin() + s);
            audioStartT_ += (double)s / outRate_;
        }
    }
}

const VideoPlayerNode::Frame* VideoPlayerNode::nearestFrame(double t) const {
    if (frames_.empty()) return nullptr;
    const Frame* best = &frames_.front();   // frames_ is sorted ascending by t
    for (const auto& f : frames_) {
        if (f.t <= t + 1e-9) best = &f; else break;
    }
    return best;
}

void VideoPlayerNode::uploadFrame(const Frame& f) {
    if (haveFrame_ && f.t == lastUploadedT_) return;
    glBindTexture(GL_TEXTURE_2D, tex_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, texW_, texH_, GL_RGBA, GL_UNSIGNED_BYTE, f.rgba.data());
    glBindTexture(GL_TEXTURE_2D, 0);
    lastUploadedT_ = f.t;
    haveFrame_ = true;
}

void VideoPlayerNode::emitAudio(EvalContext& ctx, double t0, double t1) {
    int n = audioBlockFrames(outRate_, ctx.dt);
    if (!audioValid_ || audio_.size() < 2 || t0 == t1) {
        std::fill(outBuf_.begin(), outBuf_.begin() + n, 0.0f);
    } else {
        // Map output sample j (half-open) to source time, then linearly sample the
        // window's audio. t1<t0 (reverse) naturally reads the slice backwards.
        for (int j = 0; j < n; ++j) {
            double s   = t0 + (t1 - t0) * ((double)j / n);
            double idx = (s - audioStartT_) * outRate_;
            float  v   = 0.0f;
            if (idx >= 0.0 && idx < (double)(audio_.size() - 1)) {
                int   i  = (int)idx;
                float fr = (float)(idx - i);
                v = audio_[i] * (1.0f - fr) + audio_[i + 1] * fr;
            }
            outBuf_[j] = v;
        }
    }
    lastAudioN_ = n;
    ctx.out<AudioRef>(1, AudioRef{outBuf_.data(), (std::size_t)n, outRate_});
}

void VideoPlayerNode::updateStatus(bool play, float rate) {
    char buf[96];
    if (duration_ > 0.0)
        std::snprintf(buf, sizeof(buf), "%.1f / %.1f s  x%.2f%s",
                      playhead_, duration_, rate, play ? "" : " (paused)");
    else
        std::snprintf(buf, sizeof(buf), "%.1f s  x%.2f%s", playhead_, rate, play ? "" : " (paused)");
    status_ = buf;
}

} // namespace oss
