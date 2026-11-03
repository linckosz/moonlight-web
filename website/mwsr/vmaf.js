// VMAF NEG in JavaScript: libvmaf 3.0.0's integer ADM and VIF and its SVM, and the vmaf_v0.6.1neg model (vmaf-neg.json),
// written from Netflix's sources, under their licence:
/*
LICENSE - BSD+Patent
SPDX short identifier: BSD-2-Clause-Patent

Note: This license is designed to provide: a) a simple permissive license; b) that is compatible with the GNU General
Public License (GPL), version 2; and c) which also has an express patent grant included.

Copyright (c) 2020 Netflix, Inc.

Redistribution and use in source and binary forms, with or without modification, are permitted provided that the
following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following
disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following
disclaimer in the documentation and/or other materials provided with the distribution.

Subject to the terms and conditions of this license, each copyright holder and contributor hereby grants to those
receiving rights under this license a perpetual, worldwide, non-exclusive, no-charge, royalty-free, irrevocable (except
for failure to satisfy the conditions of this license) patent license to make, have made, use, offer to sell, sell,
import, and otherwise transfer this software, where such license applies only to those patent claims, already acquired
or hereafter acquired, licensable by such copyright holder or contributor that are necessarily infringed by:

(a) their Contribution(s) (the licensed copyrights of copyright holders and non-copyrightable additions of contributors,
in source or binary form) alone; or

(b) combination of their Contribution(s) with the work of authorship to which such Contribution(s) was added by such
copyright holder or contributor, if, at the time the Contribution is added, such addition causes such combination to be
necessarily infringed. The patent license shall not apply to any other combinations which include the Contribution.

Except as expressly stated above, no rights or licenses from any copyright holder or contributor is granted under this
license, whether expressly, by implication, estoppel or otherwise.

DISCLAIMER

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.*/
// VMAF NEG (vmaf_v0.6.1neg) of one still picture, in plain JavaScript, written from libvmaf 3.0.0's C path: integer_vif.c, integer_adm.c, predict.c, with its model.
// Same steps, same fixed-point rounding. One frame: motion2 is 0, as libvmaf gives for a first frame. 64-bit products beyond 2^53 are held in doubles: the error, at most one unit in 2^53 before a shift, does not reach the float32 the C code ends in.
// libvmaf: BSD-2-Clause-Patent (Netflix). This file is a port of it, under the same licence.

// ── VIF ──
const VIF_F = [
  [489, 935, 1640, 2640, 3896, 5274, 6547, 7455, 7784, 7455, 6547, 5274, 3896, 2640, 1640, 935, 489],
  [1244, 3663, 7925, 12590, 14692, 12590, 7925, 3663, 1244],
  [3571, 16004, 26386, 16004, 3571],
  [10904, 43728, 10904],
];
const LOG2 = new Uint16Array(65537);
for (let i = 32767; i < 65536; i++) LOG2[i] = Math.round(Math.fround(Math.fround(Math.log2(i)) * 2048));
const SIGMA_NSQ = 131072;
const EPS = 65536 * 1.0e-10;
const P32 = 4294967296;

function log2u(t) {
  // log2_32 and log2_64 of the C text: the best 16 bits
  let k;
  if (t < P32) k = 16 - Math.clz32(t);
  else k = 48 - Math.clz32(Math.floor(t / P32));
  const v = k >= 0 ? Math.floor(t / 2 ** k) : t * 2 ** -k;
  return LOG2[v] + 2048 * k;
}

// (a * b + 2^31) >> 32 of two uint32
function mulRound(a, b) {
  const ah = a >>> 16;
  const al = a & 0xffff;
  const bh = b >>> 16;
  const bl = b & 0xffff;
  return ah * bh + Math.floor(((ah * bl + al * bh) * 65536 + al * bl + 2147483648) / P32);
}

const mirror = (i, n) => (i < 0 ? -i : i >= n ? 2 * (n - 1) - i : i);

