#include <doctest/doctest.h>
#include "core/Node.h"
#include "core/Value.h"
#include <variant>

using namespace oss;

namespace {
// A throwaway node that declares one choice input.
struct Choosy : Node {
    Choosy() : Node("Choosy") { addChoiceInput("pick", {"a", "b", "c"}, 1); }
    void evaluate(EvalContext&) override {}
};
} // namespace

TEST_CASE("addChoiceInput builds a Float port carrying its choice labels") {
    Choosy n;
    REQUIRE(n.inputs().size() == 1);
    const Port& p = n.inputs()[0];
    CHECK(p.type == PortType::Float);
    REQUIRE(p.choices.size() == 3);
    CHECK(p.choices[0] == "a");
    CHECK(p.choices[2] == "c");
    CHECK(std::get<float>(p.defaultValue) == doctest::Approx(1.0f));   // default index
    CHECK(p.minVal == doctest::Approx(0.0f));
    CHECK(p.maxVal == doctest::Approx(2.0f));                          // size - 1
}
