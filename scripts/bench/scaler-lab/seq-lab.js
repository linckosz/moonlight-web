// The sequence mode of the upscaler laboratory (docs/design/upscaler-mwsr.md,
// §14.3, P7.0): does a memory of the previous frames make mwsr better? The
// page makes a sequence out of a still picture — a sub-pixel pan, a stop, a
// cut to another picture; a faster pan; a striped block crossing a still
// picture — each frame at the stream's size and at the screen's from the same
// window of the source (Lanczos-3 in linear light, the reduction of
// scaler_lab.make_input), so that every frame's ground truth is known. The
// stream's frames go through WebCodecs at each bitrate, encode then decode
// (software under soft=1: no GPU of the bench machine), and every decoded
// frame through mwsr alone and through mwsr with a history (HIST below), side
// by side, against the frame's ground truth. The measures are made on the
// GPU, per frame and per variant (see METRICS_WGSL), the outputs of a few
// frames posted as crops for the driver's FLIP (p7_flows.py). P8.0 (§14.6)
// measures the same way a deblocking pass between the decoder and the kernel
// (the variants dbe and dbd).
//
// Driven by scaler-lab.html when the query has seq=1:
//   in=1280x720 out=2560x1440      the stream's and the screen's sizes
//   src=<url> src2=<url>           the picture, and the one after a cut (the
//                                  same size; the driver draws the text one)
//   vid=<url prefix>               P8.0: a clip instead of a picture, its
//                                  frames <prefix>000.png, 001.png…, the
//                                  warm-up's first, then the sequence's; pan
//                                  segments only, v=0, the window still on
//                                  the whole frame (box=0,0,<its width>)
//   box=bx,by,bw                   the window of the first frame in the
//                                  source, source pixels (its height follows
//                                  the output's ratio)
//   segs=pan:60,stop:20,cutpan:20  the sequence: `pan` moves the window by v
//                                  stream pixels a frame along dir, `stop`
//                                  holds it, `cut` and `cutpan` switch to src2
//                                  at the first window (held, or moving on),
//                                  `obj` holds it while a striped block
//                                  crosses at ov stream pixels a frame
//   v=0.3 dir=1,0.25 ov=8
//   kbps=20000,10000 vcodec=avc|vp9|av1   the encodes, one per bitrate
//   accel=sw|hw                    WebCodecs' acceleration (sw under soft=1,
//                                  whatever the query says)
//   hist=none,pull:a=0.5;t=6,…     the variants (HIST; dbe, dbd: deblocking)
//   mw=kg=1;ft=.01                 the kernel's options, as a lab cell's
//   dump=10,30,61 crop=x,y,w,h     the frames whose outputs (and truth) are
//                                  posted as crops of the screen-size picture,
//                                  raw RGBA rows (the width a multiple of 64)
//   roi=x,y,w,h                    the screen's region that is shaded and
//                                  measured, screen pixels (the whole screen
//                                  by default): the truth, the kernel and the
//                                  measures run there only, the history at
//                                  the stream's size on the whole frame, and
//                                  outmc's accumulation on the region and
//                                  ROI_MARGIN around it; the crops inside it
//   warm=60                        frames encoded before the sequence and
//                                  not measured, the rate control's warm-up:
//                                  the first segment's motion played
//                                  backwards up to the first frame (see
//                                  warmFrames), as many as fit in the source
//   tag=<name>                     the names of the posted crops
//   soft=1 fallback=1              the software adapter, as in the whole lab

const MWSR_URL = "/frontend/js/stream/renderers/MwsrShader.js";

// outmc's margin around the region (roi), screen pixels. Its history is
// fetched where the content was, and a pixel whose history lies beyond the
// drawn rectangle starts again, as at the screen's edge: 128 pixels leave the
// region ten frames of history at least at the rapid pan's 12 pixels a frame.
const ROI_MARGIN = 128;

// The variants: a history at the stream's size between the decoded frame C and
// the kernel (H' = f(C, H), the kernel reads H', H' is the next frame's H), or
// none, or (outmc) the kernel's output itself accumulated at the screen's size.
//   none             the product's chain: the kernel on the decoded frame
//   pull:a;t         H' = C + a·clamp(H − C, ±t/255) per channel: the
//                    history's pull bounded, no neighbourhood — one texture
//                    read more than pass 0, which it could live in
//   hard:a;t         mix(C, H, a) where the luma moved less than t/255, C
//                    elsewhere (the rule of plan §14.2, without its bound)
//   soft:a;t         the weight falls from a to 0 as the luma difference
//                    grows to t/255
//   clamp:a          H clamped into C's 3×3 neighbourhood, then mix(C, H, a): a
//                    TAA without motion vectors
//   mcpull, mcclamp  pull and clamp with the history fetched where the content
//                    was (Catmull-Rom, the exact motion the page made): an
//                    oracle, the ceiling of any motion estimate
//   outmc:a          the kernel's output accumulated at the screen's size, the
//                    history fetched where the content was and clamped into the
//                    output's 3×3 neighbourhood: the ceiling of an accumulation
//                    at the output's size, the one road to resolution (§14.2)
//   dbe:s            P8.0: no history; the decoded frame deblocked before the
//                    kernel, the grid's steps smoothed (vertical edges, then
//                    horizontal: an H.264-like filter, blind, strength s)
//   dbd:t;n;c        P8.0: shifted 8×8 DCTs of the frame, the coefficients
//                    under t zeroed (the DC kept), n = 16 or 4 shifts averaged,
//                    the change of a pixel's luma bounded by c (0: unbounded)
// Options: a (the history's weight), t (in 1/255 of the encoded value), f=8 or
// 16 (the history's texture: rgba8unorm, or rgba16float, the default). For
// dbe and dbd, s, t and c are in code values of the stream's Y (limited range),
// and f=16 the deblocked frame's texture (rgba8unorm by default, as a product
// pass would write it). The deblocking reads the luma of the decoded RGB and
// adds its change to the three channels: the product's frame is RGB.
const HIST_MODES = {
  none: 0,
  pull: 1,
  hard: 2,
  soft: 3,
  clamp: 4,
  mcpull: 1,
  mcclamp: 4,
  outmc: 0,
  dbe: 0,
  dbd: 0,
};

// The stream's region the deblocking works on: the kernel's view of the roi,
// and this many stream pixels around it (the kernel's taps, the edge filter's
// reach, the DCT's block), on the 8×8 grid.
const DB_MARGIN = 16;

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

// BT.709 (the stream's matrix, limited range both ways), the sRGB transfer,
// Lanczos-3, and Catmull-Rom's weights for the history's fetch.
const COMMON = /* wgsl */ `
const KR : f32 = 0.2126;
const KB : f32 = 0.0722;
const KG : f32 = 0.7152;
const L709 : vec3f = vec3f(0.2126, 0.7152, 0.0722);

fn srgbToLin(c : vec3f) -> vec3f {
    return select(pow((c + 0.055) / 1.055, vec3f(2.4)), c / 12.92, c <= vec3f(0.04045));
}

fn linToSrgb(c : vec3f) -> vec3f {
    return select(1.055 * pow(c, vec3f(1.0 / 2.4)) - 0.055, c * 12.92, c <= vec3f(0.0031308));
}

fn lanczos3(x : f32) -> f32 {
    let ax = abs(x);
    if (ax < 1e-4) {
        return 1.0;
    }
    if (ax >= 3.0) {
        return 0.0;
    }
    let px = 3.14159265 * x;
    return 3.0 * sin(px) * sin(px / 3.0) / (px * px);
}

fn cr4(x : f32) -> vec4f {
    let x2 = x * x;
    let x3 = x2 * x;
    return vec4f(-0.5 * x3 + x2 - 0.5 * x, 1.5 * x3 - 2.5 * x2 + 1.0,
                 -1.5 * x3 + 2.0 * x2 + 0.5 * x, 0.5 * x3 - 0.5 * x2);
}
`;

// The history's fetch at a position in pixels (centres at +0.5): Catmull-Rom
// over the 4×4 texels around, by loads (the lab's, not a renderer's cost).
const FETCH_CR = /* wgsl */ `
fn fetchCR(p : vec2f) -> vec3f {
    let dims = vec2i(textureDimensions(hist));
    let q = p - 0.5;
    let i0 = vec2i(floor(q));
    let f = q - floor(q);
    let wx = cr4(f.x);
    let wy = cr4(f.y);
    var acc = vec3f(0.0);
    for (var j = 0; j < 4; j++) {
        var row = vec3f(0.0);
        for (var i = 0; i < 4; i++) {
            let c = clamp(i0 + vec2i(i - 1, j - 1), vec2i(0), dims - vec2i(1));
            row += wx[i] * textureLoad(hist, c, 0).rgb;
        }
        acc += wy[j] * row;
    }
    return acc;
}
`;

// The window of the source drawn at a size: Lanczos-3 in linear light, the
// filter widened by the reduction, rows then columns. The horizontal pass reads
// every row of the source (and draws the obj sequence's block into it), the
// vertical pass encodes back to sRGB.
const LANCZOS_UNIFORM = /* wgsl */ `
struct LU {
    box : vec4f,     // the window: x, y, width, height, source pixels
    dst : vec4f,     // the destination's width and height, the source's
    sprite : vec4f,  // the block: x0, y0, x1, y1, source pixels (x1 <= x0: none)
};
`;