function vifStatistic(ref, dis, w, h, scale, limit, s8) {
  const filt = VIF_F[scale];
  const fw = filt.length;
  const half = fw >> 1;
  const pad = half;
  const n = w + 2 * pad;
  const tMu1 = new Float64Array(n);
  const tMu2 = new Float64Array(n);
  const tRef = new Float64Array(n);
  const tDis = new Float64Array(n);
  const tRd = new Float64Array(n);
  let numLog = 0;
  let denLog = 0;
  let numNon = 0;
  let denNon = 0;
  const offs = new Int32Array(fw);
  for (let i = 0; i < h; i++) {
    for (let k = 0; k < fw; k++) offs[k] = mirror(i - half + k, h) * w;
    for (let j = 0; j < w; j++) {
      let m1 = 0;
      let m2 = 0;
      let rr = 0;
      let dd = 0;
      let rd = 0;
      for (let k = 0; k < fw; k++) {
        const f = filt[k];
        const a = ref[offs[k] + j];
        const b = dis[offs[k] + j];
        const fa = f * a;
        const fb = f * b;
        m1 += fa;
        m2 += fb;
        rr += fa * a;
        dd += fb * b;
        rd += fa * b;
      }
      if (s8) {
        tMu1[pad + j] = (m1 + 128) >>> 8;
        tMu2[pad + j] = (m2 + 128) >>> 8;
        tRef[pad + j] = rr;
        tDis[pad + j] = dd;
        tRd[pad + j] = rd;
      } else {
        tMu1[pad + j] = Math.floor((m1 + 32768) / 65536) & 0xffff;
        tMu2[pad + j] = Math.floor((m2 + 32768) / 65536) & 0xffff;
        tRef[pad + j] = Math.floor((rr + 32768) / 65536);
        tDis[pad + j] = Math.floor((dd + 32768) / 65536);
        tRd[pad + j] = Math.floor((rd + 32768) / 65536);
      }
    }
    for (let f = 1; f <= half; f++) {
      tMu1[pad - f] = tMu1[pad + f];
      tMu2[pad - f] = tMu2[pad + f];
      tRef[pad - f] = tRef[pad + f];
      tDis[pad - f] = tDis[pad + f];
      tRd[pad - f] = tRd[pad + f];
      const l = pad + w - 1 - f;
      const r = pad + w - 1 + f;
      tMu1[r] = tMu1[l];
      tMu2[r] = tMu2[l];
      tRef[r] = tRef[l];
      tDis[r] = tDis[l];
      tRd[r] = tRd[l];
    }
    for (let j = 0; j < w; j++) {
      let m1 = 0;
      let m2 = 0;
      let rr = 0;
      let dd = 0;
      let rd = 0;
      const b = pad + j - half;
      for (let k = 0; k < fw; k++) {
        const f = filt[k];
        m1 += f * tMu1[b + k];
        m2 += f * tMu2[b + k];
        rr += f * tRef[b + k];
        dd += f * tDis[b + k];
        rd += f * tRd[b + k];
      }
      const s1 = Math.floor((rr + 32768) / 65536) - mulRound(m1, m1);
      let s2 = Math.floor((dd + 32768) / 65536) - mulRound(m2, m2);
      const s12 = Math.floor((rd + 32768) / 65536) - mulRound(m1, m2);
      if (s2 < 0) s2 = 0;
      if (s1 >= SIGMA_NSQ) {
        denLog += log2u(SIGMA_NSQ + s1) - 34816;
        if (s12 > 0 && s2 > 0) {
          let g = s12 / (s1 + EPS);
          let sv = Math.trunc(s2 - g * s12);
          if (sv < 0) sv = 0;
          if (g > limit) g = limit;
          const n1 = sv + SIGMA_NSQ;
          numLog += log2u(Math.trunc(g * g * s1) + n1) - log2u(n1);
        }
      } else {
        numNon += s2;
        denNon++;
      }
    }
  }
  const num = Math.fround(numLog / 2048 + (denNon - numNon / 16384 / 65025));
  const den = Math.fround(denLog / 2048 + denNon);
  return { num, den };
}

// One step down the pyramid: filtered, then every other pixel of every other row.
function vifDown(src, w, h, scale, s8) {
  const filt = VIF_F[scale];
  const fw = filt.length;
  const half = fw >> 1;
  const nw = w >> 1;
  const nh = h >> 1;
  const out = new Uint16Array(nw * nh);
  const t = new Float64Array(w + 2 * half);
  const offs = new Int32Array(fw);
  for (let i = 0; i < h; i += 2) {
    if (i >> 1 >= nh) break;
    for (let k = 0; k < fw; k++) offs[k] = mirror(i - half + k, h) * w;
    for (let j = 0; j < w; j++) {
      let a = 0;
      for (let k = 0; k < fw; k++) a += filt[k] * src[offs[k] + j];
      t[half + j] = s8 ? (a + 128) >>> 8 : Math.floor((a + 32768) / 65536) & 0xffff;
    }
    for (let f = 1; f <= half; f++) {
      t[half - f] = t[half + f];
      t[half + w - 1 + f] = t[half + w - 1 - f];
    }
    const o = (i >> 1) * nw;
    for (let j = 0; j < nw; j++) {
      let a = 0;
      const b = half + 2 * j - half;
      for (let k = 0; k < fw; k++) a += filt[k] * t[b + k];
      out[o + j] = Math.floor((a + 32768) / 65536) & 0xffff;
    }
  }
  return { a: out, w: nw, h: nh };
}

