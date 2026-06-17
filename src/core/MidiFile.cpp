#include "core/MidiFile.h"
#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iterator>

namespace oss {
namespace {

// A bounds-checked cursor over the byte buffer; `ok` goes false on overrun.
struct Reader {
    const unsigned char* p;
    const unsigned char* end;
    bool ok = true;
    unsigned char u8() { if (p >= end) { ok = false; return 0; } return *p++; }
    uint32_t u32() { uint32_t v = 0; for (int i = 0; i < 4; ++i) v = (v << 8) | u8(); return v; }
    uint16_t u16() { uint16_t hi = u8(), lo = u8(); return (uint16_t)((hi << 8) | lo); }
    uint32_t vlq() {                       // variable-length quantity (max 4 bytes)
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            unsigned char b = u8();
            v = (v << 7) | (b & 0x7Fu);
            if (!(b & 0x80u)) return v;
        }
        ok = false;                        // a 5th continuation byte -> malformed VLQ
        return v;
    }
    void skip(std::size_t k) { if ((std::size_t)(end - p) < k) { p = end; ok = false; } else p += k; }
};

int channelDataBytes(unsigned char status) {   // 1 for program/channel-pressure, else 2
    unsigned char hi = status & 0xF0u;
    return (hi == 0xC0u || hi == 0xD0u) ? 1 : 2;
}

} // namespace

MidiSequence parseMidiFile(const unsigned char* data, std::size_t n) {
    MidiSequence seq;
    Reader r{data, data + n};

    if (r.u8() != 'M' || r.u8() != 'T' || r.u8() != 'h' || r.u8() != 'd') {
        seq.error = "not a MIDI file (missing MThd)"; return seq;
    }
    uint32_t hlen = r.u32();
    r.u16();                                // format (0/1 handled the same; merged)
    uint16_t ntracks = r.u16();
    uint16_t division = r.u16();
    if (hlen > 6) r.skip(hlen - 6);
    if (!r.ok) { seq.error = "truncated header"; return seq; }
    if (division & 0x8000u) { seq.error = "SMPTE division unsupported"; return seq; }
    if (division == 0) { seq.error = "zero tick division"; return seq; }
    const double tpq = (double)division;

    double maxBeats = 0.0;
    for (uint16_t t = 0; t < ntracks && r.ok; ++t) {
        unsigned char m0 = r.u8(), m1 = r.u8(), m2 = r.u8(), m3 = r.u8();
        uint32_t tlen = r.u32();
        if (!r.ok) break;
        if (m0 != 'M' || m1 != 'T' || m2 != 'r' || m3 != 'k') { r.skip(tlen); continue; }
        const unsigned char* trackEnd = r.p + tlen;
        if (trackEnd > r.end) trackEnd = r.end;
        uint64_t tick = 0;
        unsigned char running = 0;
        while (r.p < trackEnd && r.ok) {
            tick += r.vlq();
            unsigned char b = r.u8();
            if (b == 0xFFu) {                              // meta -> skip
                r.u8(); uint32_t len = r.vlq(); r.skip(len); running = 0;
            } else if (b == 0xF0u || b == 0xF7u) {         // sysex -> skip
                uint32_t len = r.vlq(); r.skip(len); running = 0;
            } else {
                unsigned char status, d1;
                if (b & 0x80u) {
                    if ((b & 0xF0u) == 0xF0u) { r.ok = false; break; }   // system msg in track
                    status = b; running = b; d1 = r.u8();
                } else {
                    if (!running) { r.ok = false; break; }               // data byte, no status
                    status = running; d1 = b;                            // running status
                }
                unsigned char d2 = 0;
                if (channelDataBytes(status) == 2) d2 = r.u8();
                if (!r.ok) break;
                MidiFileEvent e;
                e.beats = (double)tick / tpq;
                e.ev = MidiEvent{status, d1, d2};
                seq.events.push_back(e);
                if (e.beats > maxBeats) maxBeats = e.beats;
            }
        }
        r.p = trackEnd;   // resync to the declared track end
    }

    std::stable_sort(seq.events.begin(), seq.events.end(),
        [](const MidiFileEvent& a, const MidiFileEvent& b) { return a.beats < b.beats; });
    seq.lengthBeats = maxBeats;
    seq.ok = r.ok;   // a truncated/malformed buffer must not report a clean parse
    if (!seq.ok && seq.error.empty()) seq.error = "truncated or malformed track data";
    return seq;
}

MidiSequence loadMidiFile(const std::string& path) {
    MidiSequence seq;
    if (path.empty()) { seq.error = "no file"; return seq; }
    std::ifstream f(path, std::ios::binary);
    if (!f) { seq.error = "cannot open file"; return seq; }
    std::vector<unsigned char> buf((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (buf.empty()) { seq.error = "empty file"; return seq; }
    return parseMidiFile(buf.data(), buf.size());
}

} // namespace oss