const LANCZOS_H_WGSL = /* wgsl */ `
${VS}
${COMMON}
${LANCZOS_UNIFORM}
@group(0) @binding(0) var src : texture_2d<f32>;
@group(0) @binding(1) var<uniform> u : LU;

// The block of the obj sequence, in linear light: a pale border, then red and
// dark blue stripes across its height — edges on every side, and a texture.
fn block(l : vec2f, size : vec2f) -> vec3f {
    let b = 0.06 * min(size.x, size.y);
    if (l.x < b || l.y < b || l.x > size.x - b || l.y > size.y - b) {
        return vec3f(0.9);
    }
    let stripe = fract(l.y / (0.16 * size.y)) < 0.5;
    return select(vec3f(0.02, 0.03, 0.2), vec3f(0.8, 0.12, 0.05), stripe);
}

fn texel(x : i32, y : i32) -> vec3f {
    let c = vec2f(f32(x) + 0.5, f32(y) + 0.5);
    if (u.sprite.z > u.sprite.x && c.x >= u.sprite.x && c.x < u.sprite.z &&
        c.y >= u.sprite.y && c.y < u.sprite.w) {
        return block(c - u.sprite.xy, u.sprite.zw - u.sprite.xy);
    }
    let q = clamp(vec2i(x, y), vec2i(0), vec2i(u.dst.zw) - vec2i(1));
    return srgbToLin(textureLoad(src, q, 0).rgb);
}

@fragment
fn fs(in : VSOut) -> @location(0) vec4f {
    let s = u.box.z / u.dst.x;
    let fw = max(s, 1.0);
    let xs = u.box.x + in.pos.x * s;
    let r = 3.0 * fw;
    let y = i32(in.pos.y);
    var acc = vec3f(0.0);
    var wsum = 0.0;
    for (var j = i32(ceil(xs - r - 0.5)); j <= i32(floor(xs + r - 0.5)); j++) {
        let w = lanczos3((f32(j) + 0.5 - xs) / fw);
        acc += w * texel(j, y);
        wsum += w;
    }
    return vec4f(acc / wsum, 1.0);
}
`;

const LANCZOS_V_WGSL = /* wgsl */ `
${VS}
${COMMON}
${LANCZOS_UNIFORM}
@group(0) @binding(0) var mid : texture_2d<f32>;
@group(0) @binding(1) var<uniform> u : LU;

@fragment
fn fs(in : VSOut) -> @location(0) vec4f {
    let s = u.box.w / u.dst.y;
    let fw = max(s, 1.0);
    let ys = u.box.y + in.pos.y * s;
    let r = 3.0 * fw;
    let x = i32(in.pos.x);
    let hmax = i32(u.dst.w) - 1;
    var acc = vec3f(0.0);
    var wsum = 0.0;
    for (var j = i32(ceil(ys - r - 0.5)); j <= i32(floor(ys + r - 0.5)); j++) {
        let w = lanczos3((f32(j) + 0.5 - ys) / fw);
        acc += w * textureLoad(mid, vec2i(x, clamp(j, 0, hmax)), 0).rgb;
        wsum += w;
    }
    return vec4f(linToSrgb(clamp(acc / wsum, vec3f(0.0), vec3f(1.0))), 1.0);
}
`;

// RGB → 4:2:0 (BT.709, limited range), the planes packed four bytes a word: Y,
// then Cb, then Cr; the chroma from the 2×2 mean of the encoded values, centred
// as the chroma texels are read back (yuv2rgb's bilinear sample).
const YUV_WGSL = /* wgsl */ `
${COMMON}
struct YU {
    w : u32,
    h : u32,
    pad0 : u32,
    pad1 : u32,
};
@group(0) @binding(0) var img : texture_2d<f32>;
@group(0) @binding(1) var<storage, read_write> outb : array<u32>;
@group(0) @binding(2) var<uniform> u : YU;

@compute @workgroup_size(64)
fn csY(@builtin(global_invocation_id) g : vec3u) {
    let per = u.w / 4u;
    if (g.x >= per * u.h) {
        return;
    }
    let row = g.x / per;
    let col = (g.x % per) * 4u;
    var word = 0u;
    for (var k = 0u; k < 4u; k++) {
        let c = textureLoad(img, vec2u(col + k, row), 0).rgb;
        let y = clamp(round(16.0 + 219.0 * dot(c, L709)), 0.0, 255.0);
        word |= u32(y) << (8u * k);
    }
    outb[g.x] = word;
}

@compute @workgroup_size(64)
fn csC(@builtin(global_invocation_id) g : vec3u) {
    let per = u.w / 8u;
    let n = per * (u.h / 2u);
    if (g.x >= n) {
        return;
    }
    let row = g.x / per;
    let col = (g.x % per) * 4u;
    var wu = 0u;
    var wv = 0u;
    for (var k = 0u; k < 4u; k++) {
        let x = (col + k) * 2u;
        let y = row * 2u;
        let c = (textureLoad(img, vec2u(x, y), 0).rgb + textureLoad(img, vec2u(x + 1u, y), 0).rgb +
                 textureLoad(img, vec2u(x, y + 1u), 0).rgb + textureLoad(img, vec2u(x + 1u, y + 1u), 0).rgb) * 0.25;
        let yy = dot(c, L709);
        let cb = clamp(round(128.0 + 224.0 * (c.b - yy) / 1.8556), 0.0, 255.0);
        let cr = clamp(round(128.0 + 224.0 * (c.r - yy) / 1.5748), 0.0, 255.0);
        wu |= u32(cb) << (8u * k);
        wv |= u32(cr) << (8u * k);
    }
    let yWords = (u.w * u.h) / 4u;
    outb[yWords + g.x] = wu;
    outb[yWords + n + g.x] = wv;
}
`;

// The decoded planes back to RGB: what the product's pass 0 makes of the
// external texture (BT.709, limited range, the chroma bilinear).
const YUV2RGB_WGSL = /* wgsl */ `
${VS}
${COMMON}
@group(0) @binding(0) var ty : texture_2d<f32>;
@group(0) @binding(1) var tu : texture_2d<f32>;
@group(0) @binding(2) var tv : texture_2d<f32>;
@group(0) @binding(3) var samp : sampler;

@fragment
fn fs(in : VSOut) -> @location(0) vec4f {
    let y = textureLoad(ty, vec2i(in.pos.xy), 0).r * 255.0;
    let cb = textureSampleLevel(tu, samp, in.uv, 0.0).r * 255.0;
    let cr = textureSampleLevel(tv, samp, in.uv, 0.0).r * 255.0;
    let yn = (y - 16.0) / 219.0;
    let r = yn + 1.5748 * (cr - 128.0) / 224.0;
    let b = yn + 1.8556 * (cb - 128.0) / 224.0;
    let g = (yn - KR * r - KB * b) / KG;
    return vec4f(clamp(vec3f(r, g, b), vec3f(0.0), vec3f(1.0)), 1.0);
}
`;

// The history at the stream's size (the variants of HIST_MODES): the decoded
// frame C, the history H (fetched where the content was when mc is 1), the
// next history out, which the kernel reads.
const HIST_WGSL = /* wgsl */ `
${VS}
${COMMON}
struct HU {
    alpha : f32,
    tau : f32,
    mode : u32,
    reset : u32,
    mv : vec2f,      // where this pixel's content was in the previous frame, pixels
    mc : u32,
    pad : u32,
};
@group(0) @binding(0) var cur : texture_2d<f32>;
@group(0) @binding(1) var hist : texture_2d<f32>;
@group(0) @binding(2) var<uniform> u : HU;
${FETCH_CR}

@fragment
fn fs(in : VSOut) -> @location(0) vec4f {
    let ip = vec2i(in.pos.xy);
    let C = textureLoad(cur, ip, 0).rgb;
    if (u.reset == 1u) {
        return vec4f(C, 1.0);
    }
    let dims = vec2i(textureDimensions(cur));
    var H : vec3f;
    if (u.mc == 1u) {
        let q = in.pos.xy + u.mv;
        if (any(q < vec2f(0.0)) || any(q >= vec2f(dims))) {
            return vec4f(C, 1.0);
        }
        H = clamp(fetchCR(q), vec3f(0.0), vec3f(1.0));
    } else {
        H = textureLoad(hist, ip, 0).rgb;
    }
    if (u.mode == 4u) {
        var lo = C;
        var hi = C;
        for (var j = -1; j <= 1; j++) {
            for (var i = -1; i <= 1; i++) {
                let c = textureLoad(cur, clamp(ip + vec2i(i, j), vec2i(0), dims - vec2i(1)), 0).rgb;
                lo = min(lo, c);
                hi = max(hi, c);
            }
        }
        return vec4f(mix(C, clamp(H, lo, hi), u.alpha), 1.0);
    }
    if (u.mode == 1u) {
        return vec4f(C + u.alpha * clamp(H - C, vec3f(-u.tau), vec3f(u.tau)), 1.0);
    }
    let dY = abs(dot(H - C, L709));
    if (u.mode == 2u) {
        return vec4f(select(C, mix(C, H, u.alpha), dY < u.tau), 1.0);
    }
    let w = u.alpha * clamp(1.0 - dY / max(u.tau, 1e-5), 0.0, 1.0);
    return vec4f(mix(C, H, w), 1.0);
}
`;

// P8.0's deblocking: what the passes read (the frame's size, the region of
// the stream they work on, a shift or a direction, the strength).
const DB_COMMON = /* wgsl */ `
struct DU {
    size : vec2i,
    base : vec2i,
    hi : vec2i,
    shift : vec2i,
    thr : f32,
    n : f32,
    cmax : f32,
    pad1 : f32,
};
`;

// The orthonormal 8-point DCT, D[k * 8 + n].
const DCT8 = Array.from({ length: 64 }, (_, i) => {
  const k = Math.floor(i / 8);
  const c = Math.sqrt((k ? 2 : 1) / 8);
  return (c * Math.cos((Math.PI * (2 * (i % 8) + 1) * k) / 16)).toFixed(9);
}).join(", ");

