#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct AVFormatContext;
struct AVCodecContext;
struct AVStream;
struct AVFrame;
struct AVPacket;
struct SwsContext;

namespace oss {

// Thin synchronous FFmpeg muxer: writes RGBA video frames and mono float audio to
// a movie file (H.264 + AAC in MP4 by default). GL-free -- it takes CPU buffers;
// the caller reads back the texture and pulls the audio block. The mirror of
// VideoDecoder. Drive it from one thread: open(), addVideoFrame()/addAudio() per
// frame, then close() to flush and finalise the file.
class VideoEncoder {
public:
    VideoEncoder() = default;
    ~VideoEncoder();
    VideoEncoder(const VideoEncoder&) = delete;
    VideoEncoder& operator=(const VideoEncoder&) = delete;

    // Open `path` for writing `width`x`height` video at a nominal `fps`. If
    // `audioRate` > 0 an AAC mono audio stream is added at that sample rate.
    // Returns false and fills `err` on failure.
    bool open(const std::string& path, int width, int height, int fps,
              int audioRate, std::string& err);
    bool isOpen() const { return opened_; }

    // Append one video frame. `rgba` is width*height*4 bytes, bottom row first
    // (GL/FBO order); it is flipped to top-down for encoding. `tSeconds` is the
    // frame's presentation time (the recording elapsed time).
    bool addVideoFrame(const std::uint8_t* rgba, double tSeconds);

    // Append mono float audio samples at the rate passed to open(). Buffered and
    // encoded in codec-sized frames. No-op if the file has no audio stream.
    bool addAudio(const float* samples, int count);

    // Flush the encoders, write the trailer, and close the file. Idempotent.
    bool close(std::string& err);

private:
    bool encodeWrite(AVCodecContext* ctx, AVStream* st, AVFrame* frame);
    void freeAll();

    AVFormatContext* oc_     = nullptr;
    AVStream*        vst_    = nullptr;
    AVStream*        ast_    = nullptr;
    AVCodecContext*  vctx_   = nullptr;
    AVCodecContext*  actx_   = nullptr;
    SwsContext*      sws_    = nullptr;
    AVFrame*         vframe_ = nullptr;
    AVFrame*         aframe_ = nullptr;
    AVPacket*        pkt_    = nullptr;

    int     width_ = 0, height_ = 0;
    int     audioRate_ = 0, audioFrameSize_ = 0;
    int64_t lastVpts_ = -1;          // last video pts (codec time base = 1/fps)
    int64_t aCount_   = 0;           // audio samples written (audio pts)
    std::vector<float> afifo_;       // pending mono float samples
    bool    opened_ = false;
};

} // namespace oss
