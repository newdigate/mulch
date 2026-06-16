#pragma once
#include <glad/gl.h>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "core/Node.h"
#include "gfx/VideoDecoder.h"

namespace oss {

// Plays a video file (named by its string input), streaming the picture as a
// texture (output 0) and the soundtrack as 48 kHz mono audio (output 1). The
// `rate` input is a signed playback speed: 1 = normal, 0.5 = half, 2 = double,
// and NEGATIVE values play in reverse (the audio is swept backwards and won't
// sound musical -- that's expected). `play` pauses; `loop` wraps at the ends.
//
// Decoding is synchronous on the graph thread. To play forward, backward, and at
// variable speed off a forward-only decoder, the node keeps a sliding window of
// recently decoded frames (one GOP-ish span around the playhead): it advances a
// source-time playhead by rate*dt each frame, shows the cached frame nearest the
// playhead, and re-seeks to the keyframe before the playhead to rebuild the
// window whenever the playhead leaves it. Audio for each frame is resampled out
// of the window's decoded-audio buffer over the slice [previous, current]
// playhead, so speed and direction fall out of that mapping.
class VideoPlayerNode : public Node {
public:
    VideoPlayerNode();
    ~VideoPlayerNode() override;
    void initGL() override;
    void evaluate(EvalContext& ctx) override;
    std::string statusLine() const override { return status_; }

    // Test/inspection accessors.
    double  playhead() const { return playhead_; }
    AudioRef audioOut() const { return AudioRef{outBuf_.data(), (std::size_t)lastAudioN_, outRate_}; }

private:
    struct Frame { double t = 0.0; std::vector<std::uint8_t> rgba; };  // bottom-up RGBA8

    void openPath(const std::string& path);
    void reset();
    void ensureTexture(int w, int h);
    void ensureCache(double t, bool forward);
    void rebuildCache(double from, double to);
    void extendForward(double toT);
    void trimFront();
    const Frame* nearestFrame(double t) const;
    void uploadFrame(const Frame& f);
    void emitAudio(EvalContext& ctx, double t0, double t1);
    void updateStatus(bool play, float rate);

    static constexpr double kLookahead = 0.5;   // seconds of frames to keep ahead
    // Steady-state window size (frames are trimmed back to this). The window only
    // needs to span ~kLookahead + a little, so this also bounds memory: the cache
    // holds ~kMaxFrames decoded RGBA frames, i.e. width*height*4*kMaxFrames bytes.
    static constexpr int    kMaxFrames = 48;
    static constexpr int    kMaxBlock  = 4096;  // max audio samples emitted per frame

    std::unique_ptr<VideoDecoder> dec_;
    std::string path_;
    std::string status_;
    bool   opened_   = false;
    double duration_ = 0.0;
    double playhead_ = 0.0;       // current position in source seconds

    GLuint tex_   = 0;
    int    texW_  = 0;
    int    texH_  = 0;
    bool   haveFrame_    = false; // a frame has been uploaded since (re)open
    double lastUploadedT_ = -1.0; // source time of the uploaded frame (skip re-upload)

    // Sliding window: frames sorted by ascending source time, plus the matching
    // run of decoded 48 kHz mono audio anchored at audioStartT_.
    std::vector<Frame> frames_;
    std::vector<float> audio_;
    double audioStartT_ = 0.0;
    bool   audioValid_  = false;
    double cacheStartT_ = 0.0;
    double cacheEndT_   = 0.0;
    bool   eofReached_  = false;

    int                outRate_   = VideoDecoder::kOutRate;
    std::vector<float> outBuf_;   // owns the samples audioOut/AudioRef points at
    int                lastAudioN_ = 0;
};

} // namespace oss
