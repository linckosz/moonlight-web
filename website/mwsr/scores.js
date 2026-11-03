// The page's PSNR, SSIM, MS-SSIM and FLIP (LDR, 67 pixels per degree), written from the definitions the lab scores with.
// The FLIP part follows NVIDIA's FLIP.h, under its notice:
/*
 * Copyright (c) 2020-2025, NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 * list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * SPDX-FileCopyrightText: Copyright (c) 2020-2025 NVIDIA CORPORATION & AFFILIATES
 * SPDX-License-Identifier: BSD-3-Clause
 */
// PSNR, SSIM, MS-SSIM and LDR-FLIP of two 8-bit RGBA pictures, in plain JavaScript, to the definitions the lab uses:
// PSNR on RGB; SSIM of skimage (gaussian_weights, sigma 1.5, 11 taps, on RGB, its border cropped); MS-SSIM of the lab's metrics_run on luma (Rec. 709);
// LDR-FLIP of NVIDIA's flip-evaluator (BSD-3-Clause), default 67 pixels per degree.

function gauss(sigma, r) {
  const k = new Float64Array(2 * r + 1);
  let s = 0;
  for (let i = -r; i <= r; i++) s += k[i + r] = Math.exp((-0.5 * i * i) / (sigma * sigma));
  for (let i = 0; i < k.length; i++) k[i] /= s;
  return k;
}

// One 1D convolution along x, then along y, row by row so that the memory is read in order.
// `edge`: 0 clamp to the edge (FLIP), 1 reflect d c b a | a b c d (scipy's default).
function mapIndex(i, n, edge) {
  if (i >= 0 && i < n) return i;
  if (edge === 0) return i < 0 ? 0 : n - 1;
  return i < 0 ? -1 - i : 2 * n - 1 - i;
}

function convX(src, w, h, k, dst, edge) {
  const r = k.length >> 1;
  const ix = new Int32Array(k.length);
  for (let y = 0; y < h; y++) {
    const o = y * w;
    for (let x = 0; x < w; x++) {
      let a = 0;
      if (x >= r && x < w - r) {
        const c = o + x - r;
        for (let j = 0; j < k.length; j++) a += k[j] * src[c + j];
      } else {
        for (let j = 0; j < k.length; j++) a += k[j] * src[o + mapIndex(x + j - r, w, edge)];
      }
      dst[o + x] = a;
    }
  }
}

function convY(src, w, h, k, dst, edge) {
  const r = k.length >> 1;
  const acc = new Float64Array(w);
  for (let y = 0; y < h; y++) {
    acc.fill(0);
    for (let j = 0; j < k.length; j++) {
      const o = mapIndex(y + j - r, h, edge) * w;
      const kj = k[j];
      for (let x = 0; x < w; x++) acc[x] += kj * src[o + x];
    }
    const d = y * w;
    for (let x = 0; x < w; x++) dst[d + x] = acc[x];
  }
}

function blur(src, w, h, k, tmp, edge) {
  const dst = new Float32Array(w * h);
  convX(src, w, h, k, tmp, edge);
  convY(tmp, w, h, k, dst, edge);
  return dst;
}

export function psnr(a, b, w, h) {
  let e = 0;
  const n = w * h * 4;
  for (let i = 0; i < n; i += 4) {
    const dr = a[i] - b[i];
    const dg = a[i + 1] - b[i + 1];
    const db = a[i + 2] - b[i + 2];
    e += dr * dr + dg * dg + db * db;
  }
  if (e === 0) return Infinity;
  return 10 * Math.log10((255 * 255 * 3 * w * h) / e);
}

const G11 = gauss(1.5, 5);
const C1 = 0.01 ** 2;
const C2 = 0.03 ** 2;

function plane(rgba, w, h, c) {
  const p = new Float32Array(w * h);
  for (let i = 0, j = c; i < p.length; i++, j += 4) p[i] = rgba[j] / 255;
  return p;
}

function square(a) {
  const o = new Float32Array(a.length);
  for (let i = 0; i < a.length; i++) o[i] = a[i] * a[i];
  return o;
}

function product(a, b) {
  const o = new Float32Array(a.length);
  for (let i = 0; i < a.length; i++) o[i] = a[i] * b[i];
  return o;
}

// What SSIM keeps of the reference from one upscaler to the next.
export function ssimPrepare(rgba, w, h) {
  const tmp = new Float32Array(w * h);
  const ch = [];
  for (let c = 0; c < 3; c++) {
    const x = plane(rgba, w, h, c);
    ch.push({ x, ux: blur(x, w, h, G11, tmp, 1), uxx: blur(square(x), w, h, G11, tmp, 1) });
  }
  return ch;
}

