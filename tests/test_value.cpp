#include <doctest/doctest.h>
#include "core/Value.h"

using namespace oss;

TEST_CASE("typeOf maps each Value alternative to its PortType") {
    CHECK(typeOf(Value{1.0f})                 == PortType::Float);
    CHECK(typeOf(Value{true})                 == PortType::Bool);
    CHECK(typeOf(Value{glm::vec4(1,0,0,1)})   == PortType::Colour);
    CHECK(typeOf(Value{std::string("x.wav")}) == PortType::String);
    CHECK(typeOf(Value{TexRef{7, 4, 4}})      == PortType::Texture);
    CHECK(typeOf(Value{AudioRef{}})           == PortType::Audio);
}

TEST_CASE("portTypeName is stable") {
    CHECK(std::string(portTypeName(PortType::Texture)) == "Texture");
    CHECK(std::string(portTypeName(PortType::Audio))   == "Audio");
}
