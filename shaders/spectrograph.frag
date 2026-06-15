#version 410 core
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uSpectrum;   // kBins x 1, R channel = normalized magnitude
void main() {
    float v = texture(uSpectrum, vec2(vUV.x, 0.5)).r;
    float lit = step(1.0 - vUV.y, v);                       // bar height = magnitude
    vec3 col = mix(vec3(0.04, 0.05, 0.08), vec3(0.10, 0.95, 0.45), lit);
    FragColor = vec4(col, 1.0);
}
