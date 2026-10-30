// mwsr-map.js — what an adaptive sharpness costs, for the lab only (the free
// bench of 29/10/2026; the plan's §10-§11 and their redo). Two pieces:
//   mapKernel  the product's `up` kernel (MwsrShader.js's mwsrWgsl) reading a
//              grid of sharpness factors, a small r16float texture that the
//              sampler's bilinear interpolates between the tiles' centres (as
//              the bench's verify.py reads it): the edge path's sharpness is
//              the uniform's times the grid's value there;
//   statsWgsl  the analysis: per cell of a fine grid, statistics of the luma
//   mapWgsl    at a stride (q), then per tile of the map, its cells'
//              statistics and the frame's, a decision tree, the factor.
// The kernel's text is derived like mwsr-yuv.js's: each change checked to
// hit its line exactly once, so that a kernel changed in the product breaks
// the lab loudly instead of drifting from it.

function once(text, needle) {
  const n = text.split(needle).length - 1;
  if (n !== 1) throw new Error(`mwsr-map: "${needle.trim().slice(0, 60)}" found ${n} times, not once`);
}

export function mapKernel(up) {
  const decl = "@group(0) @binding(2) var<uniform> u : U;";
  const line = "    let sharp = mix(u.sharp, u.zsharp, zg);";
  once(up, decl);
  once(up, line);
  return up
    .replace(decl, decl + "\n@group(0) @binding(3) var sharpMap : texture_2d<f32>;")
    .replace(
      line,
      "    let sharp = mix(u.sharp, u.zsharp, zg) * textureSampleLevel(sharpMap, samp, uv, 0.0).r;",
    );
}

// The same, the factor carried by the vertices instead of a texture read per
// pixel (the map's read cost +9 % of the kernel on the realme, 29/10): the pass
// draws a mesh of (W+1)x(H+1) quads whose corners sit on the tiles' centres
// and on the frame's edges, each corner reading its tile's factor (clamped),
// the rasterizer interpolating it across the quad's two triangles (linear per
// triangle, where the texture's sampler is bilinear per quad). The draw has
// 6(W+1)(H+1) vertices: vtxCount.
export function mapKernelVtx(up) {
  const decl = "@group(0) @binding(2) var<uniform> u : U;";
  const field = "    @location(0) uv : vec2f,\n};";
  const vs = `@vertex
fn vs(@builtin(vertex_index) vid : u32) -> VSOut {
    var out : VSOut;
    let uv = vec2f(f32((vid << 1u) & 2u), f32(vid & 2u));
    out.uv = uv;
    out.pos = vec4f(uv * vec2f(2.0, -2.0) + vec2f(-1.0, 1.0), 0.0, 1.0);
    return out;
}`;
  const line = "    let sharp = mix(u.sharp, u.zsharp, zg);";
  for (const n of [decl, field, vs, line]) once(up, n);
  const grid = `@vertex
fn vs(@builtin(vertex_index) vid : u32) -> VSOut {
    var out : VSOut;
    let dims = vec2i(textureDimensions(sharpMap));
    let quad = i32(vid / 6u);
    var corners = array<vec2i, 6>(vec2i(0, 0), vec2i(1, 0), vec2i(0, 1), vec2i(1, 0), vec2i(1, 1), vec2i(0, 1));
    let corner = corners[vid % 6u];
    // the corner's column and row, 0..W+1 and 0..H+1: 0 and W+1 on the edges
    let cr = vec2i(quad % (dims.x + 1), quad / (dims.x + 1)) + corner;
    let uv = clamp((vec2f(cr) - 0.5) / vec2f(dims), vec2f(0.0), vec2f(1.0));
    out.uv = uv;
    out.pos = vec4f(uv * vec2f(2.0, -2.0) + vec2f(-1.0, 1.0), 0.0, 1.0);
    out.k = textureLoad(sharpMap, clamp(cr - vec2i(1), vec2i(0), dims - vec2i(1)), 0).r;
    return out;
}`;
  return up
    .replace(decl, decl + "\n@group(0) @binding(3) var sharpMap : texture_2d<f32>;")
    .replace(field, "    @location(0) uv : vec2f,\n    @location(1) k : f32,\n};")
    .replace(vs, grid)
    .replace(line, "    let sharp = mix(u.sharp, u.zsharp, zg) * in.k;");
}

export const vtxCount = (w, h) => 6 * (w + 1) * (h + 1);

