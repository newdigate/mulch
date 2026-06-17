#pragma once
#include <cstddef>
#include <string>
#include <vector>
#include "core/Value.h"   // MidiEvent

namespace oss {

// One file event at an absolute position in quarter-note beats.
struct MidiFileEvent { double beats = 0.0; MidiEvent ev; };

struct MidiSequence {
    bool   ok = false;
    std::vector<MidiFileEvent> events;   // channel-voice events, sorted by beats
    double lengthBeats = 0.0;            // position of the last event
    std::string error;
};

// Parse a Standard MIDI File from a byte buffer. The file's tempo map is ignored;
// events are positioned in quarter-note beats. Channel-voice messages are kept;
// meta/sysex are skipped. Format 0 and 1 (tracks merged). GL-free.
MidiSequence parseMidiFile(const unsigned char* data, std::size_t n);

// Read a .mid from disk and parse it (ok=false with an error if unreadable/empty).
MidiSequence loadMidiFile(const std::string& path);

} // namespace oss
