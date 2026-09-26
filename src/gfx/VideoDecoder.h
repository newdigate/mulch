#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
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
    double               t = 0.0;   // presentation time in seconds, from the first frame
    int                  width = 0;
    int                  height = 0;
    // width*height*4 bytes, stored bottom row first (GL texture convention) so it
    // uploads straight to a texture and shows upright through the output blit.
    const std::uint8_t*  rgba = nullptr;
};

// A decoded video frame that has not been converted to RGBA yet: a counted reference to the
// decoder's picture, so holding one copies nothing. Move-only. VideoDecoder::decodeNext() fills it
// and VideoDecoder::convert() turns it into pixels, so a caller can decode a frame, look at its
// time, and only then decide whether it is worth converting.
class DecodedFrame {
public:
    DecodedFrame() = default;
    ~DecodedFrame();
    DecodedFrame(DecodedFrame&& o) noexcept;
    DecodedFrame& operator=(DecodedFrame&& o) noexcept;
    DecodedFrame(const DecodedFrame&) = delete;
    DecodedFrame& operator=(const DecodedFrame&) = delete;

    bool valid() const { return frame_ != nullptr; }
    void reset();

    double t = 0.0;   // presentation time in seconds, from the first frame

private:
    friend class VideoDecoder;
    AVFrame* frame_ = nullptr;
};

// Thin FFmpeg wrapper: decodes a media file's video and its audio resampled to 48 kHz mono float.
// NOT thread-safe and GL-free -- it only produces CPU buffers; the caller uploads frames to a
// texture and feeds the audio downstream. Drive it from one thread: open(), then seek() and
// decodeNext()/decodeFrame() to walk frames in source order. Reverse/variable-rate playback is the
// caller's job (re-seek to a keyframe and re-walk forward); this class only goes forward.
//
// Video packets are read into a bounded queue rather than decoded straight away, so pumpAudio()
// can read ahead and keep the audio ahead of the playhead however few frames the caller buffers.
//
// Every time in and out -- frames, audio, seek(), the keyframe index, duration() -- counts from the
// first video frame, which is at 0: a container may start its clock late (MPEG-TS), or a B-frame delay
// with no edit list to hide it (FLV, fragmented MP4) may put the first frame a frame or two in, and a
// playhead starts at 0. Audio from before the first frame is dropped. A frame with no timestamp -- the
// last few a decoder flushes from an AVI or MPEG-PS with B-frames, or every frame of a raw H.264 or
// HEVC stream -- follows the one before it; one that follows nothing since a seek into the file cannot
// be placed, and is skipped.
class VideoDecoder {
public:
    VideoDecoder() = default;
    ~VideoDecoder();
    VideoDecoder(const VideoDecoder&) = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;

    // Open `path`. Returns false and fills `err` on failure (bad path, no video
    // stream, unsupported codec, no frame that decodes). A file with no audio stream still opens.
    // `abort` (optional) is polled by FFmpeg during blocking I/O: once it reads true, opening or
    // reading gives up promptly (VideoStream uses it to stop its worker). Decodes the first frame,
    // to learn when it is; the first decodeNext() hands it out.
    bool open(const std::string& path, std::string& err, const std::atomic<bool>* abort = nullptr);

    bool   isOpen()   const { return fmt_ != nullptr; }
    int    width()    const { return width_; }
    int    height()   const { return height_; }
    double duration() const { return duration_; }      // seconds (0 if unknown)
    bool   hasAudio() const { return astream_ >= 0; }
    int    audioRate() const { return kOutRate; }      // we always resample to this
    int    audioChannels() const { return audioChannels_; }  // source channels (before our mono downmix)
    static constexpr int kOutRate = 48000;             // 48 kHz mono float out

    // Nominal seconds per frame, from the stream's average (else real) frame rate -- a raw stream's
    // from its decoder, which reads the rate from the stream itself -- or 1/30 if unknown.
    double frameDuration() const;

