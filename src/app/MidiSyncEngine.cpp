#include "app/MidiSyncEngine.h"
#include "app/ThreadPriority.h"
#include "core/Transport.h"
#include "core/Preferences.h"
#include "core/MidiTimecode.h"
#include <RtMidi.h>
#include <algorithm>
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

void MidiSyncEngine::update(Transport& t, const Preferences& p, double dt) {
    // --- sync-in (main thread) ---
    int mode = p.syncInMode;
    if (mode != 1 && mode != 2) {
        if (in_) { delete in_; in_ = nullptr; inPort_.clear(); }
        t.externalClock = false;
    } else {
        if (!in_ || inPort_ != p.syncInSource || inMode_ != mode) {
            if (in_) { delete in_; in_ = nullptr; }
            inPort_.clear(); reader_.reset(); mtcReader_.reset(); inClock_ = 0.0; inMode_ = mode;
            try {
                in_ = new RtMidiIn(RtMidi::UNSPECIFIED, "shader-streamer-sync");
                in_->ignoreTypes(false, false, false);    // we WANT clock + SPP + MTC + SysEx
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
                if (mode == 1) {
                    if      (s == kMidiClock)    reader_.onTick(inClock_);
                    else if (s == kMidiStart)    reader_.onStart();
                    else if (s == kMidiContinue) reader_.onContinue();
                    else if (s == kMidiStop)     reader_.onStop();
                    else if (s == 0xF2 && msg.size() >= 3) reader_.onSongPosition(sppToSixteenths(msg[1], msg[2]));
                } else {   // mode == 2 (MTC)
                    if      (s == 0xF1 && msg.size() >= 2) mtcReader_.onQuarterFrame(msg[1]);
                    else if (s == 0xF0) { SmpteTime tc; if (parseFullFrame(msg.data(), msg.size(), tc)) mtcReader_.onFullFrame(tc); }
                }
            }
            if (mode == 1) {
                t.externalClock = true;
                t.playing = reader_.playing();
                if (reader_.bpm() > 0.0) {
                    t.bpm     = reader_.bpm();
                    t.seconds = reader_.positionBeats() * t.secondsPerBeat();
                }
            } else {
                mtcReader_.onIdle(dt);
                t.externalClock = true;
                t.playing = mtcReader_.playing();
                t.seconds = mtcReader_.seconds();   // MTC carries no tempo -> leave t.bpm
            }
        } else {
            t.externalClock = false;   // couldn't open -> local clock
        }
    }

    // --- publish sync-out snapshot ---
    {
        std::lock_guard<std::mutex> lk(outMutex_);
        out_.enabled    = (p.syncOutMode == 1 || p.syncOutMode == 2);
        out_.mode       = p.syncOutMode;
        out_.port       = p.syncOutDest;
        out_.playing    = t.playing;
        out_.bpm        = t.bpm;
        out_.posBeats   = t.beats();
        out_.posSeconds = t.seconds;
        out_.frameRate  = p.syncFrameRate;
    }
}

void MidiSyncEngine::senderLoop() {
    setThisThreadTimeCritical();   // the timing thread preempts ordinary threads
    RtMidiOut*  out = nullptr;
    std::string openPort;
    int         lastMode = -1;
    bool        wasPlaying = false;
    // beat-clock send state
    double      tickPos = 0.0;
    auto        nextTick = clk::now();
    // mtc send state
    int         qfPiece = 0;
    auto        nextQf = clk::now();
    double      mtcLastSeconds = 0.0;
    SmpteTime   mtcFrozen;

    auto sendByte = [&](unsigned char b){ std::vector<unsigned char> m{b}; try { out->sendMessage(&m); } catch (...) {} };
    auto sendSpp  = [&](double beats){ unsigned char m3[3]; sppMessage(beatsToSixteenths(beats), m3);
                                       std::vector<unsigned char> m(m3, m3 + 3); try { out->sendMessage(&m); } catch (...) {} };
    auto sendVec  = [&](std::vector<unsigned char> m){ try { out->sendMessage(&m); } catch (...) {} };

    while (running_.load()) {
        SyncOut s;
        { std::lock_guard<std::mutex> lk(outMutex_); s = out_; }

        if (!s.enabled || s.port.empty()) {
            if (out) { delete out; out = nullptr; openPort.clear(); wasPlaying = false; lastMode = -1; }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        if (!out || openPort != s.port) {
            if (out) { delete out; out = nullptr; }
            openPort.clear(); wasPlaying = false; lastMode = -1;
            try {
                out = new RtMidiOut(RtMidi::UNSPECIFIED, "shader-streamer-sync");
                int idx = outPortIndex(*out, s.port);
                if (idx >= 0) { out->openPort((unsigned int)idx, "sync out"); openPort = s.port; }
                else { delete out; out = nullptr; }
            } catch (...) { if (out) { delete out; out = nullptr; } }
            if (!out) { std::this_thread::sleep_for(std::chrono::milliseconds(20)); continue; }
        }
        if (s.mode != lastMode) { wasPlaying = false; lastMode = s.mode; }   // re-init on a mode change

        if (s.mode == 1) {
            // ---- Beat Clock send ----
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

            if (std::fabs(s.posBeats - tickPos) > (2.0 / 24.0)) {
                sendSpp(s.posBeats);
                tickPos = s.posBeats;
                nextTick = clk::now();
            }
            double secPerTick = 60.0 / ((s.bpm > 0.0 ? s.bpm : 120.0) * 24.0);
            auto now = clk::now();
            if (now >= nextTick) {
                sendByte(kMidiClock);
                tickPos += 1.0 / 24.0;
                nextTick += secs(secPerTick);
                if (nextTick < now) nextTick = now + secs(secPerTick);
            }
            std::this_thread::sleep_for(std::chrono::microseconds(500));
        } else {
            // ---- MTC send ----
            MtcRate rate = (MtcRate)std::max(0, std::min(3, s.frameRate));
            if (s.playing && !wasPlaying) {
                std::vector<unsigned char> ff; fullFrameMessage(secondsToSmpte(s.posSeconds, rate), ff);
                sendVec(ff);
                qfPiece = 0; nextQf = clk::now(); mtcLastSeconds = s.posSeconds; wasPlaying = true;
            } else if (!s.playing && wasPlaying) {
                wasPlaying = false;   // slave stops on its own QF timeout
            }
            if (!s.playing) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); continue; }

            if (std::fabs(s.posSeconds - mtcLastSeconds) > 0.25) {   // a scrub/locate -> resync
                std::vector<unsigned char> ff; fullFrameMessage(secondsToSmpte(s.posSeconds, rate), ff);
                sendVec(ff);
                qfPiece = 0; nextQf = clk::now();
            }
            mtcLastSeconds = s.posSeconds;

            double qfInterval = frameDuration(rate) / 4.0;
            auto now = clk::now();
            if (now >= nextQf) {
                if (qfPiece == 0) mtcFrozen = secondsToSmpte(s.posSeconds, rate);   // freeze at piece 0
                sendVec({0xF1, quarterFrameByte(qfPiece, mtcFrozen)});
                qfPiece = (qfPiece + 1) & 7;
                nextQf += secs(qfInterval);
                if (nextQf < now) nextQf = now + secs(qfInterval);
            }
            std::this_thread::sleep_for(std::chrono::microseconds(500));
        }
    }
    if (out) delete out;
}

} // namespace oss
