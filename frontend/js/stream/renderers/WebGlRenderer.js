/*
 * MoonlightWeb — browser-based Sunshine/GameStream client.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * WebGlRenderer — the enhancers without WebGPU (debug builds only, for now).
 *
 * Why a third renderer exists at all. Measured 03/09/2026: the WebGPU path
 * costs 4 ms of render on Windows and 10 ms on macOS for the SAME frames
 * Canvas2D paints in 0.2–0.6 ms, because WebGPU has no equivalent of the
 * `desynchronized` context hint and its presentation goes through the
 * compositor. WebGL does have that hint. So this renderer asks the question
 * the WebGPU one cannot: what does an upscaler cost once presentation is out
 * of the way? Three shaders, chosen so the comparison is like for like:
 *
 *   'gl-fsr1' — AMD FidelityFX FSR 1.0 (EASU + RCAS), the same math as the
 *               WGSL port in WebGpuRenderer, two passes through an
 *               intermediate framebuffer at output resolution.
 *   'gl-sgsr' — Qualcomm Snapdragon Game Super Resolution v1, one pass.
 *   'gl-nis'  — NVIDIA Image Scaling (NVScaler, SDK 1.0.3, MIT), one pass.
 *               The SDK ships a compute shader that stages a tile of luma and
 *               an edge map in shared memory; this is the per-pixel port of it:
 *               the 6×6 luma support, the four 3×3 edge maps and the 64-phase
 *               filter banks are recomputed per fragment. Same output, more
 *               texture reads — which is the honest cost on a fragment path.
 *
 * GLSL ES 3.00 (WebGL2) has no textureGather: every gather is four texelFetch
 * on the 2×2 footprint a bilinear tap would cover, in the same component
 * order the gather instruction defines (x=(−,+), y=(+,+), z=(+,−), w=(−,−)).
 *
 * SDR only. Chrome's WebGL canvas has no HDR surface, so an HDR stream on
 * this renderer is tone-mapped by the browser on import like Canvas2D. NIS
 * knows how to sharpen PQ and linear HDR (its NIS_HDR_MODE); that knob is
 * not wired because nothing here can present the result.
 *
 * Presentation: no fence is awaited after the draw (like Canvas2D, unlike
 * WebGPU's onSubmittedWorkDone), so the overlay's Render leg measures the
 * submission, not the GPU. PipelineDiag's drop-to-latest still bounds the
 * backlog because draws are serialised by RenderPacing when the renderer asks
 * for it — this one does not, WebGL commands execute in submission order.
 */
import { VideoRenderer } from './VideoRenderer.js';

// ── Shaders ─────────────────────────────────────────────────────────────────

// Full-screen triangle from gl_VertexID, uv with v down (texture row 0 = top
// of the picture, which is how a VideoFrame uploads). `#version` must be the
// first byte of the source, hence the odd string layout.
const VS = `#version 300 es
out vec2 vUv;
void main() {
    vec2 uv = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    vUv = uv;
    gl_Position = vec4(uv * vec2(2.0, -2.0) + vec2(-1.0, 1.0), 0.0, 1.0);
}
`;

// textureGather emulation shared by the three fragment shaders.
const GATHER_GLSL = /* glsl */ `
// The 2x2 footprint a bilinear tap at p would read, as texel coordinates of
// its top-left texel, clamped to the picture like a clamp-to-edge sampler.
ivec2 gatherBase(sampler2D t, vec2 p) {
    return ivec2(floor(p * vec2(textureSize(t, 0)) - 0.5));
}
vec3 fetchClamped(sampler2D t, ivec2 c) {
    ivec2 mx = textureSize(t, 0) - 1;
    return texelFetch(t, clamp(c, ivec2(0), mx), 0).rgb;
}
// All three channels of one gather: r/g/b each hold the four texels in the
// gather order (x = (-,+), y = (+,+), z = (+,-), w = (-,-)).
void gather3(sampler2D t, vec2 p, out vec4 r, out vec4 g, out vec4 b) {
    ivec2 c = gatherBase(t, p);
    vec3 t00 = fetchClamped(t, c);
    vec3 t10 = fetchClamped(t, c + ivec2(1, 0));
    vec3 t01 = fetchClamped(t, c + ivec2(0, 1));
    vec3 t11 = fetchClamped(t, c + ivec2(1, 1));
    r = vec4(t01.r, t11.r, t10.r, t00.r);
    g = vec4(t01.g, t11.g, t10.g, t00.g);
    b = vec4(t01.b, t11.b, t10.b, t00.b);
}
vec4 gatherG(sampler2D t, vec2 p) {
    ivec2 c = gatherBase(t, p);
    return vec4(fetchClamped(t, c + ivec2(0, 1)).g, fetchClamped(t, c + ivec2(1, 1)).g,
                fetchClamped(t, c + ivec2(1, 0)).g, fetchClamped(t, c).g);
}
`;

