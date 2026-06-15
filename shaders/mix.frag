#version 410 core
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uA;
uniform sampler2D uB;
uniform float uFactor;
void main() { FragColor = mix(texture(uA, vUV), texture(uB, vUV), uFactor); }
