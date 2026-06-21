#include <doctest/doctest.h>
#include "audio/AudioBlock.h"

using namespace oss;

TEST_CASE("audioBlockFrames tracks dt, bounded by the audio dt clamp") {
    CHECK(audioBlockFrames(48000.0, 1.0 / 60.0) == 800);
    CHECK(audioBlockFrames(48000.0, 0.025) == 1200);          // a slow 25ms frame: NOT clamped to 1024
    CHECK(audioBlockFrames(48000.0, 0.0) == 1);               // floor
    CHECK(audioBlockFrames(48000.0, -1.0) == 1);              // negative -> floor
    CHECK(audioBlockFrames(48000.0, 1.0) == 4080);            // a 1s dt clamps via kMaxAudioDt (48000*0.085)
    CHECK(audioBlockFrames(48000.0, 100.0) <= kAudioMaxBlock);
}

TEST_CASE("audioRingFloats maps ms -> interleaved-stereo float capacity") {
    CHECK(audioRingFloats(150, 48000) == 14400);              // 7200 stereo frames * 2
    CHECK(audioRingFloats(20, 48000) == 1920);                // 960 * 2
    CHECK(audioRingFloats(150, 48000) > audioRingFloats(100, 48000));   // monotonic in ms
}
