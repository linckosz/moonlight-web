// mwsr-yuv.js — MWSR on the planes of a YUV frame, for the lab only (plan B,
// docs/design/upscaler-mwsr-b.md §3). The variants are texts derived from the
// product's own (MwsrShader.js's mwsrWgsl), each change checked to hit its
// line exactly once, so that a kernel changed in the product breaks the lab
// loudly instead of drifting from it (V1 and V2 do since the kernel J, 03/11/2026:
// their lines are the kernel before it, whose results stay in the journal, and
// Bruno took J on every device):
//   V1  Y through MWSR into a plane at the output size (yKernel, OUT 'plane'),
//       then composed with UV by the sampler's bilinear (composeWgsl): the
//       model of moonlight-qt's vt_metal.mm (MetalFX on Y, MPS on UV);
//   V2  Y through MWSR and UV by the bilinear, in one pass (yKernel, OUT 'rgb');
//   V3  Y, Cb and Cr through MWSR, the weights from Y (rawKernel): by
//       linearity the RGB kernel's picture, up to the texels' bound;
//   V4  each tap's Y and UV turned into RGB, then the RGB kernel (rgbKernel):
//       what the external texture does, on planes of our own.
// The planes: Y in r8unorm (r16float at 10 bits), UV in rg8unorm (rg16float),
// half the size on each axis at 4:2:0 (NV12, P010), the same at 4:4:4 (NV24);
// BT.709, limited range, the chroma sited at the centre of its 2×2 block, as
// Chrome samples it; each value rounded to its 8- or 10-bit code. The kernels
// read Y as full-range luma (Y0, YS), so that the vote's threshold means what
// it means on RGB.

const KR = 0.2126;
const KB = 0.0722;

// The conversion both ways, by the planes' depth (BITS 8 or 10).
export const YUV_COMMON_WGSL = /* wgsl */ `
override BITS : u32 = 8u;
override Y0 : f32 = select(16.0 / 255.0, 64.0 / 1023.0, BITS == 10u);
override YS : f32 = select(255.0 / 219.0, 1023.0 / 876.0, BITS == 10u);
override C0 : f32 = select(128.0 / 255.0, 512.0 / 1023.0, BITS == 10u);
override CS : f32 = select(255.0 / 224.0, 1023.0 / 896.0, BITS == 10u);
const KR : f32 = ${KR};
const KB : f32 = ${KB};
const KG : f32 = ${1 - KR - KB};

// RGB 0..1 → the planes' codes (Y', Cb, Cr), rounded to BITS.
fn toCodes(c : vec3f) -> vec3f {
    let y = dot(c, vec3f(KR, KG, KB));
    let n = vec3f(y, (c.b - y) / (2.0 * (1.0 - KB)), (c.r - y) / (2.0 * (1.0 - KR)));
    let m = select(255.0, 1023.0, BITS == 10u);
    let q = vec3f(Y0 + n.x / YS, C0 + n.y / CS, C0 + n.z / CS);
    return round(q * m) / m;
}

// Full-range luma and centred chroma → RGB.
fn rgbOf(y : f32, cb : f32, cr : f32) -> vec3f {
    let r = y + 2.0 * (1.0 - KR) * cr;
    let b = y + 2.0 * (1.0 - KB) * cb;
    return vec3f(r, (y - KR * r - KB * b) / KG, b);
}

// A UV plane's codes → centred chroma.
fn chroma(c : vec2f) -> vec2f {
    return (c - C0) * CS;
}
`;

// The feed of each kernel, in place of the 'tex' feed's S and L: the Y plane
// stays `tex` (binding 1, whose size texelBound reads), UV is binding 3.
const UV_DECL = "@group(0) @binding(3) var texUV : texture_2d<f32>;";
const UV_AT =
  "vec2i(c * vec2i(textureDimensions(texUV)) / vec2i(textureDimensions(tex)))";
const FEED = {
  y: `fn S(uv : vec2f) -> f32 {
    return (textureSampleLevel(tex, samp, uv, 0.0).r - Y0) * YS;
}
fn L(c : vec2i) -> f32 {
    return (textureLoad(tex, c, 0).r - Y0) * YS;
}`,
  raw: `fn S(uv : vec2f) -> vec3f {
    let y = (textureSampleLevel(tex, samp, uv, 0.0).r - Y0) * YS;
    return vec3f(y, chroma(textureSampleLevel(texUV, samp, uv, 0.0).rg));
}
fn L(c : vec2i) -> vec3f {
    let y = (textureLoad(tex, c, 0).r - Y0) * YS;
    return vec3f(y, chroma(textureLoad(texUV, ${UV_AT}, 0).rg));
}`,
  rgb: `fn S(uv : vec2f) -> vec3f {
    let y = (textureSampleLevel(tex, samp, uv, 0.0).r - Y0) * YS;
    let c = chroma(textureSampleLevel(texUV, samp, uv, 0.0).rg);
    return rgbOf(y, c.x, c.y);
}
fn L(c : vec2i) -> vec3f {
    let y = (textureLoad(tex, c, 0).r - Y0) * YS;
    let k = chroma(textureLoad(texUV, ${UV_AT}, 0).rg);
    return rgbOf(y, k.x, k.y);
}`,
};

