// MWSR zones, as MoonlightWeb's native host decides them for its own client
// (backend/native-host/src/convert/MwsrZones.h and windows/MwsrStats.cpp,
// docs/design/upscaler-mwsr-b.md §12 c): the statistics of the native frame
// (the host's capture) per cell of a 128 x 72 grid, a code per zone of a
// 32 x 18 grid from the tree of zonetree.js, then the neighbours' guard. Each
// code is a drop the client takes off its sharpness law there; 0 is the law.
// The full pass, every pixel: the host's default. Float32 where the C++ uses
// float, operation by operation (Math.fround), and the f16 roundings it takes
// from the bench, so that the codes are the host's. The tree comes in as an
// argument (zonetree.js's ZONE_TREE): the page loads both by their versioned
// URLs.

export const CELLS_X = 128;
export const CELLS_Y = 72;
export const ZONES_X = 32;
export const ZONES_Y = 18;
const STATS = 8;
const PER = CELLS_X / ZONES_X; // 4 x 4 cells a zone
/** The client reads zones from x1.3 up (MwsrZoneBlock.js MWSR_ZONE_MIN_SCALE). */
export const ZONE_MIN_SCALE = 1.3;

const f = Math.fround;

/**
 * The statistics pass on an RGBA8 frame (sRGB-encoded values), every pixel:
 * per cell, the means of the luma (BT.709 weights), its square, the gradient
 * |dx| + |dy| (forward differences), whether it is above 0.05 and under 0.01,
 * the |laplacian| (4 neighbours), the saturation and whether the luma is
 * under 0.15. Neighbours 1 px away, the frame's edges clamped.
 * @param {Uint8Array|Uint8ClampedArray} rgba
 * @param {number} w
 * @param {number} h
 * @returns {Float32Array} CELLS_X x CELLS_Y x 8, row by row
 */
export function zoneCells(rgba, w, h) {
  // the luma of every pixel once, in float as the shader
  const Y = new Float32Array(w * h);
  for (let i = 0, p = 0; i < w * h; i++, p += 4) {
    const r = f(rgba[p] / 255);
    const g = f(rgba[p + 1] / 255);
    const b = f(rgba[p + 2] / 255);
    Y[i] = f(f(f(r * f(0.2126)) + f(g * f(0.7152))) + f(b * f(0.0722)));
  }
  const cells = new Float32Array(CELLS_X * CELLS_Y * STATS);
  const s = new Float64Array(STATS);
  for (let cy = 0; cy < CELLS_Y; cy++) {
    const y0 = Math.floor((cy * h) / CELLS_Y);
    const y1 = Math.floor(((cy + 1) * h) / CELLS_Y);
    for (let cx = 0; cx < CELLS_X; cx++) {
      const x0 = Math.floor((cx * w) / CELLS_X);
      const x1 = Math.floor(((cx + 1) * w) / CELLS_X);
      s.fill(0);
      let n = 0;
      for (let y = y0; y < y1; y++) {
        const row = y * w;
        const up = Math.max(y - 1, 0) * w;
        const down = Math.min(y + 1, h - 1) * w;
        for (let x = x0; x < x1; x++) {
          n++;
          const c = Y[row + x];
          const lx = Y[row + Math.min(x + 1, w - 1)];
          const lw = Y[row + Math.max(x - 1, 0)];
          const ly = Y[down + x];
          const ln = Y[up + x];
          const g = f(Math.abs(f(lx - c)) + Math.abs(f(ly - c)));
          const lap = Math.abs(f(f(f(f(lx + ly) + lw) + ln) - f(4 * c)));
          const p = (row + x) * 4;
          const r = f(rgba[p] / 255);
          const gr = f(rgba[p + 1] / 255);
          const b = f(rgba[p + 2] / 255);
          s[0] += c;
          s[1] += f(c * c);
          s[2] += g;
          s[3] += g > f(0.05) ? 1 : 0;
          s[4] += g < f(0.01) ? 1 : 0;
          s[5] += lap;
          s[6] += f(Math.max(r, gr, b) - Math.min(r, gr, b));
          s[7] += c < f(0.15) ? 1 : 0;
        }
      }
      const o = (cy * CELLS_X + cx) * STATS;
      for (let k = 0; k < STATS; k++) cells[o + k] = n ? s[k] / n : 0;
    }
  }
  return cells;
}

const F32 = new Float32Array(1);
const U32 = new Uint32Array(F32.buffer);
// round half to even, for the subnormal halves
const nearbyint = (v) => {
  const r = Math.round(v);
  return Math.abs(v % 1) === 0.5 && r % 2 !== 0 ? r - 1 : r;
};

/** `x` rounded to binary16 and back, to nearest, ties to even (MwsrZones.h roundToHalf). */
export function roundToHalf(x) {
  F32[0] = x;
  const sign = U32[0] & 0x80000000;
  let a = U32[0] & 0x7fffffff;
  if (a > 0x7f800000) return F32[0]; // NaN
  if (a >= 0x477ff000) {
    a = 0x7f800000;
  } else if (a >= 0x38800000) {
    a = (a + 0x0fff + ((a >>> 13) & 1)) & ~0x1fff;
  } else {
    // a subnormal half: a multiple of 2^-24, both scalings exact
    U32[0] = a;
    F32[0] = f(nearbyint(f(F32[0] * 16777216)) * f(1 / 16777216));
    a = U32[0];
  }
  U32[0] = (sign | a) >>> 0;
  return F32[0];
}