export function ssim(ref, rgba, w, h) {
  const tmp = new Float32Array(w * h);
  const pad = 5;
  let total = 0;
  for (let c = 0; c < 3; c++) {
    const { x, ux, uxx } = ref[c];
    const yy = plane(rgba, w, h, c);
    const uy = blur(yy, w, h, G11, tmp, 1);
    const uyy = blur(square(yy), w, h, G11, tmp, 1);
    const uxy = blur(product(x, yy), w, h, G11, tmp, 1);
    let s = 0;
    for (let row = pad; row < h - pad; row++) {
      const o = row * w;
      for (let i = o + pad; i < o + w - pad; i++) {
        const mx = ux[i];
        const my = uy[i];
        const vx = uxx[i] - mx * mx;
        const vy = uyy[i] - my * my;
        const vxy = uxy[i] - mx * my;
        s += ((2 * mx * my + C1) * (2 * vxy + C2)) / ((mx * mx + my * my + C1) * (vx + vy + C2));
      }
    }
    total += s / ((w - 2 * pad) * (h - 2 * pad));
  }
  return total / 3;
}

const MS_W = [0.0448, 0.2856, 0.3001, 0.2363, 0.1333];

function luma709(rgba, w, h) {
  const p = new Float32Array(w * h);
  for (let i = 0, j = 0; i < p.length; i++, j += 4) p[i] = (0.2126 * rgba[j] + 0.7152 * rgba[j + 1] + 0.0722 * rgba[j + 2]) / 255;
  return p;
}

function halve(a, w, h) {
  const nw = w >> 1;
  const nh = h >> 1;
  const o = new Float32Array(nw * nh);
  for (let y = 0; y < nh; y++) {
    const r0 = 2 * y * w;
    const r1 = r0 + w;
    for (let x = 0; x < nw; x++) o[y * nw + x] = (a[r0 + 2 * x] + a[r0 + 2 * x + 1] + a[r1 + 2 * x] + a[r1 + 2 * x + 1]) / 4;
  }
  return { a: o, w: nw, h: nh };
}

export function msssimPrepare(rgba, w, h) {
  const levels = [];
  let cur = { a: luma709(rgba, w, h), w, h };
  for (let s = 0; s < 5; s++) {
    const tmp = new Float32Array(cur.w * cur.h);
    levels.push({ ...cur, mu: blur(cur.a, cur.w, cur.h, G11, tmp, 1), aa: blur(square(cur.a), cur.w, cur.h, G11, tmp, 1) });
    if (s < 4) cur = halve(cur.a, cur.w, cur.h);
  }
  return levels;
}

export function msssim(ref, rgba, w, h) {
  let cur = { a: luma709(rgba, w, h), w, h };
  let out = 1;
  for (let s = 0; s < 5; s++) {
    const L = ref[s];
    const tmp = new Float32Array(cur.w * cur.h);
    const mb = blur(cur.a, cur.w, cur.h, G11, tmp, 1);
    const bb = blur(square(cur.a), cur.w, cur.h, G11, tmp, 1);
    const ab = blur(product(L.a, cur.a), cur.w, cur.h, G11, tmp, 1);
    let sum = 0;
    const n = cur.w * cur.h;
    for (let i = 0; i < n; i++) {
      const ma = L.mu[i];
      const m2 = mb[i];
      const va = L.aa[i] - ma * ma;
      const vb = bb[i] - m2 * m2;
      const cov = ab[i] - ma * m2;
      let v = (2 * cov + C2) / (va + vb + C2);
      if (s === 4) v *= (2 * ma * m2 + C1) / (ma * ma + m2 * m2 + C1);
      sum += v;
    }
    out *= Math.pow(Math.max(sum / n, 0), MS_W[s]);
    if (s < 4) cur = halve(cur.a, cur.w, cur.h);
  }
  return out;
}