const TEX_SAMPLE = `fn S(uv : vec2f) -> vec3f {
    return textureSampleLevel(tex, samp, uv, 0.0).rgb;
}
fn L(c : vec2i) -> vec3f {
    return textureLoad(tex, c, 0).rgb;
}`;
const TEX_DECL = "@group(0) @binding(1) var tex : texture_2d<f32>;";

// One replacement that must hit exactly once.
function once(text, from, to, what) {
  const i = text.indexOf(from);
  if (i < 0 || text.indexOf(from, i + from.length) >= 0)
    throw new Error(
      "mwsr-yuv: " +
        what +
        " found " +
        (i < 0 ? "nowhere" : "twice") +
        " in the product kernel",
    );
  return text.slice(0, i) + to + text.slice(i + from.length);
}

// From `start` to the end of the line where `end` ends, replaced; `start`
// must be single.
function span(text, start, end, to, what) {
  const i = text.indexOf(start);
  const j = text.indexOf(end, i);
  if (i < 0 || j < 0 || text.indexOf(start, i + 1) >= 0)
    throw new Error("mwsr-yuv: " + what + " not found in the product kernel");
  const k = text.indexOf("\n", j + end.length - 1);
  return text.slice(0, i) + to + text.slice(k < 0 ? text.length : k);
}

// The feed swapped in the 'tex' text, UV declared, the conversion added.
function withFeed(code, feed) {
  code = once(code, TEX_SAMPLE, FEED[feed], "the tex feed");
  return once(
    code,
    TEX_DECL,
    TEX_DECL + "\n" + UV_DECL + "\n" + YUV_COMMON_WGSL,
    "the tex binding",
  );
}

// The two returns of the fragment stage, the flat path's and the edge path's.
function withOut(code, out) {
  code = once(
    code,
    "return vec4f(flatPath(uv, C, zg), 1.0);",
    "return OUT(uv, flatPath(uv, C, zg));",
    "the flat return",
  );
  return once(
    code,
    "    return vec4f(R, 1.0);\n}",
    "    return OUT(uv, R);\n}\n" + out,
    "the edge return",
  );
}

const OUT_RGB_FROM_Y = `
// V2: the luma MWSR drew, the chroma by the bilinear at the pixel.
fn OUT(uv : vec2f, y : f32) -> vec4f {
    let c = chroma(textureSampleLevel(texUV, samp, uv, 0.0).rg);
    return vec4f(rgbOf(y, c.x, c.y), 1.0);
}
`;
const OUT_PLANE = `
// V1: the luma alone, into a plane at the output size.
fn OUT(uv : vec2f, y : f32) -> vec4f {
    return vec4f(y, 0.0, 0.0, 1.0);
}
`;
const OUT_RGB_FROM_RAW = `
// V3: (Y, Cb, Cr) as MWSR drew them → RGB.
fn OUT(uv : vec2f, c : vec3f) -> vec4f {
    return vec4f(rgbOf(c.x, c.y, c.z), 1.0);
}
`;

/**
 * V1 and V2: MWSR on the Y plane alone, every channel of the product's kernel
 * made one (the vote on the luma's own derivatives; no CHROMA444).
 * @param {string} up the product's `up` text on the 'tex' feed (mwsrWgsl)
 * @param {'rgb'|'plane'} out V2 (RGB, UV by the bilinear) or V1 (the Y plane)
 */
export function yKernel(up, out = "rgb") {
  let c = withFeed(up, "y");
  c = once(
    c,
    "fn flatPath(uv : vec2f, C : vec3f, zg : f32) -> vec3f {",
    "fn flatPath(uv : vec2f, C : f32, zg : f32) -> f32 {",
    "flatPath",
  );
  c = once(
    c,
    "    var R : vec3f;\n    if (FLAT_TAPS == 3u)",
    "    var R : f32;\n    if (FLAT_TAPS == 3u)",
    "flatPath's R",
  );
  c = once(
    c,
    "fn texelBound(uv : vec2f, R : vec3f) -> vec3f {",
    "fn texelBound(uv : vec2f, R : f32) -> f32 {",
    "texelBound",
  );
  c = span(
    c,
    "    var gx : f32;",
    "        flat = abs(gx) + abs(gy) < tau;\n    }",
    "    let gx = dpdxCoarse(C) * u.scale.x;\n    let gy = dpdyCoarse(C) * u.scale.y;\n    let flat = abs(gx) + abs(gy) < tau;",
    "the vote",
  );
  c = once(
    c,
    "    var R = vec3f((h3(C) + hf(wX) * h3(X1 + X2)) * hf(kB));",
    "    var R = f32((hf(C) + hf(wX) * hf(X1 + X2)) * hf(kB));",
    "the edge blend",
  );
  return withOut(c, out === "plane" ? OUT_PLANE : OUT_RGB_FROM_Y);
}

