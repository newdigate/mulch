#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Forward-declare the FFmpeg types so this header stays free of <libav*> includes
// (only VideoDecoder.cpp pulls those in). These are C structs from FFmpeg.
struct AVFormatContext;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwsContext;
struct SwrContext;

namespace oss {

// One decoded video frame: tightly-packed RGBA8 pixels valid until the next
// decodeFrame() call (the decoder reuses its conversion buffer).
struct VideoFrame {
    double               t = 0.0;   // presentation time in seconds (source time)
    int                  width = 0;
    int                  height = 0;
    // width*height*4 bytes, stored bottom row first (GL texture convention) so it
    // uploads straight to a texture and shows upright through the output blit.
    const std::uint8_t*  rgba = nullptr;
};

// Thin synchronous FFmpeg wrapper: decodes a media file's video as RGBA frames
// and its audio resampled to 48 kHz mono float. NOT thread-safe and GL-free --
// it only produces CPU buffers; the caller uploads frames to a texture and feeds
// the audio downstream. Drive it from one thread: open(), then seek()/decodeFrame()
// to walk frames in source order. Reverse/variable-rate playback is the caller's
// job (re-seek to a keyframe and re-walk forward); this class only goes forward.
class VideoDecoder {
public:
    VideoDecoder() = default;
    ~VideoDecoder();
    VideoDecoder(const VideoDecoder&) = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;

    // Open `path`. Returns false and fills `err` on failure (bad path, no video
    // stream, unsupported codec). A file with no audio stream still opens.
    bool open(const std::string& path, std::string& err);

    bool   isOpen()   const { return fmt_ != nullptr; }
    int    width()    const { return width_; }
    int    height()   const { return height_; }
    double duration() const { return duration_; }      // seconds (0 if unknown)
    bool   hasAudio() const { return astream_ >= 0; }
    int    audioRate() const { return kOutRate; }      // we always resample to this
    static constexpr int kOutRate = 48000;             // 48 kHz mono float out

    // Seek so the next decodeFrame() resumes at the keyframe at or before time
    // `t` (seconds). Flushes the decoders so no stale frames leak across.
    void seek(double t);

    // Decode the next video frame in source order into `out`. Any audio decoded
    // on the way (audio packets interleaved before this video frame) is appended
    // to `audio` as 48 kHz mono float. The first time audio is appended into a
    // freshly-cleared `audio`, `audioStartT` is set to that audio's source time
    // and `audioStartValid` to true (left untouched on later calls so a multi-call
    // fill keeps one contiguous timeline). Returns false at end of stream.
    bool decodeFrame(VideoFrame& out, std::vector<float>& audio,
                     double& audioStartT, bool& audioStartValid);

private:
    void close();
    void drainAudio(std::vector<float>& audio, double& audioStartT, bool& audioStartValid);

    AVFormatContext* fmt_   = nullptr;
    AVCodecContext*  vctx_  = nullptr;   // video decoder
    AVCodecContext*  actx_  = nullptr;   // audio decoder (null if no audio)
    SwsContext*      sws_   = nullptr;   // -> RGBA
    SwrContext*      swr_   = nullptr;   // -> 48 kHz mono float
    AVFrame*         frame_ = nullptr;   // reused decode target (video or audio)
    AVPacket*        pkt_   = nullptr;

    int    vstream_ = -1;
    int    astream_ = -1;
    int    width_   = 0;
    int    height_  = 0;
    double duration_   = 0.0;
    double vTimeBase_  = 0.0;   // seconds per video stream tick
    double aTimeBase_  = 0.0;   // seconds per audio stream tick
    bool   eof_        = false; // hit end of input; still draining buffered frames

    std::vector<std::uint8_t> rgba_;     // reused RGBA conversion target
    std::vector<float>        aScratch_; // reused swr output scratch
};

} // namespace oss