// FSR1 EASU — AMD FidelityFX (MIT). Same math as the WGSL port in
// WebGpuRenderer (itself firdawolf's), with the two structs turned into inout
// parameters. One deliberate deviation: a 1e-6 floor under the two
// reciprocals of EasuSet. AMD divides by the neighbourhood's contrast, which
// is exactly zero on a flat area; that gives inf times a zero direction = NaN,
// and whether saturate(NaN) is 0 is up to the GPU. WebGPU got away with it on
// the machines tested; a WebGL port is not owed the same luck.
const EASU_FS =
    `#version 300 es
precision highp float;
uniform sampler2D uTex;
uniform vec4 uRes; // inW, inH, outW, outH
in vec2 vUv;
out vec4 oColor;
` +
    GATHER_GLSL +
    /* glsl */ `
void easuTap(inout vec3 aC, inout float aW, vec2 off, vec2 dir, vec2 len, float lob, float clp,
             vec3 c) {
    vec2 v;
    v.x = off.x * dir.x + off.y * dir.y;
    v.y = off.x * (-dir.y) + off.y * dir.x;
    v *= len;
    float d2 = min(v.x * v.x + v.y * v.y, clp);
    float wB = 2.0 / 5.0 * d2 - 1.0;
    float wA = lob * d2 - 1.0;
    wB *= wB;
    wA *= wA;
    wB = 25.0 / 16.0 * wB - (25.0 / 16.0 - 1.0);
    float w = wB * wA;
    aC += c * w;
    aW += w;
}
void easuSet(inout vec2 dir, inout float len, vec2 pp, bool biS, bool biT, bool biU, bool biV,
             float lA, float lB, float lC, float lD, float lE) {
    float w = 0.0;
    if (biS) w = (1.0 - pp.x) * (1.0 - pp.y);
    else if (biT) w = pp.x * (1.0 - pp.y);
    else if (biU) w = (1.0 - pp.x) * pp.y;
    else if (biV) w = pp.x * pp.y;
    float dc = lD - lC;
    float cb = lC - lB;
    float lenX = max(abs(dc), abs(cb));
    lenX = 1.0 / (lenX + 1e-6);
    float dirX = lD - lB;
    dir.x += dirX * w;
    lenX = clamp(abs(dirX) * lenX, 0.0, 1.0);
    lenX *= lenX;
    len += lenX * w;
    float ec = lE - lC;
    float ca = lC - lA;
    float lenY = max(abs(ec), abs(ca));
    lenY = 1.0 / (lenY + 1e-6);
    float dirY = lE - lA;
    dir.y += dirY * w;
    lenY = clamp(abs(dirY) * lenY, 0.0, 1.0);
    lenY *= lenY;
    len += lenY * w;
}
void main() {
    vec2 inputSize = uRes.xy;
    vec2 outputSize = uRes.zw;
    vec2 pp = (floor(vUv * outputSize) + 0.5) / outputSize * inputSize - 0.5;
    vec2 fp = floor(pp);
    pp -= fp;
    vec2 p0 = fp + vec2(1.0, -1.0);
    vec2 p1 = p0 + vec2(-1.0, 2.0);
    vec2 p2 = p0 + vec2(1.0, 2.0);
    vec2 p3 = p0 + vec2(0.0, 4.0);
    p0 /= inputSize;
    p1 /= inputSize;
    p2 /= inputSize;
    p3 /= inputSize;
    vec4 bczzR, bczzG, bczzB, ijfeR, ijfeG, ijfeB, klhgR, klhgG, klhgB, zzonR, zzonG, zzonB;
    gather3(uTex, p0, bczzR, bczzG, bczzB);
    gather3(uTex, p1, ijfeR, ijfeG, ijfeB);
    gather3(uTex, p2, klhgR, klhgG, klhgB);
    gather3(uTex, p3, zzonR, zzonG, zzonB);
    vec4 bczzL = bczzB * 0.5 + (bczzR * 0.5 + bczzG);
    vec4 ijfeL = ijfeB * 0.5 + (ijfeR * 0.5 + ijfeG);
    vec4 klhgL = klhgB * 0.5 + (klhgR * 0.5 + klhgG);
    vec4 zzonL = zzonB * 0.5 + (zzonR * 0.5 + zzonG);
    float bL = bczzL.x, cL = bczzL.y;
    float iL = ijfeL.x, jL = ijfeL.y, fL = ijfeL.z, eL = ijfeL.w;
    float kL = klhgL.x, lL = klhgL.y, hL = klhgL.z, gL = klhgL.w;
    float oL = zzonL.z, nL = zzonL.w;
    vec2 dir = vec2(0.0);
    float len = 0.0;
    easuSet(dir, len, pp, true, false, false, false, bL, eL, fL, gL, jL);
    easuSet(dir, len, pp, false, true, false, false, cL, fL, gL, hL, kL);
    easuSet(dir, len, pp, false, false, true, false, fL, iL, jL, kL, nL);
    easuSet(dir, len, pp, false, false, false, true, gL, jL, kL, lL, oL);
    vec2 dir2 = dir * dir;
    float dirR = dir2.x + dir2.y;
    bool zro = dirR < 1.0 / 32768.0;
    dirR = 1.0 / sqrt(dirR);
    if (zro) { dirR = 1.0; dir.x = 1.0; }
    dir *= dirR;
    len *= 0.5;
    len *= len;
    float stretch = (dir.x * dir.x + dir.y * dir.y) * (1.0 / max(abs(dir.x), abs(dir.y)));
    vec2 len2 = vec2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);
    float lob = 0.5 + ((1.0 / 4.0 - 0.04) - 0.5) * len;
    float clp = 1.0 / lob;
    vec3 min4 = min(min(vec3(ijfeR.z, ijfeG.z, ijfeB.z), min(vec3(klhgR.w, klhgG.w, klhgB.w),
                    vec3(ijfeR.y, ijfeG.y, ijfeB.y))), vec3(klhgR.x, klhgG.x, klhgB.x));
    vec3 max4 = max(max(vec3(ijfeR.z, ijfeG.z, ijfeB.z), max(vec3(klhgR.w, klhgG.w, klhgB.w),
                    vec3(ijfeR.y, ijfeG.y, ijfeB.y))), vec3(klhgR.x, klhgG.x, klhgB.x));
    vec3 aC = vec3(0.0);
    float aW = 0.0;
    easuTap(aC, aW, vec2(0.0, -1.0) - pp, dir, len2, lob, clp, vec3(bczzR.x, bczzG.x, bczzB.x));
    easuTap(aC, aW, vec2(1.0, -1.0) - pp, dir, len2, lob, clp, vec3(bczzR.y, bczzG.y, bczzB.y));
    easuTap(aC, aW, vec2(-1.0, 1.0) - pp, dir, len2, lob, clp, vec3(ijfeR.x, ijfeG.x, ijfeB.x));
    easuTap(aC, aW, vec2(0.0, 1.0) - pp, dir, len2, lob, clp, vec3(ijfeR.y, ijfeG.y, ijfeB.y));
    easuTap(aC, aW, vec2(0.0, 0.0) - pp, dir, len2, lob, clp, vec3(ijfeR.z, ijfeG.z, ijfeB.z));
    easuTap(aC, aW, vec2(-1.0, 0.0) - pp, dir, len2, lob, clp, vec3(ijfeR.w, ijfeG.w, ijfeB.w));
    easuTap(aC, aW, vec2(1.0, 1.0) - pp, dir, len2, lob, clp, vec3(klhgR.x, klhgG.x, klhgB.x));
    easuTap(aC, aW, vec2(2.0, 1.0) - pp, dir, len2, lob, clp, vec3(klhgR.y, klhgG.y, klhgB.y));
    easuTap(aC, aW, vec2(2.0, 0.0) - pp, dir, len2, lob, clp, vec3(klhgR.z, klhgG.z, klhgB.z));
    easuTap(aC, aW, vec2(1.0, 0.0) - pp, dir, len2, lob, clp, vec3(klhgR.w, klhgG.w, klhgB.w));
    easuTap(aC, aW, vec2(1.0, 2.0) - pp, dir, len2, lob, clp, vec3(zzonR.z, zzonG.z, zzonB.z));
    easuTap(aC, aW, vec2(0.0, 2.0) - pp, dir, len2, lob, clp, vec3(zzonR.w, zzonG.w, zzonB.w));
    vec3 c = min(max4, max(min4, aC * (1.0 / aW)));
    oColor = vec4(c, 1.0);
}
`;

// FSR1 RCAS — AMD FidelityFX (MIT). Sharpness 0.595 = exp2(-0.75), the same
// setting as the WebGPU path. uRes.zw are the texel sizes of the source.
//
// RCAS reads the EASU pass's framebuffer texture, and GL stores a framebuffer
// bottom-up: the row the vertex shader put at the top of the picture is the
// texture's LAST row. Sampling with v flipped puts it back on top; the ±1
// texel taps are symmetric so nothing else changes. (The single-pass shaders
// draw straight to the canvas and need no such flip.)
const RCAS_FS = `#version 300 es
precision highp float;
uniform sampler2D uTex;
uniform vec4 uRes; // outW, outH, 1/outW, 1/outH
in vec2 vUv;
out vec4 oColor;
const float sharpness = 0.595;
float min3f(float a, float b, float c) { return min(a, min(b, c)); }
float max3f(float a, float b, float c) { return max(a, max(b, c)); }
void main() {
    vec2 uv = vec2(vUv.x, 1.0 - vUv.y);
    const float FSR_RCAS_LIMIT = 0.25 - (1.0 / 16.0);
    vec3 b = texture(uTex, uv + vec2(0.0, -uRes.w)).rgb;
    vec3 d = texture(uTex, uv + vec2(-uRes.z, 0.0)).rgb;
    vec3 e = texture(uTex, uv).rgb;
    vec3 f = texture(uTex, uv + vec2(uRes.z, 0.0)).rgb;
    vec3 h = texture(uTex, uv + vec2(0.0, uRes.w)).rgb;
    float bL = b.b * 0.5 + (b.r * 0.5 + b.g);
    float dL = d.b * 0.5 + (d.r * 0.5 + d.g);
    float eL = e.b * 0.5 + (e.r * 0.5 + e.g);
    float fL = f.b * 0.5 + (f.r * 0.5 + f.g);
    float hL = h.b * 0.5 + (h.r * 0.5 + h.g);
    float nz = 0.25 * bL + 0.25 * dL + 0.25 * fL + 0.25 * hL - eL;
    nz = clamp(abs(nz) * (1.0 / (max3f(max3f(bL, dL, eL), fL, hL) - min3f(min3f(bL, dL, eL), fL, hL))),
               0.0, 1.0);
    nz = -0.5 * nz + 1.0;
    vec3 mn4 = min(min(b, d), min(f, h));
    vec3 mx4 = max(max(b, d), max(f, h));
    vec2 peakC = vec2(1.0, -1.0 * 4.0);
    vec3 hitMin = min(mn4, e) * (1.0 / (4.0 * mx4));
    vec3 hitMax = (peakC.x - max(mx4, e)) * (1.0 / (4.0 * mn4 + peakC.y));
    vec3 lobeRGB = max(-hitMin, hitMax);
    float lobe = max(-FSR_RCAS_LIMIT, min(max3f(lobeRGB.r, lobeRGB.g, lobeRGB.b), 0.0)) * sharpness;
    lobe *= nz;
    float rcpL = 1.0 / (4.0 * lobe + 1.0);
    vec3 c = (lobe * b + lobe * d + lobe * h + lobe * f + e) * rcpL;
    oColor = vec4(c, 1.0);
}
`;