// A grid of factors for the timing (the values do not change the cost): a
// smooth pattern between 0.5 and 1.5, or every factor `value`, as Float16
// bits for an r16float texture.
export function mapPattern(w, h, value) {
  const bits = new Uint16Array(w * h);
  const toHalf = (v) => {
    const f = new Float32Array([v]);
    const i = new Uint32Array(f.buffer)[0];
    const s = (i >>> 16) & 0x8000;
    const e = ((i >>> 23) & 0xff) - 127 + 15;
    const m = (i >>> 13) & 0x3ff;
    return e <= 0 ? s : e >= 31 ? s | 0x7c00 : s | (e << 10) | m;
  };
  for (let y = 0; y < h; y++)
    for (let x = 0; x < w; x++)
      bits[y * w + x] = toHalf(value ?? 1 + 0.5 * Math.sin(x * 0.9 + y * 1.7));
  return bits;
}

// The analysis, first pass: one fragment per cell of a CW×CH grid over the
// input (an rgba16float target of that size, two of them), the luma read at
// the stride Q from the cell's first texel, its neighbours D away for the
// gradient and the laplacian (D = 0: Q away, the grid's own scale; D = 1:
// the picture's own, for the same reads). R = 1 reads a quarter of those
// texels: the stride 2Q from the offset of phase P (0-3, one per frame in
// turn). Out: [mean, mean of squares, mean |gradient|, share of gradients
// over 0.05] and [share under 0.01, mean |laplacian|, mean saturation
// (max - min of RGB), share of dark texels (luma < 0.15)].
// `feed` names how the input is read: `tex` (texture_2d) or `ext`
// (texture_external, the decoder's frame).
export function statsWgsl(feed) {
  const decl =
    feed === "ext"
      ? "@group(0) @binding(0) var tex : texture_external;"
      : "@group(0) @binding(0) var tex : texture_2d<f32>;";
  const load =
    feed === "ext"
      ? "textureLoad(tex, clamp(p, vec2i(0), dims - vec2i(1))).rgb"
      : "textureLoad(tex, clamp(p, vec2i(0), dims - vec2i(1)), 0).rgb";
  return /* wgsl */ `
${decl}
override CW : u32 = 64u;
override CH : u32 = 36u;
override Q : u32 = 4u;
override D : u32 = 0u;
override R : u32 = 0u;
override P : u32 = 0u;

struct VSOut {
    @builtin(position) pos : vec4f,
};

@vertex
fn vs(@builtin(vertex_index) vid : u32) -> VSOut {
    var out : VSOut;
    let uv = vec2f(f32((vid << 1u) & 2u), f32(vid & 2u));
    out.pos = vec4f(uv * vec2f(2.0, -2.0) + vec2f(-1.0, 1.0), 0.0, 1.0);
    return out;
}

fn px(p : vec2i, dims : vec2i) -> vec3f {
    return ${load};
}

fn lum(c : vec3f) -> f32 {
    return dot(c, vec3f(0.2126, 0.7152, 0.0722));
}

struct Out {
    @location(0) a : vec4f,
    @location(1) b : vec4f,
};

@fragment
fn fs(in : VSOut) -> Out {
    let dims = vec2i(textureDimensions(tex));
    let cell = vec2i(in.pos.xy);
    let grid = vec2i(i32(CW), i32(CH));
    let p0 = cell * dims / grid;
    let p1 = (cell + vec2i(1)) * dims / grid;
    let q = i32(Q);
    let d = select(i32(D), q, D == 0u);
    let st = select(q, 2 * q, R == 1u);
    let o = select(vec2i(0), vec2i(i32(P & 1u), i32(P >> 1u)) * q, R == 1u);
    var s0 = vec4f(0.0);
    var s1 = vec4f(0.0);
    var n = 0.0;
    for (var y = p0.y + o.y; y < p1.y; y += st) {
        for (var x = p0.x + o.x; x < p1.x; x += st) {
            let c = px(vec2i(x, y), dims);
            let l = lum(c);
            let lx = lum(px(vec2i(x + d, y), dims));
            let ly = lum(px(vec2i(x, y + d), dims));
            let lw = lum(px(vec2i(x - d, y), dims));
            let ln = lum(px(vec2i(x, y - d), dims));
            let g = abs(lx - l) + abs(ly - l);
            let lap = abs(lx + ly + lw + ln - 4.0 * l);
            let sat = max(c.r, max(c.g, c.b)) - min(c.r, min(c.g, c.b));
            s0 += vec4f(l, l * l, g, select(0.0, 1.0, g > 0.05));
            s1 += vec4f(select(0.0, 1.0, g < 0.01), lap, sat, select(0.0, 1.0, l < 0.15));
            n += 1.0;
        }
    }
    let k = 1.0 / max(n, 1.0);
    return Out(s0 * k, s1 * k);
}
`;
}

