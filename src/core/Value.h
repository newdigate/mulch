#pragma once
#include <variant>
#include <string>
#include <cstddef>
#include <glm/vec4.hpp>

namespace oss {

enum class PortType { Texture, Colour, Float, Bool, Audio, String };

// Reference to a GL texture produced by a node. `id` is a GL texture name kept
// as a plain unsigned int so this header (and all of core/) stays GL-free.
struct TexRef { unsigned int id = 0; int w = 0; int h = 0; };

// Non-owning view of a node's latest audio samples.
struct AudioRef { const float* samples = nullptr; std::size_t count = 0; int sampleRate = 0; };

// Order MUST stay in sync with typeOf() below.
using Value = std::variant<float, bool, glm::vec4, std::string, TexRef, AudioRef>;

inline PortType typeOf(const Value& v) {
    switch (v.index()) {
        case 0: return PortType::Float;
        case 1: return PortType::Bool;
        case 2: return PortType::Colour;
        case 3: return PortType::String;
        case 4: return PortType::Texture;
        default: return PortType::Audio;
    }
}

inline const char* portTypeName(PortType t) {
    switch (t) {
        case PortType::Texture: return "Texture";
        case PortType::Colour:  return "Colour";
        case PortType::Float:   return "Float";
        case PortType::Bool:    return "Bool";
        case PortType::Audio:   return "Audio";
        case PortType::String:  return "String";
    }
    return "?";
}

} // namespace oss
