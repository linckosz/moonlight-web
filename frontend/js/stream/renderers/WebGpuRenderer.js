/*
 * Moonlight-Web — browser-based Sunshine/GameStream client.
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
 * WebGpuRenderer — WebGPU output path (feature On). SGSRv1 (C4) and FSR1 (C6).
 *
 * Common Pass 0 (blit): importExternalTexture(frame) → inputTex (rgba8unorm,
 * frame res). A texture_external can only be read with textureSampleBaseClampToEdge,
 * but the enhancers need textureGather → materialize the frame first.
 *
 * SGSRv1 (algo 'sgsr'): 2 passes — Pass 0 blit, Pass 1 SGSR inputTex → canvas.
 *   Port (Qualcomm, BSD-3-Clause) from sgsr1.h, mode 1, luma = GREEN, EdgeThreshold
 *   6/255, EdgeSharpness 1.5.
 *
 * FSR1 (algo 'fsr1'): 3 passes — Pass 0 blit, Pass 1 EASU inputTex → intermTex
 *   (rgba8unorm, output res), Pass 2 RCAS intermTex → canvas. WGSL port from
 *   firdawolf/AMD-FSR1-wgpu-shader (MIT; AMD FidelityFX FSR1). Adapted: fullscreen
 *   triangle VS (no vertex buffer), bindings consolidated, demo camera binding
 *   removed from RCAS, RCAS sharpness = 0.595 = exp2(-0.75) to match DX12 RCAS(0.75)
 *   under firdawolf's reparameterization (it multiplies by sharpness directly).
 *   EASU uniform = (inW, inH, outW, outH); RCAS uniform offsets are TEXEL sizes
 *   → (outW, outH, 1/outW, 1/outH).
 *
 * Dev/setting-gated (video_enhancement). Lifecycle constraints (see plan):
 *   - A canvas in 'webgpu' context can NEVER go back to '2d' → requestAdapter/
 *     requestDevice run BEFORE getContext('webgpu') so a failure falls back to
 *     Canvas2D.
 *   - External texture expires each task → its blit bind group is per-frame.
 *   - Perceptual space: non-srgb formats only.
 */
import { VideoRenderer } from './VideoRenderer.js';

// Full-screen triangle (uv 0..1, v down — matches texture sampling).
const FULLSCREEN_VS = /* wgsl */ `
struct VSOut {
    @builtin(position) pos : vec4f,
    @location(0) uv : vec2f,
};

@vertex
fn vs(@builtin(vertex_index) vid : u32) -> VSOut {
    var out : VSOut;
    let uv = vec2f(f32((vid << 1u) & 2u), f32(vid & 2u));
    out.uv = uv;
    out.pos = vec4f(uv * vec2f(2.0, -2.0) + vec2f(-1.0, 1.0), 0.0, 1.0);
    return out;
}
`;

// Pass 0: sample the external (video) texture into inputTex (materialization).
const BLIT_WGSL =
    FULLSCREEN_VS +
    /* wgsl */ `
@group(0) @binding(0) var samp : sampler;
@group(0) @binding(1) var tex : texture_external;

@fragment
fn fs(in : VSOut) -> @location(0) vec4f {
    return textureSampleBaseClampToEdge(tex, samp, in.uv);
}
`;