function vifScores(refY, disY, w, h) {
  const LIMIT = 1.0;
  const out = [];
  let r = refY;
  let d = disY;
  let cw = w;
  let ch = h;
  for (let s = 0; s < 4; s++) {
    if (s > 0) {
      const rn = vifDown(r, cw, ch, s, s === 1);
      const dn = vifDown(d, cw, ch, s, s === 1);
      r = rn.a;
      d = dn.a;
      cw = rn.w;
      ch = rn.h;
    }
    const { num, den } = vifStatistic(r, d, cw, ch, s, LIMIT, s === 0);
    out.push(Math.fround(num / den));
  }
  return out;
}

// ── ADM ──
const DIV = new Int32Array(65537);
for (let i = 1; i <= 32768; i++) {
  const recip = Math.trunc(1073741824 / i);
  DIV[32768 + i] = recip;
  DIV[32768 - i] = -recip;
}
const LO = [15826, 27411, 7345, -4240];
const HI = [-4240, -7345, 27411, -15826];
const LO_SUM = 46342;
const fr = Math.fround;
const COS1 = fr(Math.cos((1.0 * Math.PI) / 180.0) * Math.cos((1.0 * Math.PI) / 180.0));
const A_Y = fr(0.495);
const K_Y = fr(0.466);
const F0_Y = fr(0.401);
const G_Y = [fr(1.501), 1.0, fr(0.534), 1.0];
const AMP = [
  [0.62171, 0.67234, 0.72709, 0.67234],
  [0.34537, 0.41317, 0.49428, 0.41317],
  [0.18004, 0.22727, 0.28688, 0.22727],
  [0.091401, 0.11792, 0.15214, 0.11792],
].map((r) => r.map(fr));
const THIRD = fr(1 / 3);
function quant(lambda, theta) {
  const r = fr((3.0 * 1080 * Math.PI) / 180.0);
  const temp = fr(Math.log10((Math.pow(2.0, lambda + 1) * F0_Y * G_Y[theta]) / r));
  return fr((2.0 * A_Y * Math.pow(10.0, fr(fr(K_Y * temp) * temp))) / AMP[lambda][theta]);
}
function rfactors(scale) {
  const f1 = quant(scale, 1);
  const f2 = quant(scale, 2);
  return [fr(1 / f1), fr(1 / f1), fr(1 / f2)];
}
const powf3 = (x) => fr(Math.pow(fr(x), THIRD));

function indices(n) {
  // dwt2_src_indices_filt, one axis
  const half = (n + 1) >> 1;
  const ind = [new Int32Array(half), new Int32Array(half), new Int32Array(half), new Int32Array(half)];
  for (let i = 0; i < half; i++) {
    if (i === 0) {
      ind[0][0] = 1;
      ind[1][0] = 0;
      ind[2][0] = 1;
      ind[3][0] = 2;
      continue;
    }
    const f = (v) => (i >= half - 2 && v >= n ? 2 * n - v - 1 : v);
    ind[0][i] = f(2 * i - 1);
    ind[1][i] = f(2 * i);
    ind[2][i] = f(2 * i + 1);
    ind[3][i] = f(2 * i + 2);
  }
  return ind;
}