// A zone's 8 means from its 4 x 4 cells (MwsrZones.h zoneMeans).
function zoneMeans(cells, zx, zy, out) {
  const acc = new Float32Array(STATS);
  for (let y = 0; y < PER; y++)
    for (let x = 0; x < PER; x++) {
      const o = ((zy * PER + y) * CELLS_X + (zx * PER + x)) * STATS;
      for (let s = 0; s < STATS; s++) acc[s] = f(acc[s] + roundToHalf(cells[o + s]));
    }
  for (let s = 0; s < STATS; s++) out[s] = roundToHalf(f(acc[s] / (PER * PER)));
}

// Its 8 features (MwsrZones.h features).
function features(m, out) {
  const square = f(m[0] * m[0]);
  const variance = f(m[1] - square);
  out[0] = m[0];
  out[1] = f(Math.sqrt(variance > 0 ? variance : 0));
  out[2] = m[2];
  out[3] = m[3];
  out[4] = m[4];
  out[5] = f(m[5] / f(m[2] + f(1e-3)));
  out[6] = m[6];
  out[7] = m[7];
}

function decide(input, tree) {
  let i = 0;
  while (tree[i][0] >= 0) i = input[tree[i][0]] <= tree[i][1] ? tree[i][2] : tree[i][3];
  return tree[i][4];
}

/**
 * A zone drops no more than the least of its 3 x 3 neighbours, the grid's
 * edges clamped (MwsrZones.h guardNeighbours): text beside a photo keeps the law.
 * @param {Uint8Array} codes
 */
export function guardNeighbours(codes, zonesX = ZONES_X, zonesY = ZONES_Y) {
  const src = codes.slice();
  for (let zy = 0; zy < zonesY; zy++)
    for (let zx = 0; zx < zonesX; zx++) {
      let least = src[zy * zonesX + zx];
      for (let y = Math.max(zy - 1, 0); y <= Math.min(zy + 1, zonesY - 1); y++)
        for (let x = Math.max(zx - 1, 0); x <= Math.min(zx + 1, zonesX - 1); x++)
          least = Math.min(least, src[y * zonesX + x]);
      codes[zy * zonesX + zx] = least;
    }
}

/**
 * Every zone's code (MwsrZones.h decideAll): the tree on each zone's 8
 * features, the frame's 8 means, the scale (the frame's width over the
 * stream's), encoded 1 (a stream always is) and linear (the host reduced in
 * linear light), then the guard.
 * @param {Float32Array} cells zoneCells' output
 * @param {number} scale
 * @param {boolean} linear
 * @param {number[][]} tree zonetree.js's ZONE_TREE
 * @returns {Uint8Array} ZONES_X x ZONES_Y codes, row by row
 */
export function decideZones(cells, scale, linear, tree) {
  const zones = ZONES_X * ZONES_Y;
  const feat = Array.from({ length: zones }, () => new Float32Array(STATS));
  const sum = new Float64Array(STATS);
  const m = new Float32Array(STATS);
  for (let zy = 0; zy < ZONES_Y; zy++)
    for (let zx = 0; zx < ZONES_X; zx++) {
      zoneMeans(cells, zx, zy, m);
      const ft = feat[zy * ZONES_X + zx];
      features(m, ft);
      for (let s = 0; s < STATS; s++) sum[s] += ft[s];
    }
  const input = new Float32Array(19);
  input[0] = scale;
  for (let s = 0; s < STATS; s++) input[1 + STATS + s] = sum[s] / zones;
  input[17] = 1;
  input[18] = linear ? 1 : 0;
  const codes = new Uint8Array(zones);
  for (let z = 0; z < zones; z++) {
    for (let s = 0; s < STATS; s++) input[1 + s] = feat[z][s];
    codes[z] = decide(input, tree);
  }
  guardNeighbours(codes);
  return codes;
}

/** The drop a code takes off the law (MwsrZoneBlock.js mwsrZoneDrop): 0 for the law. */
export const zoneDrop = (code) => (code > 0 && code < 12 ? 0.1 + (code - 1) * 0.125 : 0);

// binary16 bits of a value roundToHalf already rounded (0, or a normal half)
function halfBits(v) {
  if (v === 0) return 0;
  F32[0] = roundToHalf(v);
  const u = U32[0];
  return ((u >>> 16) & 0x8000) | ((((u >>> 23) & 0xff) - 112) << 10) | ((u >>> 13) & 0x3ff);
}

/**
 * The zones' texels for an r16float texture, as the client writes them
 * (MwsrZoneBlock.js mwsrZoneTexels): each zone's drop, read bilinearly by
 * the kernel between the zones' centres.
 * @param {Uint8Array} codes
 * @returns {Uint16Array}
 */
export function zoneTexels(codes) {
  return Uint16Array.from(codes, (c) => halfBits(zoneDrop(c)));
}
