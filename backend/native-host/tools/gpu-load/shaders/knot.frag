#version 440

// Hexagonal panels with emissive edges cycling between the app's two neons, a
// fresnel rim on the dark core, and the shells cut down to their edges.

layout(location = 0) in vec3 vWorld;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec2 vUv;
layout(location = 3) flat in int vShell;

layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf
{
    mat4 viewProj;
    mat4 invViewProj;
    vec4 camTime;
    vec4 params;
    vec4 viewport; // width, height, kick pulse (0..1), unused
    mat4 model;    // the orientation the client gives the knot
};

const vec3 kCyan = vec3(0.0, 0.898, 1.0);
const vec3 kMagenta = vec3(1.0, 0.165, 0.427);

// Offset from the centre of the nearest hexagon of a unit hex grid.
vec2 hexCell(vec2 p)
{
    const vec2 r = vec2(1.0, 1.7320508);
    vec2 h = r * 0.5;
    vec2 a = mod(p, r) - h;
    vec2 b = mod(p - h, r) - h;
    return dot(a, a) < dot(b, b) ? a : b;
}

float hexDist(vec2 p)
{
    p = abs(p);
    return max(dot(p, normalize(vec2(1.0, 1.7320508))), p.x);
}

void main()
{
    float t = camTime.w;
    vec2 cell = hexCell(vUv * vec2(96.0, 6.0));
    float edge = smoothstep(0.08, 0.0, 0.5 - hexDist(cell));
    if (vShell > 0 && edge < 0.3) discard;

    vec3 neon = mix(kCyan, kMagenta, 0.5 + 0.5 * sin(vUv.x * 25.13 + t * 2.0 + float(vShell) * 0.3));
    vec3 n = normalize(vNormal);
    vec3 v = normalize(camTime.xyz - vWorld);
    float fresnel = pow(1.0 - max(dot(n, v), 0.0), 3.0);

    vec3 col;
    if (vShell == 0) {
        vec3 base = vec3(0.03, 0.035, 0.05) + fresnel * neon * 0.7;
        float pulse = 0.75 + 0.25 * sin(t * 3.0 + vUv.x * 60.0);
        col = mix(base, neon * 2.2 * pulse, edge);
    } else {
        col = neon * edge * (0.9 / (1.0 + float(vShell) * 0.04));
    }
    fragColor = vec4(col, 1.0);
}
