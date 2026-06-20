#include <doctest/doctest.h>
#include "core/Value.h"
#include <string>
#include "core/VertexShaders.h"
#include "modules/VertexShaderNode.h"
#include "core/Node.h"
#include <vector>
#include <variant>
#include <set>

using namespace oss;

TEST_CASE("the Shader port type round-trips through Value and portTypeName") {
    Value v = ShaderRef{ "some glsl" };
    CHECK(typeOf(v) == PortType::Shader);
    CHECK(std::string(portTypeName(PortType::Shader)) == "Shader");
    // the other variant alternatives still map correctly (regression on the typeOf chain)
    CHECK(typeOf(Value{Transform{}}) == PortType::Transform);
    CHECK(typeOf(Value{VertexRef{}}) == PortType::Vertex);
}

TEST_CASE("vertexShaderSource gives a complete shader for each preset") {
    REQUIRE(vertexShaderLabels().size() == 4);
    for (int i = 0; i < 4; ++i) {
        std::string src = vertexShaderSource(i);
        CHECK(src.find("void main") != std::string::npos);
        CHECK(src.find("vPosition")  != std::string::npos);
        CHECK(src.find("vColor")     != std::string::npos);
        CHECK(src.find("uPos")       != std::string::npos);
        CHECK(src.find("uColour")    != std::string::npos);
    }
    CHECK(vertexShaderSource(99) == vertexShaderSource(3));   // clamps high
    CHECK(vertexShaderSource(-1) == vertexShaderSource(0));   // clamps low

    // the 4 presets must be pairwise distinct (guards against a copy-pasted body)
    std::set<std::string> sources;
    for (int i = 0; i < 4; ++i) sources.insert(vertexShaderSource(i));
    CHECK(sources.size() == 4);
}

TEST_CASE("VertexShaderNode emits the selected preset's source") {
    VertexShaderNode node;
    std::vector<Value> in = { 1.0f };          // preset 1 (Twist)
    std::vector<Value> out(1);
    EvalContext ctx{ in, out, 0.0f };
    node.evaluate(ctx);
    CHECK(std::get<ShaderRef>(out[0]).vertexSrc == vertexShaderSource(1));
}