    // The first keyframe after time `t`, from the container's index -- possibly listed by its decode
    // time, a frame or two early. `key` is +inf when none is known after `t`: the index holds none, or it
    // is no guide (MPEG-TS and MPEG-PS list every packet they probe while seeking; a raw stream only
    // ever seeks to its start). False when the stream has no index (yet).
    bool nextKeyframeAfter(double t, double& key) const;

    // Seek so the next decodeNext() resumes at the keyframe at or before time `t` (seconds; at or
    // before 0, the start of the file). Where that fails -- or in a raw stream, which has no times to
    // search by -- it goes to the start of the file instead, and the caller decodes forward to its
    // target: slow, but right. Flushes the decoders, the queued packets and the pending audio, so
    // nothing stale leaks across. False when not even the start could be reached.
    bool seek(double t);

    // Decode the next video frame in source order WITHOUT converting it. Audio met on the way is
    // decoded into the pending buffer (see takeAudio). False at the end of the stream.
    bool decodeNext(DecodedFrame& out);

    // Convert a decoded frame to RGBA8 -- rows TOP-DOWN, `dstStride` (at least width * 4) bytes
    // apart -- with threaded swscale. False on failure (e.g. the frame's size changed mid-stream).
    bool convert(const DecodedFrame& f, std::uint8_t* dst, int dstStride);

    // Read ahead -- queueing video packets instead of decoding them -- until the audio is settled up
    // to time `t` (see audioSettledUpTo), the input ends, or kMaxQueuedBytes of video packets
    // are waiting.
    void pumpAudio(double t);

    // Tests only: cap the read-ahead at `bytes` instead of kMaxQueuedBytes.
    void setMaxQueuedBytes(std::size_t bytes) { maxQueuedBytes_ = bytes; }

    // The time up to which no more audio will arrive: the end of the decoded audio, or -- once
    // the demuxer has read kAudioSettleSlack past a point without meeting audio for it -- that
    // point. +inf at the end of the input or with no audio stream. When pumpAudio() finds the
    // queue still at kMaxQueuedBytes with no packet taken off it since it last stopped there, the
    // caller has stopped decoding -- it is waiting for this audio -- and nothing will ever drain
    // the queue: the audio then counts as settled up to the latest packet read. (Reading further
    // would take unbounded memory: 4K ProRes runs past 64 MB in under a second.) While the caller
    // is still taking packets, it only waits: audio that a file puts after a long run of video
    // (fragmented MOV writes each fragment's video, then its audio) still arrives.
    double audioSettledUpTo() const;

    // Move out the next run of audio decoded since the last call (48 kHz mono float): samples
    // back to back from `startT`, the time of the first. The source's timestamps are followed --
    // a gap or an overlap of more than kAudioJitter starts a new run where the audio resumes --
    // and `continues` says whether this run carries straight on from the previous one (false for
    // the first run after open() or a seek). False when there is none: call until then.
    bool takeAudio(std::vector<float>& out, double& startT, bool& continues);

    // Decode the next video frame in source order into `out`, converted to bottom-up RGBA. Any
    // audio decoded on the way (audio packets interleaved before this video frame) is appended
    // to `audio` as 48 kHz mono float, its runs back to back. The first time audio is appended
    // into a freshly-cleared `audio`, `audioStartT` is set to that audio's time and
    // `audioStartValid` to true (left untouched on later calls so a multi-call fill keeps one
    // contiguous timeline). Returns false at end of stream.
    bool decodeFrame(VideoFrame& out, std::vector<float>& audio,
                     double& audioStartT, bool& audioStartValid);

    static constexpr std::size_t kMaxQueuedBytes   = std::size_t(64) << 20;  // read-ahead cap
    static constexpr double      kAudioSettleSlack = 2.0;                    // seconds
    // Seconds an audio timestamp may stray from where the audio so far ends and still carry on
    // back to back: well above timestamp rounding (Matroska and FLV keep milliseconds), which must
    // not click, and under the ~45 ms by which audio leading the picture starts to show -- a hole
    // shorter than this is closed up, and what follows it plays that much early.
    static constexpr double      kAudioJitter      = 0.04;

private:
    // Decoded audio, samples back to back from `start` (container time).
    struct AudioRun { double start = 0.0; bool continues = false; std::vector<float> s; };

