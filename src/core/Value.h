#pragma once
#include <variant>
#include <string>
#include <cstddef>
#include <type_traits>
#include <glm/vec4.hpp>

namespace oss {

enum class PortType { Texture, Colour, Float, Bool, Audio, String, Midi, Vertex };

// Reference to a GL texture produced by a node. `id` is a GL texture name kept
// as a plain unsigned int so this header (and all of core/) stays GL-free.
struct TexRef { unsigned int id = 0; int w = 0; int h = 0; };

// Non-owning view of a node's latest audio samples.
struct AudioRef { const float* samples = nullptr; std::size_t count = 0; int sampleRate = 0; };

// A 3-byte MIDI channel message (note on/off, control change, ...).
struct MidiEvent { unsigned char status = 0; unsigned char data1 = 0; unsigned char data2 = 0; };

// Non-owning view of the MIDI events a node produced this frame.
struct MidiRef { const MidiEvent* events = nullptr; std::size_t count = 0; };

inline bool midiIsNoteOn(const MidiEvent& e)  { return (e.status & 0xF0u) == 0x90u && e.data2 > 0; }
inline bool midiIsNoteOff(const MidiEvent& e) {
    unsigned t = e.status & 0xF0u;
    return t == 0x80u || (t == 0x90u && e.data2 == 0);
}
inline MidiEvent midiNoteOn(int note, int velocity, int channel = 0) {
    return { (unsigned char)(0x90u | (channel & 0x0F)), (unsigned char)note, (unsigned char)velocity };
}
inline MidiEvent midiNoteOff(int note, int channel = 0) {
    return { (unsigned char)(0x80u | (channel & 0x0F)), (unsigned char)note, 0 };
}

// A handle to a GL vertex buffer (VBO) a node produced, plus its vertex count.
// `vbo` is a plain GL name so this header stays GL-free, like TexRef. By
// convention the buffer holds tightly-packed vec3 positions (a line strip).
struct VertexRef { unsigned int vbo = 0; int count = 0; };

// Each alternative corresponds to a PortType value (mapped type-safely by typeOf).
using Value = std::variant<float, bool, glm::vec4, std::string, TexRef, AudioRef, MidiRef, VertexRef>;

inline PortType typeOf(const Value& v) {
    return std::visit([](auto&& arg) -> PortType {
        using T = std::decay_t<decltype(arg)>;
        if constexpr (std::is_same_v<T, float>)            return PortType::Float;
        else if constexpr (std::is_same_v<T, bool>)        return PortType::Bool;
        else if constexpr (std::is_same_v<T, glm::vec4>)   return PortType::Colour;
        else if constexpr (std::is_same_v<T, std::string>) return PortType::String;
        else if constexpr (std::is_same_v<T, TexRef>)      return PortType::Texture;
        else if constexpr (std::is_same_v<T, AudioRef>)    return PortType::Audio;
        else if constexpr (std::is_same_v<T, MidiRef>)     return PortType::Midi;
        else                                                return PortType::Vertex;
    }, v);
}

inline const char* portTypeName(PortType t) {
    switch (t) {
        case PortType::Texture: return "Texture";
        case PortType::Colour:  return "Colour";
        case PortType::Float:   return "Float";
        case PortType::Bool:    return "Bool";
        case PortType::Audio:   return "Audio";
        case PortType::String:  return "String";
        case PortType::Midi:    return "Midi";
        case PortType::Vertex:  return "Vertex";
    }
    return "?";
}

} // namespace oss
