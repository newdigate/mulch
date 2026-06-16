#pragma once
#include <functional>

namespace oss {

// Runs an init function at most once and remembers whether it succeeded -- the
// "open a device on first use, then degrade to a silent no-op on failure"
// pattern shared by the audio and MIDI device nodes.
//
//   bool ensureStarted() { return lazy_.ensure([this]{ ...open...; return ok; }); }
class LazyInit {
public:
    bool ensure(const std::function<bool()>& init) {
        if (!tried_) { tried_ = true; ok_ = init(); }
        return ok_;
    }
    bool ok() const { return ok_; }

private:
    bool tried_ = false;
    bool ok_    = false;
};

} // namespace oss