function dwt8(src, w, h) {
  const nw = (w + 1) >> 1;
  const nh = (h + 1) >> 1;
  const iy = indices(h);
  const ix = indices(w);
  const a = new Int16Array(nw * nh);
  const v = new Int16Array(nw * nh);
  const hh = new Int16Array(nw * nh);
  const d = new Int16Array(nw * nh);
  const lo = new Int16Array(w);
  const hi = new Int16Array(w);
  const norm = LO_SUM * 128;
  for (let i = 0; i < nh; i++) {
    const r0 = iy[0][i] * w;
    const r1 = iy[1][i] * w;
    const r2 = iy[2][i] * w;
    const r3 = iy[3][i] * w;
    for (let j = 0; j < w; j++) {
      const s0 = src[r0 + j];
      const s1 = src[r1 + j];
      const s2 = src[r2 + j];
      const s3 = src[r3 + j];
      lo[j] = (15826 * s0 + 27411 * s1 + 7345 * s2 - 4240 * s3 - norm + 128) >> 8;
      hi[j] = (-4240 * s0 - 7345 * s1 + 27411 * s2 - 15826 * s3 + 128) >> 8;
    }
    const o = i * nw;
    for (let j = 0; j < nw; j++) {
      const j0 = ix[0][j];
      const j1 = ix[1][j];
      const j2 = ix[2][j];
      const j3 = ix[3][j];
      let s0 = lo[j0];
      let s1 = lo[j1];
      let s2 = lo[j2];
      let s3 = lo[j3];
      a[o + j] = (15826 * s0 + 27411 * s1 + 7345 * s2 - 4240 * s3 + 32768) >> 16;
      v[o + j] = (-4240 * s0 - 7345 * s1 + 27411 * s2 - 15826 * s3 + 32768) >> 16;
      s0 = hi[j0];
      s1 = hi[j1];
      s2 = hi[j2];
      s3 = hi[j3];
      hh[o + j] = (15826 * s0 + 27411 * s1 + 7345 * s2 - 4240 * s3 + 32768) >> 16;
      d[o + j] = (-4240 * s0 - 7345 * s1 + 27411 * s2 - 15826 * s3 + 32768) >> 16;
    }
  }
  return { a, v, h: hh, d, w: nw, hgt: nh };
}

const ADD_VP = [0, 32768, 32768];
const SH_VP = [0, 16, 16];
const ADD_HP = [16384, 32768, 16384];
const SH_HP = [15, 16, 15];

function dwt32(src, w, h, scale) {
  const nw = (w + 1) >> 1;
  const nh = (h + 1) >> 1;
  const iy = indices(h);
  const ix = indices(w);
  const a = new Int32Array(nw * nh);
  const v = new Int32Array(nw * nh);
  const hh = new Int32Array(nw * nh);
  const d = new Int32Array(nw * nh);
  const lo = new Int32Array(w);
  const hi = new Int32Array(w);
  const av = ADD_VP[scale - 1];
  const dv = 2 ** SH_VP[scale - 1];
  const ah = ADD_HP[scale - 1];
  const dh = 2 ** SH_HP[scale - 1];
  for (let i = 0; i < nh; i++) {
    const r0 = iy[0][i] * w;
    const r1 = iy[1][i] * w;
    const r2 = iy[2][i] * w;
    const r3 = iy[3][i] * w;
    for (let j = 0; j < w; j++) {
      const s0 = src[r0 + j];
      const s1 = src[r1 + j];
      const s2 = src[r2 + j];
      const s3 = src[r3 + j];
      lo[j] = Math.floor((15826 * s0 + 27411 * s1 + 7345 * s2 - 4240 * s3 + av) / dv);
      hi[j] = Math.floor((-4240 * s0 - 7345 * s1 + 27411 * s2 - 15826 * s3 + av) / dv);
    }
    const o = i * nw;
    for (let j = 0; j < nw; j++) {
      const j0 = ix[0][j];
      const j1 = ix[1][j];
      const j2 = ix[2][j];
      const j3 = ix[3][j];
      let s0 = lo[j0];
      let s1 = lo[j1];
      let s2 = lo[j2];
      let s3 = lo[j3];
      a[o + j] = Math.floor((15826 * s0 + 27411 * s1 + 7345 * s2 - 4240 * s3 + ah) / dh);
      v[o + j] = Math.floor((-4240 * s0 - 7345 * s1 + 27411 * s2 - 15826 * s3 + ah) / dh);
      s0 = hi[j0];
      s1 = hi[j1];
      s2 = hi[j2];
      s3 = hi[j3];
      hh[o + j] = Math.floor((15826 * s0 + 27411 * s1 + 7345 * s2 - 4240 * s3 + ah) / dh);
      d[o + j] = Math.floor((-4240 * s0 - 7345 * s1 + 27411 * s2 - 15826 * s3 + ah) / dh);
    }
  }
  return { a, v, h: hh, d, w: nw, hgt: nh };
}

function tapRegion(w, h) {
  let left = Math.trunc(w * 0.1 - 0.5 - 1);
  let top = Math.trunc(h * 0.1 - 0.5 - 1);
  let right = w - left + 2;
  let bottom = h - top + 2;
  if (left < 0) left = 0;
  if (right > w) right = w;
  if (top < 0) top = 0;
  if (bottom > h) bottom = h;
  return { left, top, right, bottom };
}

