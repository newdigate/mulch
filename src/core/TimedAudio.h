#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <utility>
#include <vector>

namespace oss {

// Decoded audio tagged with UNWRAPPED time (see core/VideoPlan.h), for the Video Player's worker.
// Audio is kept as contiguous chunks, each a start time plus mono samples at `rate`, covering
// [start, start + size / rate): a chunk's last interval holds its last sample, so adjacent chunks tile.
// A seek, a loop wrap or a reverse stretch begins a new chunk. sample() maps output samples onto source
// time exactly as the node's emitAudio always did (linear interpolation; a reversed span reads
// backwards). Times no chunk covers are silence. Where chunks overlap, the most recently begun one wins:
// at a loop seam that is the new lap; in reverse it is the stretch below, whose audio runs on unbroken
// up to where the stretch above resumed its own -- a little before that stretch's keyframe, with a
// just-flushed decoder fading in.
// GL-free. Not thread-safe (sample() included: it keeps scratch): VideoStream guards it with its mutex.
class TimedAudio {
public:
    static constexpr double kMaxSeconds = 180.0;  // cap on everything held (about 35 MB at 48 kHz)
    static constexpr double kGridEps    = 1e-6;   // samples: a time on the sample grid is not rounded past

    explicit TimedAudio(int rate = 48000) : rate_(rate) {}

    int  rate() const { return rate_; }
    bool empty() const { return chunks_.empty(); }
    void clear() { chunks_.clear(); }

    // Start a new chunk at unwrapped time `startU`; append() adds to it.
    void beginChunk(double startU) { chunks_.push_back(Chunk{startU, {}}); }

    // Append to the current chunk, keeping only the samples that start before `clipHi` (a reverse
    // stretch passes the start of the stretch above it). No-op with no chunk.
    void append(const float* s, std::size_t n,
                double clipHi = std::numeric_limits<double>::infinity()) {
        if (chunks_.empty() || n == 0) return;
        Chunk& c = chunks_.back();
        const double room = std::ceil((clipHi - end(c)) * rate_ - kGridEps);
        if (!(room > 0.0)) return;
        const std::size_t take = room < (double)n ? (std::size_t)room : n;
        c.s.insert(c.s.end(), s, s + take);
        capTotal();
    }

    // Keep the audio around the playhead `u`: [lo, hi] is forward [u - keep, +inf), reverse
    // (-inf, u + keep]. Chunks wholly outside go (never the current one). A chunk's front (forward) or back
    // (reverse) is cut once more than a second of it lies outside, so calling this every step does not
    // move memory every step. The cap spares what plays at `u`. Compacts in place: nothing is allocated.
    void retain(double lo, double hi, double u) {
        playhead_ = u;
        std::size_t kept = 0;
        for (std::size_t i = 0; i < chunks_.size(); ++i) {
            Chunk& c = chunks_[i];
            const bool current = i + 1 == chunks_.size();
            if (!current && (end(c) < lo || c.start > hi)) continue;
            if (std::isfinite(lo) && lo > c.start + 1.0) {
                const std::size_t drop = std::min(c.s.size(), (std::size_t)((lo - c.start) * rate_));
                c.s.erase(c.s.begin(), c.s.begin() + (std::ptrdiff_t)drop);
                c.start += (double)drop / rate_;
            }
            if (!current && std::isfinite(hi) && end(c) > hi + 1.0)
                c.s.resize((std::size_t)std::max(0.0, std::ceil((hi - c.start) * rate_ - kGridEps)));
            if (kept != i) chunks_[kept] = std::move(c);
            ++kept;
        }
        chunks_.erase(chunks_.begin() + (std::ptrdiff_t)kept, chunks_.end());
    }

    // n output samples spanning source time [u0, u1] (u1 < u0 reads backwards).
    void sample(double u0, double u1, float* out, int n) const {
        const double lo = std::min(u0, u1), hi = std::max(u0, u1);
        span_.clear();                                   // the chunks the span touches, newest first
        for (std::size_t k = chunks_.size(); k-- > 0;)
            if (chunks_[k].start <= hi && end(chunks_[k]) > lo) span_.push_back(&chunks_[k]);
        for (int j = 0; j < n; ++j) out[j] = at(u0 + (u1 - u0) * ((double)j / n));
    }

