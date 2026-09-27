#pragma once
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "core/TimedAudio.h"
#include "core/VideoPlan.h"
#include "gfx/VideoDecoder.h"

namespace oss {

// What the Video Player node asks its stream for each frame. Times are unwrapped (core/VideoPlan.h).
struct VideoRequest {
    double u       = 0.0;    // the playhead
    float  rate    = 0.0f;   // signed speed; 0 while paused (keeps the last direction)
    bool   loop    = true;
    bool   offline = false;  // an offline render: reverse stretches keep consecutive frames
    double lapLo   = -std::numeric_limits<double>::infinity();   // loop off: the playhead's lap
    double lapHi   =  std::numeric_limits<double>::infinity();
};

// A background decoder for one video file. A worker thread opens the file, then keeps a fixed pool
// of RGBA frames decoded ahead of the requested playhead -- or, in reverse, in keyframe-to-frame
// stretches -- plus the audio around it. The graph thread only posts requests and picks up finished
// frames, so it never waits on FFmpeg during live playback. GL-free: the node uploads the pixels.
//
// Threading: one mutex guards the request, the pool, the ready queue, the audio and the status. The
// worker holds it only for bookkeeping, never while decoding or converting. The frame frameAt()
// returns is "checked out": the worker will not reuse its buffer until the next frameAt().
class VideoStream {
public:
    enum class State { Opening, Ready, Failed };
    struct Info {
        int    width = 0, height = 0;
        double duration = 0.0;            // seconds; 0 = unknown
        double frameDur = 1.0 / 30.0;     // nominal seconds per frame
        bool   hasAudio = false;
    };
    // The frame to display: tightly packed RGBA8 rows, TOP row first, width * 4 bytes apart.
    struct FrameView {
        const std::uint8_t* rgba   = nullptr;
        double              t      = 0.0;   // unwrapped time
        std::uint64_t       serial = 0;     // changes whenever a different frame is shown
    };

    // `readAheadBytes` caps the decoder's read-ahead (tests set it small; see VideoDecoder::pumpAudio).
    explicit VideoStream(std::string path, std::size_t poolBytes = kVideoPoolBytes,
                         std::size_t readAheadBytes = VideoDecoder::kMaxQueuedBytes);
    ~VideoStream();                        // stops and joins the worker

    // Destroy `s` on a background thread and return at once: tearing a stream down -- joining its worker,
    // freeing its frames, closing its decoder -- takes 0.1-0.5 s at 4K, too long for the UI thread. Its
    // worker is told to stop straight away; streams go in the order retired, the last before the process exits.
    static void retire(std::unique_ptr<VideoStream> s);
    VideoStream(const VideoStream&) = delete;
    VideoStream& operator=(const VideoStream&) = delete;

    State       state() const;
    std::string error() const;
    Info        info() const;             // valid once Ready

    void request(const VideoRequest& r);

    // The frame for u -- the greatest decoded time <= u -- or, when none is decoded yet, whatever is
    // already on screen (or the earliest ready frame if nothing is). False when there is nothing to
    // show. The pointer stays valid until the next frameAt() call or destruction.
    bool frameAt(double u, FrameView& out);

    // Offline: the frame for u is held -- the frame decoded at or before u with none decoded between it
    // and u -- and, playing forward, no more audio can arrive for times up to u. True once Failed.
    bool frameReadyFor(double u) const;
    // Wait until frameReadyFor(u), at most timeoutSeconds; its answer (false on a timeout, or once the
    // stream is being destroyed).
    bool waitForFrame(double u, double timeoutSeconds);

    // n output samples spanning source time [u0, u1] (see TimedAudio::sample).
    void readAudio(double u0, double u1, float* out, int n) const;

    // Diagnostics: seeks made and reverse stretches decoded so far. Both grow with the ground the
    // playhead covers; a step that achieved nothing and was planned again would spin the worker, and
    // show here.
    std::uint64_t seeks() const            { return seeks_.load(); }
    std::uint64_t reverseStretches() const { return reverseStretches_.load(); }

private:
    // A queued frame. `until` is the time of the next frame decoded after it: the frame for u covers [t, until).
    struct Slot { double t = 0.0; std::uint64_t serial = 0; int buf = -1; double until = 0.0; };

