#include "app/MidiSyncEngine.h"
#include "core/Transport.h"
#include "core/Preferences.h"
#include <RtMidi.h>
#include <chrono>
#include <cmath>
#include <vector>

namespace oss {

using clk = std::chrono::steady_clock;
static clk::duration secs(double s) {
    return std::chrono::duration_cast<clk::duration>(std::chrono::duration<double>(s));
}
static int inPortIndex(RtMidiIn& mi, const std::string& name) {
    unsigned int n = mi.getPortCount();
    for (unsigned int i = 0; i < n; ++i) if (mi.getPortName(i) == name) return (int)i;
    return -1;
}
static int outPortIndex(RtMidiOut& mo, const std::string& name) {
    unsigned int n = mo.getPortCount();
    for (unsigned int i = 0; i < n; ++i) if (mo.getPortName(i) == name) return (int)i;
    return -1;
}

MidiSyncEngine::MidiSyncEngine() {
    senderThread_ = std::thread([this]{ senderLoop(); });
}

MidiSyncEngine::~MidiSyncEngine() {
    running_.store(false);
    if (senderThread_.joinable()) senderThread_.join();   // thread owns out port -> joined first
    delete in_;                                           // RtMidiIn dtor closes the port
}

void MidiSyncEngine::update(Transport& t, const Preferences& p, double /*dt*/) {
    // --- sync-in (main thread) ---
    if (p.syncInMode != 1) {
        if (in_) { delete in_; in_ = nullptr; inPort_.clear(); }
        t.externalClock = false;
    } else {
        if (!in_ || inPort_ != p.syncInSource) {
            if (in_) { delete in_; in_ = nullptr; }
            inPort_.clear(); reader_.reset(); inClock_ = 0.0;
            try {
                in_ = new RtMidiIn(RtMidi::UNSPECIFIED, "shader-streamer-sync");
                in_->ignoreTypes(false, false, false);    // we WANT clock + SPP
                int idx = inPortIndex(*in_, p.syncInSource);
                if (idx >= 0) { in_->openPort((unsigned int)idx, "sync in"); inPort_ = p.syncInSource; }
                else { delete in_; in_ = nullptr; }
            } catch (RtMidiError&) { delete in_; in_ = nullptr; }
        }
        if (in_) {
            std::vector<unsigned char> msg;
            for (;;) {
                double d = in_->getMessage(&msg);
                if (msg.empty()) break;
                inClock_ += d;
                unsigned char s = msg[0];
                if      (s == kMidiClock)    reader_.onTick(inClock_);
                else if (s == kMidiStart)    reader_.onStart();
                else if (s == kMidiContinue) reader_.onContinue();
                else if (s == kMidiStop)     reader_.onStop();
                else if (s == 0xF2 && msg.size() >= 3) reader_.onSongPosition(sppToSixteenths(msg[1], msg[2]));
            }
            t.externalClock = true;
            t.playing = reader_.playing();
            if (reader_.bpm() > 0.0) {
                t.bpm     = reader_.bpm();
                t.seconds = reader_.positionBeats() * t.secondsPerBeat();
            }
        } else {
            t.externalClock = false;   // couldn't open -> local clock
        }
    }

    // --- publish sync-out snapshot ---
    {
        std::lock_guard<std::mutex> lk(outMutex_);
        out_.enabled  = (p.syncOutMode == 1);
        out_.port     = p.syncOutDest;
        out_.playing  = t.playing;
        out_.bpm      = t.bpm;
        out_.posBeats = t.beats();
    }
}

void MidiSyncEngine::senderLoop() {
    RtMidiOut*  out = nullptr;
    std::string openPort;
    bool        wasPlaying = false;
    double      tickPos = 0.0;                 // beats already covered by sent ticks
    auto        nextTick = clk::now();

    auto sendByte = [&](unsigned char b){ std::vector<unsigned char> m{b}; try { out->sendMessage(&m); } catch (...) {} };
    auto sendSpp  = [&](double beats){ unsigned char m3[3]; sppMessage(beatsToSixteenths(beats), m3);
                                       std::vector<unsigned char> m(m3, m3 + 3); try { out->sendMessage(&m); } catch (...) {} };

    while (running_.load()) {
        SyncOut s;
        { std::lock_guard<std::mutex> lk(outMutex_); s = out_; }

        if (!s.enabled || s.port.empty()) {
            if (out) { delete out; out = nullptr; openPort.clear(); wasPlaying = false; }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        if (!out || openPort != s.port) {
            if (out) { delete out; out = nullptr; }
            openPort.clear(); wasPlaying = false;
            try {
                out = new RtMidiOut(RtMidi::UNSPECIFIED, "shader-streamer-sync");
                int idx = outPortIndex(*out, s.port);
                if (idx >= 0) { out->openPort((unsigned int)idx, "sync out"); openPort = s.port; }
                else { delete out; out = nullptr; }
            } catch (...) { if (out) { delete out; out = nullptr; } }
            if (!out) { std::this_thread::sleep_for(std::chrono::milliseconds(20)); continue; }
        }

        // play-state transitions
        if (s.playing && !wasPlaying) {
            sendSpp(s.posBeats);
            sendByte(s.posBeats < 1e-6 ? kMidiStart : kMidiContinue);
            tickPos = s.posBeats;
            nextTick = clk::now();
            wasPlaying = true;
        } else if (!s.playing && wasPlaying) {
            sendByte(kMidiStop);
            wasPlaying = false;
        }
        if (!s.playing) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); continue; }

        // locate: transport jumped away from our tick position -> relocate the slave
        if (std::fabs(s.posBeats - tickPos) > (2.0 / 24.0)) {
            sendSpp(s.posBeats);
            tickPos = s.posBeats;
            nextTick = clk::now();
        }

        // emit one tick when due, then fine-grained sleep for accuracy + responsiveness
        double secPerTick = 60.0 / ((s.bpm > 0.0 ? s.bpm : 120.0) * 24.0);
        auto now = clk::now();
        if (now >= nextTick) {
            sendByte(kMidiClock);
            tickPos += 1.0 / 24.0;
            nextTick += secs(secPerTick);
            if (nextTick < now) nextTick = now + secs(secPerTick);   // catch up after a stall
        }
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
    if (out) delete out;
}

} // namespace oss