// ── LDR-FLIP ──
const PPD = 0.7 * (3840 / 0.7) * (Math.PI / 180);
const GQC = 0.7;
const GPC = 0.4;
const GPT = 0.95;
const GW = 0.082;
const LIN = new Float32Array(256);
for (let v = 0; v < 256; v++) {
  const s = v / 255;
  LIN[v] = s > 0.04045 ? Math.pow((s + 0.055) / 1.055, 2.4) : s / 12.92;
}
const RAD = Math.ceil(3 * Math.sqrt(0.04 / (2 * Math.PI * Math.PI)) * PPD);
const gaussAB = (x2, a, b) => a * Math.sqrt(Math.PI / b) * Math.exp((-Math.PI * Math.PI * x2) / b);
const gaussSqrt = (x2, a, b) => Math.sqrt(a * Math.sqrt(Math.PI / b)) * Math.exp((-Math.PI * Math.PI * x2) / b);
const KY = new Float64Array(2 * RAD + 1);
const KCX = new Float64Array(2 * RAD + 1);
const KZ1 = new Float64Array(2 * RAD + 1);
const KZ2 = new Float64Array(2 * RAD + 1);
{
  let sy = 0;
  let sc = 0;
  let s1 = 0;
  let s2 = 0;
  for (let i = 0; i <= 2 * RAD; i++) {
    const ix = (i - RAD) / PPD;
    const x2 = ix * ix;
    sy += KY[i] = gaussAB(x2, 1, 0.0047);
    sc += KCX[i] = gaussAB(x2, 1, 0.0053);
    s1 += KZ1[i] = gaussSqrt(x2, 34.1, 0.04);
    s2 += KZ2[i] = gaussSqrt(x2, 13.5, 0.025);
  }
  const nz = 1 / Math.sqrt(s1 * s1 + s2 * s2);
  for (let i = 0; i <= 2 * RAD; i++) {
    KY[i] /= sy;
    KCX[i] /= sc;
    KZ1[i] *= nz;
    KZ2[i] *= nz;
  }
}
const SD = 0.5 * GW * PPD;
const FR = Math.ceil(3 * SD);
const FG = new Float64Array(2 * FR + 1);
const FD = new Float64Array(2 * FR + 1);
const FDD = new Float64Array(2 * FR + 1);
{
  let g = 0;
  let dp = 0;
  let dn = 0;
  let ddp = 0;
  let ddn = 0;
  for (let i = 0; i <= 2 * FR; i++) {
    const x = i - FR;
    const v = Math.exp((-x * x) / (2 * SD * SD));
    g += FG[i] = v;
    const d = -x * v;
    if (d > 0) dp += d;
    else dn -= d;
    FD[i] = d;
    const dd = ((x * x) / (SD * SD) - 1) * v;
    if (dd > 0) ddp += dd;
    else ddn -= dd;
    FDD[i] = dd;
  }
  for (let i = 0; i <= 2 * FR; i++) {
    FG[i] /= g;
    FD[i] /= FD[i] > 0 ? dp : dn;
    FDD[i] /= FDD[i] > 0 ? ddp : ddn;
  }
}
const WHITE = [0.950428545, 1, 1.088900371];
const INV = [1.052156925, 1, 0.91835767];
const M = [10135552 / 24577794, 8788810 / 24577794, 4435075 / 24577794, 2613072 / 12288897, 8788810 / 12288897, 887015 / 12288897, 1425312 / 73733382, 8788810 / 73733382, 70074185 / 73733382];
const DELTA3 = (6 / 29) ** 3;
const FACT = 1 / (3 * (6 / 29) ** 2);
const TERM = 4 / 29;
const labf = (t) => (t > DELTA3 ? Math.cbrt(t) : FACT * t + TERM);
const CMAX = (() => {
  const lab = (r, g, b) => {
    const X = (M[0] * r + M[1] * g + M[2] * b) * INV[0];
    const Y = (M[3] * r + M[4] * g + M[5] * b) * INV[1];
    const Z = (M[6] * r + M[7] * g + M[8] * b) * INV[2];
    const fx = labf(X);
    const fy = labf(Y);
    const fz = labf(Z);
    const L = 116 * fy - 16;
    return [L, 0.01 * L * 500 * (fx - fy), 0.01 * L * 200 * (fy - fz)];
  };
  const a = lab(0, 1, 0);
  const b = lab(0, 0, 1);
  return Math.pow(Math.abs(a[0] - b[0]) + Math.hypot(a[1] - b[1], a[2] - b[2]), GQC);
})();
const PCCMAX = GPC * CMAX;

