#pragma once
#include <chrono>
#include <functional>
#include <future>
#include <string>
#include <utility>

namespace oss {

// True while `f` holds a worker-thread result that is in flight AND not yet finished. A
// finished-but-unpolled future does NOT count: callers (AsyncLoader::pending(), and any node
// that hand-rolls a std::future instead of going through AsyncLoader) use this to decide
// whether to wait for more data, and the actual consumption happens via .get()/poll() on the
// next pass -- counting a finished-but-unconsumed future as pending would wait for a
// consumption that only happens after the wait ends.
template <class T>
inline bool futurePending(const std::future<T>& f) {
    return f.valid() && f.wait_for(std::chrono::seconds(0)) != std::future_status::ready;
}

// Runs a load function on a worker thread whenever its string key changes, and
// lets the main thread poll for the finished result (which it then applies --
// e.g. uploads to GL). The load runs entirely off the main thread; the future's
// destructor joins the worker, so an in-flight load is awaited at teardown.
template <class T>
class AsyncLoader {
public:
    // Begin a load for `key` if it differs from the current one. Returns true if
    // the key changed (a new load started, or it was cleared for an empty key).
    // `load` runs on a worker thread.
    bool request(const std::string& key, std::function<T()> load) {
        if (key == key_) return false;
        key_ = key;
        future_ = key.empty() ? std::future<T>{}
                              : std::async(std::launch::async, std::move(load));
        return true;
    }

    // If a load has finished, move its result into `out` (once) and return true.
    bool poll(T& out) {
        if (future_.valid() &&
            future_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            out = future_.get();
            return true;
        }
        return false;
    }

    // True while a load is in flight AND not yet finished. See futurePending() above for why
    // a finished-but-unpolled future does not count.
    bool pending() const { return futurePending(future_); }

private:
    std::string    key_;
    std::future<T> future_;
};

} // namespace oss
