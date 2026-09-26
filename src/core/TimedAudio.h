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
// GL-free. Not thread-safe: VideoStream guards it with its mutex.
class TimedAudio {
public:
    static constexpr double kMaxSeconds = 180.0;  // safety cap on everything held (about 35 MB at 48 kHz)
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

    // Keep the audio around the playhead: [lo, hi] is forward [u - keep, +inf), reverse (-inf, u + keep].
    // Chunks wholly outside go (never the current one). A chunk's front (forward) or back (reverse) is cut
    // once more than a second of it lies outside, so calling this every step does not move memory every
    // step. It also notes where the playhead is, for the cap.
    void retain(double lo, double hi) {
        focus_ = std::isfinite(lo) ? (std::isfinite(hi) ? 0.5 * (lo + hi) : lo) : hi;
        std::vector<Chunk> kept;
        kept.reserve(chunks_.size());
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
            kept.push_back(std::move(c));
        }
        chunks_.swap(kept);
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

    // While more than kMaxSeconds is held, drop the chunk farthest from the playhead (never the current
    // one; before any retain() the oldest goes first). In reverse the oldest chunk is the one being played.
    void capTotal() {
        const std::size_t cap = (std::size_t)(kMaxSeconds * rate_);
        std::size_t total = size();
        while (chunks_.size() > 1 && total > cap) {
            std::size_t victim = 0;
            double farthest = -1.0;
            for (std::size_t i = 0; i + 1 < chunks_.size(); ++i) {
                const Chunk& c = chunks_[i];
                const double d = focus_ < c.start ? c.start - focus_ : (focus_ >= end(c) ? focus_ - end(c) : 0.0);
                if (d > farthest) { farthest = d; victim = i; }
            }
            total -= chunks_[victim].s.size();
            chunks_.erase(chunks_.begin() + (std::ptrdiff_t)victim);
        }
    }

    std::vector<Chunk> chunks_;
    int rate_;
    double focus_ = -std::numeric_limits<double>::infinity();   // where retain() last put the playhead
    mutable std::vector<const Chunk*> span_;                     // sample()'s scratch
};

} // namespace oss