/** V3: MWSR on (Y, Cb, Cr), the luma weights (1, 0, 0): the weights from Y. */
export function rawKernel(up) {
  let c = withFeed(up, "raw");
  c = c.replace(
    /const LUMA : vec3f = vec3f\([^)]*\);/,
    () => "const LUMA : vec3f = vec3f(1.0, 0.0, 0.0);",
  );
  if (!c.includes("const LUMA : vec3f = vec3f(1.0, 0.0, 0.0);"))
    throw new Error("mwsr-yuv: LUMA not found");
  return withOut(c, OUT_RGB_FROM_RAW);
}

/** V4: each tap's planes turned into RGB, then the product's kernel as it is. */
export function rgbKernel(up) {
  return withFeed(up, "rgb");
}

const VS = /* wgsl */ `
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

/**
 * The planes of an RGB texture (binding 0, loaded): `y` the Y plane at its
 * size; `uv` the UV plane, each texel the average of the 2×2 RGB texels it
 * covers at 4:2:0 (CHROMA_DIV 2), the texel itself at 4:4:4 (1).
 */
export function planesFromRgbWgsl(which) {
  const body =
    which === "y"
      ? `let c = textureLoad(src, vec2i(in.pos.xy), 0).rgb;
    return vec4f(toCodes(c).x, 0.0, 0.0, 1.0);`
      : `let d = i32(CHROMA_DIV);
    let o = vec2i(in.pos.xy) * d;
    var c = vec3f(0.0);
    for (var j = 0; j < d; j++) {
        for (var i = 0; i < d; i++) {
            c += textureLoad(src, o + vec2i(i, j), 0).rgb;
        }
    }
    return vec4f(toCodes(c / f32(d * d)).yz, 0.0, 1.0);`;
  return `${VS}
@group(0) @binding(0) var src : texture_2d<f32>;
override CHROMA_DIV : u32 = 2u;
${YUV_COMMON_WGSL}
@fragment
fn fs(in : VSOut) -> @location(0) vec4f {
    ${body}
}
`;
}

/**
 * The planes of the decoder's frame (the external texture, sampled at the
 * centre of the target's texel): the browser's pass 0 split in two, `y` at the
 * frame's size, `uv` at the chroma's. At a chroma texel's centre the browser's
 * sample is that texel's UV with the bilinear luma of the 2×2 block: the codes
 * come back out of its RGB.
 */
export function planesFromExtWgsl(which) {
  const pick =
    which === "y"
      ? "vec4f(toCodes(c).x, 0.0, 0.0, 1.0)"
      : "vec4f(toCodes(c).yz, 0.0, 1.0)";
  return `${VS}
@group(0) @binding(0) var samp : sampler;
@group(0) @binding(1) var ext : texture_external;
${YUV_COMMON_WGSL}
@fragment
fn fs(in : VSOut) -> @location(0) vec4f {
    let c = textureSampleBaseClampToEdge(ext, samp, in.uv).rgb;
    return ${pick};
}
`;
}

/**
 * The planes back to RGB at the target's size, Y and UV by the sampler's
 * bilinear: the RGB texture a browser's pass 0 hands the kernel from a 4:2:0
 * frame (V0 on the same planes), or V1's composition, where Y is the luma
 * MWSR drew at the output size (Y_FULL 1: full-range already).
 */
export function rgbFromPlanesWgsl() {
  return `${VS}
@group(0) @binding(0) var samp : sampler;
@group(0) @binding(1) var texY : texture_2d<f32>;
@group(0) @binding(2) var texUV : texture_2d<f32>;
override Y_FULL : u32 = 0u;
${YUV_COMMON_WGSL}
@fragment
fn fs(in : VSOut) -> @location(0) vec4f {
    let s = textureSampleLevel(texY, samp, in.uv, 0.0).r;
    let y = select((s - Y0) * YS, s, Y_FULL == 1u);
    let c = chroma(textureSampleLevel(texUV, samp, in.uv, 0.0).rg);
    return vec4f(rgbOf(y, c.x, c.y), 1.0);
}
`;
}
