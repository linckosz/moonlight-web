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
 * WebGlRenderer — the enhancers on WebGL2, offered next to their WebGPU twins
 * in the Video Enhancement menu (Performance / Balanced / Quality, each in a
 * WebGL2 and a WebGPU flavour).
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
 *   'gl-mwsr' — MoonlightWeb's own upscaler, the GLSL twin of the WGSL text
 *               (MwsrShader.js, docs/design/upscaler-mwsr.md §3.6): the same
 *               kernel and constants from the same tables, one program per
 *               regime, the one to run picked per frame by the frame and
 *               canvas sizes. Bilinear taps and screen derivatives only, no
 *               gather to emulate. The reduction averages in software light:
 *               a WebGL2 drawing buffer has no sRGB view.
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
import { nisConfig, nisCoefTexture, readNisSharpness, nisScaleInRange } from './NisTables.js';
import { fitOutputSize, isIdentityScale } from './scaleRegime.js';
import {
    mwsrClassOverrides,
    mwsrGlsl,
    mwsrRegime,
    mwsrRegimeKey,
    mwsrRegimes,
    mwsrUniforms,
} from './MwsrShader.js';
import { GazeZone } from '../GazeZone.js';
import { IS_HANDHELD } from '../../util/BrowserDetect.js';

// The kernel's constants the product sets for this device's class (plan §9,
// Bruno's decisions of 24/10/2026), under the laboratory's overrides.
const MWSR_CLASS = mwsrClassOverrides({ handheld: IS_HANDHELD });

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
// - 0.5; the 6×6 luma support pRC is the source pixel (floor(srcX) - 2 + C,
// floor(srcY) - 2 + R) — R the row, C the column, as in the SDK's p[R][C].
// The edge map the SDK stages per source pixel is GetEdgeMap of that pixel's
// 3×3 neighbourhood; the four entries an output pixel interpolates sit at
// p22..p33, i.e. the 3×3 blocks whose top left is p11, p12, p21 and p22.
//
// The filter banks (coef_scale, coef_usm: 64 phases × 6 taps) live in a 4×64
// RGBA32F texture: columns 0–1 hold the scaler taps, columns 2–3 the USM taps,
// four per texel. Written out, with no array or loop, as NIS_WGSL is: WebKit
// keeps an array indexed in a loop in memory, which made the WebGPU twin 5.6
// times slower on an iPhone. The arithmetic and its order are as before.
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

float getY(vec3 c) { return 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b; }
float lumaAt(ivec2 c, ivec2 mx) {
    return getY(texelFetch(uTex, clamp(c, ivec2(0), mx), 0).rgb);
}

