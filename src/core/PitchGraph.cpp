#include "core/PitchGraph.h"
#include <algorithm>
#include <cmath>

namespace oss {

glm::vec3 hsvToRgb(float h, float s, float v) {
    h = h - std::floor(h);                 // wrap to [0,1)
    float i = std::floor(h * 6.0f);
    float f = h * 6.0f - i;
    float p = v * (1.0f - s);
    float q = v * (1.0f - s * f);
    float t = v * (1.0f - s * (1.0f - f));
    switch (((int)i) % 6) {
        case 0:  return glm::vec3(v, t, p);
        case 1:  return glm::vec3(q, v, p);
        case 2:  return glm::vec3(p, v, t);
        case 3:  return glm::vec3(p, q, v);
        case 4:  return glm::vec3(t, p, v);
        default: return glm::vec3(v, p, q);
    }
}

namespace {
constexpr int         kLoNote = 24, kHiNote = 96;   // C1..C7, log-frequency (by note)
constexpr std::size_t kMaxNotes = 512;
inline float clamp11(float x) { return x < -1.0f ? -1.0f : (x > 1.0f ? 1.0f : x); }
}

void PitchGraph::ingest(const MidiRef& midi, float dt, float window) {
    clock_ += (double)dt;
    for (std::size_t i = 0; i < midi.count; ++i) {
        const MidiEvent& e = midi.events[i];
        int note = e.data1 & 0x7F;
        if (midiIsNoteOn(e)) {
            for (auto it = notes_.rbegin(); it != notes_.rend(); ++it)   // retrigger: close prior
                if (it->note == note && it->endT < 0.0) { it->endT = clock_; break; }
            notes_.push_back({note, e.data2 & 0x7F, clock_, -1.0});
            if (notes_.size() > kMaxNotes) notes_.erase(notes_.begin());
        } else if (midiIsNoteOff(e)) {
            for (auto it = notes_.rbegin(); it != notes_.rend(); ++it)
                if (it->note == note && it->endT < 0.0) { it->endT = clock_; break; }
        }
    }
    double cutoff = clock_ - (double)window;   // drop closed records scrolled off the left
    notes_.erase(std::remove_if(notes_.begin(), notes_.end(),
        [cutoff](const Note& n){ return n.endT >= 0.0 && n.endT < cutoff; }), notes_.end());
}

int PitchGraph::build(float window, std::vector<float>& out) const {
    out.clear();
    double now = clock_;
    double w = window > 1e-6f ? (double)window : 1e-6;
    for (const Note& n : notes_) {
        double s = n.startT;
        double e = (n.endT < 0.0) ? now : n.endT;
        if (e < now - w) continue;   // off-screen (belt-and-suspenders if build/ingest windows differ)
        float xs = clamp11((float)(2.0 * (s - now) / w + 1.0));
        float xe = clamp11((float)(2.0 * (e - now) / w + 1.0));
        float y  = clamp11(2.0f * (float)(n.note - kLoNote) / (float)(kHiNote - kLoNote) - 1.0f);
        glm::vec3 c = hsvToRgb((float)(n.note % 12) / 12.0f, 1.0f,
                               0.25f + 0.75f * (float)n.vel / 127.0f);
        out.insert(out.end(), { xs, y, 0.0f, c.r, c.g, c.b });
        out.insert(out.end(), { xe, y, 0.0f, c.r, c.g, c.b });
    }
    return (int)(out.size() / 6);
}

int PitchGraph::activeCount() const {
    int n = 0; for (const Note& x : notes_) if (x.endT < 0.0) ++n; return n;
}

} // namespace oss
