#pragma once
#include <chrono>
#include <functional>
#include <future>
#include <string>
#include <utility>

namespace oss {

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

private:
    std::string    key_;
    std::future<T> future_;
};

} // namespace oss
