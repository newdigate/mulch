#include <doctest/doctest.h>
#include "core/Value.h"
#include <string>

using namespace oss;

TEST_CASE("the Shader port type round-trips through Value and portTypeName") {
    Value v = ShaderRef{ "some glsl" };
    CHECK(typeOf(v) == PortType::Shader);
    CHECK(std::string(portTypeName(PortType::Shader)) == "Shader");
    // the other variant alternatives still map correctly (regression on the typeOf chain)
    CHECK(typeOf(Value{Transform{}}) == PortType::Transform);
    CHECK(typeOf(Value{VertexRef{}}) == PortType::Vertex);
}
