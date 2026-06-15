#include <doctest/doctest.h>
#include "audio/SpscRingBuffer.h"
#include <cstddef>

using oss::SpscRingBuffer;

TEST_CASE("capacity rounds up to a power of two") {
    CHECK(SpscRingBuffer<float>(1000).capacity() == 1024);
    CHECK(SpscRingBuffer<float>(1024).capacity() == 1024);
    CHECK(SpscRingBuffer<float>(3).capacity()    == 4);
}

TEST_CASE("push then pop preserves FIFO order") {
    SpscRingBuffer<int> rb(16);
    int in[5] = {1, 2, 3, 4, 5};
    CHECK(rb.push(in, 5) == 5);
    CHECK(rb.sizeApprox() == 5);
    int out[5] = {0};
    CHECK(rb.pop(out, 5) == 5);
    for (int i = 0; i < 5; ++i) CHECK(out[i] == in[i]);
    CHECK(rb.sizeApprox() == 0);
}

TEST_CASE("push beyond free space writes only what fits") {
    SpscRingBuffer<int> rb(4);                 // capacity 4
    int in[6] = {10, 11, 12, 13, 14, 15};
    CHECK(rb.push(in, 6) == 4);                // only 4 fit
    int out[4] = {0};
    CHECK(rb.pop(out, 10) == 4);
    CHECK(out[0] == 10);
    CHECK(out[3] == 13);
}

TEST_CASE("pop on an empty buffer returns zero") {
    SpscRingBuffer<int> rb(8);
    int out[4];
    CHECK(rb.pop(out, 4) == 0);
}

TEST_CASE("data survives wrap-around past the capacity boundary") {
    SpscRingBuffer<int> rb(8);                 // capacity 8
    int a[6] = {0, 1, 2, 3, 4, 5};
    rb.push(a, 6);
    int drain[6];
    rb.pop(drain, 6);                          // read position now at 6
    int b[6] = {6, 7, 8, 9, 10, 11};
    CHECK(rb.push(b, 6) == 6);                 // writes wrap past index 8
    int out[6] = {0};
    CHECK(rb.pop(out, 6) == 6);
    for (int i = 0; i < 6; ++i) CHECK(out[i] == b[i]);
}

TEST_CASE("interleaved push/pop maintains one continuous FIFO stream") {
    SpscRingBuffer<int> rb(16);
    int nextPush = 0, nextPop = 0;
    for (int iter = 0; iter < 1000; ++iter) {
        int block[5];
        for (int i = 0; i < 5; ++i) block[i] = nextPush + i;
        nextPush += (int)rb.push(block, 5);
        int out[3];
        std::size_t popped = rb.pop(out, 3);
        for (std::size_t i = 0; i < popped; ++i) { CHECK(out[i] == nextPop); ++nextPop; }
    }
    CHECK(nextPop <= nextPush);
}