function scoreRegion(w, h) {
  const left = Math.trunc(w * 0.1 - 0.5);
  const top = Math.trunc(h * 0.1 - 0.5);
  return { left, top, right: w - left, bottom: h - top };
}

// adm_decouple and adm_decouple_s123, written once: i4 tells the division's form
function decouple(ref, dis, i4, limit) {
  const w = ref.w;
  const h = ref.hgt;
  const n = w * h;
  const Arr = i4 ? Int32Array : Int16Array;
  const R = { h: new Arr(n), v: new Arr(n), d: new Arr(n) };
  const A = { h: new Arr(n), v: new Arr(n), d: new Arr(n) };
  const { left, top, right, bottom } = tapRegion(w, h);
  const kOf = i4
    ? (o, t) => {
        if (o === 0) return 32768;
        const abs = Math.abs(o);
        let shift = 0;
        let msb = abs;
        if (abs >= 32768) {
          shift = 17 - Math.clz32(abs);
          msb = Math.floor((abs + 2 ** (shift - 1)) / 2 ** shift);
        }
        const sign = o < 0 ? -1 : 1;
        const k = Math.floor((DIV[msb + 32768] * t * sign + 2 ** (14 + shift)) / 2 ** (15 + shift));
        return k < 0 ? 0 : k > 32768 ? 32768 : k;
      }
    : (o, t) => {
        if (o === 0) return 32768;
        const k = Math.floor((DIV[o + 32768] * t + 16384) / 32768);
        return k < 0 ? 0 : k > 32768 ? 32768 : k;
      };
  for (let i = top; i < bottom; i++) {
    const row = i * w;
    for (let j = left; j < right; j++) {
      const x = row + j;
      const oh = ref.h[x];
      const ov = ref.v[x];
      const od = ref.d[x];
      const th = dis.h[x];
      const tv = dis.v[x];
      const td = dis.d[x];
      const dp = oh * th + ov * tv;
      const omag = oh * oh + ov * ov;
      const tmag = th * th + tv * tv;
      const fdp = fr(dp) / 4096;
      const angle = fdp >= 0 && fdp * fdp >= COS1 * (fr(omag) / 4096) * (fr(tmag) / 4096);
      const kh = kOf(oh, th);
      const kv = kOf(ov, tv);
      const kd = kOf(od, td);
      let rh = i4 ? Math.floor((kh * oh + 16384) / 32768) : (kh * oh + 16384) >> 15;
      let rv = i4 ? Math.floor((kv * ov + 16384) / 32768) : (kv * ov + 16384) >> 15;
      let rd = i4 ? Math.floor((kd * od + 16384) / 32768) : (kd * od + 16384) >> 15;
      if (angle) {
        if (kh > 0 && oh > 0) rh = Math.min(rh * limit, th);
        else if (kh > 0 && oh < 0) rh = Math.max(rh * limit, th);
        if (kv > 0 && ov > 0) rv = Math.min(rv * limit, tv);
        else if (kv > 0 && ov < 0) rv = Math.max(rv * limit, tv);
        if (kd > 0 && od > 0) rd = Math.min(rd * limit, td);
        else if (kd > 0 && od < 0) rd = Math.max(rd * limit, td);
      }
      R.h[x] = rh;
      R.v[x] = rv;
      R.d[x] = rd;
      A.h[x] = th - rh;
      A.v[x] = tv - rv;
      A.d[x] = td - rd;
    }
  }
  return { R, A };
}

function den0(b) {
  const w = b.w;
  const h = b.hgt;
  const rf = rfactors(0);
  const { left, top, right, bottom } = scoreRegion(w, h);
  let shift = Math.ceil(Math.log2((bottom - top) * (right - left)) - 20);
  if (shift < 0) shift = 0;
  const add = shift > 0 ? 2 ** (shift - 1) : 0;
  const dv = 2 ** shift;
  const res = [0, 0, 0];
  const bands = [b.h, b.v, b.d];
  for (let t = 0; t < 3; t++) {
    const s = bands[t];
    let acc = 0;
    for (let i = top; i < bottom; i++) {
      let inner = 0;
      const row = i * w;
      for (let j = left; j < right; j++) {
        const a = Math.abs(s[row + j]);
        inner += a * a * a;
      }
      acc += Math.floor((inner + add) / dv);
    }
    res[t] = (acc / 2 ** (18 - shift)) * Math.pow(rf[t], 3);
  }
  const padd = powf3(((bottom - top) * (right - left)) / 32);
  return fr(fr(fr(powf3(res[0]) + padd) + fr(powf3(res[1]) + padd)) + fr(powf3(res[2]) + padd));
}