// SGSRv1 (mode 1, green luma). Ported from sgsr1.h, SGSR_MOBILE variant.
const SGSR_WGSL =
    FULLSCREEN_VS +
    /* wgsl */ `
struct Uniforms { viewport : vec4f }; // (1/inW, 1/inH, inW, inH)
@group(0) @binding(0) var samp : sampler;
@group(0) @binding(1) var inputTex : texture_2d<f32>;
@group(0) @binding(2) var<uniform> u : Uniforms;

const EDGE_THRESHOLD : f32 = 6.0 / 255.0;
const EDGE_SHARPNESS : f32 = 1.5;

fn fastLanczos2(x : f32) -> f32 {
    var wA = x - 4.0;
    let wB = x * wA - wA;
    wA = wA * wA;
    return wB * wA;
}

fn weightY(dx : f32, dy : f32, c : f32, data : vec3f) -> vec2f {
    let stdev = data.x;
    let dir = data.yz;
    let edgeDis = (dx * dir.y) + (dy * dir.x);
    let x = ((dx * dx) + (dy * dy)) +
            ((edgeDis * edgeDis) * ((clamp((c * c) * stdev, 0.0, 1.0) * 0.7) - 1.0));
    let w = fastLanczos2(x);
    return vec2f(w, w * c);
}

fn edgeDirection(left : vec4f, right : vec4f) -> vec2f {
    let RxLz = right.x - left.z;
    let RwLy = right.w - left.y;
    var delta : vec2f;
    delta.x = RxLz + RwLy;
    delta.y = RxLz - RwLy;
    let lengthInv = inverseSqrt((delta.x * delta.x + 3.075740e-05) + (delta.y * delta.y));
    return vec2f(delta.x * lengthInv, delta.y * lengthInv);
}

@fragment
fn fs(in : VSOut) -> @location(0) vec4f {
    let uv = in.uv;
    let con1 = u.viewport;

    var pix = vec4f(textureSampleLevel(inputTex, samp, uv, 0.0).xyz, 1.0);

    let imgCoord = (uv * con1.zw) + vec2f(-0.5, 0.5);
    let imgCoordPixel = floor(imgCoord);
    var coord = imgCoordPixel * con1.xy;
    let pl = imgCoord - imgCoordPixel;
    var left = textureGather(1, inputTex, samp, coord);

    let pixL = pix.y;
    let edgeVote = abs(left.z - left.y) + abs(pixL - left.y) + abs(pixL - left.z);
    if (edgeVote > EDGE_THRESHOLD) {
        coord.x += con1.x;
        var right = textureGather(1, inputTex, samp, coord + vec2f(con1.x, 0.0));
        let udUp = textureGather(1, inputTex, samp, coord + vec2f(0.0, -con1.y));
        let udDn = textureGather(1, inputTex, samp, coord + vec2f(0.0,  con1.y));
        var upDown = vec4f(udUp.w, udUp.z, udDn.y, udDn.x);

        let mean = (left.y + left.z + right.x + right.w) * 0.25;
        left = left - vec4f(mean);
        right = right - vec4f(mean);
        upDown = upDown - vec4f(mean);
        let pixW = pixL - mean;

        let sum = (abs(left.x) + abs(left.y) + abs(left.z) + abs(left.w)) +
                  (abs(right.x) + abs(right.y) + abs(right.z) + abs(right.w)) +
                  (abs(upDown.x) + abs(upDown.y) + abs(upDown.z) + abs(upDown.w));
        let sumMean = 1.014185e+01 / sum;
        let stdev = sumMean * sumMean;

        let data = vec3f(stdev, edgeDirection(left, right));

        var aWY = weightY(pl.x,       pl.y + 1.0, upDown.x, data);
        aWY += weightY(pl.x - 1.0, pl.y + 1.0, upDown.y, data);
        aWY += weightY(pl.x - 1.0, pl.y - 2.0, upDown.z, data);
        aWY += weightY(pl.x,       pl.y - 2.0, upDown.w, data);
        aWY += weightY(pl.x + 1.0, pl.y - 1.0, left.x,   data);
        aWY += weightY(pl.x,       pl.y - 1.0, left.y,   data);
        aWY += weightY(pl.x,       pl.y,       left.z,   data);
        aWY += weightY(pl.x + 1.0, pl.y,       left.w,   data);
        aWY += weightY(pl.x - 1.0, pl.y - 1.0, right.x,  data);
        aWY += weightY(pl.x - 2.0, pl.y - 1.0, right.y,  data);
        aWY += weightY(pl.x - 2.0, pl.y,       right.z,  data);
        aWY += weightY(pl.x - 1.0, pl.y,       right.w,  data);

        var finalY = aWY.y / aWY.x;
        let max4 = max(max(left.y, left.z), max(right.x, right.w));
        let min4 = min(min(left.y, left.z), min(right.x, right.w));
        finalY = clamp(EDGE_SHARPNESS * finalY, min4, max4);

        let deltaY = finalY - pixW;
        pix.x = saturate(pix.x + deltaY);
        pix.y = saturate(pix.y + deltaY);
        pix.z = saturate(pix.z + deltaY);
    }
    pix.w = 1.0;
    return pix;
}
`;