// One shift of the DCT shrinkage: a workgroup per 8×8 block of the shifted
// grid, the luma's coefficients under thr zeroed (the DC kept), the block
// added into acc (one block per pixel and shift: no two writers).
const DB_DCT_WGSL = /* wgsl */ `
${COMMON}
${DB_COMMON}
@group(0) @binding(0) var src : texture_2d<f32>;
@group(0) @binding(1) var<storage, read_write> acc : array<f32>;
@group(0) @binding(2) var<uniform> u : DU;

var<private> D8 : array<f32, 64> = array<f32, 64>(${DCT8});
var<workgroup> sa : array<f32, 64>;
var<workgroup> sb : array<f32, 64>;

fn lumaMirror(p : vec2i) -> f32 {
    var q = select(p, -p, p < vec2i(0));
    q = select(q, 2 * (u.size - 1) - q, q >= u.size);
    return dot(textureLoad(src, q, 0).rgb, L709) * 255.0;
}

@compute @workgroup_size(8, 8)
fn cs(@builtin(workgroup_id) wg : vec3u, @builtin(local_invocation_id) l : vec3u) {
    let i = l.x;
    let j = l.y;
    let p = u.base + vec2i(wg.xy) * 8 - vec2i(8) + u.shift + vec2i(l.xy);
    sa[j * 8u + i] = lumaMirror(p);
    workgroupBarrier();
    var a = 0.0;
    for (var n = 0u; n < 8u; n++) {
        a += D8[i * 8u + n] * sa[j * 8u + n];
    }
    sb[j * 8u + i] = a;
    workgroupBarrier();
    var c = 0.0;
    for (var m = 0u; m < 8u; m++) {
        c += D8[j * 8u + m] * sb[m * 8u + i];
    }
    if ((i != 0u || j != 0u) && abs(c) < u.thr) {
        c = 0.0;
    }
    sa[j * 8u + i] = c;
    workgroupBarrier();
    var b = 0.0;
    for (var k = 0u; k < 8u; k++) {
        b += D8[k * 8u + j] * sa[k * 8u + i];
    }
    sb[j * 8u + i] = b;
    workgroupBarrier();
    var r = 0.0;
    for (var k = 0u; k < 8u; k++) {
        r += D8[k * 8u + i] * sb[j * 8u + k];
    }
    if (all(p >= u.base) && all(p < u.hi)) {
        let idx = u32(p.y) * u32(u.size.x) + u32(p.x);
        acc[idx] = acc[idx] + r;
    }
}
`;

// The shrinkage's end: the shifts' mean luma, its change (bounded by cmax if
// set) added to the RGB.
const DB_FIN_WGSL = /* wgsl */ `
${VS}
${COMMON}
${DB_COMMON}
@group(0) @binding(0) var src : texture_2d<f32>;
@group(0) @binding(1) var<storage, read> acc : array<f32>;
@group(0) @binding(2) var<uniform> u : DU;

@fragment
fn fs(in : VSOut) -> @location(0) vec4f {
    let p = vec2i(in.pos.xy);
    let c = textureLoad(src, p, 0);
    let y = dot(c.rgb, L709);
    let yd = acc[u32(p.y) * u32(u.size.x) + u32(p.x)] / (255.0 * u.n);
    var dy = yd - y;
    if (u.cmax > 0.0) {
        dy = clamp(dy, -u.cmax / 255.0, u.cmax / 255.0);
    }
    return vec4f(clamp(c.rgb + vec3f(dy), vec3f(0.0), vec3f(1.0)), c.a);
}
`;

// The edge filter, one direction (u.shift: (1, 0) the vertical edges, (0, 1)
// the horizontal ones): a step on the 8×8 grid under alpha that the pixels on
// each side do not explain (under beta) is spread over p1..q1, or p2..q2
// where both sides are flat, as H.264's filter does in the decoder's loop.
const DB_EDGE_WGSL = /* wgsl */ `
${VS}
${COMMON}
${DB_COMMON}
@group(0) @binding(0) var src : texture_2d<f32>;
@group(0) @binding(1) var<uniform> u : DU;

fn lumaAt(p : vec2i) -> f32 {
    return dot(textureLoad(src, clamp(p, vec2i(0), u.size - 1), 0).rgb, L709) * 255.0;
}

@fragment
fn fs(in : VSOut) -> @location(0) vec4f {
    let p = vec2i(in.pos.xy);
    let c = textureLoad(src, p, 0);
    let dir = u.shift;
    let x = dot(p, dir);
    let len = dot(u.size, dir);
    let k = x % 8;
    let e = select(x - k + 8, x - k, k <= 2);
    let o = x - e;
    if (k == 3 || k == 4 || e < 4 || e > len - 4) {
        return c;
    }
    // p3 p2 p1 p0 | q0 q1 q2 q3
    var L : array<f32, 8>;
    for (var i = 0; i < 8; i++) {
        L[i] = lumaAt(p + dir * (i - 4 - o));
    }
    let alpha = 3.0 * u.thr;
    let beta = 0.5 * u.thr + 1.0;
    let tc = 0.5 * u.thr;
    let d = L[4] - L[3];
    if (!(abs(d) < alpha && abs(L[2] - L[3]) < beta && abs(L[5] - L[4]) < beta)) {
        return c;
    }
    let ap = abs(L[1] - L[3]) < beta;
    let aq = abs(L[6] - L[4]) < beta;
    // the new p2 p1 p0 q0 q1 q2
    var nv : array<f32, 6>;
    if (ap && aq && abs(d) < alpha / 4.0 + 2.0) {
        nv[0] = (2.0 * L[0] + 3.0 * L[1] + L[2] + L[3] + L[4]) / 8.0;
        nv[1] = (L[1] + L[2] + L[3] + L[4]) / 4.0;
        nv[2] = (L[1] + 2.0 * L[2] + 2.0 * L[3] + 2.0 * L[4] + L[5]) / 8.0;
        nv[3] = (L[6] + 2.0 * L[5] + 2.0 * L[4] + 2.0 * L[3] + L[2]) / 8.0;
        nv[4] = (L[6] + L[5] + L[4] + L[3]) / 4.0;
        nv[5] = (2.0 * L[7] + 3.0 * L[6] + L[5] + L[4] + L[3]) / 8.0;
    } else {
        let tce = tc + f32(ap) + f32(aq);
        let delta = clamp((4.0 * d + (L[2] - L[5])) / 8.0, -tce, tce);
        let mid = (L[3] + L[4]) / 2.0;
        nv[0] = L[1];
        nv[1] = select(L[2], L[2] + clamp((L[1] + mid - 2.0 * L[2]) / 2.0, -tc, tc), ap);
        nv[2] = L[3] + delta;
        nv[3] = L[4] - delta;
        nv[4] = select(L[5], L[5] + clamp((L[6] + mid - 2.0 * L[5]) / 2.0, -tc, tc), aq);
        nv[5] = L[6];
    }
    let v = L[o + 4];
    return vec4f(clamp(c.rgb + vec3f(nv[o + 3] - v) / 255.0, vec3f(0.0), vec3f(1.0)), c.a);
}
`;

// The output's accumulation (outmc): the kernel's picture K, the history fetched
// where the content was, clamped into K's 3×3 neighbourhood, mixed; written
// twice — the next history (rgba16float) and the picture shown (rgba8unorm,
// as the canvas would hold it).
const ACC_WGSL = /* wgsl */ `
${VS}
${COMMON}
struct AU {
    alpha : f32,
    reset : u32,
    mv : vec2f,
    box : vec4f,     // the drawn rectangle x0, y0, x1, y1: no history beyond
};
@group(0) @binding(0) var kout : texture_2d<f32>;
@group(0) @binding(1) var hist : texture_2d<f32>;
@group(0) @binding(2) var<uniform> u : AU;
${FETCH_CR}

struct FO {
    @location(0) acc : vec4f,
    @location(1) disp : vec4f,
};

@fragment
fn fs(in : VSOut) -> FO {
    let ip = vec2i(in.pos.xy);
    let K = textureLoad(kout, ip, 0).rgb;
    let lo0 = vec2i(u.box.xy);
    let hi0 = vec2i(u.box.zw) - vec2i(1);
    let q = in.pos.xy + u.mv;
    var o = K;
    // the Catmull-Rom taps inside the rectangle too: two pixels in
    if (u.reset == 0u && all(q >= u.box.xy + vec2f(2.0)) && all(q < u.box.zw - vec2f(2.0))) {
        var lo = K;
        var hi = K;
        for (var j = -1; j <= 1; j++) {
            for (var i = -1; i <= 1; i++) {
                let c = textureLoad(kout, clamp(ip + vec2i(i, j), lo0, hi0), 0).rgb;
                lo = min(lo, c);
                hi = max(hi, c);
            }
        }
        o = mix(K, clamp(fetchCR(q), lo, hi), u.alpha);
    }
    return FO(vec4f(o, 1.0), vec4f(o, 1.0));
}
`;

// The measures of one output against its ground truth, per pixel, summed per
// workgroup into `partials` (REDUCE_WGSL sums those):
//   0 the squared error, the three channels' sum (encoded values: the PSNR)
//   1 |ΔE|: the change of the luma's error since the previous frame (E = out −
//     truth), the instability: a stable picture's error moves with the
//     picture, a flickering one's jumps; nothing on the first frame
//   2 |Δout|: the change of the output's luma — the flicker where the truth
//     holds still (the stop), equal to 1 there
//   3, 4, 5 the squared error, the count and |ΔE| on the truth's edges (its
//     luma's central differences above edgeTh), where the eye sees crawl
//   6, 7 the squared error and the count inside `rect` (the obj sequence's
//     wake: where the block was in the last five frames and is no more)
// The output's luma is kept per pixel for the next frame (prevOutY), the
// truth's by ADVANCE_WGSL once every variant of the frame is measured.
const METRICS_UNIFORM = /* wgsl */ `
struct MU {
    size : vec2u,
    rect0 : vec2u,
    rect1 : vec2u,
    temporal : u32,
    edgeTh : f32,
    roi0 : vec2u,    // the region measured: from roi0 to roi1, excluded
    roi1 : vec2u,
};
`;

