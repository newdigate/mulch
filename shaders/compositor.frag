#version 410 core
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uA;
uniform sampler2D uB;
uniform int uMode;
uniform float uOpacity;

// Separable (per-channel) blend for ids 0..15. Mirrors BlendModes.h channel().
float channel(int id, float a, float b) {
    if (id == 1)  return a + b;
    if (id == 2)  return a - b;
    if (id == 3)  return abs(a - b);
    if (id == 4)  return a + b - 2.0*a*b;
    if (id == 5)  return a * b;
    if (id == 6)  return 1.0 - (1.0-a)*(1.0-b);
    if (id == 7)  return a < 0.5 ? 2.0*a*b : 1.0 - 2.0*(1.0-a)*(1.0-b);
    if (id == 8)  return min(a, b);
    if (id == 9)  return max(a, b);
    if (id == 10) return b >= 1.0 ? 1.0 : min(1.0, a/(1.0-b));
    if (id == 11) return b <= 0.0 ? 0.0 : 1.0 - min(1.0, (1.0-a)/b);
    if (id == 12) return b < 0.5 ? 2.0*a*b : 1.0 - 2.0*(1.0-a)*(1.0-b);
    if (id == 13) return (1.0 - 2.0*b)*a*a + 2.0*b*a;
    if (id == 14) return b <= 0.0 ? 1.0 : min(1.0, a/b);
    if (id == 15) return 0.5*(a + b);
    return b;   // Normal (0)
}

float lum(vec3 c) { return 0.3*c.r + 0.59*c.g + 0.11*c.b; }
vec3 clipColor(vec3 c) {
    float L = lum(c);
    float n = min(c.r, min(c.g, c.b));
    float x = max(c.r, max(c.g, c.b));
    if (n < 0.0) c = vec3(L) + (c - vec3(L)) * (L / (L - n));
    if (x > 1.0) c = vec3(L) + (c - vec3(L)) * ((1.0 - L) / (x - L));
    return c;
}
vec3 setLum(vec3 c, float l) { return clipColor(c + vec3(l - lum(c))); }
float sat(vec3 c) { return max(c.r, max(c.g, c.b)) - min(c.r, min(c.g, c.b)); }
// Index-based SetSat -- matches BlendModes.h setSat() line-for-line.
vec3 setSat(vec3 c, float s) {
    int mni = 0, mxi = 0;
    if (c[1] < c[mni]) mni = 1;  if (c[2] < c[mni]) mni = 2;
    if (c[1] > c[mxi]) mxi = 1;  if (c[2] > c[mxi]) mxi = 2;
    int mdi = 3 - mni - mxi;
    vec3 o = vec3(0.0);
    if (c[mxi] > c[mni]) {
        o[mdi] = (c[mdi] - c[mni]) * s / (c[mxi] - c[mni]);
        o[mxi] = s;
    }
    return o;
}

void main() {
    vec4 a = texture(uA, vUV);
    vec4 b = texture(uB, vUV);
    vec3 blended;
    if (uMode <= 15) {
        blended = vec3(channel(uMode, a.r, b.r),
                       channel(uMode, a.g, b.g),
                       channel(uMode, a.b, b.b));
    } else if (uMode == 16) { blended = setLum(setSat(b.rgb, sat(a.rgb)), lum(a.rgb)); }   // Hue
    else if (uMode == 17)   { blended = setLum(setSat(a.rgb, sat(b.rgb)), lum(a.rgb)); }   // Saturation
    else if (uMode == 18)   { blended = setLum(b.rgb, lum(a.rgb)); }                       // Color
    else if (uMode == 19)   { blended = setLum(a.rgb, lum(b.rgb)); }                       // Luminosity
    else {                                                                                 // 20 AND/21 OR/22 XOR
        ivec3 ia = ivec3(clamp(a.rgb, 0.0, 1.0) * 255.0 + 0.5);
        ivec3 ib = ivec3(clamp(b.rgb, 0.0, 1.0) * 255.0 + 0.5);
        ivec3 r = (uMode == 20) ? (ia & ib) : (uMode == 21) ? (ia | ib) : (ia ^ ib);
        blended = vec3(r) / 255.0;
    }
    blended = clamp(blended, 0.0, 1.0);
    float amt = clamp(uOpacity, 0.0, 1.0) * b.a;
    FragColor = vec4(mix(a.rgb, blended, amt), max(a.a, b.a));
}
