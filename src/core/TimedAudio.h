#pragma once
#include <algorithm>
#include <cstddef>
#include <limits>
#include <vector>

namespace oss {

// Decoded audio tagged with UNWRAPPED time (see core/VideoPlan.h), for the Video Player's worker.
// Audio is kept as contiguous chunks, each a start time plus mono samples at `rate`. A seek, a loop
// wrap or a reverse stretch begins a new chunk. sample() maps output samples onto source time exactly
// as the node's emitAudio always did (linear interpolation; a reversed span reads backwards). Times
// no chunk covers are silence, and where chunks overlap (a loop seam) the later-starting chunk wins.
// GL-free. Not thread-safe: VideoStream guards it with its mutex.
class TimedAudio {
public:
    static constexpr double kMaxSeconds = 30.0;   // safety cap on everything held

    explicit TimedAudio(int rate = 48000) : rate_(rate) {}

    int  rate() const { return rate_; }
    bool empty() const { return chunks_.empty(); }
    void clear() { chunks_.clear(); }

    // Start a new chunk at unwrapped time `startU`; append() adds to it.
    void beginChunk(double startU) { chunks_.push_back(Chunk{startU, {}}); }

    // Append to the current chunk, keeping only samples that start before `clipHi` (a reverse stretch
    // passes the start of the stretch above it, so the two never overlap). No-op with no chunk.
    void append(const float* s, std::size_t n,
                double clipHi = std::numeric_limits<double>::infinity()) {
        if (chunks_.empty() || n == 0) return;
        Chunk& c = chunks_.back();
        const double room = (clipHi - end(c)) * rate_;
        if (room <= 0.0) return;
        const std::size_t take = room < (double)n ? (std::size_t)room : n;
        c.s.insert(c.s.end(), s, s + take);
        capTotal();
    }

    // Drop audio lying entirely outside [lo, hi]. A chunk's front is trimmed only a whole second at a
    // time, so calling this every frame does not memmove the buffer every frame.
    void retain(double lo, double hi) {
        std::vector<Chunk> kept;
        kept.reserve(chunks_.size());
        for (std::size_t i = 0; i < chunks_.size(); ++i) {
            Chunk& c = chunks_[i];
            const bool current = i + 1 == chunks_.size();
            if (!current && (end(c) < lo || c.start > hi)) continue;
            if (lo > c.start + 1.0) {
                const std::size_t drop = std::min(c.s.size(), (std::size_t)((lo - c.start) * rate_));
                c.s.erase(c.s.begin(), c.s.begin() + (std::ptrdiff_t)drop);
                c.start += (double)drop / rate_;
            }
            kept.push_back(std::move(c));
        }
        chunks_.swap(kept);
    }

    // n output samples spanning source time [u0, u1] (u1 < u0 reads backwards).
    void sample(double u0, double u1, float* out, int n) const {
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

    float at(double t) const {
        const Chunk* best = nullptr;
        for (const Chunk& c : chunks_) {
            const double idx = (t - c.start) * rate_;
            if (idx >= 0.0 && idx < (double)c.s.size() - 1.0 && (!best || c.start > best->start)) best = &c;
        }
        if (!best) return 0.0f;
        const double idx = (t - best->start) * rate_;
        const std::size_t i = (std::size_t)idx;
        const float fr = (float)(idx - (double)i);
        return best->s[i] * (1.0f - fr) + best->s[i + 1] * fr;
    }

    // Drop the oldest chunks (never the current one) while more than kMaxSeconds is held.
    void capTotal() {
        const std::size_t cap = (std::size_t)(kMaxSeconds * rate_);
        while (chunks_.size() > 1 && size() > cap) chunks_.erase(chunks_.begin());
    }

    std::vector<Chunk> chunks_;
    int rate_;
};

} // namespace oss