const METRICS_WGSL = /* wgsl */ `
${COMMON}
${METRICS_UNIFORM}
@group(0) @binding(0) var outT : texture_2d<f32>;
@group(0) @binding(1) var gtT : texture_2d<f32>;
@group(0) @binding(2) var<storage, read_write> prevOutY : array<f32>;
@group(0) @binding(3) var<storage, read> prevGtY : array<f32>;
@group(0) @binding(4) var<storage, read_write> partials : array<f32>;
@group(0) @binding(5) var<uniform> u : MU;

var<workgroup> sh : array<array<f32, 8>, 256>;

fn gy(p : vec2i) -> f32 {
    let q = clamp(p, vec2i(0), vec2i(u.size) - vec2i(1));
    return dot(textureLoad(gtT, q, 0).rgb, L709);
}

@compute @workgroup_size(16, 16)
fn cs(@builtin(global_invocation_id) gid : vec3u,
      @builtin(local_invocation_index) li : u32,
      @builtin(workgroup_id) wid : vec3u,
      @builtin(num_workgroups) nw : vec3u) {
    var m : array<f32, 8>;
    let a = u.roi0 + gid.xy;
    if (all(a < u.roi1)) {
        let p = vec2i(a);
        let o = textureLoad(outT, p, 0).rgb;
        let g = textureLoad(gtT, p, 0).rgb;
        let d = o - g;
        let se = dot(d, d);
        let yo = dot(o, L709);
        let yg = dot(g, L709);
        let idx = a.y * u.size.x + a.x;
        m[0] = se;
        var te = 0.0;
        if (u.temporal == 1u) {
            let pyo = prevOutY[idx];
            te = abs((yo - yg) - (pyo - prevGtY[idx]));
            m[1] = te;
            m[2] = abs(yo - pyo);
        }
        let grad = abs(gy(p + vec2i(1, 0)) - gy(p - vec2i(1, 0))) +
                   abs(gy(p + vec2i(0, 1)) - gy(p - vec2i(0, 1)));
        if (grad > u.edgeTh) {
            m[3] = se;
            m[4] = 1.0;
            m[5] = te;
        }
        if (all(a >= u.rect0) && all(a < u.rect1)) {
            m[6] = se;
            m[7] = 1.0;
        }
        prevOutY[idx] = yo;
    }
    sh[li] = m;
    workgroupBarrier();
    for (var s = 128u; s > 0u; s = s >> 1u) {
        if (li < s) {
            for (var k = 0u; k < 8u; k++) {
                sh[li][k] = sh[li][k] + sh[li + s][k];
            }
        }
        workgroupBarrier();
    }
    if (li == 0u) {
        let base = (wid.y * nw.x + wid.x) * 8u;
        for (var k = 0u; k < 8u; k++) {
            partials[base + k] = sh[0][k];
        }
    }
}
`;

const REDUCE_WGSL = /* wgsl */ `
struct RU {
    count : u32,
    slot : u32,
    pad0 : u32,
    pad1 : u32,
};
@group(0) @binding(0) var<storage, read> partials : array<f32>;
@group(0) @binding(1) var<storage, read_write> results : array<f32>;
@group(0) @binding(2) var<uniform> u : RU;

var<workgroup> sh : array<array<f32, 8>, 256>;

@compute @workgroup_size(256)
fn cs(@builtin(local_invocation_index) li : u32) {
    var acc : array<f32, 8>;
    for (var i = li; i < u.count; i = i + 256u) {
        for (var k = 0u; k < 8u; k++) {
            acc[k] = acc[k] + partials[i * 8u + k];
        }
    }
    sh[li] = acc;
    workgroupBarrier();
    for (var s = 128u; s > 0u; s = s >> 1u) {
        if (li < s) {
            for (var k = 0u; k < 8u; k++) {
                sh[li][k] = sh[li][k] + sh[li + s][k];
            }
        }
        workgroupBarrier();
    }
    if (li == 0u) {
        for (var k = 0u; k < 8u; k++) {
            results[u.slot * 8u + k] = sh[0][k];
        }
    }
}
`;

const ADVANCE_WGSL = /* wgsl */ `
${COMMON}
${METRICS_UNIFORM}
@group(0) @binding(0) var gtT : texture_2d<f32>;
@group(0) @binding(1) var<storage, read_write> prevGtY : array<f32>;
@group(0) @binding(2) var<uniform> u : MU;

@compute @workgroup_size(16, 16)
fn cs(@builtin(global_invocation_id) gid : vec3u) {
    let a = u.roi0 + gid.xy;
    if (any(a >= u.roi1)) {
        return;
    }
    prevGtY[a.y * u.size.x + a.x] = dot(textureLoad(gtT, vec2i(a), 0).rgb, L709);
}
`;

// ── the sequence ───────────────────────────────────────────────────────────

/**
 * The frames of a sequence: per frame the source (0 the picture, 1 the one
 * after the cut), the window's origin, the block's rectangle (or null), the
 * motion of the content since the previous frame in stream pixels (0 across a
 * cut: an estimator sees no motion there) and the segment it belongs to.
 * @param {object} q  segs, v, dir, ov, box, the sizes and the source's
 * @returns {Array<object>}
 */
export function seqFrames({ segs, v, dir, ov, box, inW, ratio, srcW, srcH }) {
  const [bx0, by0, bw] = box;
  const sIn = bw / inW; // source pixels per stream pixel
  const n = Math.hypot(dir[0], dir[1]) || 1;
  const step = [(v * dir[0] * sIn) / n, (v * dir[1] * sIn) / n];
  const frames = [];
  let src = 0;
  let x = bx0;
  let y = by0;
  let sprite = null;
  const bh = bw * ratio;
  for (const seg of segs) {
    const [kind, count] = [seg.kind, seg.n];
    if (kind === "cut" || kind === "cutpan") {
      src = 1;
      x = bx0;
      y = by0;
    }
    if (kind === "obj" && !sprite) {
      const w = bw / 6;
      const h = bh / 5;
      sprite = { x: bx0 + bw * 0.05, y: by0 + (bh - h) / 2, w, h };
    }
    for (let k = 0; k < count; k++) {
      frames.push({
        seg: kind,
        first: k === 0,
        src,
        x,
        y,
        sprite: sprite && kind === "obj" ? { ...sprite } : null,
      });
      if (kind === "pan" || kind === "cutpan") {
        x += step[0];
        y += step[1];
      }
      if (kind === "obj") sprite.x += ov * sIn;
    }
  }
  for (let t = 0; t < frames.length; t++) {
    const f = frames[t];
    const p = frames[t - 1];
    f.mv =
      p && p.src === f.src ? [(f.x - p.x) / sIn, (f.y - p.y) / sIn] : [0, 0];
    if (f.x < 0 || f.y < 0 || f.x + bw > srcW + 1e-6 || f.y + bh > srcH + 1e-6)
      throw new Error(
        `frame ${t}: the window leaves the source (${f.x.toFixed(1)}, ${f.y.toFixed(1)})`,
      );
  }
  return frames;
}

/**
 * The encoder's warm-up before a sequence: its first segment's motion played
 * backwards up to the first frame — the window's (pan), the block's (obj) —,
 * up to `warm` frames, as many as fit in the source (the farthest dropped). The
 * rate control then settles on pictures that cost what the sequence's do: on a
 * still warm-up it banks bits and spends them on the first measured frames
 * (74 Mbit/s for 10 asked, 25/10), with no warm-up the first key frame sets
 * the bits of a short sequence whatever the bitrate. A still first segment
 * gives a still warm-up.
 * @param {object} q  seqFrames's parameters
 * @param {Array<object>} frames  seqFrames's frames
 * @param {number} warm
 * @returns {Array<object>} the warm-up's frames, in their order
 */
export function warmFrames(q, frames, warm) {
  const { segs, v, dir, ov, box, inW, ratio, srcW, srcH } = q;
  const bw = box[2];
  const bh = bw * ratio;
  const sIn = bw / inW;
  const n = Math.hypot(dir[0], dir[1]) || 1;
  const kind = segs[0].kind;
  const moving = kind === "pan" || kind === "cutpan";
  const step = moving
    ? [(v * dir[0] * sIn) / n, (v * dir[1] * sIn) / n]
    : [0, 0];
  const sstep = kind === "obj" ? ov * sIn : 0;
  const f0 = frames[0];
  const out = [];
  for (let k = warm; k >= 1; k--) {
    const x = f0.x + k * step[0];
    const y = f0.y + k * step[1];
    const sprite = f0.sprite
      ? { ...f0.sprite, x: f0.sprite.x + k * sstep }
      : null;
    if (
      x < 0 ||
      y < 0 ||
      x + bw > srcW + 1e-6 ||
      y + bh > srcH + 1e-6 ||
      (sprite && sprite.x + sprite.w > x + bw)
    )
      continue;
    out.push({ seg: "warm", first: false, src: f0.src, x, y, sprite });
  }
  return out;
}

/** `pan:60,stop:20,cutpan:20` → [{kind, n}]. */
export function parseSegs(s) {
  return s.split(",").map((part) => {
    const [kind, n] = part.split(":");
    if (!["pan", "stop", "cut", "cutpan", "obj"].includes(kind))
      throw new Error("segs: " + kind + "?");
    return { kind, n: Number(n) };
  });
}

/** `pull:a=0.5;t=6` → {name, mode, mc, out, alpha, tau, fmt}; the name
 *  (`pull-a0.5-t6`) keeps to the characters the server's file names keep. */
export function parseHist(s) {
  const [kind, opts = ""] = s.split(":");
  if (!(kind in HIST_MODES)) throw new Error("hist: " + kind + "?");
  const pairs = opts
    .split(";")
    .filter(Boolean)
    .map((kv) => kv.split("="));
  const o = Object.fromEntries(pairs);
  const db = { dbe: "edge", dbd: "dct" }[kind] || null;
  for (const k of Object.keys(o))
    if (!(db ? ["s", "t", "n", "c", "f"] : ["a", "t", "f"]).includes(k))
      throw new Error(s + ": option " + k + "?");
  const shifts = Number(o.n ?? 16);
  if (db && shifts !== 16 && shifts !== 4)
    throw new Error(s + ": n=16 or n=4");
  return {
    name: [kind, ...pairs.map(([k, val]) => k + val)].join("-"),
    kind,
    mode: HIST_MODES[kind],
    mc: kind.startsWith("mc") ? 1 : 0,
    out: kind === "outmc",
    alpha: Number(o.a ?? 0.75),
    tau: Number(o.t ?? 10) / 255,
    fmt: (db ? o.f === "16" : o.f !== "8") ? "rgba16float" : "rgba8unorm",
    db,
    // the strength and the bound in the luma the shaders read (full range, 0
    // to 255)
    dbThr: db ? (Number(db === "edge" ? (o.s ?? 3) : (o.t ?? 8)) * 255) / 219 : 0,
    dbClamp: db ? (Number(o.c ?? 0) * 255) / 219 : 0,
    shifts: db ? shifts : 0,
  };
}

// ── WebCodecs ──────────────────────────────────────────────────────────────

const CODECS = {
  // High, Main, then Constrained Baseline (the software encoder of Chrome,
  // OpenH264, takes the last one only); level 5.1 holds 1080p60.
  avc: ["avc1.640033", "avc1.4D0033", "avc1.42E033"],
  vp9: ["vp09.00.41.08"],
  av1: ["av01.0.08M.08"],
};

