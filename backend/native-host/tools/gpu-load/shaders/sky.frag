#version 440

// The backdrop: a synthwave grid scrolling toward the viewer under a purple
// horizon, and a volumetric neon glow around the knot. The glow is the pixel
// load — `params.x` raymarch steps through noise per pixel, with no early exit
// so the cost stays the level's, not the picture's.

layout(location = 0) in vec2 vNdc;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf
{
    mat4 viewProj;
    mat4 invViewProj;
    vec4 camTime; // eye xyz, time
    vec4 params;  // steps, octaves, shells, level
    vec4 viewport; // width, height, kick pulse (0..1), unused
    mat4 model;    // the orientation the client gives the knot
};

const vec3 kCyan = vec3(0.0, 0.898, 1.0);    // --neon-cyan #00e5ff
const vec3 kMagenta = vec3(1.0, 0.165, 0.427); // --neon-magenta #ff2a6d
const float kGlowRadius = 3.2;

float hash(vec3 p)
{
    p = fract(p * 0.3183099 + 0.1);
    p *= 17.0;
    return fract(p.x * p.y * p.z * (p.x + p.y + p.z));
}

float noise(vec3 x)
{
    vec3 i = floor(x);
    vec3 f = fract(x);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(mix(hash(i + vec3(0, 0, 0)), hash(i + vec3(1, 0, 0)), f.x),
                   mix(hash(i + vec3(0, 1, 0)), hash(i + vec3(1, 1, 0)), f.x), f.y),
               mix(mix(hash(i + vec3(0, 0, 1)), hash(i + vec3(1, 0, 1)), f.x),
                   mix(hash(i + vec3(0, 1, 1)), hash(i + vec3(1, 1, 1)), f.x), f.y),
               f.z);
}

float fbm(vec3 p, int octaves)
{
    float amplitude = 0.5;
    float sum = 0.0;
    for (int i = 0; i < octaves; ++i) {
        sum += amplitude * noise(p);
        p = p * 2.03 + vec3(1.7, 9.2, 3.1);
        amplitude *= 0.5;
    }
    return sum;
}

void main()
{
    vec4 a = invViewProj * vec4(vNdc, 0.0, 1.0);
    vec4 b = invViewProj * vec4(vNdc, 0.5, 1.0);
    vec3 ro = camTime.xyz;
    vec3 rd = normalize(b.xyz / b.w - a.xyz / a.w);
    float t = camTime.w;

    // Horizon and sky.
    float h = clamp(rd.y * 0.5 + 0.5, 0.0, 1.0);
    vec3 col = mix(vec3(0.20, 0.03, 0.24), vec3(0.01, 0.01, 0.03), smoothstep(0.47, 0.75, h));
    col += kMagenta * 0.35 * exp(-abs(rd.y) * 18.0);

    // The grid floor, scrolling toward the viewer.
    if (rd.y < -0.001) {
        float d = (-1.8 - ro.y) / rd.y;
        vec3 p = ro + rd * d;
        vec2 cell = abs(fract(p.xz * 0.5 + vec2(0.0, t * 0.6)) - 0.5);
        float line = min(cell.x, cell.y);
        float width = fwidth(line) * 1.5 + 0.004;
        float glow = smoothstep(width, 0.0, line) + 0.25 * exp(-line * 12.0);
        col += mix(kMagenta, kCyan, 0.3) * glow * exp(-d * 0.07);
    }

    // The volumetric glow, inside a sphere around the knot.
    float bq = dot(ro, rd);
    float c = dot(ro, ro) - kGlowRadius * kGlowRadius;
    float disc = bq * bq - c;
    if (disc > 0.0) {
        float s = sqrt(disc);
        float t0 = max(-bq - s, 0.0);
        float t1 = -bq + s;
        int steps = int(params.x);
        int octaves = int(params.y);
        float dt = (t1 - t0) / float(steps);
        vec3 glow = vec3(0.0);
        float transmittance = 1.0;
        for (int i = 0; i < steps; ++i) {
            vec3 p = ro + rd * (t0 + (float(i) + 0.5) * dt);
            float r = length(p);
            float shell = smoothstep(kGlowRadius, 0.6, r);
            float density = fbm(p * 1.4 + vec3(0.0, t * 0.5, t * 0.2), octaves) * shell;
            density = max(density - 0.32, 0.0) * 2.2;
            vec3 tint = mix(kCyan, kMagenta, 0.5 + 0.5 * sin(r * 3.0 - t * 1.5));
            glow += transmittance * density * tint * dt;
            transmittance *= exp(-density * dt * 0.5);
        }
        col = col * transmittance + glow * 0.9;
    }

    fragColor = vec4(col, 1.0);
}
