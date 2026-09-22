#include <doctest/doctest.h>
#include "core/AsyncLoader.h"
#include "core/Node.h"
#include <chrono>
#include <future>
#include <thread>

using namespace oss;

TEST_CASE("AsyncLoader::pending is true only while the worker has not finished") {
    AsyncLoader<int> loader;
    CHECK_FALSE(loader.pending());                              // nothing requested

    std::promise<void> gate;
    std::shared_future<void> open = gate.get_future().share();
    REQUIRE(loader.request("a", [open]{ open.wait(); return 42; }));
    CHECK(loader.pending());                                    // worker blocked on the gate

    gate.set_value();
    for (int i = 0; i < 1000 && loader.pending(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    CHECK_FALSE(loader.pending());                              // finished but NOT yet polled -> not pending

    int out = 0;
    CHECK(loader.poll(out));
    CHECK(out == 42);
    CHECK_FALSE(loader.pending());

    loader.request("", []{ return 0; });                        // cleared
    CHECK_FALSE(loader.pending());
}

namespace {
struct Plain : Node { Plain() : Node("plain") {} void evaluate(EvalContext&) override {} };
}

TEST_CASE("Node::loading defaults to false") {
    Plain n;
    CHECK_FALSE(n.loading());
}