// SGSRv1 — Qualcomm (BSD-3-Clause), mode 1, green as luma. Same constants as
// the WebGPU port (EdgeThreshold 6/255, EdgeSharpness 1.5).
const SGSR_FS =
    `#version 300 es
precision highp float;
uniform sampler2D uTex;
uniform vec4 uView; // 1/inW, 1/inH, inW, inH
in vec2 vUv;
out vec4 oColor;
` +
    GATHER_GLSL +
    /* glsl */ `
const float EDGE_THRESHOLD = 6.0 / 255.0;
const float EDGE_SHARPNESS = 1.5;
float fastLanczos2(float x) {
    float wA = x - 4.0;
    float wB = x * wA - wA;
    wA *= wA;
    return wB * wA;
}
vec2 weightY(float dx, float dy, float c, vec3 data) {
    float stdev = data.x;
    vec2 dir = data.yz;
    float edgeDis = (dx * dir.y) + (dy * dir.x);
    float x = ((dx * dx) + (dy * dy)) +
              ((edgeDis * edgeDis) * ((clamp((c * c) * stdev, 0.0, 1.0) * 0.7) - 1.0));
    float w = fastLanczos2(x);
    return vec2(w, w * c);
}
vec2 edgeDirection(vec4 left, vec4 right) {
    float RxLz = right.x - left.z;
    float RwLy = right.w - left.y;
    vec2 delta = vec2(RxLz + RwLy, RxLz - RwLy);
    float lengthInv = inversesqrt((delta.x * delta.x + 3.075740e-05) + (delta.y * delta.y));
    return delta * lengthInv;
}
void main() {
    vec2 uv = vUv;
    vec4 con1 = uView;
    vec4 pix = vec4(texture(uTex, uv).rgb, 1.0);
    vec2 imgCoord = (uv * con1.zw) + vec2(-0.5, 0.5);
    vec2 imgCoordPixel = floor(imgCoord);
    vec2 coord = imgCoordPixel * con1.xy;
    vec2 pl = imgCoord - imgCoordPixel;
    vec4 left = gatherG(uTex, coord);
    float pixL = pix.y;
    float edgeVote = abs(left.z - left.y) + abs(pixL - left.y) + abs(pixL - left.z);
    if (edgeVote > EDGE_THRESHOLD) {
        coord.x += con1.x;
        vec4 right = gatherG(uTex, coord + vec2(con1.x, 0.0));
        vec4 udUp = gatherG(uTex, coord + vec2(0.0, -con1.y));
        vec4 udDn = gatherG(uTex, coord + vec2(0.0, con1.y));
        vec4 upDown = vec4(udUp.w, udUp.z, udDn.y, udDn.x);
        float mean = (left.y + left.z + right.x + right.w) * 0.25;
        left -= vec4(mean);
        right -= vec4(mean);
        upDown -= vec4(mean);
        float pixW = pixL - mean;
        float sum = (abs(left.x) + abs(left.y) + abs(left.z) + abs(left.w)) +
                    (abs(right.x) + abs(right.y) + abs(right.z) + abs(right.w)) +
                    (abs(upDown.x) + abs(upDown.y) + abs(upDown.z) + abs(upDown.w));
        float sumMean = 1.014185e+01 / sum;
        float stdev = sumMean * sumMean;
        vec3 data = vec3(stdev, edgeDirection(left, right));
        vec2 aWY = weightY(pl.x, pl.y + 1.0, upDown.x, data);
        aWY += weightY(pl.x - 1.0, pl.y + 1.0, upDown.y, data);
        aWY += weightY(pl.x - 1.0, pl.y - 2.0, upDown.z, data);
        aWY += weightY(pl.x, pl.y - 2.0, upDown.w, data);
        aWY += weightY(pl.x + 1.0, pl.y - 1.0, left.x, data);
        aWY += weightY(pl.x, pl.y - 1.0, left.y, data);
        aWY += weightY(pl.x, pl.y, left.z, data);
        aWY += weightY(pl.x + 1.0, pl.y, left.w, data);
        aWY += weightY(pl.x - 1.0, pl.y - 1.0, right.x, data);
        aWY += weightY(pl.x - 2.0, pl.y - 1.0, right.y, data);
        aWY += weightY(pl.x - 2.0, pl.y, right.z, data);
        aWY += weightY(pl.x - 1.0, pl.y, right.w, data);
        float finalY = aWY.y / aWY.x;
        float max4 = max(max(left.y, left.z), max(right.x, right.w));
        float min4 = min(min(left.y, left.z), min(right.x, right.w));
        finalY = clamp(EDGE_SHARPNESS * finalY, min4, max4);
        float deltaY = finalY - pixW;
        pix.rgb = clamp(pix.rgb + vec3(deltaY), 0.0, 1.0);
    }
    pix.w = 1.0;
    oColor = pix;
}
`;