// FSR1 EASU — AMD FidelityFX (firdawolf WGSL port, MIT). Verbatim fragment math;
// VS replaced with a fullscreen triangle, bindings consolidated, custom saturate()
// dropped in favor of the WGSL builtin.
const EASU_WGSL = /* wgsl */ `
struct Resolution { inputwidth : f32, inputheight : f32, outputwidth : f32, outputheight : f32, }
@group(0) @binding(0) var input : texture_2d<f32>;
@group(0) @binding(1) var sam : sampler;
@group(0) @binding(2) var<uniform> resolution : Resolution;

struct VertexOutput {
    @builtin(position) clip_position : vec4<f32>,
    @location(0) tex_coords : vec2<f32>,
}

@vertex
fn vs_main(@builtin(vertex_index) vid : u32) -> VertexOutput {
    var out : VertexOutput;
    let uv = vec2<f32>(f32((vid << 1u) & 2u), f32(vid & 2u));
    out.tex_coords = uv;
    out.clip_position = vec4<f32>(uv * vec2<f32>(2.0, -2.0) + vec2<f32>(-1.0, 1.0), 0.0, 1.0);
    return out;
}

fn min3(a:vec3<f32>,b:vec3<f32>,c:vec3<f32>)->vec3<f32>{
    return min(a, min(b,c));
}
fn max3(a:vec3<f32>,b:vec3<f32>,c:vec3<f32>)->vec3<f32>{
    return max(a, max(b,c));
}

struct FsrTap{
    aC:vec3<f32>,
    aW:f32,
}
struct FsrSet{
    dir:vec2<f32>,
    len:f32,
}
fn matchSet(pp:vec2<f32>,biS:bool, biT:bool, biU:bool, biV:bool) -> f32{
	if(biS){
		return (1.0 - pp.x) * (1.0 - pp.y);
	}
	else if(biT){
        return pp.x * (1.0 - pp.y);
	}
	else if(biU){
       return (1.0 - pp.x) * pp.y;
	}
	else if(biV){
       return pp.x * pp.y;
	}
	else{
	   return 0.0;
	}
}

fn FsrEasuTap(
	fsr:FsrTap,
	off:vec2<f32>,
	dir:vec2<f32>,
	len:vec2<f32>,
	lob:f32,
	clp:f32,
	c:vec3<f32>
)-> FsrTap {
	var fsr1 : FsrTap = fsr;
    var aC : vec3<f32> = fsr1.aC;
    var aW : f32 = fsr1.aW;
	var v : vec2<f32> = vec2<f32>(0.0);
	v.x = (off.x * (dir.x)) + (off.y * dir.y);
	v.y = (off.x * (-dir.y)) + (off.y * dir.x);
	v = v * len;
	var d2 : f32 = v.x * v.x + v.y * v.y;
	d2 = min(d2, clp);
	var wB : f32 = 2.0 / 5.0 * d2 - 1.0;
	var wA : f32 = lob * d2 - 1.0;
	wB = wB * wB;
	wA = wA * wA;
	wB = 25.0 / 16.0 * wB - (25.0 / 16.0 - 1.0);
	var w : f32 = wB * wA;
	aC = aC + c * w;
    aW = aW + w;
    fsr1.aC = aC;
    fsr1.aW = aW;
    return fsr1;
}

fn FsrEasuSet(
	fsr : FsrSet,
	pp:vec2<f32>,
	biS:bool, biT:bool, biU:bool, biV:bool,
	lA:f32, lB:f32, lC:f32, lD:f32, lE:f32) -> FsrSet{
	var fsr1 : FsrSet = fsr;
    var dir : vec2<f32> = fsr1.dir;
    var len : f32 = fsr1.len;
	var w : f32 = 0.0;
	w = matchSet(pp,biS,biT,biU,biV);
	var dc:f32 = lD - lC;
	var cb:f32 = lC - lB;
	var lenX:f32 = max(abs(dc), abs(cb));
	lenX = 1.0 / lenX;
	var dirX :f32 = lD - lB;
	dir.x = dir.x + (dirX * w);
	lenX = saturate(abs(dirX) * lenX);
	lenX = lenX * lenX;
	len = len + (lenX * w);
	var ec :f32 = lE - lC;
	var ca : f32 = lC - lA;
	var lenY :f32 = max(abs(ec), abs(ca));
	lenY = 1.0 / lenY;
	var dirY:f32 = lE - lA;
	dir.y = dir.y + (dirY * w);
	lenY = saturate(abs(dirY) * lenY);
	lenY = lenY * lenY;
	len = len + (lenY * w);
    fsr1.dir = dir;
    fsr1.len = len;
    return fsr1;
}

fn gather_red_components(c: vec2<f32>) -> vec4<f32> {
   return textureGather(0,input,sam,c);
}
fn gather_green_components(c: vec2<f32>) -> vec4<f32> {
   return textureGather(1,input,sam,c);
}
fn gather_blue_components(c: vec2<f32>) -> vec4<f32> {
   return textureGather(2,input,sam,c);
}

@fragment
fn fs_main(in: VertexOutput) -> @location(0) vec4<f32>{
	var inputSize : vec2<f32>;
    inputSize.x = resolution.inputwidth;
    inputSize.y = resolution.inputheight;
	var outputSize : vec2<f32>;
    outputSize.x = resolution.outputwidth;
    outputSize.y = resolution.outputheight;

	var pp:vec2<f32> = (floor(in.tex_coords * outputSize) + 0.5) / outputSize * inputSize - 0.5;
	var fp : vec2<f32> = floor(pp);
	pp = pp - fp;
	var p0 :vec2<f32> = fp + vec2<f32>(1.0, -1.0);
	var p1 : vec2<f32> = p0 + vec2<f32>(-1.0, 2.0);
	var p2: vec2<f32> = p0 + vec2<f32>(1.0, 2.0);
	var p3: vec2<f32> = p0 + vec2<f32>(0.0, 4.0);

	p0 = p0 / inputSize;
	p1 = p1 / inputSize;
	p2 = p2 / inputSize;
	p3 = p3 / inputSize;

	var bczzR : vec4<f32> = gather_red_components(p0);
	var bczzG : vec4<f32> = gather_green_components(p0);
	var bczzB : vec4<f32> = gather_blue_components(p0);
	var ijfeR : vec4<f32> = gather_red_components(p1);
	var ijfeG : vec4<f32> = gather_green_components(p1);
	var ijfeB : vec4<f32> = gather_blue_components(p1);
	var klhgR : vec4<f32> = gather_red_components(p2);
	var klhgG : vec4<f32> = gather_green_components(p2);
	var klhgB : vec4<f32> = gather_blue_components(p2);
	var zzonR : vec4<f32> = gather_red_components(p3);
	var zzonG : vec4<f32> = gather_green_components(p3);
	var zzonB : vec4<f32> = gather_blue_components(p3);
	var bczzL : vec4<f32> = bczzB * 0.5 + (bczzR * 0.5 + bczzG);
	var ijfeL : vec4<f32> = ijfeB * 0.5 + (ijfeR * 0.5 + ijfeG);
	var klhgL : vec4<f32> = klhgB * 0.5 + (klhgR * 0.5 + klhgG);
	var zzonL : vec4<f32> = zzonB * 0.5 + (zzonR * 0.5 + zzonG);
	var bL :f32 = bczzL.x;
	var cL :f32 = bczzL.y;
	var iL :f32 = ijfeL.x;
	var jL :f32 = ijfeL.y;
	var fL :f32 = ijfeL.z;
	var eL :f32 = ijfeL.w;
	var kL :f32 = klhgL.x;
	var lL :f32 = klhgL.y;
	var hL :f32 = klhgL.z;
	var gL :f32 = klhgL.w;
	var oL :f32 = zzonL.z;
	var nL :f32 = zzonL.w;
    var fsr:FsrSet;
	fsr.dir = vec2<f32>(0.0,0.0);
	fsr.len = 0.0;
	fsr = FsrEasuSet(fsr, pp, true, false, false, false, bL, eL, fL, gL, jL);
	fsr = FsrEasuSet(fsr, pp, false, true, false, false, cL, fL, gL, hL, kL);
	fsr = FsrEasuSet(fsr, pp, false, false, true, false, fL, iL, jL, kL, nL);
    fsr = FsrEasuSet(fsr, pp, false, false, false, true, gL, jL, kL, lL, oL);
	var dir2 :vec2<f32> = fsr.dir * fsr.dir;
	var dirR :f32 = dir2.x + dir2.y;
	var zro : bool = dirR < 1.0 / 32768.0;
	dirR = 1.0/(sqrt(dirR));
	if (zro) {dirR = 1.0;};
	if (zro) {fsr.dir.x = 1.0;};
	fsr.dir = fsr.dir * dirR;
	fsr.len = fsr.len * 0.5;
	fsr.len = fsr.len * fsr.len;
	var stretch :f32 = (fsr.dir.x * fsr.dir.x + fsr.dir.y * fsr.dir.y) * 1.0/(max(abs(fsr.dir.x), abs(fsr.dir.y)));
	var len2:vec2<f32> = vec2<f32>(1.0 + (stretch - 1.0) * fsr.len, 1.0 - 0.5 * fsr.len );
	var lob : f32 = 0.5 + ((1.0 / 4.0 - 0.04) - 0.5) * fsr.len;
	var clp : f32 = 1.0/lob;
	var min4 = min(min3(vec3<f32>(ijfeR.z, ijfeG.z, ijfeB.z), vec3<f32>(klhgR.w, klhgG.w, klhgB.w), vec3<f32>(ijfeR.y, ijfeG.y, ijfeB.y)),
		vec3<f32>(klhgR.x, klhgG.x, klhgB.x));

	var max4:vec3<f32> = max(max3(vec3<f32>(ijfeR.z, ijfeG.z, ijfeB.z), vec3<f32>(klhgR.w, klhgG.w, klhgB.w), vec3<f32>(ijfeR.y, ijfeG.y, ijfeB.y)),
		vec3<f32>(klhgR.x, klhgG.x, klhgB.x));
	var fsr2:FsrTap;
	fsr2.aC = vec3<f32>(0.0,0.0,0.0);
	fsr2.aW =0.0;
	fsr2=FsrEasuTap(fsr2, vec2<f32>(0.0, -1.0) - pp, fsr.dir, len2, lob, clp, vec3<f32>(bczzR.x, bczzG.x, bczzB.x));
	fsr2=FsrEasuTap(fsr2, vec2<f32>(1.0, -1.0) - pp, fsr.dir, len2, lob, clp, vec3<f32>(bczzR.y, bczzG.y, bczzB.y));
	fsr2=FsrEasuTap(fsr2, vec2<f32>(-1.0, 1.0) - pp, fsr.dir, len2, lob, clp, vec3<f32>(ijfeR.x, ijfeG.x, ijfeB.x));
	fsr2=FsrEasuTap(fsr2, vec2<f32>(0.0, 1.0) - pp, fsr.dir, len2, lob, clp, vec3<f32>(ijfeR.y, ijfeG.y, ijfeB.y));
	fsr2=FsrEasuTap(fsr2, vec2<f32>(0.0, 0.0) - pp, fsr.dir, len2, lob, clp, vec3<f32>(ijfeR.z, ijfeG.z, ijfeB.z));
	fsr2=FsrEasuTap(fsr2, vec2<f32>(-1.0, 0.0) - pp, fsr.dir, len2, lob, clp, vec3<f32>(ijfeR.w, ijfeG.w, ijfeB.w));
	fsr2=FsrEasuTap(fsr2, vec2<f32>(1.0, 1.0) - pp, fsr.dir, len2, lob, clp, vec3<f32>(klhgR.x, klhgG.x, klhgB.x));
	fsr2=FsrEasuTap(fsr2, vec2<f32>(2.0, 1.0) - pp, fsr.dir, len2, lob, clp, vec3<f32>(klhgR.y, klhgG.y, klhgB.y));
	fsr2=FsrEasuTap(fsr2, vec2<f32>(2.0, 0.0) - pp, fsr.dir, len2, lob, clp, vec3<f32>(klhgR.z, klhgG.z, klhgB.z));
	fsr2=FsrEasuTap(fsr2, vec2<f32>(1.0, 0.0) - pp, fsr.dir, len2, lob, clp, vec3<f32>(klhgR.w, klhgG.w, klhgB.w));
	fsr2=FsrEasuTap(fsr2, vec2<f32>(1.0, 2.0) - pp, fsr.dir, len2, lob, clp, vec3<f32>(zzonR.z, zzonG.z, zzonB.z));
	fsr2=FsrEasuTap(fsr2, vec2<f32>(0.0, 2.0) - pp, fsr.dir, len2, lob, clp, vec3<f32>(zzonR.w, zzonG.w, zzonB.w));
	var c : vec3<f32> = min(max4, max(min4, fsr2.aC * (1.0/fsr2.aW)));
	return vec4<f32>(c, 1.0);
}
`;

