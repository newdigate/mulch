#version 410 core
in vec2 vUV;
out vec4 FragColor;

uniform sampler2D uImage;
uniform int   uSegments;   // mirror count (>= 1)
uniform float uRotation;   // radians, added after the fold (spin)
uniform float uZoom;       // > 0; scales sampled radius
uniform vec2  uCenter;     // fold centre in UV space

const float TAU = 6.28318530718;

void main() {
    vec2  p   = vUV - uCenter;
    float r   = length(p) / max(uZoom, 0.0001);
    float a   = atan(p.y, p.x);
    float seg = TAU / float(max(uSegments, 1));

    // Fold the angle into one wedge, mirrored so adjacent wedges meet seamlessly.
    // mod() by seg makes value(angle + seg) == value(angle): N-fold rotational symmetry.
    a = mod(a, seg);
    a = abs(a - 0.5 * seg);
    a += uRotation;

    vec2 uv = uCenter + r * vec2(cos(a), sin(a));
    FragColor = texture(uImage, clamp(uv, 0.0, 1.0));
}