const DEN_SQ = [31, 30, 31];
const DEN_CONV = [32, 27, 23];

function denN(b, scale) {
  const w = b.w;
  const h = b.hgt;
  const rf = rfactors(scale);
  const { left, top, right, bottom } = scoreRegion(w, h);
  const sq = 2 ** DEN_SQ[scale - 1];
  const shc = Math.ceil(Math.log2(right - left));
  const addc = 2 ** (shc - 1);
  const dc = 2 ** shc;
  const sha = Math.ceil(Math.log2(bottom - top));
  const adda = 2 ** (sha - 1);
  const da = 2 ** sha;
  const res = [0, 0, 0];
  const bands = [b.h, b.v, b.d];
  for (let t = 0; t < 3; t++) {
    const s = bands[t];
    let acc = 0;
    for (let i = top; i < bottom; i++) {
      let inner = 0;
      const row = i * w;
      for (let j = left; j < right; j++) {
        const a = Math.abs(s[row + j]);
        inner += Math.floor((Math.floor((a * a + sq) / sq) * a + addc) / dc);
      }
      acc += Math.floor((inner + adda) / da);
    }
    res[t] = (acc / 2 ** (DEN_CONV[scale - 1] - sha - shc)) * Math.pow(rf[t], 3);
  }
  const padd = powf3(((bottom - top) * (right - left)) / 32);
  return fr(fr(fr(powf3(res[0]) + padd) + fr(powf3(res[1]) + padd)) + fr(powf3(res[2]) + padd));
}

const IRF0 = [36453, 36453, 49417];

function csf0(A, w, h) {
  const n = w * h;
  const out = [];
  const shifts = [15, 15, 17];
  const adds = [16384, 16384, 65535];
  const { left, top, right, bottom } = tapRegion(w, h);
  const bands = [A.h, A.v, A.d];
  for (let t = 0; t < 3; t++) {
    const src = bands[t];
    const a = new Int16Array(n);
    const f = new Int16Array(n);
    const rf = IRF0[t];
    const sh = shifts[t];
    const ad = adds[t];
    for (let i = top; i < bottom; i++) {
      const row = i * w;
      for (let j = left; j < right; j++) {
        const v = ((rf * src[row + j] + ad) >> sh) << 16 >> 16;
        a[row + j] = v;
        f[row + j] = (4369 * Math.abs(v) + 2048) >> 12;
      }
    }
    out.push({ a, f });
  }
  return out;
}

function cm0(R, csf, w, h) {
  const { left, top, right, bottom } = scoreRegion(w, h);
  const startC = left > 1 ? left : 1;
  const endC = right < w - 1 ? right : w - 1;
  const startR = top > 1 ? top : 1;
  const endR = bottom < h - 1 ? bottom : h - 1;
  const shCubHV = Math.ceil(Math.log2(w) - 4);
  const shCubD = Math.ceil(Math.log2(w) - 3);
  const shIn = Math.ceil(Math.log2(h));
  const addIn = 2 ** (shIn - 1);
  const dIn = 2 ** shIn;
  const addHV = 2 ** (shCubHV - 1);
  const dHV = 2 ** shCubHV;
  const addD = 2 ** (shCubD - 1);
  const dD = 2 ** shCubD;
  const P29 = 536870912;
  const P30 = 1073741824;
  let accH = 0;
  let accV = 0;
  let accD = 0;
  const [c0, c1, c2] = csf;
  const fa = [c0.f, c1.f, c2.f];
  const aa = [c0.a, c1.a, c2.a];
  for (let i = startR; i < endR; i++) {
    let ih = 0;
    let iv = 0;
    let id = 0;
    const row = i * w;
    for (let j = startC; j < endC; j++) {
      const x = row + j;
      let thr = 0;
      for (let t = 0; t < 3; t++) {
        const f = fa[t];
        thr += f[x - w - 1] + f[x - w] + f[x - w + 1] + f[x - 1] + f[x + 1] + f[x + w - 1] + f[x + w] + f[x + w + 1];
        thr += ((8738 * Math.abs(aa[t][x]) + 2048) >> 12) << 16 >> 16;
      }
      let xh = Math.abs(R.h[x] * IRF0[0]) - thr * 1024;
      let xv = Math.abs(R.v[x] * IRF0[1]) - thr * 1024;
      let xd = Math.abs(R.d[x] * IRF0[2]) - thr * 4096;
      if (xh > 0) ih += Math.floor((Math.floor((xh * xh + 268435456) / P29) * xh + addHV) / dHV);
      if (xv > 0) iv += Math.floor((Math.floor((xv * xv + 268435456) / P29) * xv + addHV) / dHV);
      if (xd > 0) id += Math.floor((Math.floor((xd * xd + P29) / P30) * xd + addD) / dD);
    }
    accH += Math.floor((ih + addIn) / dIn);
    accV += Math.floor((iv + addIn) / dIn);
    accD += Math.floor((id + addIn) / dIn);
  }
  const fh = fr(accH / 2 ** (52 - shCubHV - shIn));
  const fv = fr(accV / 2 ** (52 - shCubHV - shIn));
  const fd = fr(accD / 2 ** (57 - shCubD - shIn));
  const padd = powf3(((bottom - top) * (right - left)) / 32);
  return fr(fr(fr(powf3(fh) + padd) + fr(powf3(fv) + padd)) + fr(powf3(fd) + padd));
}