// FSR1 RCAS — AMD FidelityFX (firdawolf WGSL port, MIT). Verbatim fragment math;
// VS replaced with a fullscreen triangle, demo camera binding removed, module-scope
// 'let' → 'const', sharpness 0.595 (= exp2(-0.75) → DX12 RCAS(0.75) equivalent),
// custom saturate() dropped (WGSL builtin). resolution.output{width,height} are
// TEXEL sizes here (set by the renderer to 1/outW, 1/outH).
const RCAS_WGSL = /* wgsl */ `
const sharpness : f32 = 0.595;

struct Resolution { inputwidth : f32, inputheight : f32, outputwidth : f32, outputheight : f32, }
@group(0) @binding(0) var input : texture_2d<f32>;
@group(0) @binding(1) var sam : sampler;
@group(0) @binding(2) var<uniform> resolution : Resolution;

struct VertexOutput {
    @builtin(position) clip_position : vec4<f32>,
    @location(0) tex_coords : vec2<f32>,
}

fn min3f(a:f32,b:f32,c:f32)->f32{
    return min(a, min(b,c));
}
fn max3f(a:f32,b:f32,c:f32)->f32{
    return max(a, max(b,c));
}

@vertex
fn vs_main(@builtin(vertex_index) vid : u32) -> VertexOutput {
    var out : VertexOutput;
    let uv = vec2<f32>(f32((vid << 1u) & 2u), f32(vid & 2u));
    out.tex_coords = uv;
    out.clip_position = vec4<f32>(uv * vec2<f32>(2.0, -2.0) + vec2<f32>(-1.0, 1.0), 0.0, 1.0);
    return out;
}

@fragment
fn fs_main(in: VertexOutput) -> @location(0) vec4<f32>{
    let FSR_RCAS_LIMIT :f32 = 0.25-(1.0/16.0);
	let b : vec3<f32> = textureSample(input,sam,in.tex_coords + vec2<f32>(0.0, -resolution.outputheight)).rgb;
	let d : vec3<f32> = textureSample(input,sam,in.tex_coords + vec2<f32>(-resolution.outputwidth, 0.0)).rgb;
	var e : vec3<f32> = textureSample(input,sam,in.tex_coords).rgb;
	let f : vec3<f32> = textureSample(input,sam,in.tex_coords + vec2<f32>(resolution.outputwidth, 0.0)).rgb;
	let h : vec3<f32> = textureSample(input,sam,in.tex_coords + vec2<f32>(0.0, resolution.outputheight)).rgb;
	var bR :f32 = b.r;
	var bG :f32  = b.g;
	var bB :f32  = b.b;
	var dR :f32  = d.r;
	var dG :f32  = d.g;
	var dB :f32  = d.b;
	var eR :f32  = e.r;
	var eG :f32  = e.g;
	var eB :f32  = e.b;
	var fR :f32 = f.r;
	var fG :f32  = f.g;
	var fB :f32  = f.b;
	var hR :f32 = h.r;
	var hG :f32  = h.g;
	var hB:f32  = h.b;

	var nz:f32 = 0.0;

	var bL :f32  = bB * 0.5 + (bR * 0.5 + bG);
	var dL:f32  = dB * 0.5 + (dR * 0.5 + dG);
	var eL:f32  = eB * 0.5 + (eR * 0.5 + eG);
	var fL:f32  = fB * 0.5 + (fR * 0.5 + fG);
	var hL:f32  = hB * 0.5 + (hR * 0.5 + hG);

	nz = 0.25 * bL + 0.25 * dL + 0.25 * fL + 0.25 * hL - eL;
	nz = saturate(abs(nz) * 1.0/(max3f(max3f(bL, dL, eL), fL, hL) - min3f(min3f(bL, dL, eL), fL, hL)));
	nz = -0.5 * nz + 1.0;

	var mn4R :f32 =  min(min3f(bR, dR, fR), hR);
	var mn4G :f32  = min(min3f(bG, dG, fG), hG);
	var mn4B :f32  = min(min3f(bB, dB, fB), hB);
	var mx4R :f32  = max(max3f(bR, dR, fR), hR);
	var mx4G :f32  = max(max3f(bG, dG, fG), hG);
	var mx4B :f32  = max(max3f(bB, dB, fB), hB);
	var peakC :vec2<f32> = vec2<f32>( 1.0, -1.0 * 4.0 );
	var hitMinR :f32  = min(mn4R, eR) * 1.0/(4.0 * mx4R);
	var hitMinG :f32 = min(mn4G, eG) * 1.0/(4.0 * mx4G);
	var hitMinB :f32  = min(mn4B, eB) * 1.0/(4.0 * mx4B);
	var hitMaxR :f32 = (peakC.x - max(mx4R, eR)) * 1.0/(4.0 * mn4R + peakC.y);
	var hitMaxG :f32  = (peakC.x - max(mx4G, eG)) * 1.0/(4.0 * mn4G + peakC.y);
	var hitMaxB :f32 = (peakC.x - max(mx4B, eB)) * 1.0/(4.0 * mn4B + peakC.y);
	var lobeR:f32  = max(-hitMinR, hitMaxR);
	var lobeG:f32  = max(-hitMinG, hitMaxG);
	var lobeB :f32  = max(-hitMinB, hitMaxB);
	var lobe :f32  = max(-FSR_RCAS_LIMIT, min(max3f(lobeR, lobeG, lobeB), 0.0)) * sharpness;

	lobe = lobe * nz;

	var rcpL :f32  = 1.0/(4.0 * lobe + 1.0);
	var c:vec3<f32>  = vec3<f32>(
		(lobe * bR + lobe * dR + lobe * hR + lobe * fR + eR) * rcpL,
		(lobe * bG + lobe * dG + lobe * hG + lobe * fG + eG) * rcpL,
		(lobe * bB + lobe * dB + lobe * hB + lobe * fB + eB) * rcpL
	);
	return vec4<f32>(c, 1.0);
}
`;