// NVIDIA Image Scaling — NVScaler, SDK 1.0.3 (MIT), SDR mode, per-pixel port
// of NIS_Scaler.h. Naming follows the SDK so the two can be read side by side.
//
// Coordinates: for the output pixel (dstX, dstY), srcX = (dstX + 0.5) * kScaleX
// - 0.5; the 6×6 luma support P(i, j) is the source pixel (floor(srcX) - 2 + j,
// floor(srcY) - 2 + i) — i indexes rows, j columns, as in the SDK's p[i][j].
// The edge map the SDK stages per source pixel is GetEdgeMap of that pixel's
// 3×3 neighbourhood; the four entries an output pixel interpolates sit at
// P(2..3, 2..3), i.e. GetEdgeMap(P, 1 + i, 1 + j) for i, j in {0, 1}.
//
// The filter banks (coef_scale, coef_usm: 64 phases × 6 taps) live in a 4×64
// RGBA32F texture: columns 0–1 hold the scaler taps, columns 2–3 the USM taps,
// four per texel. GLSL ES 3.00 has no arrays of arrays, so the support is a
// flat float[36] behind the P() macro.
const NIS_FS = `#version 300 es
precision highp float;
uniform sampler2D uTex;   // linear, clamp
uniform sampler2D uCoef;  // 4x64 RGBA32F filter banks
uniform vec4 uScale;      // kScaleX, kScaleY, kSrcNormX, kSrcNormY
uniform vec4 uDetect;     // kDetectRatio, kDetectThres, kMinContrastRatio, kRatioNorm
uniform vec4 uSharpA;     // kContrastBoost, kEps, kSharpStartY, kSharpScaleY
uniform vec4 uSharpB;     // kSharpStrengthMin, kSharpStrengthScale, kSharpLimitMin, kSharpLimitScale
uniform vec2 uOut;        // outW, outH
in vec2 vUv;
out vec4 oColor;

#define kPhaseCount 64
#define P(i, j) p[(i) * 6 + (j)]

float getY(vec3 c) { return 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b; }
float coefScaler(int phase, int i) {
    vec4 v = texelFetch(uCoef, ivec2(i >> 2, phase), 0);
    return v[i & 3];
}
float coefUSM(int phase, int i) {
    vec4 v = texelFetch(uCoef, ivec2(2 + (i >> 2), phase), 0);
    return v[i & 3];
}
float lumaAt(ivec2 c) {
    ivec2 mx = textureSize(uTex, 0) - 1;
    return getY(texelFetch(uTex, clamp(c, ivec2(0), mx), 0).rgb);
}

vec4 GetEdgeMap(float p[36], int i, int j) {
    float g_0 = abs(P(0 + i, 0 + j) + P(0 + i, 1 + j) + P(0 + i, 2 + j) - P(2 + i, 0 + j) - P(2 + i, 1 + j) - P(2 + i, 2 + j));
    float g_45 = abs(P(1 + i, 0 + j) + P(0 + i, 0 + j) + P(0 + i, 1 + j) - P(2 + i, 1 + j) - P(2 + i, 2 + j) - P(1 + i, 2 + j));
    float g_90 = abs(P(0 + i, 0 + j) + P(1 + i, 0 + j) + P(2 + i, 0 + j) - P(0 + i, 2 + j) - P(1 + i, 2 + j) - P(2 + i, 2 + j));
    float g_135 = abs(P(1 + i, 0 + j) + P(2 + i, 0 + j) + P(2 + i, 1 + j) - P(0 + i, 1 + j) - P(0 + i, 2 + j) - P(1 + i, 2 + j));
    float g_0_90_max = max(g_0, g_90);
    float g_0_90_min = min(g_0, g_90);
    float g_45_135_max = max(g_45, g_135);
    float g_45_135_min = min(g_45, g_135);
    if (g_0_90_max + g_45_135_max == 0.0) return vec4(0.0);
    float e_0_90 = min(g_0_90_max / (g_0_90_max + g_45_135_max), 1.0);
    float e_45_135 = 1.0 - e_0_90;
    bool c_0_90 = (g_0_90_max > (g_0_90_min * uDetect.x)) && (g_0_90_max > uDetect.y) && (g_0_90_max > g_45_135_min);
    bool c_45_135 = (g_45_135_max > (g_45_135_min * uDetect.x)) && (g_45_135_max > uDetect.y) && (g_45_135_max > g_0_90_min);
    bool c_g_0_90 = g_0_90_max == g_0;
    bool c_g_45_135 = g_45_135_max == g_45;
    float f_e_0_90 = (c_0_90 && c_45_135) ? e_0_90 : 1.0;
    float f_e_45_135 = (c_0_90 && c_45_135) ? e_45_135 : 1.0;
    float weight_0 = (c_0_90 && c_g_0_90) ? f_e_0_90 : 0.0;
    float weight_90 = (c_0_90 && !c_g_0_90) ? f_e_0_90 : 0.0;
    float weight_45 = (c_45_135 && c_g_45_135) ? f_e_45_135 : 0.0;
    float weight_135 = (c_45_135 && !c_g_45_135) ? f_e_45_135 : 0.0;
    return vec4(weight_0, weight_90, weight_45, weight_135);
}

float CalcLTI(float p0, float p1, float p2, float p3, float p4, float p5, int phase_index) {
    bool selector = (phase_index <= kPhaseCount / 2);
    float sel = selector ? p0 : p3;
    float a_min = min(min(p1, p2), sel);
    float a_max = max(max(p1, p2), sel);
    sel = selector ? p2 : p5;
    float b_min = min(min(p3, p4), sel);
    float b_max = max(max(p3, p4), sel);
    float a_cont = a_max - a_min;
    float b_cont = b_max - b_min;
    float cont_ratio = max(a_cont, b_cont) / (min(a_cont, b_cont) + uSharpA.y);
    return (1.0 - clamp((cont_ratio - uDetect.z) * uDetect.w, 0.0, 1.0)) * uSharpA.x;
}

float EvalPoly6(float pxl[6], int phase_int) {
    float y = 0.0;
    for (int i = 0; i < 6; ++i) y += coefScaler(phase_int, i) * pxl[i];
    float y_usm = 0.0;
    for (int i = 0; i < 6; ++i) y_usm += coefUSM(phase_int, i) * pxl[i];
    float y_scale = 1.0 - clamp((y - uSharpA.z) * uSharpA.w, 0.0, 1.0);
    float y_sharpness = y_scale * uSharpB.y + uSharpB.x;
    y_usm *= y_sharpness;
    float y_sharpness_limit = (y_scale * uSharpB.w + uSharpB.z) * y;
    y_usm = min(y_sharpness_limit, max(-y_sharpness_limit, y_usm));
    y_usm *= CalcLTI(pxl[0], pxl[1], pxl[2], pxl[3], pxl[4], pxl[5], phase_int);
    return y + y_usm;
}

float FilterNormal(float p[36], int phase_x_frac_int, int phase_y_frac_int) {
    float h_acc = 0.0;
    for (int j = 0; j < 6; ++j) {
        float v_acc = 0.0;
        for (int i = 0; i < 6; ++i) v_acc += P(i, j) * coefScaler(phase_y_frac_int, i);
        h_acc += v_acc * coefScaler(phase_x_frac_int, j);
    }
    return h_acc;
}

float AddDirFilters(float p[36], float phase_x_frac, float phase_y_frac, int phase_x_frac_int,
                    int phase_y_frac_int, vec4 w) {
    float f = 0.0;
    if (w.x > 0.0) {
        float interp0Deg[6];
        for (int i = 0; i < 6; ++i) interp0Deg[i] = mix(P(i, 2), P(i, 3), phase_x_frac);
        f += EvalPoly6(interp0Deg, phase_y_frac_int) * w.x;
    }
    if (w.y > 0.0) {
        float interp90Deg[6];
        for (int i = 0; i < 6; ++i) interp90Deg[i] = mix(P(2, i), P(3, i), phase_y_frac);
        f += EvalPoly6(interp90Deg, phase_x_frac_int) * w.y;
    }
    if (w.z > 0.0) {
        float pphase_b45 = 0.5 + 0.5 * (phase_x_frac - phase_y_frac);
        float temp_interp45Deg[7];
        temp_interp45Deg[1] = mix(P(2, 1), P(1, 2), pphase_b45);
        temp_interp45Deg[3] = mix(P(3, 2), P(2, 3), pphase_b45);
        temp_interp45Deg[5] = mix(P(4, 3), P(3, 4), pphase_b45);
        pphase_b45 = pphase_b45 - 0.5;
        float a = (pphase_b45 >= 0.0) ? P(0, 2) : P(2, 0);
        float b = (pphase_b45 >= 0.0) ? P(1, 3) : P(3, 1);
        float c = (pphase_b45 >= 0.0) ? P(2, 4) : P(4, 2);
        float d = (pphase_b45 >= 0.0) ? P(3, 5) : P(5, 3);
        temp_interp45Deg[0] = mix(P(1, 1), a, abs(pphase_b45));
        temp_interp45Deg[2] = mix(P(2, 2), b, abs(pphase_b45));
        temp_interp45Deg[4] = mix(P(3, 3), c, abs(pphase_b45));
        temp_interp45Deg[6] = mix(P(4, 4), d, abs(pphase_b45));
        float interp45Deg[6];
        float pphase_p45 = phase_x_frac + phase_y_frac;
        if (pphase_p45 >= 1.0) {
            for (int i = 0; i < 6; i++) interp45Deg[i] = temp_interp45Deg[i + 1];
            pphase_p45 = pphase_p45 - 1.0;
        } else {
            for (int i = 0; i < 6; i++) interp45Deg[i] = temp_interp45Deg[i];
        }
        f += EvalPoly6(interp45Deg, min(int(pphase_p45 * 64.0), 63)) * w.z;
    }
    if (w.w > 0.0) {
        float pphase_b135 = 0.5 * (phase_x_frac + phase_y_frac);
        float temp_interp135Deg[7];
        temp_interp135Deg[1] = mix(P(3, 1), P(4, 2), pphase_b135);
        temp_interp135Deg[3] = mix(P(2, 2), P(3, 3), pphase_b135);
        temp_interp135Deg[5] = mix(P(1, 3), P(2, 4), pphase_b135);
        pphase_b135 = pphase_b135 - 0.5;
        float a = (pphase_b135 >= 0.0) ? P(5, 2) : P(3, 0);
        float b = (pphase_b135 >= 0.0) ? P(4, 3) : P(2, 1);
        float c = (pphase_b135 >= 0.0) ? P(3, 4) : P(1, 2);
        float d = (pphase_b135 >= 0.0) ? P(2, 5) : P(0, 3);
        temp_interp135Deg[0] = mix(P(4, 1), a, abs(pphase_b135));
        temp_interp135Deg[2] = mix(P(3, 2), b, abs(pphase_b135));
        temp_interp135Deg[4] = mix(P(2, 3), c, abs(pphase_b135));
        temp_interp135Deg[6] = mix(P(1, 4), d, abs(pphase_b135));
        float interp135Deg[6];
        float pphase_p135 = 1.0 + (phase_x_frac - phase_y_frac);
        if (pphase_p135 >= 1.0) {
            for (int i = 0; i < 6; ++i) interp135Deg[i] = temp_interp135Deg[i + 1];
            pphase_p135 = pphase_p135 - 1.0;
        } else {
            for (int i = 0; i < 6; ++i) interp135Deg[i] = temp_interp135Deg[i];
        }
        f += EvalPoly6(interp135Deg, min(int(pphase_p135 * 64.0), 63)) * w.w;
    }
    return f;
}

void main() {
    vec2 dst = floor(vUv * uOut);
    vec2 src = (dst + 0.5) * uScale.xy - 0.5;
    vec2 fsrc = floor(src);
    vec2 fr = src - fsrc;
    ivec2 fi = min(ivec2(fr * float(kPhaseCount)), ivec2(kPhaseCount - 1));
    ivec2 s = ivec2(fsrc);

    float p[36];
    for (int i = 0; i < 6; ++i)
        for (int j = 0; j < 6; ++j) P(i, j) = lumaAt(s + ivec2(j - 2, i - 2));

    vec4 e00 = GetEdgeMap(p, 1, 1);
    vec4 e01 = GetEdgeMap(p, 1, 2);
    vec4 e10 = GetEdgeMap(p, 2, 1);
    vec4 e11 = GetEdgeMap(p, 2, 2);
    vec4 h0 = mix(e00, e01, fr.x);
    vec4 h1 = mix(e10, e11, fr.x);
    vec4 w = mix(h0, h1, fr.y);

    float baseWeight = 1.0 - w.x - w.y - w.z - w.w;
    float opY = FilterNormal(p, fi.x, fi.y) * baseWeight;
    opY += AddDirFilters(p, fr.x, fr.y, fi.x, fi.y, w);

    vec2 coord = (src + 0.5) * uScale.zw;
    vec4 op = texture(uTex, coord);
    float y = getY(op.rgb);
    float corr = opY - y;
    op.rgb += vec3(corr);
    oColor = vec4(clamp(op.rgb, 0.0, 1.0), 1.0);
}
`;