// What FLIP keeps of one picture: L*, Hunt's a and b after the spatial filter, and its edges and points.
export function flipPrepare(rgba, w, h) {
  const n = w * h;
  const Y = new Float32Array(n);
  const Cx = new Float32Array(n);
  const Cz = new Float32Array(n);
  const y01 = new Float32Array(n);
  for (let i = 0, j = 0; i < n; i++, j += 4) {
    const r = LIN[rgba[j]];
    const g = LIN[rgba[j + 1]];
    const b = LIN[rgba[j + 2]];
    const x = (M[0] * r + M[1] * g + M[2] * b) * INV[0];
    const yy = M[3] * r + M[4] * g + M[5] * b;
    const z = (M[6] * r + M[7] * g + M[8] * b) * INV[2];
    y01[i] = yy;
    Y[i] = 116 * yy - 16;
    Cx[i] = 500 * (x - yy);
    Cz[i] = 200 * (yy - z);
  }
  const tmp = new Float32Array(n);
  const fY = blur(Y, w, h, KY, tmp, 0);
  const fC = blur(Cx, w, h, KCX, tmp, 0);
  const fZ1 = blur(Cz, w, h, KZ1, tmp, 0);
  const fZ2 = blur(Cz, w, h, KZ2, tmp, 0);
  const L = new Float32Array(n);
  const A = new Float32Array(n);
  const B = new Float32Array(n);
  for (let i = 0; i < n; i++) {
    const vy = (fY[i] + 16) / 116;
    const X0 = (vy + fC[i] / 500) * WHITE[0];
    const Z0 = (vy - (fZ1[i] + fZ2[i]) / 200) * WHITE[2];
    let r = 3.241003275 * X0 - 1.537398934 * vy - 0.498615861 * Z0;
    let g = -0.969224334 * X0 + 1.875930071 * vy + 0.041554224 * Z0;
    let b = 0.055639423 * X0 - 0.204011202 * vy + 1.057148933 * Z0;
    r = r < 0 ? 0 : r > 1 ? 1 : r;
    g = g < 0 ? 0 : g > 1 ? 1 : g;
    b = b < 0 ? 0 : b > 1 ? 1 : b;
    const fx = labf((M[0] * r + M[1] * g + M[2] * b) * INV[0]);
    const fy = labf(M[3] * r + M[4] * g + M[5] * b);
    const fz = labf((M[6] * r + M[7] * g + M[8] * b) * INV[2]);
    const l = 116 * fy - 16;
    L[i] = l;
    A[i] = 0.01 * l * 500 * (fx - fy);
    B[i] = 0.01 * l * 200 * (fy - fz);
  }
  // edges and points, on the normalized Y (116 Y - 16 back to Y): dx and ddx along x then the Gaussian along y; the Gaussian along x then dy and ddy along y
  const gx = new Float32Array(n);
  const dx = new Float32Array(n);
  const ddx = new Float32Array(n);
  convX(y01, w, h, FG, gx, 0);
  convX(y01, w, h, FD, dx, 0);
  convX(y01, w, h, FDD, ddx, 0);
  const t = new Float32Array(n);
  const edge = new Float32Array(n);
  const point = new Float32Array(n);
  convY(dx, w, h, FG, t, 0);
  convY(gx, w, h, FD, dx, 0);
  for (let i = 0; i < n; i++) edge[i] = Math.sqrt(t[i] * t[i] + dx[i] * dx[i]);
  convY(ddx, w, h, FG, t, 0);
  convY(gx, w, h, FDD, dx, 0);
  for (let i = 0; i < n; i++) point[i] = Math.sqrt(t[i] * t[i] + dx[i] * dx[i]);
  return { L, A, B, edge, point };
}

export function flipMean(ref, test, w, h, map = null) {
  const n = w * h;
  let sum = 0;
  const NORM = 1 / Math.SQRT2;
  const mul = GPT / PCCMAX;
  const k = (1 - GPT) / (CMAX - PCCMAX);
  for (let i = 0; i < n; i++) {
    const da = ref.A[i] - test.A[i];
    const db = ref.B[i] - test.B[i];
    let c = Math.pow(Math.abs(ref.L[i] - test.L[i]) + Math.sqrt(da * da + db * db), GQC);
    c = c < PCCMAX ? c * mul : GPT + (c - PCCMAX) * k;
    const de = Math.abs(ref.edge[i] - test.edge[i]);
    const dp = Math.abs(ref.point[i] - test.point[i]);
    const f = Math.sqrt(NORM * (de > dp ? de : dp));
    const e = Math.fround(Math.pow(c, 1 - f));
    if (map) map[i] = e;
    // flip-evaluator's own mean, the figure the lab reports, is a 32-bit running sum, row by row
    sum = Math.fround(sum + e);
  }
  return sum / n;
}