export class WebGpuRenderer extends VideoRenderer {
    static async create(canvas, opts) {
        if (!navigator.gpu) throw new Error('WebGPU unavailable (no navigator.gpu)');

        // ── Steps that may fail on unsupported platforms — BEFORE getContext ──
        const adapter = await navigator.gpu.requestAdapter();
        if (!adapter) throw new Error('WebGPU requestAdapter returned null');
        const device = await adapter.requestDevice();

        const r = new WebGpuRenderer();
        r.canvas = canvas;
        r.videoCodec = opts.videoCodec;
        // 'off' = WebGPU pass-through (blit only, no upscaler); sgsr is the safe default.
        r._algo = opts.algo === 'fsr1' || opts.algo === 'off' ? opts.algo : 'sgsr';
        // HDR: rgba16float canvas in extended tone-mapping mode (values > 1.0
        // map to the display's HDR headroom). Gated on opts.hdr; _configure()
        // self-downgrades to SDR if the browser rejects the HDR canvas config.
        r._hdr = !!opts.hdr;
        r._adapter = adapter;
        r._device = device;
        r._ready = false;
        r._disposed = false;
        r._outW = 0;
        r._outH = 0;
        r._hasOutputSize = false;
        r._armDeviceLost();

        // ── Canvas is now committed to WebGPU (no going back to '2d') ─────────
        r.ctx = canvas.getContext('webgpu');
        r._configure(); // sets _format / _interFormat (HDR-aware)
        r._buildResources();
        canvas.width = 1920;
        canvas.height = 1080;
        r._ready = true;
        return r;
    }

