#pragma once
#include <string>
#include <vector>

namespace oss {

// Preset transform names for the Vertex Shader node's `preset` dropdown.
inline const std::vector<std::string>& vertexShaderLabels() {
    static const std::vector<std::string> labels = { "Identity", "Twist", "Wave", "Bulge" };
    return labels;
}

// A complete GLSL 410 vertex shader for transform feedback (preset clamped to a valid
// index). Convention the Deform node relies on: in vec3 aPos (loc 0) + aColor (loc 1);
// uniforms uPos (the Deform `position` scalar) + uColour; outputs vPosition + vColor
// (captured by transform feedback varyings {"vPosition","vColor"}). Each preset differs
// only in how it maps aPos -> vPosition; all tint vColor by uColour.
inline std::string vertexShaderSource(int preset) {
    static const char* kPreamble =
        "#version 410 core\n"
        "layout(location = 0) in vec3 aPos;\n"
        "layout(location = 1) in vec3 aColor;\n"
        "uniform float uPos;\n"
        "uniform vec4  uColour;\n"
        "out vec3 vPosition;\n"
        "out vec3 vColor;\n"
        "void main() {\n";
    static const char* kEpilogue =
        "    vColor = aColor + uColour.rgb;\n"            // colourless input (aColor=0) -> uColour
        "    gl_Position = vec4(vPosition, 1.0);\n"       // unused under rasterizer discard
        "}\n";
    static const char* kBodies[4] = {
        "    vPosition = aPos;\n",                                                    // Identity
        "    float a = uPos * aPos.y; float c = cos(a), s = sin(a);\n"
        "    vPosition = vec3(aPos.x*c - aPos.z*s, aPos.y, aPos.x*s + aPos.z*c);\n",  // Twist
        "    vPosition = aPos + vec3(0.0, uPos * sin(aPos.x * 6.2831853), 0.0);\n",   // Wave
        "    vPosition = aPos * (1.0 + uPos * length(aPos));\n",                      // Bulge
    };
    int n = (int)vertexShaderLabels().size();
    if (preset < 0) preset = 0;
    if (preset >= n) preset = n - 1;
    return std::string(kPreamble) + kBodies[preset] + kEpilogue;
}

} // namespace oss