async function pickEncoder(family, w, h, kbps, accel) {
  const tried = [];
  for (const codec of CODECS[family] || []) {
    for (const bitrateMode of ["constant", "variable"]) {
      const cfg = {
        codec,
        width: w,
        height: h,
        bitrate: kbps * 1000,
        bitrateMode,
        framerate: 60,
        latencyMode: "realtime",
        hardwareAcceleration: accel,
      };
      if (family === "avc") cfg.avc = { format: "annexb" };
      let ok = false;
      try {
        ok = (await VideoEncoder.isConfigSupported(cfg)).supported;
      } catch (e) {
        ok = false;
      }
      tried.push(`${codec}/${bitrateMode}:${ok}`);
      if (ok) return { cfg, tried };
    }
  }
  throw new Error(`no ${accel} encoder for ${family} (${tried.join(" ")})`);
}

// The colour of the frames the page makes: BT.709, limited range, as a host's.
const COLOR = {
  primaries: "bt709",
  transfer: "bt709",
  matrix: "bt709",
  fullRange: false,
};

// Planes of a decoded frame, I420 whatever the decoder gave (NV12 split).
async function copyPlanes(frame, w, h) {
  const rect = { x: 0, y: 0, width: w, height: h };
  const buf = new Uint8Array(frame.allocationSize({ rect }));
  const layout = await frame.copyTo(buf, { rect });
  const fmt = frame.format;
  const cw = w / 2;
  const ch = h / 2;
  const y = new Uint8Array(w * h);
  for (let r = 0; r < h; r++)
    y.set(
      buf.subarray(
        layout[0].offset + r * layout[0].stride,
        layout[0].offset + r * layout[0].stride + w,
      ),
      r * w,
    );
  const u = new Uint8Array(cw * ch);
  const v = new Uint8Array(cw * ch);
  if (fmt === "I420") {
    for (let r = 0; r < ch; r++) {
      u.set(
        buf.subarray(
          layout[1].offset + r * layout[1].stride,
          layout[1].offset + r * layout[1].stride + cw,
        ),
        r * cw,
      );
      v.set(
        buf.subarray(
          layout[2].offset + r * layout[2].stride,
          layout[2].offset + r * layout[2].stride + cw,
        ),
        r * cw,
      );
    }
  } else if (fmt === "NV12") {
    for (let r = 0; r < ch; r++) {
      const o = layout[1].offset + r * layout[1].stride;
      for (let c = 0; c < cw; c++) {
        u[r * cw + c] = buf[o + 2 * c];
        v[r * cw + c] = buf[o + 2 * c + 1];
      }
    }
  } else throw new Error("decoded frame format " + fmt + "?");
  return { y, u, v, format: fmt };
}

function psnrY(a, b, n) {
  let se = 0;
  for (let i = 0; i < n; i++) {
    const d = a[i] - b[i];
    se += d * d;
  }
  const mse = se / n / (255 * 255);
  return mse > 0 ? 10 * Math.log10(1 / mse) : 99;
}

/**
 * The stream's frames (I420, BT.709 limited) through an encode and a decode at
 * one bitrate: the decoded planes per frame, the size and type of each chunk,
 * the Y PSNR of each decoded frame against what was encoded.
 */
async function encodeDecode(seq, w, h, family, kbps, accel, warm, log) {
  const { cfg, tried } = await pickEncoder(family, w, h, kbps, accel);
  const chunks = [];
  let decoderConfig = null;
  let err = null;
  const encoder = new VideoEncoder({
    output: (chunk, meta) => {
      chunks.push(chunk);
      if (meta && meta.decoderConfig) decoderConfig = meta.decoderConfig;
    },
    error: (e) => {
      err = e;
    },
  });
  encoder.configure(cfg);
  // seq: the warm-up's frames, then the sequence's; the sequence's keep their
  // indices from 0, the warm-up's are negative
  const orig = seq.slice(warm);
  for (let t = 0; t < seq.length; t++) {
    const vf = new VideoFrame(seq[t], {
      format: "I420",
      codedWidth: w,
      codedHeight: h,
      timestamp: t * 16667,
      duration: 16667,
      colorSpace: COLOR,
    });
    encoder.encode(vf, { keyFrame: t === 0 });
    vf.close();
    while (encoder.encodeQueueSize > 4)
      await new Promise((r) => setTimeout(r, 2));
    if (err) break;
  }
  await encoder.flush();
  encoder.close();
  if (err) throw new Error("encoder: " + err.message);
  const decCfg = {
    ...(decoderConfig || {}),
    codec: (decoderConfig && decoderConfig.codec) || cfg.codec,
    hardwareAcceleration: accel,
    optimizeForLatency: true,
  };
  if (!(await VideoDecoder.isConfigSupported(decCfg)).supported)
    throw new Error("no " + accel + " decoder for " + decCfg.codec);
  const outs = [];
  let derr = null;
  const decoder = new VideoDecoder({
    output: (f) => {
      const t = Math.round(f.timestamp / 16667) - warm;
      if (t < 0) {
        f.close();
        return;
      }
      outs.push(
        copyPlanes(f, w, h)
          .then((pl) => ({ ...pl, t }))
          .finally(() => {
            f.close();
          }),
      );
    },
    error: (e) => {
      derr = e;
    },
  });
  decoder.configure(decCfg);
  for (const c of chunks) {
    decoder.decode(c);
    while (decoder.decodeQueueSize > 4)
      await new Promise((r) => setTimeout(r, 2));
  }
  await decoder.flush();
  decoder.close();
  if (derr) throw new Error("decoder: " + derr.message);
  // A frame the encoder skipped (its rate control may, in real time) shows
  // the previous one, as on a client: the frame keeps its index.
  const got = new Map((await Promise.all(outs)).map((pl) => [pl.t, pl]));
  if (!got.has(0)) throw new Error("decoder: no first frame");
  const planes = [];
  const dropped = [];
  for (let t = 0; t < orig.length; t++) {
    if (!got.has(t)) dropped.push(t);
    planes.push(got.get(t) || planes[t - 1]);
  }
  const at = (c) => Math.round(c.timestamp / 16667) - warm;
  const bitsAt = new Map(chunks.map((c) => [at(c), c.byteLength * 8]));
  const bits = orig.map((_, t) => bitsAt.get(t) || 0);
  const warmBits = chunks
    .filter((c) => at(c) < 0)
    .reduce((a, c) => a + c.byteLength * 8, 0);
  const info = {
    codec: cfg.codec,
    bitrateMode: cfg.bitrateMode,
    acceleration: accel,
    tried,
    decodedFormat: planes[0].format,
    kbps,
    meanKbps: Math.round(
      ((bits.reduce((a, b) => a + b, 0) / bits.length) * 60) / 1000,
    ),
    warm,
    warmKbps: warm ? Math.round(((warmBits / warm) * 60) / 1000) : null,
    keys: chunks.filter((c) => c.type === "key").map(at),
    dropped,
  };
  const psnr = planes.map((p, t) => psnrY(p.y, orig[t], w * h));
  log(
    `encode ${kbps} kbit/s: ${JSON.stringify(info)} | Y PSNR mean ${(psnr.reduce((a, b) => a + b, 0) / psnr.length).toFixed(2)} dB`,
  );
  return { planes, bits, psnr, info };
}

// ── the page ───────────────────────────────────────────────────────────────

async function loadSource(device, url) {
  const r = await fetch(url);
  if (!r.ok) throw new Error(url + ": " + r.status);
  const bmp = await createImageBitmap(await r.blob(), {
    colorSpaceConversion: "none",
    premultiplyAlpha: "none",
  });
  const tex = device.createTexture({
    size: { width: bmp.width, height: bmp.height },
    format: "rgba8unorm",
    usage:
      GPUTextureUsage.TEXTURE_BINDING |
      GPUTextureUsage.COPY_DST |
      GPUTextureUsage.RENDER_ATTACHMENT,
  });
  device.queue.copyExternalImageToTexture(
    { source: bmp },
    { texture: tex },
    { width: bmp.width, height: bmp.height },
  );
  const size = [bmp.width, bmp.height];
  bmp.close();
  return { tex, view: tex.createView(), w: size[0], h: size[1] };
}

// A post that fails at the network (once in the campaign of 25/10, late in a
// ten-minute page, which it cost) is sent again, three times, before the page
// fails.
async function postBlob(url, blob) {
  for (let k = 0; ; k++) {
    try {
      const r = await fetch(url, { method: "POST", body: blob });
      if (!r.ok) throw new Error("POST " + url + ": " + r.status);
      return;
    } catch (e) {
      if (k >= 3) throw e;
      await new Promise((res) => setTimeout(res, 250 * (k + 1)));
    }
  }
}

// A mapped crop, its rgba8unorm rows as they are (the width a multiple of 64:
// no padding), posted raw — no canvas, whose 2D context may draw on the
// machine's GPU; the driver reads them back with the crop's size.
async function cropBytes(buf) {
  await buf.mapAsync(GPUMapMode.READ);
  const a = new Uint8Array(buf.getMappedRange()).slice(0);
  buf.unmap();
  buf.destroy();
  return a;
}

/**
 * The sequence run: see the head of the file for the query.
 * @param {Object<string, string>} p the query
 * @param {(s: string) => void} log
 */
