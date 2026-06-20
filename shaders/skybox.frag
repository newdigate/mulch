#version 410 core
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uPX, uNX, uPY, uNY, uPZ, uNZ;
uniform float uYaw, uPitch, uAspect;

vec3 rotX(vec3 d, float a) { float c = cos(a), s = sin(a); return vec3(d.x, d.y*c - d.z*s, d.y*s + d.z*c); }
vec3 rotY(vec3 d, float a) { float c = cos(a), s = sin(a); return vec3(d.x*c + d.z*s, d.y, -d.x*s + d.z*c); }

void main() {
    vec2 ndc = vUV * 2.0 - 1.0;
    float t = tan(radians(45.0) * 0.5);
    vec3 d = normalize(vec3(ndc.x * uAspect * t, ndc.y * t, -1.0));
    d = rotY(rotX(d, uPitch), uYaw);                 // pitch (about X) then yaw (about Y)

    // Major-axis cube-face selection -- mirrors core/CubeMap.h cubeFaceUV().
    float ax = abs(d.x), ay = abs(d.y), az = abs(d.z);
    int face; float sc, tc, ma;
    if (ax >= ay && ax >= az) {
        ma = ax;
        if (d.x > 0.0) { face = 0; sc = -d.z; tc = -d.y; }
        else           { face = 1; sc =  d.z; tc = -d.y; }
    } else if (ay >= az) {
        ma = ay;
        if (d.y > 0.0) { face = 2; sc =  d.x; tc =  d.z; }
        else           { face = 3; sc =  d.x; tc = -d.z; }
    } else {
        ma = az;
        if (d.z > 0.0) { face = 4; sc =  d.x; tc = -d.y; }
        else           { face = 5; sc = -d.x; tc = -d.y; }
    }
    vec2 uv = vec2(sc, tc) / ma * 0.5 + 0.5;

    vec3 col;
    if      (face == 0) col = texture(uPX, uv).rgb;
    else if (face == 1) col = texture(uNX, uv).rgb;
    else if (face == 2) col = texture(uPY, uv).rgb;
    else if (face == 3) col = texture(uNY, uv).rgb;
    else if (face == 4) col = texture(uPZ, uv).rgb;
    else                col = texture(uNZ, uv).rgb;
    FragColor = vec4(col, 1.0);
}
