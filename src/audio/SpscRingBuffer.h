#pragma once
#include <atomic>
#include <cstddef>
#include <vector>

namespace oss {

// Single-producer / single-consumer lock-free ring buffer for trivially-copyable
// samples. Exactly one thread may call push() and exactly one (other) thread may
// call pop(); any other sharing is undefined. pop() is real-time-safe: it never
// allocates, locks, or blocks -- suitable for an audio device callback.
//
// Uses monotonically increasing positions masked into a power-of-two backing
// store, so all slots are usable and the indices never need explicit wrapping
// (size_t exhaustion at audio rates is geological).
template <class T>
class SpscRingBuffer {
public:
    explicit SpscRingBuffer(std::size_t minCapacity)
        : buf_(roundUpPow2(minCapacity)), mask_(buf_.size() - 1) {}

    std::size_t capacity() const { return buf_.size(); }

    // Producer thread: copy up to n items in. Returns the number actually
    // written (less than n only if the buffer filled).
    std::size_t push(const T* data, std::size_t n) {
        const std::size_t w = write_.load(std::memory_order_relaxed);
        const std::size_t r = read_.load(std::memory_order_acquire);
        const std::size_t freeSlots = buf_.size() - (w - r);
        const std::size_t k = n < freeSlots ? n : freeSlots;
        for (std::size_t i = 0; i < k; ++i) buf_[(w + i) & mask_] = data[i];
        write_.store(w + k, std::memory_order_release);
        return k;
    }

    // Consumer thread: copy up to n items out. Returns the number actually read
    // (less than n only if the buffer drained).
    std::size_t pop(T* out, std::size_t n) {
        const std::size_t r = read_.load(std::memory_order_relaxed);
        const std::size_t w = write_.load(std::memory_order_acquire);
        const std::size_t avail = w - r;
        const std::size_t k = n < avail ? n : avail;
        for (std::size_t i = 0; i < k; ++i) out[i] = buf_[(r + i) & mask_];
        read_.store(r + k, std::memory_order_release);
        return k;
    }

    // Approximate number of items available to read. Safe from either thread,
    // but only a snapshot -- the true value may change concurrently.
    std::size_t sizeApprox() const {
        return write_.load(std::memory_order_acquire) -
               read_.load(std::memory_order_acquire);
    }

private:
    static std::size_t roundUpPow2(std::size_t v) {
        std::size_t p = 1;
        while (p < v) p <<= 1;
        return p;
    }

    std::vector<T>           buf_;
    std::size_t              mask_;
    std::atomic<std::size_t> write_{0};   // monotonic; advanced only by producer
    std::atomic<std::size_t> read_{0};    // monotonic; advanced only by consumer
};

} // namespace oss