    get ready() {
        return this._ready;
    }

    get kind() {
        return 'webgpu';
    }

    /** Effective algorithm name for the overlay: 'SGSR' | 'FSR1' | 'Off'. */
    get algoName() {
        if (this._algo === 'fsr1') return 'FSR1';
        if (this._algo === 'off') return 'Off';
        return 'SGSR';
    }

    /** True HDR is active (rgba16float canvas accepted). Read by the overlay. */
    get hdrActive() {
        return !!this._hdr;
    }

    _configure() {
        if (this._hdr) {
            // HDR canvas: float16 backbuffer + 'extended' tone mapping so values
            // above 1.0 reach the display's HDR headroom; wide-gamut display-p3.
            // Intermediate textures must also be float16 to keep 10-bit precision.
            this._format = 'rgba16float';
            this._interFormat = 'rgba16float';
            this._importColorSpace = 'display-p3';
            try {
                this.ctx.configure({
                    device: this._device,
                    format: this._format,
                    alphaMode: 'opaque',
                    colorSpace: 'display-p3',
                    toneMapping: { mode: 'extended' },
                });
                return;
            } catch (e) {
                console.warn(
                    '[WebGpuRenderer] HDR canvas config rejected, falling back to SDR: ' +
                        e.message,
                );
                this._hdr = false;
            }
        }
        // Standard 8-bit sRGB canvas (SDR).
        this._format = navigator.gpu.getPreferredCanvasFormat();
        this._interFormat = 'rgba8unorm';
        this._importColorSpace = 'srgb';
        this.ctx.configure({
            device: this._device,
            format: this._format,
            alphaMode: 'opaque',
            colorSpace: 'srgb',
        });
    }

    _buildResources() {
        const device = this._device;
        this._sampler = device.createSampler({
            magFilter: 'linear',
            minFilter: 'linear',
            addressModeU: 'clamp-to-edge',
            addressModeV: 'clamp-to-edge',
        });

        // Pass 0 — blit external → inputTex (both algos).
        this._blitLayout = device.createBindGroupLayout({
            entries: [
                { binding: 0, visibility: GPUShaderStage.FRAGMENT, sampler: {} },
                { binding: 1, visibility: GPUShaderStage.FRAGMENT, externalTexture: {} },
            ],
        });
        const blitModule = device.createShaderModule({ code: BLIT_WGSL });
        this._blitPipeline = device.createRenderPipeline({
            layout: device.createPipelineLayout({ bindGroupLayouts: [this._blitLayout] }),
            vertex: { module: blitModule, entryPoint: 'vs' },
            fragment: {
                module: blitModule,
                entryPoint: 'fs',
                targets: [{ format: this._interFormat }],
            },
            primitive: { topology: 'triangle-list' },
        });

        if (this._algo === 'fsr1') {
            this._buildFsr1Resources();
        } else if (this._algo === 'off') {
            this._buildPassthroughResources();
        } else {
            this._buildSgsrResources();
        }

        // Size-dependent resources (lazily (re)created in draw()).
        this._inputTex = null;
        this._inputTexView = null;
        this._inW = 0;
        this._inH = 0;
        this._intermTex = null;
        this._intermTexView = null;
        this._intermW = 0;
        this._intermH = 0;
        this._enhanceBindGroup = null; // SGSR / EASU bind group (samples inputTex)
        this._rcasBindGroup = null; // FSR1 RCAS bind group (samples intermTex)
    }

    // 'off' mode: a blit pipeline targeting the canvas (external → canvas, no upscaler).
    _buildPassthroughResources() {
        const device = this._device;
        const blitModule = device.createShaderModule({ code: BLIT_WGSL });
        this._passthroughPipeline = device.createRenderPipeline({
            layout: device.createPipelineLayout({ bindGroupLayouts: [this._blitLayout] }),
            vertex: { module: blitModule, entryPoint: 'vs' },
            fragment: { module: blitModule, entryPoint: 'fs', targets: [{ format: this._format }] },
            primitive: { topology: 'triangle-list' },
        });
    }

    _buildSgsrResources() {
        const device = this._device;
        this._uniformBuf = device.createBuffer({
            size: 16,
            usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST,
        });
        this._enhanceLayout = device.createBindGroupLayout({
            entries: [
                { binding: 0, visibility: GPUShaderStage.FRAGMENT, sampler: {} },
                {
                    binding: 1,
                    visibility: GPUShaderStage.FRAGMENT,
                    texture: { sampleType: 'float' },
                },
                { binding: 2, visibility: GPUShaderStage.FRAGMENT, buffer: {} },
            ],
        });
        const sgsrModule = device.createShaderModule({ code: SGSR_WGSL });
        this._enhancePipeline = device.createRenderPipeline({
            layout: device.createPipelineLayout({ bindGroupLayouts: [this._enhanceLayout] }),
            vertex: { module: sgsrModule, entryPoint: 'vs' },
            fragment: { module: sgsrModule, entryPoint: 'fs', targets: [{ format: this._format }] },
            primitive: { topology: 'triangle-list' },
        });
    }

