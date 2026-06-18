#pragma once
#include <algorithm>
#include <string>
#include <vector>

namespace oss {

// 12 pitch-class labels for a root-note dropdown (index = semitone within an octave).
inline const std::vector<std::string>& rootNoteLabels() {
    static const std::vector<std::string> labels = {
        "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
    };
    return labels;
}

// 14 chord-type labels for a chord dropdown (index = chord type).
inline const std::vector<std::string>& chordNames() {
    static const std::vector<std::string> names = {
        "maj", "min", "dim", "aug", "sus2", "sus4", "maj7",
        "min7", "dom7", "6", "m6", "m7b5", "dim7", "add9"
    };
    return names;
}

// Append the MIDI notes of a chord to `out` (does not clear it). rootPitchClass 0..11,
// octave with C4 = 60 (note = (octave+1)*12 + rootPitchClass + interval), chordIndex
// into chordNames(). Notes > 127 are dropped; root and chord indices are clamped.
// `octave` is NOT clamped (the caller's responsibility) — an out-of-range octave simply
// drops every note via the 0..127 guard rather than producing a wrong one.
inline void buildChordNotes(int rootPitchClass, int octave, int chordIndex,
                            std::vector<int>& out) {
    static const std::vector<std::vector<int>> intervals = {
        {0, 4, 7},      // maj
        {0, 3, 7},      // min
        {0, 3, 6},      // dim
        {0, 4, 8},      // aug
        {0, 2, 7},      // sus2
        {0, 5, 7},      // sus4
        {0, 4, 7, 11},  // maj7
        {0, 3, 7, 10},  // min7
        {0, 4, 7, 10},  // dom7
        {0, 4, 7, 9},   // 6
        {0, 3, 7, 9},   // m6
        {0, 3, 6, 10},  // m7b5
        {0, 3, 6, 9},   // dim7
        {0, 4, 7, 14},  // add9
    };
    int pc   = std::clamp(rootPitchClass, 0, 11);
    int ci   = std::clamp(chordIndex, 0, (int)intervals.size() - 1);
    int base = (octave + 1) * 12 + pc;   // C4 = 60
    for (int iv : intervals[ci]) {
        int note = base + iv;
        if (note >= 0 && note <= 127) out.push_back(note);
    }
}

} // namespace oss