    // Total samples held (for tests and the cap).
    std::size_t size() const {
        std::size_t n = 0;
        for (const Chunk& c : chunks_) n += c.s.size();
        return n;
    }

private:
    struct Chunk { double start; std::vector<float> s; };

    double end(const Chunk& c) const { return c.start + (double)c.s.size() / rate_; }

    // The sample at t from the newest chunk of span_ covering it.
    float at(double t) const {
        for (const Chunk* c : span_) {
            const double idx = (t - c->start) * rate_;
            if (!(idx >= 0.0 && idx < (double)c->s.size())) continue;
            const std::size_t i = (std::size_t)idx;
            const float fr = (float)(idx - (double)i);
            const float next = i + 1 < c->s.size() ? c->s[i + 1] : c->s[i];
            return c->s[i] * (1.0f - fr) + next * fr;
        }
        return 0.0f;
    }

    // The index of the chunk that plays at t (the newest covering it), or chunks_.size() if none does.
    std::size_t playingAt(double t) const {
        for (std::size_t k = chunks_.size(); k-- > 0;) {
            const double idx = (t - chunks_[k].start) * rate_;
            if (idx >= 0.0 && idx < (double)chunks_[k].s.size()) return k;
        }
        return chunks_.size();
    }

    // How far c lies from the playhead (0 when it covers it).
    double distance(const Chunk& c) const {
        return playhead_ < c.start ? c.start - playhead_ : (playhead_ >= end(c) ? playhead_ - end(c) : 0.0);
    }

    // Cut up to n samples from the end of c farther from the playhead, never past it; how many went.
    std::size_t trimFarEnd(Chunk& c, std::size_t n) {
        const std::size_t before = c.s.size();
        const double at = (playhead_ - c.start) * rate_;          // the playhead's place in c, in samples
        if (!std::isfinite(playhead_) || at >= 0.5 * (double)before) {           // the front is farther
            const double room = std::isfinite(at) ? std::min(std::floor(at), (double)before) : (double)before;
            const std::size_t cut = std::min(n, (std::size_t)std::max(0.0, room));
            c.s.erase(c.s.begin(), c.s.begin() + (std::ptrdiff_t)cut);
            c.start += (double)cut / rate_;
        } else {                                                                 // the back is
            const std::size_t keep = (std::size_t)std::max(0.0, std::floor(at) + 1.0);
            c.s.resize(std::max(keep, before - std::min(n, before)));
        }
        return before - c.s.size();
    }

    // While more than kMaxSeconds is held, drop the audio farthest from the playhead, never what plays at
    // it: the farthest chunk goes whole -- or, if it is the one being filled (appends go to it), loses its
    // far end, never past the playhead. In live reverse that one is the prefetch below the stretch that
    // plays next, so its far end goes first. Before any retain() there is no playhead: the oldest audio
    // goes first.
    void capTotal() {
        const std::size_t cap = (std::size_t)(kMaxSeconds * rate_);
        std::size_t total = size();
        bool fillingSpent = false;                       // nothing more can come off the one being filled
        while (total > cap) {
            const std::size_t last = chunks_.size() - 1, playing = playingAt(playhead_);
            std::size_t victim = chunks_.size();
            double farthest = -1.0;
            for (std::size_t i = 0; i < chunks_.size(); ++i) {
                if (i == playing || (i == last && fillingSpent)) continue;
                const double d = distance(chunks_[i]);
                if (d > farthest) { farthest = d; victim = i; }
            }
            if (victim == chunks_.size()) break;
            if (victim == last) {
                const std::size_t cut = trimFarEnd(chunks_[last], total - cap);
                total -= cut;
                fillingSpent = cut == 0 || chunks_[last].s.empty();
            } else {
                total -= chunks_[victim].s.size();
                chunks_.erase(chunks_.begin() + (std::ptrdiff_t)victim);
            }
        }
        if (total > cap && playingAt(playhead_) == chunks_.size() - 1)          // the one playing is being filled
            trimFarEnd(chunks_.back(), total - cap);
    }

    std::vector<Chunk> chunks_;
    int rate_;
    double playhead_ = -std::numeric_limits<double>::infinity();   // as retain() last saw it
    mutable std::vector<const Chunk*> span_;                        // sample()'s scratch
};

} // namespace oss