// NIS filter banks — coef_scale and coef_usm from NIS_Config.h (SDK 1.0.3),
// 64 phases × 6 taps each (the SDK pads rows to 8 with zeros; those are
// dropped here and re-added when the texture is packed).
// prettier-ignore
const NIS_COEF_SCALE = [
    0.0,0.0,1.0000,0.0,0.0,0.0, 0.0029,-0.0127,1.0000,0.0132,-0.0034,0.0, 0.0063,-0.0249,0.9985,0.0269,-0.0068,0.0,
    0.0088,-0.0361,0.9956,0.0415,-0.0103,0.0005, 0.0117,-0.0474,0.9932,0.0562,-0.0142,0.0005, 0.0142,-0.0576,0.9897,0.0713,-0.0181,0.0005,
    0.0166,-0.0674,0.9844,0.0874,-0.0220,0.0010, 0.0186,-0.0762,0.9785,0.1040,-0.0264,0.0015, 0.0205,-0.0850,0.9727,0.1206,-0.0308,0.0020,
    0.0225,-0.0928,0.9648,0.1382,-0.0352,0.0024, 0.0239,-0.1006,0.9575,0.1558,-0.0396,0.0029, 0.0254,-0.1074,0.9487,0.1738,-0.0439,0.0034,
    0.0264,-0.1138,0.9390,0.1929,-0.0488,0.0044, 0.0278,-0.1191,0.9282,0.2119,-0.0537,0.0049, 0.0288,-0.1245,0.9170,0.2310,-0.0581,0.0059,
    0.0293,-0.1294,0.9058,0.2510,-0.0630,0.0063, 0.0303,-0.1333,0.8926,0.2710,-0.0679,0.0073, 0.0308,-0.1367,0.8789,0.2915,-0.0728,0.0083,
    0.0308,-0.1401,0.8657,0.3120,-0.0776,0.0093, 0.0313,-0.1426,0.8506,0.3330,-0.0825,0.0103, 0.0313,-0.1445,0.8354,0.3540,-0.0874,0.0112,
    0.0313,-0.1460,0.8193,0.3755,-0.0923,0.0122, 0.0313,-0.1470,0.8022,0.3965,-0.0967,0.0137, 0.0308,-0.1479,0.7856,0.4185,-0.1016,0.0146,
    0.0303,-0.1479,0.7681,0.4399,-0.1060,0.0156, 0.0298,-0.1479,0.7505,0.4614,-0.1104,0.0166, 0.0293,-0.1470,0.7314,0.4829,-0.1147,0.0181,
    0.0288,-0.1460,0.7119,0.5049,-0.1187,0.0190, 0.0278,-0.1445,0.6929,0.5264,-0.1226,0.0200, 0.0273,-0.1431,0.6724,0.5479,-0.1260,0.0215,
    0.0264,-0.1411,0.6528,0.5693,-0.1299,0.0225, 0.0254,-0.1387,0.6323,0.5903,-0.1328,0.0234, 0.0244,-0.1357,0.6113,0.6113,-0.1357,0.0244,
    0.0234,-0.1328,0.5903,0.6323,-0.1387,0.0254, 0.0225,-0.1299,0.5693,0.6528,-0.1411,0.0264, 0.0215,-0.1260,0.5479,0.6724,-0.1431,0.0273,
    0.0200,-0.1226,0.5264,0.6929,-0.1445,0.0278, 0.0190,-0.1187,0.5049,0.7119,-0.1460,0.0288, 0.0181,-0.1147,0.4829,0.7314,-0.1470,0.0293,
    0.0166,-0.1104,0.4614,0.7505,-0.1479,0.0298, 0.0156,-0.1060,0.4399,0.7681,-0.1479,0.0303, 0.0146,-0.1016,0.4185,0.7856,-0.1479,0.0308,
    0.0137,-0.0967,0.3965,0.8022,-0.1470,0.0313, 0.0122,-0.0923,0.3755,0.8193,-0.1460,0.0313, 0.0112,-0.0874,0.3540,0.8354,-0.1445,0.0313,
    0.0103,-0.0825,0.3330,0.8506,-0.1426,0.0313, 0.0093,-0.0776,0.3120,0.8657,-0.1401,0.0308, 0.0083,-0.0728,0.2915,0.8789,-0.1367,0.0308,
    0.0073,-0.0679,0.2710,0.8926,-0.1333,0.0303, 0.0063,-0.0630,0.2510,0.9058,-0.1294,0.0293, 0.0059,-0.0581,0.2310,0.9170,-0.1245,0.0288,
    0.0049,-0.0537,0.2119,0.9282,-0.1191,0.0278, 0.0044,-0.0488,0.1929,0.9390,-0.1138,0.0264, 0.0034,-0.0439,0.1738,0.9487,-0.1074,0.0254,
    0.0029,-0.0396,0.1558,0.9575,-0.1006,0.0239, 0.0024,-0.0352,0.1382,0.9648,-0.0928,0.0225, 0.0020,-0.0308,0.1206,0.9727,-0.0850,0.0205,
    0.0015,-0.0264,0.1040,0.9785,-0.0762,0.0186, 0.0010,-0.0220,0.0874,0.9844,-0.0674,0.0166, 0.0005,-0.0181,0.0713,0.9897,-0.0576,0.0142,
    0.0005,-0.0142,0.0562,0.9932,-0.0474,0.0117, 0.0005,-0.0103,0.0415,0.9956,-0.0361,0.0088, 0.0,-0.0068,0.0269,0.9985,-0.0249,0.0063,
    0.0,-0.0034,0.0132,1.0000,-0.0127,0.0029,
];
// prettier-ignore
const NIS_COEF_USM = [
    0.0,-0.6001,1.2002,-0.6001,0.0,0.0, 0.0029,-0.6084,1.1987,-0.5903,-0.0029,0.0, 0.0049,-0.6147,1.1958,-0.5791,-0.0068,0.0005,
    0.0073,-0.6196,1.1890,-0.5659,-0.0103,0.0, 0.0093,-0.6235,1.1802,-0.5513,-0.0151,0.0, 0.0112,-0.6265,1.1699,-0.5352,-0.0195,0.0005,
    0.0122,-0.6270,1.1582,-0.5181,-0.0259,0.0005, 0.0142,-0.6284,1.1455,-0.5005,-0.0317,0.0005, 0.0156,-0.6265,1.1274,-0.4790,-0.0386,0.0005,
    0.0166,-0.6235,1.1089,-0.4570,-0.0454,0.0010, 0.0176,-0.6187,1.0879,-0.4346,-0.0532,0.0010, 0.0181,-0.6138,1.0659,-0.4102,-0.0615,0.0015,
    0.0190,-0.6069,1.0405,-0.3843,-0.0698,0.0015, 0.0195,-0.6006,1.0161,-0.3574,-0.0796,0.0020, 0.0200,-0.5928,0.9893,-0.3286,-0.0898,0.0024,
    0.0200,-0.5820,0.9580,-0.2988,-0.1001,0.0029, 0.0200,-0.5728,0.9292,-0.2690,-0.1104,0.0034, 0.0200,-0.5620,0.8975,-0.2368,-0.1226,0.0039,
    0.0205,-0.5498,0.8643,-0.2046,-0.1343,0.0044, 0.0200,-0.5371,0.8301,-0.1709,-0.1465,0.0049, 0.0195,-0.5239,0.7944,-0.1367,-0.1587,0.0054,
    0.0195,-0.5107,0.7598,-0.1021,-0.1724,0.0059, 0.0190,-0.4966,0.7231,-0.0649,-0.1865,0.0063, 0.0186,-0.4819,0.6846,-0.0288,-0.1997,0.0068,
    0.0186,-0.4668,0.6460,0.0093,-0.2144,0.0073, 0.0176,-0.4507,0.6055,0.0479,-0.2290,0.0083, 0.0171,-0.4370,0.5693,0.0859,-0.2446,0.0088,
    0.0161,-0.4199,0.5283,0.1255,-0.2598,0.0098, 0.0161,-0.4048,0.4883,0.1655,-0.2754,0.0103, 0.0151,-0.3887,0.4497,0.2041,-0.2910,0.0107,
    0.0142,-0.3711,0.4072,0.2446,-0.3066,0.0117, 0.0137,-0.3555,0.3672,0.2852,-0.3228,0.0122, 0.0132,-0.3394,0.3262,0.3262,-0.3394,0.0132,
    0.0122,-0.3228,0.2852,0.3672,-0.3555,0.0137, 0.0117,-0.3066,0.2446,0.4072,-0.3711,0.0142, 0.0107,-0.2910,0.2041,0.4497,-0.3887,0.0151,
    0.0103,-0.2754,0.1655,0.4883,-0.4048,0.0161, 0.0098,-0.2598,0.1255,0.5283,-0.4199,0.0161, 0.0088,-0.2446,0.0859,0.5693,-0.4370,0.0171,
    0.0083,-0.2290,0.0479,0.6055,-0.4507,0.0176, 0.0073,-0.2144,0.0093,0.6460,-0.4668,0.0186, 0.0068,-0.1997,-0.0288,0.6846,-0.4819,0.0186,
    0.0063,-0.1865,-0.0649,0.7231,-0.4966,0.0190, 0.0059,-0.1724,-0.1021,0.7598,-0.5107,0.0195, 0.0054,-0.1587,-0.1367,0.7944,-0.5239,0.0195,
    0.0049,-0.1465,-0.1709,0.8301,-0.5371,0.0200, 0.0044,-0.1343,-0.2046,0.8643,-0.5498,0.0205, 0.0039,-0.1226,-0.2368,0.8975,-0.5620,0.0200,
    0.0034,-0.1104,-0.2690,0.9292,-0.5728,0.0200, 0.0029,-0.1001,-0.2988,0.9580,-0.5820,0.0200, 0.0024,-0.0898,-0.3286,0.9893,-0.5928,0.0200,
    0.0020,-0.0796,-0.3574,1.0161,-0.6006,0.0195, 0.0015,-0.0698,-0.3843,1.0405,-0.6069,0.0190, 0.0015,-0.0615,-0.4102,1.0659,-0.6138,0.0181,
    0.0010,-0.0532,-0.4346,1.0879,-0.6187,0.0176, 0.0010,-0.0454,-0.4570,1.1089,-0.6235,0.0166, 0.0005,-0.0386,-0.4790,1.1274,-0.6265,0.0156,
    0.0005,-0.0317,-0.5005,1.1455,-0.6284,0.0142, 0.0005,-0.0259,-0.5181,1.1582,-0.6270,0.0122, 0.0005,-0.0195,-0.5352,1.1699,-0.6265,0.0112,
    0.0,-0.0151,-0.5513,1.1802,-0.6235,0.0093, 0.0,-0.0103,-0.5659,1.1890,-0.6196,0.0073, 0.0005,-0.0068,-0.5791,1.1958,-0.6147,0.0049,
    0.0,-0.0029,-0.5903,1.1987,-0.6084,0.0029,
];

