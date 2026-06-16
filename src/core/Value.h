#pragma once
#include <variant>
#include <string>
#include <cstddef>
#include <type_traits>
#include <glm/vec4.hpp>

namespace oss {

enum class PortType { Texture, Colour, Float, Bool, Audio, String, Midi, Vertex, Transform };

// Reference to a GL texture produced by a node. `id` is a GL texture name kept
// as a plain unsigned int so this header (and all of core/) stays GL-free.
struct TexRef { unsigned int id = 0; int w = 0; int h = 0; };

// Non-owning view of a node's latest audio samples. `count` is the total number
// of floats in `samples` (interleaved); `channels` is 1 (mono) or 2 (interleaved
// stereo L,R,L,R). `frames()` is the per-channel sample count. Defaulting
// channels to 1 keeps every existing `AudioRef{ptr, n, sr}` a mono buffer.
struct AudioRef {
    const float* samples    = nullptr;
    std::size_t  count      = 0;
    int          sampleRate = 0;
    int          channels   = 1;
    std::size_t  frames() const { return channels > 0 ? count / (std::size_t)channels : count; }
};

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

// How a VertexRef's vertices are assembled when drawn.
enum class Primitive { LineStrip, Lines, Triangles };

// The per-vertex layout of a VertexRef's buffer:
//   Pos3        - 3 floats: position (stride 12)
//   Pos3Normal3 - 6 floats: position + normal (stride 24, normal at offset 12)
enum class VertexFormat { Pos3, Pos3Normal3 };

// A handle to a GL vertex buffer (VBO) a node produced, with its vertex count,
// primitive, and per-vertex format. `vbo` is a plain GL name so this header
// stays GL-free, like TexRef.
struct VertexRef {
    unsigned int vbo       = 0;
    int          count     = 0;
    Primitive    primitive = Primitive::LineStrip;
    VertexFormat format    = VertexFormat::Pos3;
};

// A shared world transform so several 3D renderers can be aligned. Currently a
// single rotation about Y (radians). `active` distinguishes a real transform
// (from a World Transform node) from the default an unconnected input carries, so
// a renderer can fall back to rotating itself when nothing is connected.
struct Transform {
    float angle  = 0.0f;
    bool  active = false;
};

// Each alternative corresponds to a PortType value (mapped type-safely by typeOf).
using Value = std::variant<float, bool, glm::vec4, std::string, TexRef, AudioRef, MidiRef, VertexRef, Transform>;

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
        else if constexpr (std::is_same_v<T, VertexRef>)   return PortType::Vertex;
        else                                                return PortType::Transform;
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
        case PortType::Transform: return "Transform";
    }
    return "?";
}

} // namespace oss