    _buildFsr1Resources() {
        const device = this._device;
        this._easuUniform = device.createBuffer({
            size: 16,
            usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST,
        });
        this._rcasUniform = device.createBuffer({
            size: 16,
            usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST,
        });
        // EASU and RCAS share the same layout shape: texture(0) + sampler(1) + uniform(2).
        this._fsrLayout = device.createBindGroupLayout({
            entries: [
                {
                    binding: 0,
                    visibility: GPUShaderStage.FRAGMENT,
                    texture: { sampleType: 'float' },
                },
                { binding: 1, visibility: GPUShaderStage.FRAGMENT, sampler: {} },
                { binding: 2, visibility: GPUShaderStage.FRAGMENT, buffer: {} },
            ],
        });
        const fsrPipelineLayout = device.createPipelineLayout({
            bindGroupLayouts: [this._fsrLayout],
        });
        const easuModule = device.createShaderModule({ code: EASU_WGSL });
        this._easuPipeline = device.createRenderPipeline({
            layout: fsrPipelineLayout,
            vertex: { module: easuModule, entryPoint: 'vs_main' },
            fragment: {
                module: easuModule,
                entryPoint: 'fs_main',
                targets: [{ format: this._interFormat }],
            },
            primitive: { topology: 'triangle-list' },
        });
        const rcasModule = device.createShaderModule({ code: RCAS_WGSL });
        this._rcasPipeline = device.createRenderPipeline({
            layout: fsrPipelineLayout,
            vertex: { module: rcasModule, entryPoint: 'vs_main' },
            fragment: {
                module: rcasModule,
                entryPoint: 'fs_main',
                targets: [{ format: this._format }],
            },
            primitive: { topology: 'triangle-list' },
        });
    }

    // inputTex = frame resolution (Pass 0 target / enhancer source).
    _ensureInputTex(inW, inH) {
        if (this._inputTex && this._inW === inW && this._inH === inH) return;
        if (this._inputTex) {
            try {
                this._inputTex.destroy();
            } catch (e) {}
        }
        this._inputTex = this._device.createTexture({
            size: { width: inW, height: inH },
            format: this._interFormat,
            usage: GPUTextureUsage.RENDER_ATTACHMENT | GPUTextureUsage.TEXTURE_BINDING,
        });
        this._inputTexView = this._inputTex.createView();
        this._inW = inW;
        this._inH = inH;

        if (this._algo === 'fsr1') {
            this._enhanceBindGroup = this._device.createBindGroup({
                layout: this._fsrLayout,
                entries: [
                    { binding: 0, resource: this._inputTexView },
                    { binding: 1, resource: this._sampler },
                    { binding: 2, resource: { buffer: this._easuUniform } },
                ],
            });
        } else {
            this._enhanceBindGroup = this._device.createBindGroup({
                layout: this._enhanceLayout,
                entries: [
                    { binding: 0, resource: this._sampler },
                    { binding: 1, resource: this._inputTexView },
                    { binding: 2, resource: { buffer: this._uniformBuf } },
                ],
            });
        }
    }

    // intermTex = output resolution (FSR1 only: EASU target / RCAS source).
    _ensureIntermTex(w, h) {
        if (this._intermTex && this._intermW === w && this._intermH === h) return;
        if (this._intermTex) {
            try {
                this._intermTex.destroy();
            } catch (e) {}
        }
        this._intermTex = this._device.createTexture({
            size: { width: w, height: h },
            format: this._interFormat,
            usage: GPUTextureUsage.RENDER_ATTACHMENT | GPUTextureUsage.TEXTURE_BINDING,
        });
        this._intermTexView = this._intermTex.createView();
        this._intermW = w;
        this._intermH = h;
        this._rcasBindGroup = this._device.createBindGroup({
            layout: this._fsrLayout,
            entries: [
                { binding: 0, resource: this._intermTexView },
                { binding: 1, resource: this._sampler },
                { binding: 2, resource: { buffer: this._rcasUniform } },
            ],
        });
    }

    _armDeviceLost() {
        this._device.lost.then((info) => {
            if (this._disposed || info.reason === 'destroyed') return;
            console.warn('[WebGpuRenderer] device lost (' + info.reason + '): ' + info.message);
            this._ready = false;
            this._recover();
        });
    }

    async _recover() {
        try {
            const adapter = await navigator.gpu.requestAdapter();
            if (!adapter) throw new Error('no adapter on recovery');
            this._adapter = adapter;
            this._device = await adapter.requestDevice();
            this._armDeviceLost();
            this._configure();
            this._buildResources(); // resets size-dependent textures (recreated on next draw)
            this._ready = true;
            console.log('[WebGpuRenderer] device recovered');
        } catch (e) {
            console.error('[WebGpuRenderer] device recovery failed:', e.message);
        }
    }

    setOutputSize(width, height) {
        if (width <= 0 || height <= 0) return;
        this._outW = width;
        this._outH = height;
        this._hasOutputSize = true;
    }