/**
 * NVScalerUpdateConfig from NIS_Config.h, SDR branch. `sharpness` is the
 * SDK's 0..1 slider (0.5 = neutral); the derived constants are what the
 * shader's uniforms carry.
 */
export function nisConfig(sharpness) {
    const s = Math.max(0, Math.min(1, sharpness));
    const slider = s - 0.5;
    const maxScale = slider >= 0 ? 1.25 : 1.75;
    const minScale = slider >= 0 ? 1.25 : 1.0;
    const limitScale = slider >= 0 ? 1.25 : 1.0;
    const kDetectRatio = (2 * 1127) / 1024;
    const kDetectThres = 64 / 1024;
    const kMinContrastRatio = 2.0;
    const kMaxContrastRatio = 10.0;
    const kSharpStartY = 0.45;
    const kSharpEndY = 0.9;
    const kSharpStrengthMin = Math.max(0, 0.4 + slider * minScale * 1.2);
    const kSharpStrengthMax = 1.6 + slider * maxScale * 1.8;
    const kSharpLimitMin = Math.max(0.1, 0.14 + slider * limitScale * 0.32);
    const kSharpLimitMax = 0.5 + slider * limitScale * 0.6;
    return {
        detect: [
            kDetectRatio,
            kDetectThres,
            kMinContrastRatio,
            1 / (kMaxContrastRatio - kMinContrastRatio),
        ],
        sharpA: [1.0, 1 / 255, kSharpStartY, 1 / (kSharpEndY - kSharpStartY)],
        sharpB: [
            kSharpStrengthMin,
            kSharpStrengthMax - kSharpStrengthMin,
            kSharpLimitMin,
            kSharpLimitMax - kSharpLimitMin,
        ],
    };
}