    // Worker thread.
    void run();
    void step();
    void waitForWork();
    bool peekNext();
    void takeAudio();
    void pumpAudio(double u, bool offline);
    void seekDecoder(double t);
    bool seekLanding(double end, double slack);
    void seekTo(double target);
    void catchUp(double target);
    void fill();
    void wrap();
    void reverseStretch(double to, bool fresh, bool offline);
    bool publish(DecodedFrame& f, double t, bool runStart);
    bool publishSlot(DecodedFrame& f, double t, double until);
    double nextFrameTime() const;
    bool directionChanged() const;
    void setFailed(const std::string& msg);

    // Under m_.
    int  readyFrameLocked(double u) const;   // index in ready_ of the frame for u, or -1
    bool holdsLocked(double u) const;
    bool readyLocked(double u) const;
    int  acquireLocked();
    void releaseLocked(int buf);
    void insertReadyLocked(const Slot& s);
    void flushReadyLocked();
    void recycleLocked(const VideoRequest& r, int dir);
    void setRunHiLocked();

    const std::string path_;
    const std::size_t poolBytes_;
    const std::size_t readAheadBytes_;
    std::atomic<bool> stop_{false};
    std::atomic<std::uint64_t> seeks_{0}, reverseStretches_{0};

    // --- guarded by m_ ---
    mutable std::mutex      m_;
    std::condition_variable cv_;
    State         state_ = State::Opening;
    std::string   error_;
    Info          info_;
    VideoRequest  req_;
    int           dir_ = 1;                // direction of the latest request (a pause keeps it)
    int           planDir_ = 1;            // the direction the worker last planned for...
    bool          planOffline_ = false;    // ...and whether for an offline render
    std::uint64_t reqSerial_ = 0, freedSerial_ = 0;
    std::vector<std::unique_ptr<std::uint8_t[]>> pool_;
    std::vector<int>  free_;
    std::vector<Slot> ready_;              // ascending t
    Slot          shown_;                  // checked out by frameAt() (buf -1: none)
    std::uint64_t nextSerial_ = 1;
    TimedAudio    audio_;
    double        audioSettledU_ = -std::numeric_limits<double>::infinity();
    double        audioLapStart_ = 0.0;    // audio for earlier laps is complete
    bool          runValid_ = false;       // consecutive frames decided over [runLo_, runHi_)
    bool          runOffline_ = false;     // ...by offline stretches (reverse: live ones keep every stride-th)
    double        runLo_ = 0.0, runHi_ = 0.0;

    // --- worker only ---
    VideoDecoder  dec_;
    Info          winfo_;                  // copy of info_ (constant once Ready)
    DecodedFrame  next_;                   // decoded, not yet converted: one frame of lookahead
    double        lapOffset_ = 0.0;        // unwrapped time of the decoder's lap start
    double        lastT_ = 0.0;            // unwrapped time of the last frame taken from the decoder
    bool          eof_ = false;
    bool          coverValid_ = false;     // reverse: this run's stretches reach down to coverLo_...
    double        coverLo_ = 0.0;
    double        coverHi_ = 0.0;          // ...from here, the end of the run's first stretch
    double        lastStretchSeconds_ = 0.0;   // wall time the last reverse stretch took to decode
    bool          seekPinned_ = false;     // the last seek could not land at or before...
    double        pinnedFrom_ = 0.0;       // ...this target (it precedes the file's first frame)
    double        noSeekBelow_ = -std::numeric_limits<double>::infinity();   // see seekTo()
    bool          audioChunkOpen_ = false;
    double        audioClipHi_ = std::numeric_limits<double>::infinity();
    std::uint64_t seenReq_ = 0, seenFreed_ = 0;
    std::vector<float> audioTmp_;

    std::thread thread_;                   // LAST: started once everything above exists
};

} // namespace oss
