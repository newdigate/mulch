#version 410 core
in vec2 vUV;
out vec4 FragColor;

uniform sampler2D uFrom;
uniform sampler2D uTo;
uniform float uMix;   // 0 = from, 1 = to

void main() {
    FragColor = mix(texture(uFrom, vUV), texture(uTo, vUV), uMix);
}