/** The 4×64 RGBA32F filter-bank texture, packed as the shader reads it. */
export function nisCoefTexture() {
    const data = new Float32Array(4 * 64 * 4);
    for (let phase = 0; phase < 64; phase++) {
        const row = phase * 16;
        for (let i = 0; i < 6; i++) {
            data[row + i] = NIS_COEF_SCALE[phase * 6 + i]; // texels 0-1
            data[row + 8 + i] = NIS_COEF_USM[phase * 6 + i]; // texels 2-3
        }
    }
    return data;
}

export const WEBGL_ALGOS = ['gl-fsr1', 'gl-sgsr', 'gl-nis'];

export class WebGlRenderer extends VideoRenderer {
    constructor() {
        super();
        this.videoCodec = '';
        /** @type {'gl-fsr1'|'gl-sgsr'|'gl-nis'} */
        this._algo = 'gl-fsr1';
        this.gl = null;
        this._outW = 0;
        this._outH = 0;
        this._hasOutputSize = false;
        this._inW = 0;
        this._inH = 0;
        this._rendered = 0;
        this._lost = false;
        this._uploadWarned = false;
        this._nisScaleWarned = false;
        this._nisSharpness = 0.5;
        this._knobsReadMs = 0;
    }

    /**
     * @param {HTMLCanvasElement|OffscreenCanvas} canvas
     * @param {{videoCodec: string, algo: string, desynchronized?: boolean}} opts
     */
    static async create(canvas, opts) {
        const r = new WebGlRenderer();
        r.canvas = canvas;
        r.videoCodec = opts.videoCodec;
        r._algo = WEBGL_ALGOS.includes(opts.algo) ? opts.algo : 'gl-fsr1';
        // desynchronized is the whole point (see the file comment); the other
        // flags remove work the stream never needs: no depth, no blending with
        // the page, no MSAA, no readback of the previous frame.
        const gl = canvas.getContext('webgl2', {
            desynchronized: opts.desynchronized !== false,
            alpha: false,
            antialias: false,
            depth: false,
            stencil: false,
            premultipliedAlpha: false,
            preserveDrawingBuffer: false,
            powerPreference: 'high-performance',
        });
        if (!gl) throw new Error('WebGL2 unavailable');
        r.gl = gl;
        canvas.addEventListener?.('webglcontextlost', (e) => {
            e.preventDefault();
            r._lost = true;
            console.warn('[WebGlRenderer] context lost');
        });
        canvas.addEventListener?.('webglcontextrestored', () => {
            r._lost = false;
            r._buildResources();
            console.log('[WebGlRenderer] context restored');
        });
        r._buildResources();
        canvas.width = 1920;
        canvas.height = 1080;
        console.log('[WebGlRenderer] created algo=' + r._algo);
        return r;
    }

    get kind() {
        return 'webgl';
    }

    /** Effective algorithm name for the overlay. */
    get algoName() {
        if (this._algo === 'gl-nis') return 'WebGL NIS';
        if (this._algo === 'gl-sgsr') return 'WebGL SGSR';
        return 'WebGL FSR1';
    }

    isContextLost() {
        return this._lost || (this.gl ? this.gl.isContextLost() : false);
    }

    setOutputSize(width, height) {
        if (width <= 0 || height <= 0) return;
        this._outW = width;
        this._outH = height;
        this._hasOutputSize = true;
    }

    _compile(type, src) {
        const gl = this.gl;
        const sh = gl.createShader(type);
        gl.shaderSource(sh, src);
        gl.compileShader(sh);
        if (!gl.getShaderParameter(sh, gl.COMPILE_STATUS)) {
            const log = gl.getShaderInfoLog(sh);
            gl.deleteShader(sh);
            throw new Error('shader compile failed: ' + log);
        }
        return sh;
    }

    _program(fsSrc, uniforms) {
        const gl = this.gl;
        const prog = gl.createProgram();
        const vs = this._compile(gl.VERTEX_SHADER, VS);
        const fs = this._compile(gl.FRAGMENT_SHADER, fsSrc);
        gl.attachShader(prog, vs);
        gl.attachShader(prog, fs);
        gl.linkProgram(prog);
        gl.deleteShader(vs);
        gl.deleteShader(fs);
        if (!gl.getProgramParameter(prog, gl.LINK_STATUS)) {
            const log = gl.getProgramInfoLog(prog);
            gl.deleteProgram(prog);
            throw new Error('program link failed: ' + log);
        }
        const u = {};
        for (const name of uniforms) u[name] = gl.getUniformLocation(prog, name);
        return { prog, u };
    }

