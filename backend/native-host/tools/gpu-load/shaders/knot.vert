#version 440

// The knot and its shells. Instance 0 is the solid core; every other instance
// is a larger copy spinning its own way, drawn as a cage of hexagon edges —
// the geometry load, since each shell is the whole knot again.

layout(location = 0) in vec3 position;
layout(location = 1) in vec3 normal;
layout(location = 2) in vec2 uv;

layout(location = 0) out vec3 vWorld;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec2 vUv;
layout(location = 3) flat out int vShell;

layout(std140, binding = 0) uniform buf
{
    mat4 viewProj;
    mat4 invViewProj;
    vec4 camTime;
    vec4 params;
    vec4 viewport; // width, height, kick pulse (0..1), unused
    mat4 model;    // the orientation the client gives the knot
};

mat3 rotY(float a)
{
    float c = cos(a), s = sin(a);
    return mat3(c, 0.0, -s, 0.0, 1.0, 0.0, s, 0.0, c);
}

mat3 rotX(float a)
{
    float c = cos(a), s = sin(a);
    return mat3(1.0, 0.0, 0.0, 0.0, c, s, 0.0, -s, c);
}

void main()
{
    int shell = gl_InstanceIndex;
    float fs = float(shell);
    float t = camTime.w;
    // Shells interleave between 1x and 1.8x the core instead of growing without
    // bound: all of them stay on screen, all of them cost fragments.
    float scale = shell == 0 ? 1.0 : 1.0 + 0.8 * fract(fs * 0.61803);
    float dir = (shell % 2 == 0) ? 1.0 : -1.0;
    mat3 rot = rotY(dir * t * (0.35 + 0.013 * fs)) * rotX(0.4 * sin(t * 0.21 + fs * 0.17));
    // A ripple running along the tube: a little vertex work per shell.
    vec3 p = position + normal * 0.03 * sin(uv.x * 188.5 + t * 4.0 + fs);
    // The kick swells the whole knot a little, like a heartbeat: on the
    // client, the swell and the kick heard should land together.
    float beat = 1.0 + 0.045 * viewport.z;
    mat3 held = mat3(model);
    vWorld = held * (rot * (p * scale * beat));
    vNormal = held * (rot * normal);
    vUv = uv;
    vShell = shell;
    gl_Position = viewProj * vec4(vWorld, 1.0);
}