export async function runSeq(p, log = () => {}) {
  const t0 = performance.now();
  const [inW, inH] = (p.in || "1280x720").split("x").map(Number);
  const [outW, outH] = (p.out || "2560x1440").split("x").map(Number);
  if (inW % 8 || inH % 2)
    throw new Error("in: the width must divide by 8, the height by 2");
  if (Math.abs(inH / inW - outH / outW) > 1e-3)
    throw new Error("in and out: one window, so one ratio");
  const roi = (p.roi || `0,0,${outW},${outH}`).split(",").map(Number);
  const [rx, ry, rw, rh] = roi;
  if (
    roi.length !== 4 ||
    !roi.every(Number.isInteger) ||
    rx < 0 ||
    ry < 0 ||
    rw <= 0 ||
    rh <= 0 ||
    rx + rw > outW ||
    ry + rh > outH
  )
    throw new Error("roi: x,y,w,h, whole pixels inside the screen");
  const warm = Math.max(0, Math.round(Number(p.warm || 0)));
  const soft = p.soft === "1";
  // Under soft=1 the codecs stay in software: a hardware encoder or decoder
  // is a GPU of the bench machine (59's rule, as the adapter below).
  const accel =
    soft || p.accel !== "hw" ? "prefer-software" : "prefer-hardware";
  const kbpsList = (p.kbps || "20000,10000").split(",").map(Number);
  const family = p.vcodec || "avc";
  const variants = (
    p.hist ||
    "none,pull:a=0.5;t=6,pull:a=0.75;t=10,clamp:a=0.75,mcclamp:a=0.75,outmc:a=0.75"
  )
    .split(",")
    .map(parseHist);
  if (variants[0].kind !== "none") variants.unshift(parseHist("none"));
  const segs = parseSegs(p.segs || "pan:60,stop:20,cutpan:20");
  const dir = (p.dir || "1,0.25").split(",").map(Number);
  const v = Number(p.v || 0.3);
  const ov = Number(p.ov || 8);
  const tag = p.tag || "seq";

  if (!navigator.gpu) throw new Error("no WebGPU (navigator.gpu)");
  const adapterOpts = {};
  if (p.fallback === "1") adapterOpts.forceFallbackAdapter = true;
  const adapter = await navigator.gpu.requestAdapter(adapterOpts);
  if (!adapter) throw new Error("requestAdapter returned null");
  const ai = adapter.info || {};
  const adapterName =
    `${ai.vendor || "?"} ${ai.architecture || ""} ${ai.device || ""} ${ai.description || ""}`
      .replace(/\s+/g, " ")
      .trim();
  if (soft && !/swiftshader/i.test(adapterName))
    throw new Error("soft=1 but the WebGPU adapter is " + adapterName);
  const device = await adapter.requestDevice({
    requiredLimits: {
      maxStorageBufferBindingSize: Math.min(
        adapter.limits.maxStorageBufferBindingSize,
        256 * 1024 * 1024,
      ),
    },
  });
  const errors = [];
  device.onuncapturederror = (e) => errors.push(e.error.message);
  log(`adapter: ${adapterName} | codecs ${accel}`);

  // P8.0's clip (vid): one texture, each frame's picture copied into it
  // before the frame is drawn (phase 1, then its truth in phase 3)
  const vidUrl = (k) => p.vid + String(k).padStart(3, "0") + ".png";
  const src = [await loadSource(device, p.vid ? vidUrl(0) : p.src)];
  src.push(
    p.src2 && !p.vid ? await loadSource(device, p.src2) : src[0],
  );
  if (src[1].w !== src[0].w || src[1].h !== src[0].h)
    throw new Error("src2: another size than src");
  if (
    p.vid &&
    (v !== 0 ||
      segs.some((x) => x.kind !== "pan") ||
      variants.some((x) => x.mc || x.out))
  )
    throw new Error("vid: pan segments at v=0 only, no mc or outmc variant");
  const loadVid = async (k) => {
    const r = await fetch(vidUrl(k));
    if (!r.ok) throw new Error(vidUrl(k) + ": " + r.status);
    const bmp = await createImageBitmap(await r.blob(), {
      colorSpaceConversion: "none",
      premultiplyAlpha: "none",
    });
    if (bmp.width !== src[0].w || bmp.height !== src[0].h)
      throw new Error(vidUrl(k) + ": another size than the first frame");
    device.queue.copyExternalImageToTexture(
      { source: bmp },
      { texture: src[0].tex },
      { width: bmp.width, height: bmp.height },
    );
    bmp.close();
  };
  const srcW = src[0].w;
  const srcH = src[0].h;
  const box = (p.box || `0,0,${Math.floor(srcW * 0.75)}`)
    .split(",")
    .map(Number);
  const bw = box[2];
  const bh = (bw * outH) / outW;
  const seqQ = { segs, v, dir, ov, box, inW, ratio: outH / outW, srcW, srcH };
  const frames = seqFrames(seqQ);
  const N = frames.length;
  const wf = warmFrames(seqQ, frames, warm);
  // the clip's frame of each (vid): the warm-up's first
  wf.forEach((f, k) => (f.vk = k));
  frames.forEach((f, t) => (f.vk = wf.length + t));
  const sOut = bw / outW; // source pixels per screen pixel
  log(
    `sequence: ${N} frames, ${segs.map((s) => s.kind + ":" + s.n).join(",")}, v ${v}, window ${bw}x${bh.toFixed(1)} of ${srcW}x${srcH}, region ${rx},${ry} ${rw}x${rh}, warm-up ${wf.length} frames${p.vid ? ", clip " + p.vid : ""}`,
  );

  const T = GPUTextureUsage;
  const B = GPUBufferUsage;
  const tex = (w, h, format, usage) =>
    device.createTexture({ size: { width: w, height: h }, format, usage });
  const buffer = (size, usage) =>
    device.createBuffer({ size: Math.ceil(size / 4) * 4, usage });
  const ubuf = (size) => buffer(Math.max(16, size), B.UNIFORM | B.COPY_DST);
  const sampler = device.createSampler({
    magFilter: "linear",
    minFilter: "linear",
    addressModeU: "clamp-to-edge",
    addressModeV: "clamp-to-edge",
  });
  // Every module's compilation messages, read before the first frame: a
  // shader the adapter refuses fails the run with its line, not a silence.
  const modules = [];
  const shader = (code, name) => {
    const module = device.createShaderModule({ code, label: name });
    modules.push([name, module]);
    return module;
  };
  const render = (code, formats, name) => {
    const module = shader(code, name);
    return device.createRenderPipeline({
      layout: "auto",
      vertex: { module, entryPoint: "vs" },
      fragment: {
        module,
        entryPoint: "fs",
        targets: formats.map((format) => ({ format })),
      },
      primitive: { topology: "triangle-list" },
    });
  };
  const compute = (code, name, entryPoint = "cs") =>
    device.createComputePipeline({
      layout: "auto",
      compute: { module: shader(code, name), entryPoint },
    });
  const checkModules = async () => {
    for (const [name, module] of modules.splice(0)) {
      const info = await module.getCompilationInfo();
      const bad = info.messages.filter((m) => m.type === "error");
      if (bad.length)
        throw new Error(
          name +
            ": " +
            bad
              .map((m) => `${m.lineNum}:${m.linePos} ${m.message}`)
              .join(" | "),
        );
    }
    await device.queue.onSubmittedWorkDone();
    if (errors.length)
      throw new Error("GPU: " + errors.slice(0, 3).join(" | "));
  };
  const bind = (pl, resources) =>
    device.createBindGroup({
      layout: pl.getBindGroupLayout(0),
      entries: resources.map((resource, binding) => ({
        binding,
        resource:
          resource instanceof GPUBuffer ? { buffer: resource } : resource,
      })),
    });
  // sc: the scissor (x, y, w, h), the rest of the target cleared
  const draw = (enc, pl, group, views, sc = null) => {
    const pass = enc.beginRenderPass({
      colorAttachments: views.map((view) => ({
        view,
        loadOp: "clear",
        storeOp: "store",
        clearValue: [0, 0, 0, 1],
      })),
    });
    pass.setPipeline(pl);
    pass.setBindGroup(0, group);
    if (sc) pass.setScissorRect(sc[0], sc[1], sc[2], sc[3]);
    pass.draw(3);
    pass.end();
  };
  const dispatch = (enc, pl, group, x, y = 1) => {
    const pass = enc.beginComputePass();
    pass.setPipeline(pl);
    pass.setBindGroup(0, group);
    pass.dispatchWorkgroups(x, y);
    pass.end();
  };

  // The ground truth and the stream's frames from the source's window.
  const plH = render(LANCZOS_H_WGSL, ["rgba16float"], "lanczos-h");
  const plV = render(LANCZOS_V_WGSL, ["rgba8unorm"], "lanczos-v");
  const midIn = tex(
    inW,
    srcH,
    "rgba16float",
    T.RENDER_ATTACHMENT | T.TEXTURE_BINDING,
  );
  const midGt = tex(
    outW,
    srcH,
    "rgba16float",
    T.RENDER_ATTACHMENT | T.TEXTURE_BINDING,
  );
  const inRgb = tex(
    inW,
    inH,
    "rgba8unorm",
    T.RENDER_ATTACHMENT | T.TEXTURE_BINDING,
  );
  const gtTex = tex(
    outW,
    outH,
    "rgba8unorm",
    T.RENDER_ATTACHMENT | T.TEXTURE_BINDING | T.COPY_SRC,
  );
  const viewOf = (t) => t.createView();
  // One window drawn at a size: its uniform, the horizontal pass per source
  // (into `mid`, every row of the source), the vertical one (into `out`).
  const resampler = (mid, out, w, h) => {
    const u = ubuf(48);
    return {
      u,
      gH: src.map((s) => bind(plH, [s.view, u])),
      gV: bind(plV, [viewOf(mid), u]),
      mid: viewOf(mid),
      out: viewOf(out),
      w,
      h,
    };
  };
  const lanczos = {
    in: resampler(midIn, inRgb, inW, inH),
    gt: resampler(midGt, gtTex, outW, outH),
  };
  const writeLanczos = (L, f) => {
    const sp = f.sprite;
    device.queue.writeBuffer(
      L.u,
      0,
      new Float32Array([
        f.x,
        f.y,
        bw,
        bh,
        L.w,
        L.h,
        srcW,
        srcH,
        sp ? sp.x : 0,
        sp ? sp.y : 0,
        sp ? sp.x + sp.w : 0,
        sp ? sp.y + sp.h : 0,
      ]),
    );
  };
  const drawLanczos = (enc, L, f, scH = null, scV = null) => {
    draw(enc, plH, L.gH[f.src], [L.mid], scH);
    draw(enc, plV, L.gV, [L.out], scV);
  };
  // The rectangles shaded at the screen's size (x, y, w, h): the region grown
  // by m, inside the screen. The truth's: two pixels more (its edges read
  // their neighbours); outmc's: ROI_MARGIN more. The truth's horizontal pass:
  // its columns, and the source rows the vertical taps read in that frame.
  const grow = (m) => {
    const x0 = Math.max(0, rx - m);
    const y0 = Math.max(0, ry - m);
    return [
      x0,
      y0,
      Math.min(outW, rx + rw + m) - x0,
      Math.min(outH, ry + rh + m) - y0,
    ];
  };
  const scGt = grow(2);
  const scK = grow(0);
  const scAcc = grow(ROI_MARGIN);
  const scMid = (f) => {
    const s = bh / outH;
    const y0 = Math.max(0, Math.floor(f.y + scGt[1] * s - 3 * s - 2));
    const y1 = Math.min(
      srcH,
      Math.ceil(f.y + (scGt[1] + scGt[3]) * s + 3 * s + 2),
    );
    return [scGt[0], y0, scGt[2], y1 - y0];
  };

  // Phase 1: the stream's frames, as a host would send them (4:2:0).
  const plY = compute(YUV_WGSL, "yuv-y", "csY");
  const plC = compute(YUV_WGSL, "yuv-c", "csC");
  await checkModules();
  const yuvBytes = inW * inH * 1.5;
  const yuvBuf = buffer(yuvBytes, B.STORAGE | B.COPY_SRC);
  const yuvU = ubuf(16);
  device.queue.writeBuffer(yuvU, 0, new Uint32Array([inW, inH, 0, 0]));
  const gY = bind(plY, [viewOf(inRgb), yuvBuf, yuvU]);
  const gC = bind(plC, [viewOf(inRgb), yuvBuf, yuvU]);
  const made = [...wf, ...frames];
  const orig = [];
  for (const f of made) {
    if (p.vid) await loadVid(f.vk);
    writeLanczos(lanczos.in, f);
    const enc = device.createCommandEncoder();
    drawLanczos(enc, lanczos.in, f);
    dispatch(enc, plY, gY, Math.ceil(((inW / 4) * inH) / 64));
    dispatch(enc, plC, gC, Math.ceil(((inW / 8) * (inH / 2)) / 64));
    const rd = buffer(yuvBytes, B.COPY_DST | B.MAP_READ);
    enc.copyBufferToBuffer(yuvBuf, 0, rd, 0, rd.size);
    device.queue.submit([enc.finish()]);
    await rd.mapAsync(GPUMapMode.READ);
    orig.push(new Uint8Array(rd.getMappedRange()).slice(0, yuvBytes));
    rd.unmap();
    rd.destroy();
  }
  log(
    `frames made: ${made.length} (${wf.length} of warm-up) in ${((performance.now() - t0) / 1000).toFixed(1)} s`,
  );

  // Phase 2: one encode and decode per bitrate.
  const streams = [];
  for (const kbps of kbpsList)
    streams.push(
      await encodeDecode(orig, inW, inH, family, kbps, accel, wf.length, log),
    );
  orig.length = 0;

  // Phase 3: every decoded frame through every variant.
  const kernel = await import(MWSR_URL);
  const mwOpts = {};
  const uni = {};
  for (const kv of (p.mw || "").split(";").filter(Boolean)) {
    const [k, val] = kv.split("=");
    if (["kc", "ft", "kg", "kr", "kwc", "kl"].includes(k)) uni[k] = Number(val);
    else throw new Error("mw: " + k + "?");
  }
  if (kernel.mwsrRegime(inW, inH, outW, outH).mode !== "up")
    throw new Error("the sequence mode measures the upscale (out > in)");
  const kModule = shader(kernel.mwsrWgsl({ feed: "tex", mode: "up" }), "mwsr");
  const plK = device.createRenderPipeline({
    layout: "auto",
    vertex: { module: kModule, entryPoint: "vs" },
    fragment: {
      module: kModule,
      entryPoint: "fs",
      targets: [{ format: "rgba8unorm" }],
      constants: mwOpts,
    },
    primitive: { topology: "triangle-list" },
  });
  const kU = ubuf(kernel.MWSR_UNIFORM_FLOATS * 4);
  device.queue.writeBuffer(
    kU,
    0,
    kernel.mwsrUniforms(inW, inH, outW, outH, uni),
  );

  const plYuv = render(YUV2RGB_WGSL, ["rgba8unorm"], "yuv2rgb");
  const plHist = {
    rgba16float: render(HIST_WGSL, ["rgba16float"], "hist-16"),
    rgba8unorm: render(HIST_WGSL, ["rgba8unorm"], "hist-8"),
  };
  const plAcc = render(ACC_WGSL, ["rgba16float", "rgba8unorm"], "acc");
  const fmts = ["rgba8unorm", "rgba16float"];
  const plDbDct = compute(DB_DCT_WGSL, "db-dct");
  const plDbFin = Object.fromEntries(
    fmts.map((f) => [f, render(DB_FIN_WGSL, [f], "db-fin-" + f)]),
  );
  const plDbEdge = Object.fromEntries(
    fmts.map((f) => [f, render(DB_EDGE_WGSL, [f], "db-edge-" + f)]),
  );
  // the deblocking's region of the stream (DB_MARGIN), and its uniforms
  const g8 = (x, up) => (up ? Math.ceil(x / 8) : Math.floor(x / 8)) * 8;
  const dbX0 = Math.max(0, g8((rx * inW) / outW - DB_MARGIN, false));
  const dbY0 = Math.max(0, g8((ry * inH) / outH - DB_MARGIN, false));
  const dbX1 = Math.min(inW, g8(((rx + rw) * inW) / outW + DB_MARGIN, true));
  const dbY1 = Math.min(inH, g8(((ry + rh) * inH) / outH + DB_MARGIN, true));
  const dbSc = [dbX0, dbY0, dbX1 - dbX0, dbY1 - dbY0];
  const dbWide = [
    Math.max(0, dbX0 - 8),
    Math.max(0, dbY0 - 8),
    Math.min(inW, dbX1 + 8) - Math.max(0, dbX0 - 8),
    Math.min(inH, dbY1 + 8) - Math.max(0, dbY0 - 8),
  ];
  const dbUniform = (sh, thr, n, cmax = 0) => {
    const b = new ArrayBuffer(48);
    new Int32Array(b, 0, 8).set([inW, inH, dbX0, dbY0, dbX1, dbY1, ...sh]);
    new Float32Array(b, 32, 3).set([thr, n, cmax]);
    const u = ubuf(48);
    device.queue.writeBuffer(u, 0, b);
    return u;
  };
  const plM = compute(METRICS_WGSL, "metrics");
  const plR = compute(REDUCE_WGSL, "reduce");
  const plA = compute(ADVANCE_WGSL, "advance");
  await checkModules();

  const outTex = tex(
    outW,
    outH,
    "rgba8unorm",
    T.RENDER_ATTACHMENT | T.TEXTURE_BINDING | T.COPY_SRC,
  );
  const dispTex = tex(
    outW,
    outH,
    "rgba8unorm",
    T.RENDER_ATTACHMENT | T.TEXTURE_BINDING | T.COPY_SRC,
  );
  const wgx = Math.ceil(rw / 16);
  const wgy = Math.ceil(rh / 16);
  const partials = buffer(wgx * wgy * 8 * 4, B.STORAGE);
  const V = variants.length;
  const NB = streams.length;
  const results = buffer(N * NB * V * 8 * 4, B.STORAGE | B.COPY_SRC);
  const prevGtY = buffer(outW * outH * 4, B.STORAGE);
  const mU = ubuf(48);
  const gAdvance = bind(plA, [viewOf(gtTex), prevGtY, mU]);

  const lanes = streams.map((s, b) => {
    const ty = tex(inW, inH, "r8unorm", T.TEXTURE_BINDING | T.COPY_DST);
    const tu = tex(inW / 2, inH / 2, "r8unorm", T.TEXTURE_BINDING | T.COPY_DST);
    const tv = tex(inW / 2, inH / 2, "r8unorm", T.TEXTURE_BINDING | T.COPY_DST);
    const cTex = tex(
      inW,
      inH,
      "rgba8unorm",
      T.RENDER_ATTACHMENT | T.TEXTURE_BINDING,
    );
    const lane = {
      b,
      ty,
      tu,
      tv,
      cView: viewOf(cTex),
      gYuv: bind(plYuv, [viewOf(ty), viewOf(tu), viewOf(tv), sampler]),
    };
    lane.slots = variants.map((vr, k) => {
      const slot = { vr, k, hU: ubuf(32), aU: ubuf(32), rU: ubuf(16) };
      slot.prevOutY = buffer(outW * outH * 4, B.STORAGE);
      if (vr.mode) {
        const h = [0, 1].map(() =>
          tex(inW, inH, vr.fmt, T.RENDER_ATTACHMENT | T.TEXTURE_BINDING),
        );
        const hv = h.map(viewOf);
        // parity q: read h[1 - q], write h[q]; the kernel reads h[q]
        slot.gHist = [0, 1].map((q) =>
          bind(plHist[vr.fmt], [lane.cView, hv[1 - q], slot.hU]),
        );
        slot.histOut = hv;
        slot.gK = [0, 1].map((q) => bind(plK, [sampler, hv[q], kU]));
      } else if (vr.db) {
        const usage = T.RENDER_ATTACHMENT | T.TEXTURE_BINDING;
        slot.dbOut = viewOf(tex(inW, inH, vr.fmt, usage));
        if (vr.db === "dct") {
          slot.acc = buffer(inW * inH * 4, B.STORAGE | B.COPY_DST);
          const step = vr.shifts === 4 ? 4 : 2;
          slot.dbDct = [];
          for (let dy = 0; dy < 8; dy += step)
            for (let dx = 0; dx < 8; dx += step)
              slot.dbDct.push(
                bind(plDbDct, [
                  lane.cView,
                  slot.acc,
                  dbUniform([dx, dy], vr.dbThr, vr.shifts),
                ]),
              );
          slot.dbFin = bind(plDbFin[vr.fmt], [
            lane.cView,
            slot.acc,
            dbUniform([0, 0], vr.dbThr, vr.shifts, vr.dbClamp),
          ]);
        } else {
          slot.dbMid = viewOf(tex(inW, inH, vr.fmt, usage));
          slot.dbEdge = [
            bind(plDbEdge[vr.fmt], [lane.cView, dbUniform([1, 0], vr.dbThr, 0)]),
            bind(plDbEdge[vr.fmt], [slot.dbMid, dbUniform([0, 1], vr.dbThr, 0)]),
          ];
        }
        slot.gK = [0, 1].map(() => bind(plK, [sampler, slot.dbOut, kU]));
      } else {
        slot.gK = [0, 1].map(() => bind(plK, [sampler, lane.cView, kU]));
      }
      if (vr.out) {
        const acc = [0, 1].map(() =>
          tex(
            outW,
            outH,
            "rgba16float",
            T.RENDER_ATTACHMENT | T.TEXTURE_BINDING,
          ),
        );
        const av = acc.map(viewOf);
        slot.gAcc = [0, 1].map((q) =>
          bind(plAcc, [viewOf(outTex), av[1 - q], slot.aU]),
        );
        slot.accOut = av;
      }
      const shown = vr.out ? dispTex : outTex;
      slot.shown = shown;
      slot.gM = bind(plM, [
        viewOf(shown),
        viewOf(gtTex),
        slot.prevOutY,
        prevGtY,
        partials,
        mU,
      ]);
      slot.gR = bind(plR, [partials, results, slot.rU]);
      return slot;
    });
    return lane;
  });
  const dispView = viewOf(dispTex);
  const outView = viewOf(outTex);

  // The dumps: crops of the screen-size picture, the truth and each output.
  const dumpSet = new Set(
    (p.dump || "").split(",").filter(Boolean).map(Number),
  );
  const [cx, cy, cw, ch] =
    (p.crop || "").split(",").filter(Boolean).length === 4
      ? p.crop.split(",").map(Number)
      : [Math.floor((outW - 640) / 2), Math.floor((outH - 360) / 2), 640, 360];
  if ((cw * 4) % 256)
    throw new Error("crop: the width must be a multiple of 64");
  if (cx < rx || cy < ry || cx + cw > rx + rw || cy + ch > ry + rh)
    throw new Error("crop: inside the region (roi)");
  const dumped = [];

  // The wake of the block (obj): where it was in the last five frames and is
  // no more, in screen pixels; nothing elsewhere.
  const wake = (t) => {
    const f = frames[t];
    const old = frames[Math.max(0, t - 5)];
    if (!f.sprite || !old.sprite || t === 0) return [0, 0, 0, 0];
    const toX = (x) => Math.round((x - f.x) / sOut);
    const toY = (y) => Math.round((y - f.y) / sOut);
    const x0 = Math.min(old.sprite.x, f.sprite.x);
    const x1 = Math.max(old.sprite.x, f.sprite.x);
    return [
      Math.max(0, toX(x0)),
      Math.max(0, toY(f.sprite.y)),
      Math.min(outW, toX(x1)),
      Math.min(outH, toY(f.sprite.y + f.sprite.h)),
    ];
  };

  const tStart = performance.now();
  for (let t = 0; t < N; t++) {
    const f = frames[t];
    const q = t & 1;
    if (p.vid) await loadVid(f.vk);
    writeLanczos(lanczos.gt, f);
    const r = wake(t);
    const mb = new ArrayBuffer(48);
    new Uint32Array(mb, 0, 7).set([
      outW,
      outH,
      r[0],
      r[1],
      r[2],
      r[3],
      t > 0 ? 1 : 0,
    ]);
    new Float32Array(mb, 28, 1)[0] = 0.1;
    new Uint32Array(mb, 32, 4).set([rx, ry, rx + rw, ry + rh]);
    device.queue.writeBuffer(mU, 0, mb);
    const enc = device.createCommandEncoder();
    drawLanczos(enc, lanczos.gt, f, scMid(f), scGt);
    const dumps = [];
    for (const lane of lanes) {
      const pl = streams[lane.b].planes[t];
      device.queue.writeTexture(
        { texture: lane.ty },
        pl.y,
        { bytesPerRow: inW },
        { width: inW, height: inH },
      );
      device.queue.writeTexture(
        { texture: lane.tu },
        pl.u,
        { bytesPerRow: inW / 2 },
        { width: inW / 2, height: inH / 2 },
      );
      device.queue.writeTexture(
        { texture: lane.tv },
        pl.v,
        { bytesPerRow: inW / 2 },
        { width: inW / 2, height: inH / 2 },
      );
      draw(enc, plYuv, lane.gYuv, [lane.cView]);
      for (const s of lane.slots) {
        const hb = new ArrayBuffer(32);
        new Float32Array(hb, 0, 2).set([s.vr.alpha, s.vr.tau]);
        new Uint32Array(hb, 8, 2).set([s.vr.mode, t === 0 ? 1 : 0]);
        new Float32Array(hb, 16, 2).set(s.vr.mc ? f.mv : [0, 0]);
        new Uint32Array(hb, 24, 2).set([s.vr.mc, 0]);
        device.queue.writeBuffer(s.hU, 0, hb);
        // outmc: the same motion in screen pixels
        const ab = new ArrayBuffer(32);
        new Float32Array(ab, 0, 1)[0] = s.vr.alpha;
        new Uint32Array(ab, 4, 1)[0] = t === 0 ? 1 : 0;
        new Float32Array(ab, 8, 6).set([
          (f.mv[0] * outW) / inW,
          (f.mv[1] * outH) / inH,
          scAcc[0],
          scAcc[1],
          scAcc[0] + scAcc[2],
          scAcc[1] + scAcc[3],
        ]);
        device.queue.writeBuffer(s.aU, 0, ab);
        device.queue.writeBuffer(
          s.rU,
          0,
          new Uint32Array([wgx * wgy, (t * NB + lane.b) * V + s.k, 0, 0]),
        );
        if (s.vr.mode) draw(enc, plHist[s.vr.fmt], s.gHist[q], [s.histOut[q]]);
        if (s.vr.db === "dct") {
          enc.clearBuffer(s.acc);
          const wx = (dbX1 - dbX0) / 8 + 1;
          const wy = (dbY1 - dbY0) / 8 + 1;
          for (const g of s.dbDct) dispatch(enc, plDbDct, g, wx, wy);
          draw(enc, plDbFin[s.vr.fmt], s.dbFin, [s.dbOut], dbSc);
        } else if (s.vr.db === "edge") {
          draw(enc, plDbEdge[s.vr.fmt], s.dbEdge[0], [s.dbMid], dbWide);
          draw(enc, plDbEdge[s.vr.fmt], s.dbEdge[1], [s.dbOut], dbSc);
        }
        draw(enc, plK, s.gK[q], [outView], s.vr.out ? scAcc : scK);
        if (s.vr.out)
          draw(enc, plAcc, s.gAcc[q], [s.accOut[q], dispView], scAcc);
        dispatch(enc, plM, s.gM, wgx, wgy);
        dispatch(enc, plR, s.gR, 1);
        if (dumpSet.has(t)) {
          const bpr = cw * 4;
          const rd = buffer(bpr * ch, B.COPY_DST | B.MAP_READ);
          enc.copyTextureToBuffer(
            { texture: s.shown, origin: { x: cx, y: cy } },
            { buffer: rd, bytesPerRow: bpr },
            { width: cw, height: ch },
          );
          dumps.push({
            rd,
            name: `${tag}-f${t}-b${kbpsList[lane.b]}-${s.vr.name}`,
          });
        }
      }
    }
    dispatch(enc, plA, gAdvance, wgx, wgy);
    if (dumpSet.has(t)) {
      const bpr = cw * 4;
      const rd = buffer(bpr * ch, B.COPY_DST | B.MAP_READ);
      enc.copyTextureToBuffer(
        { texture: gtTex, origin: { x: cx, y: cy } },
        { buffer: rd, bytesPerRow: bpr },
        { width: cw, height: ch },
      );
      dumps.push({ rd, name: `${tag}-f${t}-gt` });
    }
    device.queue.submit([enc.finish()]);
    for (const d of dumps) {
      await postBlob(
        "/__bin?name=" + encodeURIComponent(d.name),
        await cropBytes(d.rd),
      );
      dumped.push(d.name);
    }
    await device.queue.onSubmittedWorkDone();
    if (errors.length)
      throw new Error("GPU: " + errors.slice(0, 3).join(" | "));
    if (t % 10 === 9 || t === N - 1)
      log(
        `frame ${t + 1}/${N}: ${((performance.now() - tStart) / (t + 1) / 1000).toFixed(2)} s a frame`,
      );
  }

  // The sums back, per frame, stream and variant.
  const rd = buffer(results.size, B.COPY_DST | B.MAP_READ);
  const enc = device.createCommandEncoder();
  enc.copyBufferToBuffer(results, 0, rd, 0, results.size);
  device.queue.submit([enc.finish()]);
  await rd.mapAsync(GPUMapMode.READ);
  const sums = new Float32Array(rd.getMappedRange().slice(0));
  rd.unmap();
  const npx = rw * rh;
  const metrics = {};
  for (let b = 0; b < NB; b++) {
    const byV = {};
    for (let k = 0; k < V; k++) {
      const m = {
        mse: [],
        te: [],
        oc: [],
        mseEdge: [],
        edgeShare: [],
        teEdge: [],
        mseRect: [],
        rectN: [],
      };
      for (let t = 0; t < N; t++) {
        const o = ((t * NB + b) * V + k) * 8;
        const s = sums.subarray(o, o + 8);
        m.mse.push(s[0] / (npx * 3));
        m.te.push(t ? s[1] / npx : null);
        m.oc.push(t ? s[2] / npx : null);
        m.mseEdge.push(s[4] ? s[3] / (s[4] * 3) : null);
        m.edgeShare.push(s[4] / npx);
        m.teEdge.push(t && s[4] ? s[5] / s[4] : null);
        m.mseRect.push(s[7] ? s[6] / (s[7] * 3) : null);
        m.rectN.push(s[7]);
      }
      byV[variants[k].name] = m;
    }
    metrics[kbpsList[b]] = byV;
  }
  return {
    seq: true,
    adapter: { name: adapterName },
    params: {
      in: `${inW}x${inH}`,
      out: `${outW}x${outH}`,
      segs: p.segs,
      v,
      dir,
      ov,
      box: [box[0], box[1], bw, bh],
      src: p.src,
      src2: p.src2 || p.src,
      vid: p.vid || null,
      vcodec: family,
      roi: [rx, ry, rw, rh],
      warm: wf.length,
    },
    variants: variants.map((x) => ({ ...x })),
    frames: frames.map((f) => ({
      seg: f.seg,
      first: f.first,
      src: f.src,
      mv: f.mv,
    })),
    streams: Object.fromEntries(
      streams.map((s, b) => [
        kbpsList[b],
        { info: s.info, bits: s.bits, psnrY: s.psnr },
      ]),
    ),
    metrics,
    crop: [cx, cy, cw, ch],
    dumped,
    errors,
    seconds: (performance.now() - t0) / 1000,
  };
}
