#include "gfx/VideoStream.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
#include <exception>
#include <new>
#include <stdexcept>

namespace oss {

namespace { constexpr double kInf = std::numeric_limits<double>::infinity(); }

VideoStream::VideoStream(std::string path, std::size_t poolBytes, std::size_t readAheadBytes)
    : path_(std::move(path)), poolBytes_(poolBytes), readAheadBytes_(readAheadBytes), audio_(VideoDecoder::kOutRate) {
    thread_ = std::thread([this] { run(); });
}

VideoStream::~VideoStream() {
    { std::lock_guard<std::mutex> lk(m_); stop_ = true; }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();   // bounded: the worker checks stop_ between frames,
}                                               // and FFmpeg's I/O polls it while blocked

namespace {

// Destroys retired streams one after another on its own thread; joined at exit.
class Reaper {
public:
    ~Reaper() {
        { std::lock_guard<std::mutex> lk(m_); done_ = true; }
        cv_.notify_all();
        if (thread_.joinable()) thread_.join();
    }
    // Never throws: it runs in destructors. With no thread (or no room to queue), the stream goes here.
    void add(std::unique_ptr<VideoStream> s) {
        std::unique_lock<std::mutex> lk(m_);
        try {
            if (!thread_.joinable()) thread_ = std::thread([this] { run(); });
            queue_.push_back(std::move(s));
        } catch (...) {
            lk.unlock();
            s.reset();
            return;
        }
        cv_.notify_all();
    }

private:
    void run() {
        std::unique_lock<std::mutex> lk(m_);
        for (;;) {
            cv_.wait(lk, [this] { return done_ || !queue_.empty(); });
            if (queue_.empty()) return;                // done, and nothing left to destroy
            std::unique_ptr<VideoStream> s = std::move(queue_.front());
            queue_.pop_front();
            lk.unlock();
            s.reset();
            lk.lock();
        }
    }
    std::mutex m_;
    std::condition_variable cv_;
    std::deque<std::unique_ptr<VideoStream>> queue_;
    bool done_ = false;
    std::thread thread_;
};

} // namespace

void VideoStream::retire(std::unique_ptr<VideoStream> s) {
    if (!s) return;
    { std::lock_guard<std::mutex> lk(s->m_); s->stop_ = true; }   // stop decoding now, not when its turn comes
    s->cv_.notify_all();
    try {
        static Reaper reaper;                      // (building it can allocate)
        reaper.add(std::move(s));
    } catch (...) {
        s.reset();
    }
}

VideoStream::State VideoStream::state() const { std::lock_guard<std::mutex> lk(m_); return state_; }
std::string VideoStream::error() const        { std::lock_guard<std::mutex> lk(m_); return error_; }
VideoStream::Info VideoStream::info() const   { std::lock_guard<std::mutex> lk(m_); return info_; }

void VideoStream::request(const VideoRequest& r) {
    {
        std::lock_guard<std::mutex> lk(m_);
        req_ = r;
        if (r.rate > 0.0f)      dir_ = 1;
        else if (r.rate < 0.0f) dir_ = -1;        // 0 (paused) keeps the direction
        ++reqSerial_;
    }
    cv_.notify_all();
}

bool VideoStream::frameAt(double u, FrameView& out) {
    bool freed = false;
    {
        std::lock_guard<std::mutex> lk(m_);
        if (state_ != State::Ready) return false;
        int idx = readyFrameLocked(u);
        if (idx < 0 && shown_.buf < 0 && !ready_.empty()) idx = 0;   // nothing up yet: the nearest
        const bool shownFits = shown_.buf >= 0 && shown_.t <= u + kVideoTimeEps;
        if (idx >= 0 && (!shownFits || ready_[(std::size_t)idx].t > shown_.t)) {
            if (shown_.buf >= 0) { releaseLocked(shown_.buf); freed = true; }
            shown_ = ready_[(std::size_t)idx];
            ready_.erase(ready_.begin() + idx);
        }
        if (shown_.buf < 0) return false;
        // Frames the playhead has passed can never be shown again.
        for (std::size_t i = 0; i < ready_.size();) {
            const bool passed = dir_ >= 0 ? ready_[i].t < shown_.t : ready_[i].t > shown_.t;
            if (passed) { releaseLocked(ready_[i].buf); ready_.erase(ready_.begin() + (std::ptrdiff_t)i); freed = true; }
            else ++i;
        }
        out.rgba   = pool_[(std::size_t)shown_.buf].get();
        out.t      = shown_.t;
        out.serial = shown_.serial;
    }
    if (freed) cv_.notify_all();
    return true;
}

bool VideoStream::frameReadyFor(double u) const {
    std::lock_guard<std::mutex> lk(m_);
    return readyLocked(u);
}

bool VideoStream::waitForFrame(double u, double timeoutSeconds) {
    std::unique_lock<std::mutex> lk(m_);
    return cv_.wait_for(lk, std::chrono::duration<double>(timeoutSeconds),
                        [&] { return stop_.load() || readyLocked(u); }) && readyLocked(u);
}

void VideoStream::readAudio(double u0, double u1, float* out, int n) const {
    std::lock_guard<std::mutex> lk(m_);
    audio_.sample(u0, u1, out, n);
}

// --- under m_ -------------------------------------------------------------------------------

int VideoStream::readyFrameLocked(double u) const {
    return videoSelectFrame((int)ready_.size(), u, [this](int i) { return ready_[(std::size_t)i].t; });
}

bool VideoStream::readyLocked(double u) const {
    if (state_ == State::Failed) return true;                 // nothing more will come
    if (state_ != State::Ready || !runValid_) return false;
    if (dir_ != planDir_ || (dir_ < 0 && req_.offline && !planOffline_)) return false;   // step() flushes it all first
    if (dir_ < 0 && !runOffline_) return false;               // live stretches skipped frames
    if (u < runLo_ - kVideoTimeEps || u >= runHi_ - kVideoTimeEps) return false;
    if (!holdsLocked(u)) return false;
    if (dir_ < 0 || !info_.hasAudio) return true;             // reverse: the stretch brought its audio
    return u < audioLapStart_ - kVideoTimeEps || audioSettledU_ >= u - kVideoTimeEps;
}

int VideoStream::acquireLocked() {
    if (free_.empty()) return -1;
    const int b = free_.back();
    free_.pop_back();
    return b;
}

void VideoStream::releaseLocked(int buf) {
    free_.push_back(buf);
    ++freedSerial_;
}

void VideoStream::insertReadyLocked(const Slot& s) {
    auto at = std::upper_bound(ready_.begin(), ready_.end(), s.t,
                               [](double t, const Slot& o) { return t < o.t; });
    ready_.insert(at, s);
}

void VideoStream::flushReadyLocked() {
    for (const Slot& s : ready_) releaseLocked(s.buf);
    ready_.clear();
}

// Release queued frames that can never be shown for this request: in forward play those older than
// the frame for the target, in reverse those after it (the playhead has passed them), and with loop
// off those before the playhead's lap. Frames past its end stay: the loop-off playhead stops just
// short of them, and they are the next ones to show if loop comes back on. Offline, the request is
// the node's guess at the next frame's playhead, which an automated rate can move: while the guess's frame
// is held (so nothing has to make room for it), the frames between the one on screen and the guess stay
// too -- one may be the frame the real playhead needs, and a frame given up is a seek to decode it again.
// Only while the frame on screen is on the way to the guess, though: after a jump or a flip it lies
// beyond it, and so does nothing to keep.
void VideoStream::recycleLocked(const VideoRequest& r, int dir) {
    double frameT = (shown_.buf >= 0 && shown_.t <= r.u + kVideoTimeEps) ? shown_.t : -kInf;
    const int best = readyFrameLocked(r.u);
    if (best >= 0) frameT = std::max(frameT, ready_[(std::size_t)best].t);
    double lo = frameT;                                                   // forward: frames before lo go
    double hi = std::isfinite(frameT) ? frameT : r.u + kVideoTimeEps;     // reverse: frames after hi go
    if (r.offline && shown_.buf >= 0 && holdsLocked(r.u)) {
        if (shown_.t <= lo) lo = shown_.t;
        if (shown_.t >= hi) hi = shown_.t;
    }
    for (std::size_t i = 0; i < ready_.size();) {
        const Slot& s = ready_[i];
        bool drop = dir >= 0 ? s.t < lo : s.t > hi;
        if (!r.loop && s.t < r.lapLo - kVideoTimeEps) drop = true;
        if (drop) { releaseLocked(s.buf); ready_.erase(ready_.begin() + (std::ptrdiff_t)i); }
        else ++i;
    }
}

// The exclusive end of the consecutive run of decided frames (forward): the next frame's time when
// it has been peeked, at the end of the file the lap's end (forever when the duration is unknown),
// else just past the last frame taken. Reads worker state: called by the worker only.
void VideoStream::setRunHiLocked() {
    if (next_.valid())  runHi_ = lapOffset_ + next_.t;
    else if (eof_)      runHi_ = winfo_.duration > 0.0 ? lapOffset_ + winfo_.duration : kInf;
    else                runHi_ = lastT_ + 2.0 * kVideoTimeEps;
}

// The frame held for u, exactly: a held slot with t <= u < until (the next frame decoded after it).
bool VideoStream::holdsLocked(double u) const {
    if (shown_.buf >= 0 && shown_.t <= u + kVideoTimeEps && u < shown_.until - kVideoTimeEps) return true;
    const int i = readyFrameLocked(u);
    return i >= 0 && u < ready_[(std::size_t)i].until - kVideoTimeEps;
}

// --- worker ---------------------------------------------------------------------------------

// The time of the next frame the decoder gives (the lap's end at its end): the exclusive end of the last frame taken.
double VideoStream::nextFrameTime() const {
    if (next_.valid()) return lapOffset_ + next_.t;
    if (eof_)          return winfo_.duration > 0.0 ? lapOffset_ + winfo_.duration : kInf;
    return lastT_ + 2.0 * kVideoTimeEps;
}

void VideoStream::setFailed(const std::string& msg) {
    {
        std::lock_guard<std::mutex> lk(m_);
        state_ = State::Failed;
        error_ = msg;
    }
    cv_.notify_all();
}

void VideoStream::run() {
    try {
        std::string err;
        dec_.setMaxQueuedBytes(readAheadBytes_);
        if (!dec_.open(path_, err, &stop_)) { setFailed(err); return; }
        Info inf;
        inf.width    = dec_.width();
        inf.height   = dec_.height();
        inf.duration = dec_.duration();
        inf.frameDur = dec_.frameDuration();
        inf.hasAudio = dec_.hasAudio();
        const int n = videoPoolFrames(poolBytes_, inf.width, inf.height);
        std::vector<std::unique_ptr<std::uint8_t[]>> pool;
        try {
            pool.reserve((std::size_t)n);
            for (int i = 0; i < n; ++i)
                pool.emplace_back(new std::uint8_t[(std::size_t)inf.width * inf.height * 4]);
        } catch (const std::bad_alloc&) {
            setFailed("not enough memory for " + std::to_string(inf.width) + "x" +
                      std::to_string(inf.height) + " frames");
            return;
        }
        winfo_ = inf;
        lastT_ = -inf.frameDur;                                // the first frame is expected at 0
        {
            std::lock_guard<std::mutex> lk(m_);
            info_ = inf;
            pool_ = std::move(pool);
            for (int i = n - 1; i >= 0; --i) free_.push_back(i);
            state_ = State::Ready;
        }
        cv_.notify_all();
        while (!stop_) step();
    } catch (const std::exception& e) {
        setFailed(std::string("video worker: ") + e.what());
    } catch (...) {
        setFailed("video worker: unknown error");
    }
}

void VideoStream::step() {
    VideoRequest r;
    int dir = 1;
    bool changed = false, toOffline = false, lostTarget = false;
    VideoPlanInput in;
    {
        std::lock_guard<std::mutex> lk(m_);
        r = req_;
        dir = dir_;
        seenReq_ = reqSerial_;
        seenFreed_ = freedSerial_;
        changed = dir != planDir_;
        // An offline render starting in reverse cannot use live stretches: they kept every stride-th frame.
        toOffline = dir < 0 && r.offline && !planOffline_;
        if (changed || toOffline) { flushReadyLocked(); runValid_ = false; }
        planDir_ = dir;
        planOffline_ = r.offline;
        recycleLocked(r, dir);
        if (!r.loop && runValid_ && runLo_ < r.lapLo) {    // what was decided before the lap is gone
            if (runHi_ <= r.lapLo) runValid_ = false;
            else                   runLo_ = r.lapLo;
        }
        if (dir >= 0) audio_.retain(r.u - kVideoAudioKeep, kInf, r.u);
        else          audio_.retain(-kInf, r.u + kVideoAudioKeep, r.u);
        in.lowest = kInf;
        if (shown_.buf >= 0) in.lowest = shown_.t;
        if (!ready_.empty()) in.lowest = std::min(in.lowest, ready_.front().t);
        in.freeBuffers = (int)free_.size();
        in.poolSize    = (int)pool_.size();
        // Offline forward, readiness can wait for ever on what only a seek back to u mends: u's frame was
        // released while frames past it are held (the planner counts what is held), or it is held but a
        // catch-up to a guess far ahead restarted the run past it. Reverse needs no such help: while the
        // guess's frame is held nothing between it and the frame on screen is released (recycleLocked), and
        // when it is not, the guess lies below what the stretches cover and gets a fresh stretch -- which
        // leaves the real playhead above it, stranded, to get another.
        if (r.offline && dir >= 0) {
            if (!holdsLocked(r.u)) {
                lostTarget = shown_.buf >= 0 && shown_.t > r.u + kVideoTimeEps;
                for (const Slot& s : ready_) lostTarget = lostTarget || s.t > r.u + kVideoTimeEps;
            } else {
                lostTarget = runValid_ && r.u < runLo_ - kVideoTimeEps;
            }
        }
    }
    if (lostTarget) in.lowest = kInf;                             // seek back to u
    if (changed || toOffline) coverValid_ = false;
    if (!r.loop && coverValid_ && coverLo_ < r.lapLo) {     // likewise what reverse had covered
        if (coverHi_ <= r.lapLo) coverValid_ = false;
        else                     coverLo_ = r.lapLo;
    }

    in.target     = r.u;
    in.dir        = dir;
    in.dirChanged = changed;
    in.loop       = r.loop;
    in.duration   = winfo_.duration;
    in.frameDur   = winfo_.frameDur;
    in.lapLo      = r.lapLo;
    in.lapHi      = r.lapHi;
    in.head       = next_.valid() ? lapOffset_ + next_.t : lastT_ + winfo_.frameDur;
    in.eof        = eof_ && !next_.valid();
    double key = 0.0;
    in.keyKnown   = dec_.nextKeyframeAfter(in.head - lapOffset_, key);
    in.nextKey    = lapOffset_ + key;
    in.lapEnd     = winfo_.duration > 0.0 ? lapOffset_ + winfo_.duration : kInf;
    if (!std::isfinite(in.lowest) && !lostTarget) in.lowest = in.head;
    in.seekPinned = seekPinned_;
    in.pinnedFrom = pinnedFrom_;
    in.noSeekBelow = noSeekBelow_;
    in.coverValid = coverValid_;
    in.coverLo    = coverLo_;
    in.coverHi    = coverHi_;
    in.lead       = r.offline ? 0.0 : std::min(2.0, std::fabs((double)r.rate) * lastStretchSeconds_);

    const VideoStep s = videoNextStep(in);
    // Keep the audio ahead of the playhead -- but not before a seek, which throws the read-ahead away.
    if (dir >= 0 && s.kind != VideoStepKind::Seek) pumpAudio(r.u, r.offline);
    switch (s.kind) {
        case VideoStepKind::Wait:    waitForWork(); break;
        case VideoStepKind::Fill:    fill(); break;
        case VideoStepKind::CatchUp: catchUp(s.to); break;
        case VideoStepKind::Seek:    seekTo(s.to); break;
        case VideoStepKind::Wrap:    wrap(); break;
        case VideoStepKind::Reverse: reverseStretch(s.to, s.fresh, r.offline); break;
    }
}

void VideoStream::waitForWork() {
    std::unique_lock<std::mutex> lk(m_);
    cv_.wait_for(lk, std::chrono::milliseconds(100), [this] {
        return stop_.load() || reqSerial_ != seenReq_ || freedSerial_ != seenFreed_;
    });
}

bool VideoStream::directionChanged() const {
    std::lock_guard<std::mutex> lk(m_);
    return dir_ != planDir_;
}

// Decode the next frame into next_ (without converting it) unless one is already waiting. When the
// duration is known, a frame at or past it ends the lap.
bool VideoStream::peekNext() {
    if (next_.valid()) return true;
    if (eof_) return false;
    bool ok = dec_.decodeNext(next_);
    if (ok && winfo_.duration > 0.0 && next_.t >= winfo_.duration - kVideoTimeEps) {
        next_.reset();
        ok = false;
    }
    if (!ok) eof_ = true;
    takeAudio();
    return ok;
}

// Move the decoder's pending audio into the store -- a new chunk after a seek or wrap, and wherever the
// source's audio jumps (a gap, or an overlap) -- clipped below audioClipHi_, and publish how far the
// audio is settled.
void VideoStream::takeAudio() {
    double start = 0.0;
    bool continues = false;
    while (dec_.takeAudio(audioTmp_, start, continues)) {
        std::lock_guard<std::mutex> lk(m_);
        if (!audioChunkOpen_ || !continues) { audio_.beginChunk(lapOffset_ + start); audioChunkOpen_ = true; }
        audio_.append(audioTmp_.data(), audioTmp_.size(), audioClipHi_);
    }
    const double settled = dec_.audioSettledUpTo();
    {
        std::lock_guard<std::mutex> lk(m_);
        audioSettledU_ = lapOffset_ + settled;
        audioLapStart_ = lapOffset_;
    }
    cv_.notify_all();
}

// Read the audio ahead of u. An offline render cannot go on without it: the decoder may give it up
// only then (VideoDecoder::pumpAudio).
void VideoStream::pumpAudio(double u, bool offline) {
    if (!winfo_.hasAudio) return;
    dec_.pumpAudio(u + kVideoAudioLead - lapOffset_, offline);
    takeAudio();
}

// Seek the decoder. One that cannot even get back to the start of its file can neither loop nor play in
// reverse, and every step would seek again: fail instead of spinning.
void VideoStream::seekDecoder(double t) {
    if (!dec_.seek(t) && !stop_) throw std::runtime_error("cannot seek in this file");
}

// Seek in the decoder's lap so the next frame is at or before unwrapped time `end` (+ `slack`: callers
// pass the rule their decode loop admits frames by). A seek can land late: where the index holds
// decode times (FLV, fragmented MP4, B-frames without an edit list) a seek just below a keyframe lands
// ON it; a demuxer that searches by timestamp (MPEG-TS) can overshoot by a keyframe interval; some
// (FLV, MPEG-TS) find no frame at all when asked for the last ones. So retry further back -- 1 s, 2 s,
// 4 s... -- down to the start of the file. False when even that lands after `end` or finds nothing.
bool VideoStream::seekLanding(double end, double slack) {
    const double src = end - lapOffset_;
    for (double back = 0.0;; back = back > 0.0 ? 2.0 * back : 1.0) {
        const double at = std::max(0.0, src - back);
        seekDecoder(at);
        next_.reset();
        eof_ = false;
        audioChunkOpen_ = false;
        if (peekNext() && lapOffset_ + next_.t <= end + slack) return true;
        if (at <= 0.0) return false;
    }
}

void VideoStream::seekTo(double target) {
    const double before = next_.valid() ? lapOffset_ + next_.t : lastT_ + winfo_.frameDur;   // the head
    seeks_.fetch_add(1);
    {
        // Frames after the target are stale; the newest one at or before it stays up until the
        // seek delivers a better one (recycleLocked drops the older ones).
        std::lock_guard<std::mutex> lk(m_);
        for (std::size_t i = 0; i < ready_.size();) {
            if (ready_[i].t > target + kVideoTimeEps) { releaseLocked(ready_[i].buf); ready_.erase(ready_.begin() + (std::ptrdiff_t)i); }
            else ++i;
        }
        runValid_ = false;
        audioSettledU_ = -kInf;
    }
    lapOffset_ = winfo_.duration > 0.0 ? videoLapStart(target, winfo_.duration) : 0.0;
    const bool landed = seekLanding(target, kVideoTimeEps);
    seekPinned_ = !landed;
    pinnedFrom_ = target;
    noSeekBelow_ = videoNoSeekBelow(target, before, next_.valid() ? lapOffset_ + next_.t : kInf);
    lastT_ = target;
    catchUp(target);
}

// Decode forward without converting until the next frame is past `target`, then convert only the
// frame for `target`. Live, the target chases a playhead that keeps moving meanwhile -- but only for
// kVideoCatchUpSlice: a decoder slower than the playhead (4K HEVC at 2x) would otherwise chase it
// forever and never show a frame. After the slice it shows the best frame reached and re-plans,
// which may seek ahead instead.
void VideoStream::catchUp(double target) {
    DecodedFrame held;
    double heldT = 0.0;
    const auto began = std::chrono::steady_clock::now();
    bool offline = false;
    { std::lock_guard<std::mutex> lk(m_); offline = req_.offline; }
    while (!stop_ && peekNext()) {
        if (!offline && held.valid() &&
            std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count() > kVideoCatchUpSlice)
            break;
        const double t = lapOffset_ + next_.t;
        if (t > target + kVideoTimeEps) break;
        held = std::move(next_);
        heldT = t;
        lastT_ = t;
        pumpAudio(target, offline);
        if (directionChanged()) return;
        std::lock_guard<std::mutex> lk(m_);
        if (!req_.offline && req_.u > target) target = req_.u;
    }
    if (!held.valid() && next_.valid()) {          // the target precedes the first frame: show that one
        held = std::move(next_);
        heldT = lapOffset_ + held.t;
        lastT_ = heldT;
        peekNext();
    }
    if (held.valid()) {
        publish(held, heldT, true);
    } else {
        { std::lock_guard<std::mutex> lk(m_); setRunHiLocked(); }
        cv_.notify_all();
    }
}

void VideoStream::fill() {
    if (!peekNext()) {                             // end of the lap: extend the run to its end
        { std::lock_guard<std::mutex> lk(m_); setRunHiLocked(); }
        cv_.notify_all();
        return;
    }
    DecodedFrame f = std::move(next_);
    const double t = lapOffset_ + f.t;
    lastT_ = t;
    peekNext();                                    // one frame of lookahead bounds the run exactly
    publish(f, t, false);
}

void VideoStream::wrap() {
    lapOffset_ += winfo_.duration;
    noSeekBelow_ = -kInf;                          // what a seek taught about keyframes was in the old lap
    seekDecoder(0.0);
    next_.reset();
    eof_ = false;
    audioChunkOpen_ = false;
    lastT_ = lapOffset_ - winfo_.frameDur;
    std::lock_guard<std::mutex> lk(m_);
    audioSettledU_ = -kInf;
    audioLapStart_ = lapOffset_;                   // the previous lap's audio is complete
}

// Convert `f` into a free buffer and queue it. False if no buffer is free or conversion fails.
bool VideoStream::publishSlot(DecodedFrame& f, double t, double until) {
    int b = -1;
    { std::lock_guard<std::mutex> lk(m_); b = acquireLocked(); }
    if (b < 0) return false;
    if (!dec_.convert(f, pool_[(std::size_t)b].get(), winfo_.width * 4)) {   // pool_ is fixed once Ready
        std::lock_guard<std::mutex> lk(m_);
        releaseLocked(b);
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(m_);
        insertReadyLocked(Slot{t, nextSerial_++, b, until});
    }
    cv_.notify_all();
    return true;
}

// Forward: queue the frame and extend the run of consecutive decided frames.
bool VideoStream::publish(DecodedFrame& f, double t, bool runStart) {
    if (!publishSlot(f, t, nextFrameTime())) return false;
    {
        std::lock_guard<std::mutex> lk(m_);
        if (runStart || !runValid_) { runValid_ = true; runLo_ = t; }
        setRunHiLocked();
    }
    cv_.notify_all();
    return true;
}

// Decode a stretch forward from its keyframe and queue the frames it keeps: live every stride-th, counted
// back from its top frame; offline a ring of the newest pool/2, every one of them. A `fresh` stretch
// ends AT `to` -- the playhead, inclusive -- and starts a new run; otherwise the stretch lies strictly
// BELOW `to`, the start of the stretch above, whose frame is already queued. The kept frames are queued
// together when the stretch is complete: decoding runs forward, so queueing them one by one would show
// the stretch's EARLIEST frame first and then play it forwards.
void VideoStream::reverseStretch(double to, bool fresh, bool offline) {
    const double D = winfo_.duration, fd = winfo_.frameDur;
    const auto began = std::chrono::steady_clock::now();
    reverseStretches_.fetch_add(1);
    if (fresh) {
        std::lock_guard<std::mutex> lk(m_);
        flushReadyLocked();
        runValid_ = false;
        coverValid_ = false;
    }
    const double end = fresh ? to : to - kVideoTimeEps;          // the latest time this stretch may hold
    lapOffset_ = D > 0.0 ? videoLapStart(end, D) : 0.0;           // just below a lap start: the lap before
    audioClipHi_ = coverValid_ ? coverLo_ : kInf;                 // never duplicate the stretch above's audio
    // Land where the loop below admits frames: a prefetch landing ON the stretch above's keyframe
    // would decode nothing, cover nothing, and be planned again forever.
    const bool landed = seekLanding(end, fresh ? kVideoTimeEps : 0.0);
    int cap = 1;
    { std::lock_guard<std::mutex> lk(m_); cap = videoStretchBudget((int)pool_.size()); }

    double lo = lapOffset_;                        // how far down this stretch covers
    std::vector<Slot> ring;                        // the kept frames, oldest first (offline: the newest `cap`)
    bool ringPending = false;                      // ring.back().until awaits the next frame's time
    auto keep = [&](DecodedFrame& f, double t, bool evict) {
        int b = -1;
        if (!evict || (int)ring.size() < cap) { std::lock_guard<std::mutex> lk(m_); b = acquireLocked(); }
        if (b < 0 && evict && !ring.empty()) { b = ring.front().buf; ring.erase(ring.begin()); }
        if (b < 0) return;                         // live: no room for this one -- skip it
        if (dec_.convert(f, pool_[(std::size_t)b].get(), winfo_.width * 4)) {
            ring.push_back(Slot{t, 0, b, kInf});
            ringPending = true;
        } else {
            std::lock_guard<std::mutex> lk(m_);
            releaseLocked(b);
        }
    };
    if (!next_.valid()) {
        // Nothing decodable at all: treat the lap as covered.
    } else if (!landed) {
        // `end` precedes the file's first frame: that frame is the one to show, and nothing is earlier.
        DecodedFrame f = std::move(next_);
        const double t = lapOffset_ + f.t;
        lastT_ = t;
        peekNext();
        keep(f, t, false);
        if (ringPending) { ring.back().until = nextFrameTime(); ringPending = false; }
    } else {
        // Live strides count back from half a frame below the stretch above, on the keyframe's grid, so a
        // frame a container rounded (by up to half a frame) still counts from the right place.
        const double key = lapOffset_ + next_.t;
        const VideoStretch plan = videoPlanStretch(key, fresh ? to : to - 0.5 * fd, fd, cap, offline);
        cap = plan.keep;
        DecodedFrame spare;                        // live: the newest frame not kept, in case none is
        double spareT = 0.0, spareUntil = kInf;
        bool sparePending = false;
        while (!stop_ && peekNext()) {
            const double t = lapOffset_ + next_.t;
            if (ringPending && !ring.empty()) { ring.back().until = t; ringPending = false; }
            if (sparePending) { spareUntil = t; sparePending = false; }
            if (fresh ? t > end + kVideoTimeEps : t > end) break;
            DecodedFrame f = std::move(next_);
            lastT_ = t;
            if (plan.contiguous) {
                keep(f, t, true);
            } else if (videoStretchKeeps(plan, t, fd)) {
                keep(f, t, false);
                spare.reset();
            } else {
                spare = std::move(f);
                spareT = t;
                sparePending = true;
            }
            if (directionChanged()) {              // abandon the stretch; the next step re-plans
                std::lock_guard<std::mutex> lk(m_);
                for (const Slot& s : ring) releaseLocked(s.buf);
                audioClipHi_ = kInf;
                return;
            }
        }
        if (ringPending && !ring.empty()) { ring.back().until = nextFrameTime(); ringPending = false; }
        if (sparePending) spareUntil = nextFrameTime();
        if (!plan.contiguous && ring.empty() && spare.valid()) { keep(spare, spareT, false); if (!ring.empty()) ring.back().until = spareUntil; ringPending = false; }   // never empty
        // Covered down to the keyframe -- or the lap start, when the keyframe is the lap's first frame --
        // unless the offline ring evicted the stretch's lower frames: then only down to its oldest.
        const bool evicted = plan.contiguous && !ring.empty() && ring.front().t > key + kVideoTimeEps;
        const bool atLapStart = key - lapOffset_ < fd * 0.5;
        lo = evicted ? ring.front().t : (atLapStart ? lapOffset_ : key);
    }
    dec_.pumpAudio(end - lapOffset_ + fd);         // settle the stretch's audio up to its end
    takeAudio();
    audioClipHi_ = kInf;
    {
        std::lock_guard<std::mutex> lk(m_);
        for (const Slot& s : ring) insertReadyLocked(Slot{s.t, nextSerial_++, s.buf, s.until});
        const bool firstOfRun = !coverValid_;
        coverLo_ = lo;
        if (firstOfRun) coverHi_ = end;
        coverValid_ = true;
        // Offline, stretches are consecutive: the run grows downwards from the first stretch's end.
        if (firstOfRun || !runValid_) {
            runHi_ = next_.valid() ? lapOffset_ + next_.t : (D > 0.0 ? lapOffset_ + D : kInf);
        }
        runLo_ = lo;
        runValid_ = true;
        runOffline_ = offline;
    }
    lastStretchSeconds_ = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
    cv_.notify_all();
}

} // namespace oss
