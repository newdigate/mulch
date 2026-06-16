#include "modules/AudioPlayerNode.h"
#include "core/Value.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace oss {

AudioPlayerNode::AudioPlayerNode() : Node("Audio File"), outBuf_(kMaxBlock * 2, 0.0f) {
    addInput("file", PortType::String, std::string(""));
    addInput("rate", PortType::Float,  1.0f, -2.0f, 2.0f);   // signed: negative = reverse
    addInput("play", PortType::Bool,   true);
    addInput("loop", PortType::Bool,   true);
    addOutput("audio", PortType::Audio);                      // 48 kHz stereo
}

void AudioPlayerNode::evaluate(EvalContext& ctx) {
    const std::string& path = ctx.in<std::string>(0);
    float rate = ctx.in<float>(1);
    bool  play = ctx.in<bool>(2);
    bool  loop = ctx.in<bool>(3);

    // Start a worker-thread decode whenever the file path changes.
    if (loader_.request(path, [path] { return decodeAudioFile(path); })) {
        haveClip_ = false; clip_ = AudioClip{}; playhead_ = 0.0; duration_ = 0.0;
        if (path.empty()) status_.clear();
        else { status_ = "loading..."; std::fprintf(stderr, "[AudioFile] loading %s\n", path.c_str()); }
    }
    AudioClip done;
    if (loader_.poll(done)) {
        clip_       = std::move(done);
        haveClip_   = clip_.ok;
        sampleRate_ = clip_.sampleRate;
        duration_   = (double)clip_.frames() / sampleRate_;
        if (clip_.ok) {
            status_ = "loaded";
            std::fprintf(stderr, "[AudioFile] loaded %.1fs stereo\n", duration_);
        } else {
            status_ = "load failed: " + clip_.error;
            std::fprintf(stderr, "[AudioFile] %s\n", status_.c_str());
        }
    }

    if (!haveClip_) { ctx.out<AudioRef>(0, AudioRef{}); lastN_ = 0; return; }

    double prev = playhead_;
    if (play) playhead_ += (double)rate * (double)ctx.dt;

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

    // A wrap (or pause) makes the source slice discontinuous -> emit silence for
    // that one block rather than a swept glitch.
    double a0 = prev, a1 = playhead_;
    if (wrapped || !play) a1 = a0;
    emitAudio(ctx, a0, a1);
    updateStatus(play, rate);
}

void AudioPlayerNode::emitAudio(EvalContext& ctx, double t0, double t1) {
    int n = std::clamp((int)std::lround(sampleRate_ * (double)ctx.dt), 1, kMaxBlock);
    std::size_t frames = clip_.frames();
    if (!haveClip_ || frames < 2 || t0 == t1) {
        std::fill(outBuf_.begin(), outBuf_.begin() + (std::size_t)n * 2, 0.0f);
    } else {
        // Map each output frame (half-open) to source time and linearly sample the
        // clip. t1<t0 (reverse) naturally reads the clip backwards.
        for (int j = 0; j < n; ++j) {
            double s    = t0 + (t1 - t0) * ((double)j / n);
            double fidx = s * sampleRate_;
            float  L = 0.0f, R = 0.0f;
            if (fidx >= 0.0 && fidx < (double)(frames - 1)) {
                int   i0 = (int)fidx;
                float fr = (float)(fidx - i0);
                L = clip_.samples[(std::size_t)i0 * 2]     * (1 - fr) + clip_.samples[((std::size_t)i0 + 1) * 2]     * fr;
                R = clip_.samples[(std::size_t)i0 * 2 + 1] * (1 - fr) + clip_.samples[((std::size_t)i0 + 1) * 2 + 1] * fr;
            }
            outBuf_[(std::size_t)j * 2]     = L;
            outBuf_[(std::size_t)j * 2 + 1] = R;
        }
    }
    lastN_ = n;
    ctx.out<AudioRef>(0, AudioRef{outBuf_.data(), (std::size_t)n * 2, sampleRate_, 2});
}

void AudioPlayerNode::updateStatus(bool play, float rate) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%.1f / %.1f s  x%.2f%s",
                  playhead_, duration_, rate, play ? "" : " (paused)");
    status_ = buf;
}

} // namespace oss
