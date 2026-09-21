#include <doctest/doctest.h>
#include "core/DynLib.h"
#include "system_lib.h"
#include <utility>

using namespace oss;

TEST_CASE("DynLib: a nonexistent path fails cleanly with an error message") {
    DynLib lib;
    CHECK_FALSE(lib.open("/nonexistent/dir/libnope-12345.so"));
    CHECK_FALSE(lib.isOpen());
    CHECK_FALSE(lib.error().empty());
    CHECK(lib.symbol<double (*)(double)>("cos") == nullptr);   // closed -> null, no crash
}

TEST_CASE("DynLib: opens a system library, resolves and calls a symbol") {
    DynLib lib;
    REQUIRE(lib.open(kSystemLibPath));
    CHECK(lib.isOpen());
    CHECK(lib.error().empty());
    auto cosFn = lib.symbol<double (*)(double)>("cos");
    REQUIRE(cosFn != nullptr);
    CHECK(cosFn(0.0) == doctest::Approx(1.0));
}

TEST_CASE("DynLib: a missing symbol returns null") {
    DynLib lib;
    REQUIRE(lib.open(kSystemLibPath));
    CHECK(lib.symbol<void (*)()>("oss_definitely_not_a_symbol_xyz") == nullptr);
}

TEST_CASE("DynLib: move transfers ownership") {
    DynLib a;
    REQUIRE(a.open(kSystemLibPath));
    DynLib b(std::move(a));
    CHECK_FALSE(a.isOpen());
    CHECK(b.isOpen());
    DynLib c;
    c = std::move(b);
    CHECK_FALSE(b.isOpen());
    CHECK(c.symbol<double (*)(double)>("cos") != nullptr);
}