    _texture(filter) {
        const gl = this.gl;
        const tex = gl.createTexture();
        gl.bindTexture(gl.TEXTURE_2D, tex);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, filter);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, filter);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
        return tex;
    }

    _buildResources() {
        const gl = this.gl;
        // A VAO with nothing in it: the vertex shader reads gl_VertexID only.
        this._vao = gl.createVertexArray();
        gl.bindVertexArray(this._vao);

        this._inputTex = this._texture(gl.LINEAR);
        this._inW = 0;
        this._inH = 0;

        if (this._algo === 'gl-fsr1') {
            this._easu = this._program(EASU_FS, ['uTex', 'uRes']);
            this._rcas = this._program(RCAS_FS, ['uTex', 'uRes']);
            this._intermTex = this._texture(gl.LINEAR);
            this._intermW = 0;
            this._intermH = 0;
            this._fbo = gl.createFramebuffer();
        } else if (this._algo === 'gl-sgsr') {
            this._sgsr = this._program(SGSR_FS, ['uTex', 'uView']);
        } else {
            this._nis = this._program(NIS_FS, [
                'uTex',
                'uCoef',
                'uScale',
                'uDetect',
                'uSharpA',
                'uSharpB',
                'uOut',
            ]);
            this._coefTex = this._texture(gl.NEAREST);
            gl.bindTexture(gl.TEXTURE_2D, this._coefTex);
            gl.texImage2D(
                gl.TEXTURE_2D,
                0,
                gl.RGBA32F,
                4,
                64,
                0,
                gl.RGBA,
                gl.FLOAT,
                nisCoefTexture(),
            );
        }
        gl.disable(gl.DEPTH_TEST);
        gl.disable(gl.BLEND);
        gl.disable(gl.SCISSOR_TEST);
    }

    // NIS sharpness, live from the console (`localStorage.mw_nis_sharpness`,
    // 0..1, 0.5 = the SDK's neutral), re-read at most twice a second. Only on
    // the main thread — the worker has no localStorage and keeps the default.
    _readKnobs() {
        if (typeof localStorage === 'undefined') return;
        const now = performance.now();
        if (now - this._knobsReadMs < 500) return;
        this._knobsReadMs = now;
        try {
            const v = parseFloat(localStorage.getItem('mw_nis_sharpness'));
            this._nisSharpness = v >= 0 && v <= 1 ? v : 0.5;
        } catch (e) {}
    }

    _ensureInput(inW, inH) {
        const gl = this.gl;
        gl.bindTexture(gl.TEXTURE_2D, this._inputTex);
        if (this._inW === inW && this._inH === inH) return;
        gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, inW, inH, 0, gl.RGBA, gl.UNSIGNED_BYTE, null);
        this._inW = inW;
        this._inH = inH;
    }

    _ensureInterm(w, h) {
        const gl = this.gl;
        if (this._intermW === w && this._intermH === h) return;
        gl.bindTexture(gl.TEXTURE_2D, this._intermTex);
        gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, w, h, 0, gl.RGBA, gl.UNSIGNED_BYTE, null);
        gl.bindFramebuffer(gl.FRAMEBUFFER, this._fbo);
        gl.framebufferTexture2D(
            gl.FRAMEBUFFER,
            gl.COLOR_ATTACHMENT0,
            gl.TEXTURE_2D,
            this._intermTex,
            0,
        );
        gl.bindFramebuffer(gl.FRAMEBUFFER, null);
        this._intermW = w;
        this._intermH = h;
    }

    async draw(frame) {
        const gl = this.gl;
        if (!this.canvas || !gl || this.isContextLost()) {
            try {
                frame.close();
            } catch (e) {}
            return;
        }
        const drawStart = performance.now();
        let waitMs = 0;

        const inW = frame.displayWidth || frame.codedWidth || 0;
        const inH = frame.displayHeight || frame.codedHeight || 0;
        if (inW <= 0 || inH <= 0) {
            try {
                frame.close();
            } catch (e) {}
            return;
        }

        // Canvas backing = frame-aspect rect fitting the output box (same rule
        // as WebGpuRenderer), else the frame's own size.
        let cw = inW,
            ch = inH;
        if (this._hasOutputSize) {
            const frameAspect = inW / inH;
            const boxAspect = this._outW / this._outH;
            cw = frameAspect >= boxAspect ? this._outW : Math.round(this._outH * frameAspect);
            ch = Math.round(cw / frameAspect);
            const MAX_DIM = 4096;
            if (cw > MAX_DIM || ch > MAX_DIM) {
                const k = MAX_DIM / Math.max(cw, ch);
                cw = Math.round(cw * k);
                ch = Math.round(ch * k);
            }
        }
        if (cw > 0 && ch > 0 && (this.canvas.width !== cw || this.canvas.height !== ch)) {
            this.canvas.width = cw;
            this.canvas.height = ch;
        }

        // ── Upload: the VideoFrame is a TexImageSource in Chromium ─────────
        // texSubImage2D into a texture allocated once per size. On a browser
        // that refuses the VideoFrame, go through an ImageBitmap (one extra
        // GPU copy, the same one Canvas2D's fallback pays).
        let path = 'frame';
        try {
            this._ensureInput(inW, inH);
            try {
                gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, gl.RGBA, gl.UNSIGNED_BYTE, frame);
            } catch (e) {
                path = 'bitmap';
                const t0 = performance.now();
                const bmp = await createImageBitmap(frame);
                waitMs += performance.now() - t0;
                gl.bindTexture(gl.TEXTURE_2D, this._inputTex);
                gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, gl.RGBA, gl.UNSIGNED_BYTE, bmp);
                bmp.close();
                if (!this._uploadWarned) {
                    this._uploadWarned = true;
                    console.warn(
                        '[WebGlRenderer] VideoFrame upload refused, using ImageBitmap: ' +
                            e.message,
                    );
                }
            }
        } catch (e) {
            console.error('[WebGlRenderer] upload failed: ' + e.message);
            frame.close();
            this._noteDraw(drawStart, waitMs, 'failed');
            return;
        }
        frame.close();

        try {
            gl.bindVertexArray(this._vao);
            if (this._algo === 'gl-fsr1') {
                this._ensureInterm(cw, ch);
                // Pass 1: EASU input → interm (output res).
                gl.bindFramebuffer(gl.FRAMEBUFFER, this._fbo);
                gl.viewport(0, 0, cw, ch);
                gl.useProgram(this._easu.prog);
                gl.activeTexture(gl.TEXTURE0);
                gl.bindTexture(gl.TEXTURE_2D, this._inputTex);
                gl.uniform1i(this._easu.u.uTex, 0);
                gl.uniform4f(this._easu.u.uRes, inW, inH, cw, ch);
                gl.drawArrays(gl.TRIANGLES, 0, 3);
                // Pass 2: RCAS interm → canvas.
                gl.bindFramebuffer(gl.FRAMEBUFFER, null);
                gl.viewport(0, 0, cw, ch);
                gl.useProgram(this._rcas.prog);
                gl.bindTexture(gl.TEXTURE_2D, this._intermTex);
                gl.uniform1i(this._rcas.u.uTex, 0);
                gl.uniform4f(this._rcas.u.uRes, cw, ch, 1 / cw, 1 / ch);
                gl.drawArrays(gl.TRIANGLES, 0, 3);
            } else if (this._algo === 'gl-sgsr') {
                gl.bindFramebuffer(gl.FRAMEBUFFER, null);
                gl.viewport(0, 0, cw, ch);
                gl.useProgram(this._sgsr.prog);
                gl.activeTexture(gl.TEXTURE0);
                gl.bindTexture(gl.TEXTURE_2D, this._inputTex);
                gl.uniform1i(this._sgsr.u.uTex, 0);
                gl.uniform4f(this._sgsr.u.uView, 1 / inW, 1 / inH, inW, inH);
                gl.drawArrays(gl.TRIANGLES, 0, 3);
            } else {
                // NIS is specified for scale factors in [0.5, 1] (up to 2× up).
                // Outside that it still runs — it just is not what NVIDIA
                // validated — so say so once rather than refuse the frame.
                const kScaleX = inW / cw,
                    kScaleY = inH / ch;
                if (
                    !this._nisScaleWarned &&
                    (kScaleX < 0.5 || kScaleX > 1 || kScaleY < 0.5 || kScaleY > 1)
                ) {
                    this._nisScaleWarned = true;
                    console.warn(
                        '[WebGlRenderer] NIS scale ' +
                            kScaleX.toFixed(3) +
                            '×' +
                            kScaleY.toFixed(3) +
                            ' is outside the SDK range [0.5, 1] (2× upscale max, no downscale)',
                    );
                }
                this._readKnobs();
                const cfg = nisConfig(this._nisSharpness);
                gl.bindFramebuffer(gl.FRAMEBUFFER, null);
                gl.viewport(0, 0, cw, ch);
                gl.useProgram(this._nis.prog);
                gl.activeTexture(gl.TEXTURE0);
                gl.bindTexture(gl.TEXTURE_2D, this._inputTex);
                gl.activeTexture(gl.TEXTURE1);
                gl.bindTexture(gl.TEXTURE_2D, this._coefTex);
                gl.uniform1i(this._nis.u.uTex, 0);
                gl.uniform1i(this._nis.u.uCoef, 1);
                gl.uniform4f(this._nis.u.uScale, kScaleX, kScaleY, 1 / inW, 1 / inH);
                gl.uniform4fv(this._nis.u.uDetect, cfg.detect);
                gl.uniform4fv(this._nis.u.uSharpA, cfg.sharpA);
                gl.uniform4fv(this._nis.u.uSharpB, cfg.sharpB);
                gl.uniform2f(this._nis.u.uOut, cw, ch);
                gl.drawArrays(gl.TRIANGLES, 0, 3);
                gl.activeTexture(gl.TEXTURE0);
            }
        } catch (e) {
            console.error('[WebGlRenderer] draw failed: ' + e.message);
            path = 'failed';
        }

        this._rendered++;
        this._noteDraw(drawStart, waitMs, this._algo + ':' + path);
    }

    /** Record the draw split for PipelineDiag (see VideoRenderer.lastDraw). */
    _noteDraw(startMs, waitMs, path) {
        const total = performance.now() - startMs;
        this.lastDraw.submitMs = Math.max(0, total - waitMs);
        this.lastDraw.waitMs = waitMs;
        this.lastDraw.path = path;
    }

    dispose() {
        const gl = this.gl;
        if (gl) {
            try {
                for (const t of [this._inputTex, this._intermTex, this._coefTex])
                    if (t) gl.deleteTexture(t);
                if (this._fbo) gl.deleteFramebuffer(this._fbo);
                if (this._vao) gl.deleteVertexArray(this._vao);
                for (const p of [this._easu, this._rcas, this._sgsr, this._nis])
                    if (p) gl.deleteProgram(p.prog);
                const ext = gl.getExtension('WEBGL_lose_context');
                if (ext) ext.loseContext();
            } catch (e) {}
        }
        this.gl = null;
        this.canvas = null;
    }
}
