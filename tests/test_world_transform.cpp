#include <doctest/doctest.h>
#include "modules/WorldTransformNode.h"
#include "core/Value.h"
#include <vector>

using namespace oss;

TEST_CASE("World Transform integrates the rate into an active transform") {
    WorldTransformNode n;
    std::vector<Value> in = { 2.0f };          // rate (port 0), rad/s
    std::vector<Value> out(1);
    EvalContext ctx{in, out, 0.5f};            // dt = 0.5 s
    n.evaluate(ctx);
    Transform t = std::get<Transform>(out[0]);
    CHECK(t.active);
    CHECK(t.angle == doctest::Approx(1.0f));   // 2.0 * 0.5
    n.evaluate(ctx);                           // accumulates across frames
    CHECK(std::get<Transform>(out[0]).angle == doctest::Approx(2.0f));
}

TEST_CASE("a default Transform is inactive and maps to PortType::Transform") {
    Transform t;
    CHECK_FALSE(t.active);                     // -> renderers fall back to self-rotation
    CHECK(typeOf(Value{Transform{}}) == PortType::Transform);
}