    void close();
    void resetStreamState();
    void clearQueue();
    bool readPacket();
    void drainAudio();
    void placeAudio(double start, const float* s, std::size_t n);

    AVFormatContext* fmt_     = nullptr;
    AVCodecContext*  vctx_    = nullptr;   // video decoder
    AVCodecContext*  actx_    = nullptr;   // audio decoder (null if no audio)
    SwsContext*      sws_     = nullptr;   // decodeFrame(): -> RGBA, single-threaded, bottom-up
    SwsContext*      swsThr_  = nullptr;   // convert(): -> RGBA, threaded, top-down
    int              swsThrFmt_ = -1;      // the pixel format swsThr_ was built for
    SwrContext*      swr_     = nullptr;   // -> 48 kHz mono float, for the input below
    int              swrFormat_ = -1, swrRate_ = 0;
    std::uint64_t    swrLayout_ = 0;       // the input channel mask (0: not a mask)...
    int              swrChannels_ = 0;     // ...and count
    AVFrame*         frame_   = nullptr;   // reused decode target (video or audio)
    AVFrame*         dstFrame_ = nullptr;  // convert()'s destination wrapper
    AVPacket*        pkt_     = nullptr;

    std::deque<AVPacket*> vq_;             // video packets read ahead, not yet decoded
    std::size_t           queuedBytes_ = 0;
    std::size_t           maxQueuedBytes_ = kMaxQueuedBytes;

    int    vstream_ = -1;
    int    astream_ = -1;
    int    width_   = 0;
    int    height_  = 0;
    double duration_   = 0.0;
    int    audioChannels_ = 0;  // source audio channel count (0 if no audio)
    double vTimeBase_  = 0.0;   // seconds per video stream tick
    bool   raw_        = false; // a raw stream (H.264, HEVC...): no times to seek by
    bool   indexUsable_ = true; // false: the container's index lists more than keyframes (MPEG-TS/-PS)
    double startT_     = 0.0;   // the container's time of the first video frame: subtracted from
                                // every time handed out, added to every time taken in
    double nextT_      = 0.0;   // container time a frame with no timestamp is given: just after the
                                // last; NaN when unknown (see decodeNext)
    DecodedFrame first_;        // decoded by open() to find startT_; the first decodeNext() returns it
    double aTimeBase_  = 0.0;   // seconds per audio stream tick
    bool   demuxEof_   = false; // read the last packet (and flushed the audio decoder)
    bool   vflushed_   = false; // sent the video decoder its end of stream
    double demuxedT_   = -std::numeric_limits<double>::infinity();   // latest packet time read (container time)
    double audioEndT_  = -std::numeric_limits<double>::infinity();   // end of the decoded audio (container time)
    double capSettledT_ = -std::numeric_limits<double>::infinity();  // settled when the read-ahead got stuck at the cap
    std::uint64_t packetsTaken_ = 0;                                  // video packets taken off the queue so far...
    std::uint64_t takenAtCap_   = ~std::uint64_t(0);                  // ...when pumpAudio last stopped at the cap
    double audioFloorT_ = -std::numeric_limits<double>::infinity();  // audio before this is dropped (the first frame)
    bool   runOpen_    = false; // the last audio decoded was kept: audio carrying on from it continues its run

    std::deque<AudioRun>      audioRuns_;             // decoded since the last takeAudio(), oldest first
    std::vector<std::uint8_t> rgba_;                  // decodeFrame()'s RGBA target
    std::vector<float>        aScratch_;              // reused swr output scratch
    std::vector<float>        legacyAudio_;           // decodeFrame()'s takeAudio scratch
};

} // namespace oss