// The analysis, second pass: one fragment per tile of the MW×MH map, its
// cells' statistics averaged (two rgba16float targets of the map's size).
export const TILE_WGSL = /* wgsl */ `
@group(0) @binding(0) var statA : texture_2d<f32>;
@group(0) @binding(1) var statB : texture_2d<f32>;
override MW : u32 = 16u;
override MH : u32 = 9u;

struct VSOut {
    @builtin(position) pos : vec4f,
};

@vertex
fn vs(@builtin(vertex_index) vid : u32) -> VSOut {
    var out : VSOut;
    let uv = vec2f(f32((vid << 1u) & 2u), f32(vid & 2u));
    out.pos = vec4f(uv * vec2f(2.0, -2.0) + vec2f(-1.0, 1.0), 0.0, 1.0);
    return out;
}

struct Out {
    @location(0) a : vec4f,
    @location(1) b : vec4f,
};

@fragment
fn fs(in : VSOut) -> Out {
    let cells = vec2i(textureDimensions(statA));
    let tile = vec2i(in.pos.xy);
    let c0 = tile * cells / vec2i(i32(MW), i32(MH));
    let c1 = (tile + vec2i(1)) * cells / vec2i(i32(MW), i32(MH));
    var a = vec4f(0.0);
    var b = vec4f(0.0);
    var n = 0.0;
    for (var y = c0.y; y < c1.y; y++) {
        for (var x = c0.x; x < c1.x; x++) {
            a += textureLoad(statA, vec2i(x, y), 0);
            b += textureLoad(statB, vec2i(x, y), 0);
            n += 1.0;
        }
    }
    return Out(a / max(n, 1.0), b / max(n, 1.0));
}
`;

// The analysis, third pass: per tile, the frame's means over the tiles, then
// a tree (the bench's, written as WGSL by its exporter) gives the factor of
// the law, into the map (r16float, MW×MH). The default tree is a depth-4
// stand-in of the same shape: its cost, not its choices, is what this pass
// measures until the bench's tree replaces it.
export function decideWgsl(tree) {
  const body =
    tree ||
    `    if (f[2] <= 0.02) {
        if (f[4] <= 0.6) { if (F[3] <= 0.1) { return 1.0; } return 1.2; }
        if (f[0] <= 0.3) { return 0.9; } return 1.0;
    }
    if (f[6] <= 0.15) {
        if (F[1] <= 0.4) { if (f[5] <= 0.1) { return 1.3; } return 1.1; }
        return 1.0;
    }
    if (f[3] <= 0.3) { return 0.6; }
    return 0.8;`;
  return /* wgsl */ `
@group(0) @binding(0) var tileA : texture_2d<f32>;
@group(0) @binding(1) var tileB : texture_2d<f32>;

struct VSOut {
    @builtin(position) pos : vec4f,
};

@vertex
fn vs(@builtin(vertex_index) vid : u32) -> VSOut {
    var out : VSOut;
    let uv = vec2f(f32((vid << 1u) & 2u), f32(vid & 2u));
    out.pos = vec4f(uv * vec2f(2.0, -2.0) + vec2f(-1.0, 1.0), 0.0, 1.0);
    return out;
}

fn decide(f : array<f32, 8>, F : array<f32, 8>) -> f32 {
${body}
}

@fragment
fn fs(in : VSOut) -> @location(0) vec4f {
    let tiles = vec2i(textureDimensions(tileA));
    let me = vec2i(in.pos.xy);
    var fa = vec4f(0.0);
    var fb = vec4f(0.0);
    for (var y = 0; y < tiles.y; y++) {
        for (var x = 0; x < tiles.x; x++) {
            fa += textureLoad(tileA, vec2i(x, y), 0);
            fb += textureLoad(tileB, vec2i(x, y), 0);
        }
    }
    let k = 1.0 / f32(tiles.x * tiles.y);
    let ta = textureLoad(tileA, me, 0);
    let tb = textureLoad(tileB, me, 0);
    let f = array<f32, 8>(ta.x, ta.y, ta.z, ta.w, tb.x, tb.y, tb.z, tb.w);
    let F = array<f32, 8>(fa.x * k, fa.y * k, fa.z * k, fa.w * k, fb.x * k, fb.y * k, fb.z * k, fb.w * k);
    return vec4f(decide(f, F), 0.0, 0.0, 1.0);
}
`;
}
