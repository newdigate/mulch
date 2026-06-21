#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include "core/MidiClock.h"
#include "core/MidiTimecode.h"

class RtMidiIn;    // opaque; <RtMidi.h> stays out of this header
class RtMidiOut;

namespace oss {

struct Transport;
struct Preferences;

// Receives MIDI Beat Clock from a selected input (polled on the main thread, driving the
// Transport via Transport::externalClock) and sends it to a selected output from a dedicated
// timer thread (which solely owns the output port, so no cross-thread rtmidi access).
class MidiSyncEngine {
public:
    MidiSyncEngine();
    ~MidiSyncEngine();
    MidiSyncEngine(const MidiSyncEngine&) = delete;
    MidiSyncEngine& operator=(const MidiSyncEngine&) = delete;

    // Main thread, once per frame: poll sync-in -> drive `t`; publish the sync-out snapshot.
    void update(Transport& t, const Preferences& p, double dt);

private:
    void senderLoop();

    // sync-in (main thread only)
    RtMidiIn*       in_ = nullptr;
    std::string     inPort_;          // open input port name ("" = none)
    int             inMode_ = 0;      // open input's mode (1 = Beat Clock, 2 = MTC; reopen on change)
    double          inClock_ = 0.0;   // accumulated rtmidi delta-times (absolute tick time)
    BeatClockReader reader_;
    MtcReader       mtcReader_;

    // sync-out snapshot (main thread writes; sender thread reads)
    struct SyncOut { bool enabled = false; std::string port; bool playing = false; double bpm = 120.0;
                     double posBeats = 0.0; int mode = 1; double posSeconds = 0.0; int frameRate = 3; };
    std::mutex        outMutex_;
    SyncOut           out_;
    std::atomic<bool> running_{true};
    std::thread       senderThread_;
};

} // namespace oss
