#pragma once
#include <glad/gl.h>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "core/Node.h"
#include "core/VideoPlan.h"
#include "gfx/VideoStream.h"
#include "audio/AudioBlock.h"

namespace oss {

// Plays a video file (named by its string input), streaming the picture as a
// texture (output 0) and the soundtrack as 48 kHz mono audio (output 1). The
// `rate` input is a signed playback speed: 1 = normal, 0.5 = half, 2 = double,
// and NEGATIVE values play in reverse (the audio is swept backwards and won't
// sound musical -- that's expected). `play` pauses; `loop` wraps at the ends.
//
// Decoding runs on a background worker (gfx/VideoStream), so the UI never waits on FFmpeg.
// Each frame the node advances an UNWRAPPED playhead (it keeps counting past the end rather
// than jumping back, so the worker can decode the next lap early -- see core/VideoPlan.h),
// posts it to the stream, and uploads the newest ready frame at or before it. The worker
// converts rows top-down, so the upload lands in a staging texture and one flipped
// glBlitFramebuffer puts it bottom-up into the published texture.
//
// Offline renders stay frame-exact: evaluate() waits for the exact frame, and loading()
// reports the NEXT frame not ready yet, so the renderer's gate does the waiting between frames.
// A stream that fails mid-render keeps loading() true, so the render fails rather than going on
// without the picture. Old streams are handed to VideoStream::retire(), never waited for.
class VideoPlayerNode : public Node {
public:
    VideoPlayerNode();
    ~VideoPlayerNode() override;
    void initGL() override;
    void evaluate(EvalContext& ctx) override;
    std::string statusLine() const override { return status_; }
    bool loading() const override;

    // Test/inspection accessors.
    double   playhead() const { return videoPosition(ph_, loop_, duration_); }   // position in the clip
    bool     hasFrame() const { return shownSerial_ != 0; }   // a frame of the current file is on screen
    double   shownFrameTime() const { return shownT_; }       // its unwrapped time (valid when hasFrame())
    AudioRef audioOut() const { return AudioRef{outBuf_.data(), (std::size_t)lastAudioN_, outRate_}; }

    // Offline: how long evaluate() waits for a frame the prefetch did not predict (an automated rate, a
    // direction flip). A reverse stretch through a long keyframe interval at 4K takes seconds, and the UI
    // waits with it; a frame that takes longer latches a stall, and the render fails naming the node.
    static constexpr double kOfflineFrameWaitSeconds = 10.0;

private:
    void openPath(const std::string& path);
    VideoRequest makeRequest(const VideoPlayhead& p, bool play, float rate, bool loop) const;
    void ensureTextures(int w, int h);
    void freeGL();
    void upload(const VideoStream::FrameView& f);
    void publishEmpty(EvalContext& ctx);
    void updateStatus(bool play, float rate);

    std::unique_ptr<VideoStream> stream_;
    std::string   path_;
    std::string   status_;
    bool          needInfo_ = false;     // the stream is new: read its info once it is Ready
    bool          opened_ = false;       // it has been Ready: a failure now is mid-play
    bool          failLogged_ = false;
    double        duration_ = 0.0;
    double        frameDur_ = 1.0 / 30.0;
    VideoPlayhead ph_;
    bool          loop_ = true;

    GLuint        stageTex_ = 0;         // receives the worker's top-down rows
    GLuint        tex_      = 0;         // published, bottom-up
    GLuint        readFbo_  = 0, drawFbo_ = 0;
    int           texW_ = 0, texH_ = 0;
    std::uint64_t shownSerial_ = 0;      // 0: nothing uploaded from this stream yet
    double        shownT_ = -1.0;

    bool          offline_  = false;     // the last evaluate() was part of an offline render
    bool          stalled_  = false;     // offline: a frame missed its wait (latched until live again)
    double        pendingU_ = 0.0;       // offline: the next frame's playhead, prefetched

    int                outRate_   = VideoDecoder::kOutRate;
    std::vector<float> outBuf_;          // owns the samples audioOut/AudioRef points at
    int                lastAudioN_ = 0;
};

} // namespace oss