    async draw(frame) {
        if (!this._ready || !this.ctx) {
            try {
                frame.close();
            } catch (e) {}
            return;
        }

        const inW = frame.displayWidth || frame.codedWidth || 0;
        const inH = frame.displayHeight || frame.codedHeight || 0;
        if (inW <= 0 || inH <= 0) {
            try {
                frame.close();
            } catch (e) {}
            return;
        }

        // Canvas backing = frame-aspect rect fitting the output box (else frame res).
        let cw = inW,
            ch = inH;
        if (this._hasOutputSize) {
            const frameAspect = inW / inH;
            const boxAspect = this._outW / this._outH;
            cw = frameAspect >= boxAspect ? this._outW : Math.round(this._outH * frameAspect);
            ch = Math.round(cw / frameAspect);
            // Clamp to the max texture dimension (preserve aspect): bounds GPU cost
            // when the output box is inflated by pinch-zoom.
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

        try {
            // External texture + its blit bind group are per-frame (texture expires).
            const externalTex = this._device.importExternalTexture({
                source: frame,
                colorSpace: this._importColorSpace || 'srgb',
            });
            const blitBindGroup = this._device.createBindGroup({
                layout: this._blitLayout,
                entries: [
                    { binding: 0, resource: this._sampler },
                    { binding: 1, resource: externalTex },
                ],
            });

            const encoder = this._device.createCommandEncoder();

            // 'off' mode: single pass, external → canvas (linear scale, no upscaler).
            if (this._algo === 'off') {
                const pass = encoder.beginRenderPass({
                    colorAttachments: [
                        {
                            view: this.ctx.getCurrentTexture().createView(),
                            loadOp: 'clear',
                            storeOp: 'store',
                            clearValue: { r: 0, g: 0, b: 0, a: 1 },
                        },
                    ],
                });
                pass.setPipeline(this._passthroughPipeline);
                pass.setBindGroup(0, blitBindGroup);
                pass.draw(3);
                pass.end();
                this._device.queue.submit([encoder.finish()]);
                frame.close();
                // Backpressure: resolve only when the GPU finished, so the caller's
                // render guard reflects real GPU throughput (drop-to-latest then
                // discards the backlog instead of letting it grow → no lag creep).
                try {
                    await this._device.queue.onSubmittedWorkDone();
                } catch (e) {}
                return;
            }

            this._ensureInputTex(inW, inH);

            // Pass 0: external → inputTex (frame res).
            const p0 = encoder.beginRenderPass({
                colorAttachments: [
                    {
                        view: this._inputTexView,
                        loadOp: 'clear',
                        storeOp: 'store',
                        clearValue: { r: 0, g: 0, b: 0, a: 1 },
                    },
                ],
            });
            p0.setPipeline(this._blitPipeline);
            p0.setBindGroup(0, blitBindGroup);
            p0.draw(3);
            p0.end();

            if (this._algo === 'fsr1') {
                this._ensureIntermTex(cw, ch);
                // EASU uniform = (inW, inH, outW, outH) in pixels.
                this._device.queue.writeBuffer(
                    this._easuUniform,
                    0,
                    new Float32Array([inW, inH, cw, ch]),
                );
                // RCAS samples intermTex (cw×ch) at TEXEL offsets → output* = 1/cw, 1/ch.
                this._device.queue.writeBuffer(
                    this._rcasUniform,
                    0,
                    new Float32Array([cw, ch, 1 / cw, 1 / ch]),
                );

                // Pass 1: EASU inputTex → intermTex (output res).
                const p1 = encoder.beginRenderPass({
                    colorAttachments: [
                        {
                            view: this._intermTexView,
                            loadOp: 'clear',
                            storeOp: 'store',
                            clearValue: { r: 0, g: 0, b: 0, a: 1 },
                        },
                    ],
                });
                p1.setPipeline(this._easuPipeline);
                p1.setBindGroup(0, this._enhanceBindGroup);
                p1.draw(3);
                p1.end();

                // Pass 2: RCAS intermTex → canvas.
                const p2 = encoder.beginRenderPass({
                    colorAttachments: [
                        {
                            view: this.ctx.getCurrentTexture().createView(),
                            loadOp: 'clear',
                            storeOp: 'store',
                            clearValue: { r: 0, g: 0, b: 0, a: 1 },
                        },
                    ],
                });
                p2.setPipeline(this._rcasPipeline);
                p2.setBindGroup(0, this._rcasBindGroup);
                p2.draw(3);
                p2.end();
            } else {
                // SGSR: viewport = (1/inW, 1/inH, inW, inH).
                this._device.queue.writeBuffer(
                    this._uniformBuf,
                    0,
                    new Float32Array([1 / inW, 1 / inH, inW, inH]),
                );
                // Pass 1: SGSR inputTex → canvas.
                const p1 = encoder.beginRenderPass({
                    colorAttachments: [
                        {
                            view: this.ctx.getCurrentTexture().createView(),
                            loadOp: 'clear',
                            storeOp: 'store',
                            clearValue: { r: 0, g: 0, b: 0, a: 1 },
                        },
                    ],
                });
                p1.setPipeline(this._enhancePipeline);
                p1.setBindGroup(0, this._enhanceBindGroup);
                p1.draw(3);
                p1.end();
            }

            this._device.queue.submit([encoder.finish()]);
        } catch (e) {
            console.error('[WebGpuRenderer] draw failed:', e.message);
        }

        frame.close();
        // Backpressure: resolve only when the GPU finished this frame's work, so
        // the caller's render guard self-paces to GPU capacity. Without it the
        // VSync-off / worker path submits faster than the GPU presents and the
        // backlog grows unbounded → progressive latency (4K + FSR1 + zoom).
        try {
            await this._device.queue.onSubmittedWorkDone();
        } catch (e) {}
    }

    dispose() {
        this._disposed = true;
        this._ready = false;
        try {
            if (this._inputTex) this._inputTex.destroy();
        } catch (e) {}
        try {
            if (this._intermTex) this._intermTex.destroy();
        } catch (e) {}
        try {
            if (this._device) this._device.destroy();
        } catch (e) {}
        this._inputTex = null;
        this._intermTex = null;
        this._device = null;
        this.ctx = null;
        this.canvas = null;
    }
}