function csfN(A, w, h, scale) {
  const n = w * h;
  const out = [];
  const rf1 = rfactors(scale);
  const rf = rf1.map((x) => Math.trunc(x * P32));
  const P27 = 134217728;
  const P28 = 268435456;
  const P31 = 2147483648;
  const { left, top, right, bottom } = tapRegion(w, h);
  const bands = [A.h, A.v, A.d];
  for (let t = 0; t < 3; t++) {
    const src = bands[t];
    const a = new Int32Array(n);
    const f = new Int32Array(n);
    const r = rf[t];
    for (let i = top; i < bottom; i++) {
      const row = i * w;
      for (let j = left; j < right; j++) {
        const v = Math.floor((r * src[row + j] + P27) / P28);
        a[row + j] = v;
        f[row + j] = Math.floor((143165577 * Math.abs(v) + P31) / P32);
      }
    }
    out.push({ a, f });
  }
  return { csf: out, rf };
}

function cmN(R, csf, rf, w, h, scale) {
  const { left, top, right, bottom } = scoreRegion(w, h);
  const startC = left > 1 ? left : 1;
  const endC = right < w - 1 ? right : w - 1;
  const startR = top > 1 ? top : 1;
  const endR = bottom < h - 1 ? bottom : h - 1;
  const shc = Math.ceil(Math.log2(w));
  const addc = 2 ** (shc - 1);
  const dc = 2 ** shc;
  const shIn = Math.ceil(Math.log2(h));
  const addIn = 2 ** (shIn - 1);
  const dIn = 2 ** shIn;
  const P27 = 134217728;
  const P28 = 268435456;
  const P29 = 536870912;
  const P30 = 1073741824;
  const P31 = 2147483648;
  const finals = [fr(2 ** (45 - shc - shIn)), fr(2 ** (39 - shc - shIn)), fr(2 ** (36 - shc - shIn))];
  let accH = 0;
  let accV = 0;
  let accD = 0;
  const fa = [csf[0].f, csf[1].f, csf[2].f];
  const aa = [csf[0].a, csf[1].a, csf[2].a];
  for (let i = startR; i < endR; i++) {
    let ih = 0;
    let iv = 0;
    let id = 0;
    const row = i * w;
    for (let j = startC; j < endC; j++) {
      const x = row + j;
      let thr = 0;
      for (let t = 0; t < 3; t++) {
        const f = fa[t];
        thr += f[x - w - 1] + f[x - w] + f[x - w + 1] + f[x - 1] + f[x + 1] + f[x + w - 1] + f[x + w] + f[x + w + 1];
        thr += Math.floor((286331153 * Math.abs(aa[t][x]) + P31) / P32);
      }
      let xh = Math.abs(Math.floor((R.h[x] * rf[0] + P27) / P28)) - thr;
      let xv = Math.abs(Math.floor((R.v[x] * rf[1] + P27) / P28)) - thr;
      let xd = Math.abs(Math.floor((R.d[x] * rf[2] + P27) / P28)) - thr;
      if (xh > 0) ih += Math.floor((Math.floor((xh * xh + P29) / P30) * xh + addc) / dc);
      if (xv > 0) iv += Math.floor((Math.floor((xv * xv + P29) / P30) * xv + addc) / dc);
      if (xd > 0) id += Math.floor((Math.floor((xd * xd + P29) / P30) * xd + addc) / dc);
    }
    accH += Math.floor((ih + addIn) / dIn);
    accV += Math.floor((iv + addIn) / dIn);
    accD += Math.floor((id + addIn) / dIn);
  }
  const fin = finals[scale - 1];
  const fh = fr(fr(accH) / fin);
  const fv = fr(fr(accV) / fin);
  const fd = fr(fr(accD) / fin);
  const padd = powf3(((bottom - top) * (right - left)) / 32);
  return fr(fr(fr(powf3(fh) + padd) + fr(powf3(fv) + padd)) + fr(powf3(fd) + padd));
}