// The edge map of the 3x3 block e00..e22, its rows then its columns.
vec4 GetEdgeMap(float e00, float e01, float e02, float e10, float e11, float e12, float e20, float e21, float e22) {
    float g_0 = abs(e00 + e01 + e02 - e20 - e21 - e22);
    float g_45 = abs(e10 + e00 + e01 - e21 - e22 - e12);
    float g_90 = abs(e00 + e10 + e20 - e02 - e12 - e22);
    float g_135 = abs(e10 + e20 + e21 - e01 - e02 - e12);
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

// The six taps across one direction: the scaler's bank (texels 0 and 1 of the
// phase's row) and the USM's (texels 2 and 3), each read once.
float EvalPoly6(float t0, float t1, float t2, float t3, float t4, float t5, int phase_int) {
    vec4 a = texelFetch(uCoef, ivec2(0, phase_int), 0);
    vec4 b = texelFetch(uCoef, ivec2(1, phase_int), 0);
    vec4 ua = texelFetch(uCoef, ivec2(2, phase_int), 0);
    vec4 ub = texelFetch(uCoef, ivec2(3, phase_int), 0);
    float y = 0.0;
    y += a.x * t0;
    y += a.y * t1;
    y += a.z * t2;
    y += a.w * t3;
    y += b.x * t4;
    y += b.y * t5;
    float y_usm = 0.0;
    y_usm += ua.x * t0;
    y_usm += ua.y * t1;
    y_usm += ua.z * t2;
    y_usm += ua.w * t3;
    y_usm += ub.x * t4;
    y_usm += ub.y * t5;
    float y_scale = 1.0 - clamp((y - uSharpA.z) * uSharpA.w, 0.0, 1.0);
    float y_sharpness = y_scale * uSharpB.y + uSharpB.x;
    y_usm *= y_sharpness;
    float y_sharpness_limit = (y_scale * uSharpB.w + uSharpB.z) * y;
    y_usm = min(y_sharpness_limit, max(-y_sharpness_limit, y_usm));
    y_usm *= CalcLTI(t0, t1, t2, t3, t4, t5, phase_int);
    return y + y_usm;
}

void main() {
    vec2 dst = floor(vUv * uOut);
    vec2 src = (dst + 0.5) * uScale.xy - 0.5;
    vec2 fsrc = floor(src);
    vec2 fr = src - fsrc;
    ivec2 fi = min(ivec2(fr * float(kPhaseCount)), ivec2(kPhaseCount - 1));
    ivec2 s = ivec2(fsrc);
    ivec2 mx = textureSize(uTex, 0) - 1;

    // The 6x6 window of luma: pRC at row R and column C, from -2 to +3 around
    // the source texel.
    float p00 = lumaAt(s + ivec2(-2, -2), mx);
    float p01 = lumaAt(s + ivec2(-1, -2), mx);
    float p02 = lumaAt(s + ivec2(0, -2), mx);
    float p03 = lumaAt(s + ivec2(1, -2), mx);
    float p04 = lumaAt(s + ivec2(2, -2), mx);
    float p05 = lumaAt(s + ivec2(3, -2), mx);
    float p10 = lumaAt(s + ivec2(-2, -1), mx);
    float p11 = lumaAt(s + ivec2(-1, -1), mx);
    float p12 = lumaAt(s + ivec2(0, -1), mx);
    float p13 = lumaAt(s + ivec2(1, -1), mx);
    float p14 = lumaAt(s + ivec2(2, -1), mx);
    float p15 = lumaAt(s + ivec2(3, -1), mx);
    float p20 = lumaAt(s + ivec2(-2, 0), mx);
    float p21 = lumaAt(s + ivec2(-1, 0), mx);
    float p22 = lumaAt(s + ivec2(0, 0), mx);
    float p23 = lumaAt(s + ivec2(1, 0), mx);
    float p24 = lumaAt(s + ivec2(2, 0), mx);
    float p25 = lumaAt(s + ivec2(3, 0), mx);
    float p30 = lumaAt(s + ivec2(-2, 1), mx);
    float p31 = lumaAt(s + ivec2(-1, 1), mx);
    float p32 = lumaAt(s + ivec2(0, 1), mx);
    float p33 = lumaAt(s + ivec2(1, 1), mx);
    float p34 = lumaAt(s + ivec2(2, 1), mx);
    float p35 = lumaAt(s + ivec2(3, 1), mx);
    float p40 = lumaAt(s + ivec2(-2, 2), mx);
    float p41 = lumaAt(s + ivec2(-1, 2), mx);
    float p42 = lumaAt(s + ivec2(0, 2), mx);
    float p43 = lumaAt(s + ivec2(1, 2), mx);
    float p44 = lumaAt(s + ivec2(2, 2), mx);
    float p45 = lumaAt(s + ivec2(3, 2), mx);
    float p50 = lumaAt(s + ivec2(-2, 3), mx);
    float p51 = lumaAt(s + ivec2(-1, 3), mx);
    float p52 = lumaAt(s + ivec2(0, 3), mx);
    float p53 = lumaAt(s + ivec2(1, 3), mx);
    float p54 = lumaAt(s + ivec2(2, 3), mx);
    float p55 = lumaAt(s + ivec2(3, 3), mx);

    vec4 e00 = GetEdgeMap(p11, p12, p13, p21, p22, p23, p31, p32, p33);
    vec4 e01 = GetEdgeMap(p12, p13, p14, p22, p23, p24, p32, p33, p34);
    vec4 e10 = GetEdgeMap(p21, p22, p23, p31, p32, p33, p41, p42, p43);
    vec4 e11 = GetEdgeMap(p22, p23, p24, p32, p33, p34, p42, p43, p44);
    vec4 h0 = mix(e00, e01, fr.x);
    vec4 h1 = mix(e10, e11, fr.x);
    vec4 w = mix(h0, h1, fr.y);

    float baseWeight = 1.0 - w.x - w.y - w.z - w.w;

    // FilterNormal: each column down, then across.
    vec4 ya = texelFetch(uCoef, ivec2(0, fi.y), 0);
    vec4 yb = texelFetch(uCoef, ivec2(1, fi.y), 0);
    vec4 xa = texelFetch(uCoef, ivec2(0, fi.x), 0);
    vec4 xb = texelFetch(uCoef, ivec2(1, fi.x), 0);
    float h_acc = 0.0;
    float v_acc;
    v_acc = 0.0;
    v_acc += p00 * ya.x;
    v_acc += p10 * ya.y;
    v_acc += p20 * ya.z;
    v_acc += p30 * ya.w;
    v_acc += p40 * yb.x;
    v_acc += p50 * yb.y;
    h_acc += v_acc * xa.x;
    v_acc = 0.0;
    v_acc += p01 * ya.x;
    v_acc += p11 * ya.y;
    v_acc += p21 * ya.z;
    v_acc += p31 * ya.w;
    v_acc += p41 * yb.x;
    v_acc += p51 * yb.y;
    h_acc += v_acc * xa.y;
    v_acc = 0.0;
    v_acc += p02 * ya.x;
    v_acc += p12 * ya.y;
    v_acc += p22 * ya.z;
    v_acc += p32 * ya.w;
    v_acc += p42 * yb.x;
    v_acc += p52 * yb.y;
    h_acc += v_acc * xa.z;
    v_acc = 0.0;
    v_acc += p03 * ya.x;
    v_acc += p13 * ya.y;
    v_acc += p23 * ya.z;
    v_acc += p33 * ya.w;
    v_acc += p43 * yb.x;
    v_acc += p53 * yb.y;
    h_acc += v_acc * xa.w;
    v_acc = 0.0;
    v_acc += p04 * ya.x;
    v_acc += p14 * ya.y;
    v_acc += p24 * ya.z;
    v_acc += p34 * ya.w;
    v_acc += p44 * yb.x;
    v_acc += p54 * yb.y;
    h_acc += v_acc * xb.x;
    v_acc = 0.0;
    v_acc += p05 * ya.x;
    v_acc += p15 * ya.y;
    v_acc += p25 * ya.z;
    v_acc += p35 * ya.w;
    v_acc += p45 * yb.x;
    v_acc += p55 * yb.y;
    h_acc += v_acc * xb.y;
    float opY = h_acc * baseWeight;

    // AddDirFilters: the four directions the edge map weighs.
    float f = 0.0;
    if (w.x > 0.0) {
        f += EvalPoly6(mix(p02, p03, fr.x), mix(p12, p13, fr.x), mix(p22, p23, fr.x), mix(p32, p33, fr.x), mix(p42, p43, fr.x), mix(p52, p53, fr.x), fi.y) * w.x;
    }
    if (w.y > 0.0) {
        f += EvalPoly6(mix(p20, p30, fr.y), mix(p21, p31, fr.y), mix(p22, p32, fr.y), mix(p23, p33, fr.y), mix(p24, p34, fr.y), mix(p25, p35, fr.y), fi.x) * w.y;
    }
    if (w.z > 0.0) {
        float pphase_b45 = 0.5 + 0.5 * (fr.x - fr.y);
        float q1 = mix(p21, p12, pphase_b45);
        float q3 = mix(p32, p23, pphase_b45);
        float q5 = mix(p43, p34, pphase_b45);
        pphase_b45 = pphase_b45 - 0.5;
        bool pos = pphase_b45 >= 0.0;
        float a = pos ? p02 : p20;
        float b = pos ? p13 : p31;
        float c = pos ? p24 : p42;
        float d = pos ? p35 : p53;
        float q0 = mix(p11, a, abs(pphase_b45));
        float q2 = mix(p22, b, abs(pphase_b45));
        float q4 = mix(p33, c, abs(pphase_b45));
        float q6 = mix(p44, d, abs(pphase_b45));
        float pphase_p45 = fr.x + fr.y;
        bool up = pphase_p45 >= 1.0;
        if (up) pphase_p45 = pphase_p45 - 1.0;
        f += EvalPoly6(up ? q1 : q0, up ? q2 : q1, up ? q3 : q2, up ? q4 : q3, up ? q5 : q4, up ? q6 : q5, min(int(pphase_p45 * 64.0), 63)) * w.z;
    }
    if (w.w > 0.0) {
        float pphase_b135 = 0.5 * (fr.x + fr.y);
        float q1 = mix(p31, p42, pphase_b135);
        float q3 = mix(p22, p33, pphase_b135);
        float q5 = mix(p13, p24, pphase_b135);
        pphase_b135 = pphase_b135 - 0.5;
        bool pos = pphase_b135 >= 0.0;
        float a = pos ? p52 : p30;
        float b = pos ? p43 : p21;
        float c = pos ? p34 : p12;
        float d = pos ? p25 : p03;
        float q0 = mix(p41, a, abs(pphase_b135));
        float q2 = mix(p32, b, abs(pphase_b135));
        float q4 = mix(p23, c, abs(pphase_b135));
        float q6 = mix(p14, d, abs(pphase_b135));
        float pphase_p135 = 1.0 + (fr.x - fr.y);
        bool up = pphase_p135 >= 1.0;
        if (up) pphase_p135 = pphase_p135 - 1.0;
        f += EvalPoly6(up ? q1 : q0, up ? q2 : q1, up ? q3 : q2, up ? q4 : q3, up ? q5 : q4, up ? q6 : q5, min(int(pphase_p135 * 64.0), 63)) * w.w;
    }
    opY += f;

    vec2 coord = (src + 0.5) * uScale.zw;
    vec4 op = texture(uTex, coord);
    float y = getY(op.rgb);
    float corr = opY - y;
    op.rgb += vec3(corr);
    oColor = vec4(clamp(op.rgb, 0.0, 1.0), 1.0);
}
`;

// The enhancer ladder's last rung: the picture as it is, stretched by the
// sampler's bilinear filter. Reached through setAlgo — the menu's "no
// enhancer" is Canvas2D — so a session the ladder stepped down can climb back
// without changing renderer; and every rung's frame at 1:1, where there is
// nothing to reconstruct (upscaler-mwsr.md §5).
const BLIT_FS = `#version 300 es
precision mediump float;
uniform sampler2D uTex;
in vec2 vUv;
out vec4 oColor;
void main() {
    oColor = vec4(texture(uTex, vUv).rgb, 1.0);
}
`;

export const WEBGL_ALGOS = ['gl-fsr1', 'gl-sgsr', 'gl-nis', 'gl-mwsr'];

/**
 * A draw's fence is looked at once it is this old (takeGpuBehind): at the next
 * draw at 60 fps, as when the signal was calibrated (25/09/2026), two draws
 * later at 120 fps. In Chrome a fence's status reaches the page with a delay
 * of its own, and on a 120 Hz Radeon 780M (28/10/2026) that delay was often
 * longer than one frame: a plain copy, 0.3 ms of GPU work, read "behind" in
 * half the windows, and the ladder cut MWSR for a lag that was not there.
 */
const FENCE_MIN_AGE_MS = 12;
/** Fences waiting for that age, at most; past it the oldest goes unread. */
const FENCE_MAX_PENDING = 4;

export class WebGlRenderer extends VideoRenderer {
    constructor() {
        super();
        this.videoCodec = '';
        /** @type {string} one of WEBGL_ALGOS, or 'off' */
        this._algo = 'gl-fsr1';
        /** The last draw copied the frame 1:1 instead of running the pass. */
        this._passthrough = false;
        /** 4:4:4 stream: mwsr votes and sharpens per channel (CHROMA444). */
        this._yuv444 = false;
        /** @type {Map<string, {prog: WebGLProgram, u: Object}>|null} mwsr's programs, by regime. */
        this._mwsr = null;
        this._mwsrUni = null;
        this._mwsrUniKey = '';
        /** gl-mwsr's gaze zone (setMwsrZone): null, none; else its values over MWSR_ZONE's. */
        this._mwsrZone = null;
        /** Its weight pinned by setMwsrGaze (the lab), or null: the stream's motion decides. */
        this._mwsrGazePinned = null;
        /** @type {GazeZone|null} that motion, fed by noteEncodedFrame. */
        this._gaze = null;
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
        /** Click-to-photon probe: read the flag pixels after each draw. */
        this._probeActive = false;
        this._probePixels = null;
        this._probeDrawnAt = null;
        /**
         * GPU lag, see takeGpuBehind(): the fences set after the draws and not
         * looked at yet, oldest first ({sync, at}), and the draws counted since
         * the last read.
         */
        this._fences = [];
        this._gpuChecked = 0;
        this._gpuBehind = 0;
    }

    /**
     * Share of draws whose GPU work was still running when a later frame came
     * to be drawn, since the last call — or null when none could be judged.
     *
     * drawArrays only records commands, so the draw timing measures submission
     * and a pass that is too heavy for the GPU never shows there: it keeps the
     * frame rate while every picture is finished frames late (EnhancerGovernor,
     * GPU_BEHIND_DEGRADE). A fence after each draw, looked at when a later one
     * starts — a later task, so its status has had the chance to move — says
     * whether the GPU had caught up. It is looked at once FENCE_MIN_AGE_MS old:
     * the next draw at 60 fps, the one after at 120 fps, and frames back to
     * back after a network hiccup leave theirs for a later draw instead of
     * reading them before any GPU could have finished.
     */
    takeGpuBehind() {
        const n = this._gpuChecked;
        const share = n > 0 ? this._gpuBehind / n : null;
        this._gpuChecked = 0;
        this._gpuBehind = 0;
        return share;
    }

    _checkFences(now) {
        const gl = this.gl;
        // Oldest first: once one is too young, so are the ones after it.
        while (this._fences.length > 0 && now - this._fences[0].at >= FENCE_MIN_AGE_MS) {
            const sync = this._fences.shift().sync;
            try {
                this._gpuChecked++;
                if (gl.getSyncParameter(sync, gl.SYNC_STATUS) !== gl.SIGNALED) this._gpuBehind++;
                gl.deleteSync(sync);
            } catch (e) {
                // a lost context takes its syncs with it
            }
        }
    }

    _setFence(now) {
        const gl = this.gl;
        try {
            const sync = gl.fenceSync(gl.SYNC_GPU_COMMANDS_COMPLETE, 0);
            if (sync) this._fences.push({ sync, at: now });
            if (this._fences.length > FENCE_MAX_PENDING) gl.deleteSync(this._fences.shift().sync);
        } catch (e) {
            // a lost context sets no fence
        }
    }

    /**
     * See VideoRenderer.probeActive. A desynchronized WebGL2 canvas cannot be
     * sampled with drawImage once presented, so while a measurement is on the
     * three flag pixels are read with readPixels at the end of draw() — a
     * synchronous GPU round trip, paid only during the 200 ms of a probe. One
     * read of the row between the three, not three reads (see
     * Canvas2DRenderer.probeActive).
     */
    set probeActive(on) {
        this._probeActive = !!on;
        this._probePixels = null;
        this._probeDrawnAt = null;
    }
    get probePixels() {
        return this._probePixels;
    }
    get probeDrawnAt() {
        return this._probeDrawnAt;
    }

    _readProbePixels(cw, ch) {
        const gl = this.gl;
        const drawnAt = performance.now();
        // Same spots as LatencyProbe: 46.5 %, 50.5 %, 54.5 % of the width,
        // 2.5 % from the top (GL rows count from the bottom).
        const y = Math.min(ch - 1, Math.max(0, ch - 1 - Math.floor(ch * 0.025)));
        const xs = [0.465, 0.505, 0.545].map((f) => Math.min(cw - 1, Math.floor(cw * f)));
        const n = xs[2] - xs[0] + 1;
        const row = new Uint8Array(n * 4);
        gl.readPixels(xs[0], y, n, 1, gl.RGBA, gl.UNSIGNED_BYTE, row);
        const px = new Uint8ClampedArray(12);
        for (let i = 0; i < 3; i++) {
            const at = (xs[i] - xs[0]) * 4;
            px.set(row.subarray(at, at + 4), i * 4);
        }
        this._probePixels = px;
        this._probeDrawnAt = drawnAt;
    }

    /**
     * @param {HTMLCanvasElement|OffscreenCanvas} canvas
     * @param {{videoCodec: string, algo: string, desynchronized?: boolean}} opts
     *        `algo` may be 'off' when the enhancer ladder starts this session
     *        below the setting (see EnhancerGovernor's memory)
     */
    static async create(canvas, opts) {
        const r = new WebGlRenderer();
        r.canvas = canvas;
        r.videoCodec = opts.videoCodec;
        r._algo = WEBGL_ALGOS.includes(opts.algo) || opts.algo === 'off' ? opts.algo : 'gl-fsr1';
        r._yuv444 = opts.yuv444 === true;
        // The gaze zone the caller wants (BrowserDetect.mwsrGazeWanted): the
        // kernel's programs take it, its weight follows the stream's motion.
        if (opts.gazeZone) r.setMwsrZone({});
        // Diagnostics: mw_webgl_power picks the GPU preference, for the A/B of
        // 25/09/2026 — FSR1 cost +34 ms of click-to-photon on a three-GPU
        // client and nothing on a one-GPU one, and "high-performance" may put
        // the context on another adapter than the one that decodes and scans
        // out. Not a setting, same family as mw_raw_pointer.
        let power = 'high-performance';
        try {
            const p = localStorage.getItem('mw_webgl_power');
            if (p === 'default' || p === 'low-power' || p === 'high-performance') power = p;
        } catch (e) {
            // storage refused: the default stands
        }
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
            powerPreference: power,
        });
        if (!gl) throw new Error('WebGL2 unavailable');
        r.gl = gl;
        // What was actually granted, and on which GPU: a hint the browser
        // ignored, or a context on the wrong adapter, is invisible otherwise.
        try {
            const attrs = gl.getContextAttributes() || {};
            const dbg = gl.getExtension('WEBGL_debug_renderer_info');
            const gpu = dbg ? gl.getParameter(dbg.UNMASKED_RENDERER_WEBGL) : '?';
            console.log(
                `[WebGlRenderer] context: gpu=${gpu} desynchronized=${attrs.desynchronized} ` +
                    `power=${attrs.powerPreference || power}`,
            );
        } catch (e) {
            // a context without the extension still renders
        }
        canvas.addEventListener?.('webglcontextlost', (e) => {
            e.preventDefault();
            r._lost = true;
            console.warn('[WebGlRenderer] context lost');
        });
        canvas.addEventListener?.('webglcontextrestored', () => {
            r._lost = false;
            // the lost context's fences went with it, unread
            r._fences = [];
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

    /** See VideoRenderer.passthrough: the last frame was copied 1:1, no pass run. */
    get passthrough() {
        return this._passthrough;
    }

    /** Effective algorithm name for the overlay. */
    get algoName() {
        if (this._algo === 'gl-nis') return 'WebGL NIS';
        if (this._algo === 'gl-sgsr') return 'WebGL SGSR';
        if (this._algo === 'gl-mwsr') return 'WebGL MWSR';
        if (this._algo === 'off') return 'WebGL';
        return 'WebGL FSR1';
    }

    /**
     * Switch the enhancement pass at runtime — the enhancer ladder's lever, as
     * on WebGPU. The program of the new pass is built the first time it is
     * needed; a build that fails leaves the current pass running.
     * @param {string} algo one of WEBGL_ALGOS, or 'off' for the plain copy
     * @returns {boolean} true when the renderer switched.
     */
    setAlgo(algo) {
        if (algo === this._algo) return false;
        if (algo !== 'off' && !WEBGL_ALGOS.includes(algo)) return false;
        if (!this.gl || this.isContextLost()) return false;
        try {
            this._buildAlgo(algo);
        } catch (e) {
            console.error('[WebGlRenderer] setAlgo(' + algo + ') failed: ' + e.message);
            return false;
        }
        this._algo = algo;
        console.log('[WebGlRenderer] algo → ' + algo);
        return true;
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

    /**
     * The gl-mwsr kernel's per-draw values (MwsrShader's mwsrLaw: the vote
     * threshold `ft`, the sharpness `kg` and the others), for the laboratory's
     * side-by-side viewer; the product draws with the law. Takes effect at the
     * next draw.
     * @param {{ft?: number, kg?: number}|null} tuning null: the law
     */
    setMwsrTuning(tuning) {
        this._mwsrTuning = tuning || null;
        this._mwsrUniKey = '';
    }

    /**
     * The gl-mwsr kernel's constants of the `up` mode beyond the product's
     * (MwsrShader's MWSR_OVERRIDE_SPEC: SHOW, CHROMA444…), baked into the
     * texts, for the laboratory's side-by-side viewer, over the device class's
     * (mwsrClassOverrides), the only ones the product sets. A changed set
     * rebuilds the kernel's programs now, when the pass is the current one,
     * else at its next build.
     * @param {Object<string, number>|null} overrides null or {}: the product's
     */
    setMwsrOverrides(overrides) {
        const next = overrides || {};
        if (JSON.stringify(next) === JSON.stringify(this._mwsrOverrides || {})) return;
        this._mwsrOverrides = next;
        if (!this._mwsr) return;
        const gl = this.gl;
        if (gl && !this.isContextLost())
            for (const p of this._mwsr.values()) gl.deleteProgram(p.prog);
        this._mwsr = null;
        if (this._algo === 'gl-mwsr' && gl && !this.isContextLost()) this._buildAlgo('gl-mwsr');
    }

    /**
     * The gl-mwsr kernel's gaze zone (MwsrShader's MWSR_ZONE, plan §15.2):
     * null, the texts have no zone (the product's default); an object, the
     * zone's values over the defaults, the kernel's programs built with
     * GAZE 1 (rebuilt now when the pass is the current one) and its weight
     * following the stream's motion (noteEncodedFrame, GazeZone) unless
     * setMwsrGaze pins it.
     * @param {{cx?: number, cy?: number, rx?: number, ry?: number, ring?: number, ft?: number, boost?: number}|null} zone
     */
    setMwsrZone(zone) {
        const next = zone ? { ...zone } : null;
        if (JSON.stringify(next) === JSON.stringify(this._mwsrZone)) return;
        const flip = !next !== !this._mwsrZone;
        this._mwsrZone = next;
        this._mwsrUniKey = '';
        if (next && !this._gaze) this._gaze = new GazeZone();
        if (!flip || !this._mwsr) return;
        const gl = this.gl;
        if (gl && !this.isContextLost())
            for (const p of this._mwsr.values()) gl.deleteProgram(p.prog);
        this._mwsr = null;
        if (this._algo === 'gl-mwsr' && gl && !this.isContextLost()) this._buildAlgo('gl-mwsr');
    }

    /**
     * The zone's weight pinned, 0 to 1 (the laboratory: 1 is the zone as on
     * a moving picture), or null: the stream's motion decides.
     * @param {number|null} level
     */
    setMwsrGaze(level) {
        this._mwsrGazePinned =
            level === null || level === undefined ? null : Math.min(Math.max(level, 0), 1);
        this._mwsrUniKey = '';
    }

    /** @override The picture's motion, for the gaze zone; nothing without one. */
    noteEncodedFrame(bytes, key) {
        if (this._gaze) this._gaze.note(bytes, key, performance.now());
    }

    /** The zone's weight for a draw of a frame of `area` pixels: 0 without a zone. */
    _mwsrGazeFor(area) {
        if (!this._mwsrZone) return 0;
        if (this._mwsrGazePinned !== null) return this._mwsrGazePinned;
        return this._gaze ? this._gaze.level(performance.now(), area) : 0;
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

        // Only the pass in use is built, plus the plain copy (the frame at 1:1,
        // and the ladder's last rung); setAlgo builds the others on demand.
        // Whatever a lost context held is gone with it.
        this._easu = this._rcas = this._sgsr = this._nis = this._blit = null;
        this._mwsr = null;
        this._intermTex = this._fbo = this._coefTex = null;
        this._buildAlgo(this._algo);
        this._buildAlgo('off');
        gl.disable(gl.DEPTH_TEST);
        gl.disable(gl.BLEND);
        gl.disable(gl.SCISSOR_TEST);
    }

    _buildAlgo(algo) {
        const gl = this.gl;
        if (algo === 'off') {
            if (!this._blit) this._blit = this._program(BLIT_FS, ['uTex']);
        } else if (algo === 'gl-fsr1') {
            if (this._easu) return;
            this._easu = this._program(EASU_FS, ['uTex', 'uRes']);
            this._rcas = this._program(RCAS_FS, ['uTex', 'uRes']);
            this._intermTex = this._texture(gl.LINEAR);
            this._intermW = 0;
            this._intermH = 0;
            this._fbo = gl.createFramebuffer();
        } else if (algo === 'gl-sgsr') {
            if (!this._sgsr) this._sgsr = this._program(SGSR_FS, ['uTex', 'uView']);
        } else if (algo === 'gl-mwsr') {
            if (this._mwsr) return;
            // One program per regime, all built here so that nothing compiles
            // while the stream runs; the overrides baked into each text.
            const programs = new Map();
            for (const reg of mwsrRegimes()) {
                const overrides =
                    reg.mode === 'up'
                        ? {
                              CHROMA444: this._yuv444 ? 1 : 0,
                              GAZE: this._mwsrZone ? 1 : 0,
                              ...MWSR_CLASS,
                              ...(this._mwsrOverrides || {}),
                          }
                        : { R_DOWN: reg.rDown, LINEAR_LIGHT: reg.linearLight };
                programs.set(
                    mwsrRegimeKey(reg),
                    this._program(mwsrGlsl({ mode: reg.mode, taps: reg.taps, overrides }), [
                        'uTex',
                        'uTexelScale',
                        'uDimsAcross',
                        'uKernel',
                        'uZone',
                        'uZoneBonus',
                        'uBound',
                    ]),
                );
            }
            this._mwsr = programs;
        } else {
            if (this._nis) return;
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
    }

    // NIS sharpness, live from the console (see NisTables.readNisSharpness),
    // re-read at most twice a second.
    _readKnobs() {
        const now = performance.now();
        if (now - this._knobsReadMs < 500) return;
        this._knobsReadMs = now;
        this._nisSharpness = readNisSharpness();
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
        this._checkFences(drawStart);
        // Closed before the draw below: the content-age probe dates it by this.
        const frameTs = frame.timestamp;

        const inW = frame.displayWidth || frame.codedWidth || 0;
        const inH = frame.displayHeight || frame.codedHeight || 0;
        if (inW <= 0 || inH <= 0) {
            try {
                frame.close();
            } catch (e) {}
            return;
        }

        // Canvas backing = frame-aspect rect fitting the output box (same rule
        // as WebGpuRenderer, scaleRegime.js), else the frame's own size. At
        // exactly the frame size the pass has nothing to do: the plain copy
        // presents the frame, and the pass resumes when the size differs.
        const { cw, ch } = fitOutputSize(
            inW,
            inH,
            this._hasOutputSize ? this._outW : 0,
            this._hasOutputSize ? this._outH : 0,
        );
        const identity = isIdentityScale(inW, inH, cw, ch);
        const pass = identity ? 'off' : this._algo;
        this._passthrough = identity && this._algo !== 'off';
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
            if (pass === 'gl-fsr1') {
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
            } else if (pass === 'gl-sgsr') {
                gl.bindFramebuffer(gl.FRAMEBUFFER, null);
                gl.viewport(0, 0, cw, ch);
                gl.useProgram(this._sgsr.prog);
                gl.activeTexture(gl.TEXTURE0);
                gl.bindTexture(gl.TEXTURE_2D, this._inputTex);
                gl.uniform1i(this._sgsr.u.uTex, 0);
                gl.uniform4f(this._sgsr.u.uView, 1 / inW, 1 / inH, inW, inH);
                gl.drawArrays(gl.TRIANGLES, 0, 3);
            } else if (pass === 'gl-mwsr') {
                // The regime by the frame and canvas sizes — never 'copy', the
                // 1:1 frame took the plain copy; the uniform block of the WGSL
                // twin, kept while the sizes hold, in six vec4.
                const reg = mwsrRegime(inW, inH, cw, ch);
                const p = this._mwsr.get(mwsrRegimeKey(reg));
                // The gaze zone's weight for this draw, 0 without a zone: it
                // moves in steps of 1/64, so the block is remade rarely.
                const gaze = this._mwsrGazeFor(inW * inH);
                const uniKey = inW + 'x' + inH + '>' + cw + 'x' + ch + '@' + gaze;
                if (this._mwsrUniKey !== uniKey) {
                    this._mwsrUniKey = uniKey;
                    this._mwsrUni = mwsrUniforms(inW, inH, cw, ch, {
                        ...(this._mwsrTuning || {}),
                        gaze,
                        zone: this._mwsrZone || undefined,
                    });
                }
                gl.bindFramebuffer(gl.FRAMEBUFFER, null);
                gl.viewport(0, 0, cw, ch);
                gl.useProgram(p.prog);
                gl.activeTexture(gl.TEXTURE0);
                gl.bindTexture(gl.TEXTURE_2D, this._inputTex);
                gl.uniform1i(p.u.uTex, 0);
                gl.uniform4fv(p.u.uTexelScale, this._mwsrUni.subarray(0, 4));
                gl.uniform4fv(p.u.uDimsAcross, this._mwsrUni.subarray(4, 8));
                gl.uniform4fv(p.u.uKernel, this._mwsrUni.subarray(8, 12));
                // The zone's values: inactive, so null locations silently
                // ignored, in a text built without GAZE.
                gl.uniform4fv(p.u.uZone, this._mwsrUni.subarray(12, 16));
                gl.uniform4fv(p.u.uZoneBonus, this._mwsrUni.subarray(16, 20));
                gl.uniform4fv(p.u.uBound, this._mwsrUni.subarray(20, 24));
                gl.drawArrays(gl.TRIANGLES, 0, 3);
            } else if (pass === 'off') {
                gl.bindFramebuffer(gl.FRAMEBUFFER, null);
                gl.viewport(0, 0, cw, ch);
                gl.useProgram(this._blit.prog);
                gl.activeTexture(gl.TEXTURE0);
                gl.bindTexture(gl.TEXTURE_2D, this._inputTex);
                gl.uniform1i(this._blit.u.uTex, 0);
                gl.drawArrays(gl.TRIANGLES, 0, 3);
            } else {
                // NIS is specified for scale factors in [0.5, 1] (up to 2× up).
                // Outside that it still runs — it just is not what NVIDIA
                // validated — so say so once rather than refuse the frame.
                const kScaleX = inW / cw,
                    kScaleY = inH / ch;
                if (!this._nisScaleWarned && !nisScaleInRange(kScaleX, kScaleY)) {
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
            if (this._probeActive) this._readProbePixels(cw, ch);
            if (this.afterDraw) this.afterDraw(this, frameTs);
            this._setFence(performance.now());
        } catch (e) {
            console.error('[WebGlRenderer] draw failed: ' + e.message);
            path = 'failed';
        }

        this._rendered++;
        this._noteDraw(drawStart, waitMs, (this._passthrough ? '1:1' : this._algo) + ':' + path);
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
                for (const f of this._fences) gl.deleteSync(f.sync);
                this._fences = [];
                for (const p of [this._easu, this._rcas, this._sgsr, this._nis, this._blit])
                    if (p) gl.deleteProgram(p.prog);
                if (this._mwsr) for (const p of this._mwsr.values()) gl.deleteProgram(p.prog);
                const ext = gl.getExtension('WEBGL_lose_context');
                if (ext) ext.loseContext();
            } catch (e) {}
        }
        this.gl = null;
        this.canvas = null;
    }
}
