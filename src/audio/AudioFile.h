#pragma once
#include <cstddef>
#include <string>
#include <vector>

namespace oss {

// A whole audio file decoded into memory as interleaved 48 kHz stereo float
// (L,R,L,R). Audio is small enough per second that decoding the entire file up
// front is cheap and makes forward/reverse/variable-rate playback a simple index
// into `samples`. GL-free; FFmpeg is confined to AudioFile.cpp.
struct AudioClip {
    bool               ok = false;
    int                sampleRate = 48000;
    int                channels   = 2;
    std::vector<float> samples;          // interleaved stereo
    std::string        error;

    std::size_t frames() const { return channels > 0 ? samples.size() / (std::size_t)channels : 0; }
};

// Decode `path` (any container/codec FFmpeg supports) to a 48 kHz stereo clip.
// Returns a clip with ok == false and `error` set on failure.
AudioClip decodeAudioFile(const std::string& path);

} // namespace oss