function adm2(refY, disY, w0, h0) {
  const LIMIT = 1.0;
  let num = 0;
  let den = 0;
  let refA = null;
  let disA = null;
  let w = w0;
  let h = h0;
  for (let s = 0; s < 4; s++) {
    const rb = s === 0 ? dwt8(refY, w, h) : dwt32(refA, w, h, s);
    const db = s === 0 ? dwt8(disY, w, h) : dwt32(disA, w, h, s);
    w = rb.w;
    h = rb.hgt;
    const { R, A } = decouple(rb, db, s > 0, LIMIT);
    if (s === 0) {
      den += den0(rb);
      const csf = csf0(A, w, h);
      num += cm0(R, csf, w, h);
      refA = Int32Array.from(rb.a);
      disA = Int32Array.from(db.a);
    } else {
      den += denN(rb, s);
      const { csf, rf } = csfN(A, w, h, s);
      num += cmN(R, csf, rf, w, h, s);
      refA = rb.a;
      disA = db.a;
    }
  }
  const limit = (1e-10 * (w0 * h0)) / (1920.0 * 1080.0);
  if (num < limit) num = 0;
  if (den < limit) den = 0;
  return den === 0 ? 1 : num / den;
}

// ── the model ──
export function parseModel(json) {
  const m = json.model_dict;
  const lines = m.model.split("\n");
  let i = 0;
  const head = {};
  while (lines[i] !== "SV") {
    const [k, v] = lines[i].split(" ");
    head[k] = v;
    i++;
  }
  i++;
  const coef = [];
  const sv = [];
  for (; i < lines.length; i++) {
    if (!lines[i].trim()) continue;
    const p = lines[i].trim().split(" ");
    coef.push(Number(p[0]));
    const x = new Float64Array(m.feature_names.length);
    for (const q of p.slice(1)) {
      const [k, v] = q.split(":");
      x[Number(k) - 1] = Number(v);
    }
    sv.push(x);
  }
  return { gamma: Number(head.gamma), rho: Number(head.rho), coef, sv, slopes: m.slopes, intercepts: m.intercepts, transform: m.score_transform, clip: m.score_clip };
}

export function predict(M, feats) {
  // feats in the model's order: adm2, motion2, vif0, vif1, vif2, vif3
  const x = feats.map((f, i) => M.slopes[i + 1] * f + M.intercepts[i + 1]);
  let sum = 0;
  for (let k = 0; k < M.sv.length; k++) {
    let d = 0;
    const s = M.sv[k];
    for (let i = 0; i < x.length; i++) d += (x[i] - s[i]) * (x[i] - s[i]);
    sum += M.coef[k] * Math.exp(-M.gamma * d);
  }
  // libvmaf applies the model's score_transform only when asked (ffmpeg and the lab do not): denormalized, then clipped
  const y = (sum - M.rho - M.intercepts[0]) / M.slopes[0];
  return Math.min(Math.max(y, M.clip[0]), M.clip[1]);
}

// BT.709, limited range, as ffmpeg's swscale writes the luma plane the lab scores.
const RY = Math.round((0.2126 * 219) / 255 * 32768);
const GY = Math.round((0.7152 * 219) / 255 * 32768);
const BY = Math.round((0.0722 * 219) / 255 * 32768);
export function lumaOf(rgba, w, h) {
  const y = new Uint8Array(w * h);
  const add = (32 << 14) + 256;
  for (let i = 0, j = 0; i < y.length; i++, j += 4) {
    const v14 = (RY * rgba[j] + GY * rgba[j + 1] + BY * rgba[j + 2] + add) >> 9;
    y[i] = (v14 + 32) >> 6;
  }
  return y;
}

export function vmafNeg(model, refY, disY, w, h) {
  const adm = adm2(refY, disY, w, h);
  const vif = vifScores(refY, disY, w, h);
  return { score: predict(model, [adm, 0, ...vif]), adm, vif };
}
