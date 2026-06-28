#pragma once
#include <cmath>
#include <vector>
#include "core/Value.h"
#include "core/MidiFile.h"

namespace oss {

// Turns a transport position into the MIDI events to emit this frame, playing a
// MidiSequence anchored at a start offset and optionally looping a region. Tracks
// sounding notes so it can release them across loop seams / stops / mutes. GL-free.
class MidiClipPlayer {
public:
    std::vector<MidiEvent> advance(const MidiSequence& seq, double transportBeats,
                                   double beatsPerBar, double startOffsetBars,
                                   bool loop, double loopLenBars, const bool muted[16]) {
        std::vector<MidiEvent> out;
        double local = transportBeats - startOffsetBars * beatsPerBar;
        if (!seq.ok || local < 0.0) {                    // before the clip (or no file)
            appendFlush(out);
            prevPlay_ = -1.0;
            for (int c = 0; c < 16; ++c) prevMuted_[c] = muted[c];   // keep mute edge-detect sane
            return out;
        }

        double loopLen = loopLenBars * beatsPerBar;
        if (loopLen < 1e-6) loopLen = 1e-6;
        double playPos = loop ? std::fmod(local, loopLen) : local;

        // A channel that just became muted releases its sounding notes.
        for (int c = 0; c < 16; ++c) {
            if (muted[c] && !prevMuted_[c]) appendChannelFlush(out, c);
            prevMuted_[c] = muted[c];
        }

        if (prevPlay_ < 0.0) {
            prevPlay_ = playPos;                         // just entered; emit nothing yet
        } else if (playPos >= prevPlay_) {
            emitRange(out, seq, prevPlay_, playPos, muted);    // forward window
        } else if (loop) {                               // loop wrap: tail -> release -> head
            emitRange(out, seq, prevPlay_, loopLen, muted);
            appendFlush(out);
            emitRange(out, seq, 0.0, playPos, muted);
        } else {                                         // non-loop backward jump: reset cleanly
            appendFlush(out);                            // (e.g. the project transport looped)
        }
        prevPlay_ = playPos;

        // Non-loop clip end: once the final event's window has been emitted, release
        // any held note so nothing hangs on the (monophonic) downstream synth.
        if (!loop && local >= seq.lengthBeats) { appendFlush(out); prevPlay_ = -1.0; }
        return out;
    }

    // Release every sounding note (note-offs appended to `out`) and reset the playhead so the
    // next advance() re-enters cleanly. Call on a playback discontinuity the position math
    // can't see -- e.g. the underlying sequence was swapped (a new MIDI file) -- so a note the
    // old content left sounding doesn't hang until the next loop seam.
    void reset(std::vector<MidiEvent>& out) {
        appendFlush(out);     // note-offs for all active_ notes; clears active_
        prevPlay_ = -1.0;     // next advance() enters fresh
    }

private:
    void emitRange(std::vector<MidiEvent>& out, const MidiSequence& seq,
                   double a, double b, const bool muted[16]) {
        for (const auto& fe : seq.events) {
            if (fe.beats < a) continue;
            if (fe.beats >= b) break;                    // events are sorted by beats
            int ch = fe.ev.status & 0x0F;
            if (muted[ch]) continue;
            out.push_back(fe.ev);
            unsigned char hi = fe.ev.status & 0xF0u;
            int note = fe.ev.data1 & 0x7F;
            if (hi == 0x90u && fe.ev.data2 > 0)               active_[ch][note] = true;
            else if (hi == 0x80u || (hi == 0x90u && fe.ev.data2 == 0)) active_[ch][note] = false;
        }
    }
    void appendFlush(std::vector<MidiEvent>& out) { for (int c = 0; c < 16; ++c) appendChannelFlush(out, c); }
    void appendChannelFlush(std::vector<MidiEvent>& out, int c) {
        for (int note = 0; note < 128; ++note)
            if (active_[c][note]) {
                out.push_back(MidiEvent{(unsigned char)(0x80 | c), (unsigned char)note, 0});
                active_[c][note] = false;
            }
    }

    double prevPlay_ = -1.0;          // last play position (-1 = not in the clip)
    bool   active_[16][128] = {};     // sounding notes per channel/note
    bool   prevMuted_[16]   = {};     // last frame's mute state per channel
};

} // namespace oss
