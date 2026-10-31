// The comparison page's CPU side (website/mwsr/): the pictures, the reduced
// inputs, the 4:2:0 stream, and the metrics. Nothing here leaves the browser:
// a picture loaded from the disk is decoded, reduced and measured in this
// worker and goes nowhere else.
//
// The arithmetic follows the lab that made the README's figures
// (scripts/bench/scaler-lab/): the reduction is scaler_lab.make_input
// (Pillow's Lanczos-3 on float planes, in linear light, the float32 steps of
// NumPy emulated by Math.fround), the SSIM is scikit-image's (Gaussian window,
// sigma 1.5, reflect borders), the overshoot is metrics.overshoot.

const f = Math.fround;

// ── sRGB ────────────────────────────────────────────────────────────────────

// The decode of an 8-bit value as make_input does it, in float32.
const DEC8 = new Float32Array(256);
for (let v = 0; v < 256; v++) {
  const x = f(v / 255);
  DEC8[v] = x <= f(0.04045) ? f(x / f(12.92)) : f(Math.pow(f(f(x + f(0.055)) / f(1.055)), f(2.4)));
}
const encode8 = (l) => {
  // l: linear light clipped to 0..1, back to 8 bits as make_input writes it
  const s = l <= f(0.0031308) ? f(l * f(12.92)) : f(f(f(1.055) * f(Math.pow(l, f(1 / 2.4)))) - f(0.055));
  return Math.trunc(f(f(s * 255) + 0.5));
};
// The HDR feed's transfer: sRGB continued past 1.0 (hdr_mires.py).
const linToEnc = (c) => (c <= 0 ? 0 : c <= 0.0031308 ? c * 12.92 : 1.055 * Math.pow(c, 1 / 2.4) - 0.055);
const encToLin = (v) => (v <= 0 ? 0 : v <= 0.04045 ? v / 12.92 : Math.pow((v + 0.055) / 1.055, 2.4));
const REF_WHITE = 203;

// ── Lanczos-3, as Pillow's ImagingResample on a float image ────────────────

function sinc(x) {
  if (x === 0) return 1;
  x *= Math.PI;
  return Math.sin(x) / x;
}
const lanczos = (x) => (x >= -3 && x < 3 ? sinc(x) * sinc(x / 3) : 0);

function coeffs(inSize, outSize) {
  const scale = inSize / outSize;
  const fscale = Math.max(scale, 1);
  const support = 3 * fscale;
  const ksize = Math.ceil(support) * 2 + 1;
  const kk = new Float64Array(outSize * ksize);
  const bounds = new Int32Array(outSize * 2);
  for (let xx = 0; xx < outSize; xx++) {
    const center = (xx + 0.5) * scale;
    const ss = 1 / fscale;
    let xmin = Math.trunc(center - support + 0.5);
    if (xmin < 0) xmin = 0;
    let xmax = Math.trunc(center + support + 0.5);
    if (xmax > inSize) xmax = inSize;
    xmax -= xmin;
    let ww = 0;
    for (let x = 0; x < xmax; x++) {
      const w = lanczos((x + xmin - center + 0.5) * ss);
      kk[xx * ksize + x] = w;
      ww += w;
    }
    if (ww !== 0) for (let x = 0; x < xmax; x++) kk[xx * ksize + x] /= ww;
    bounds[xx * 2] = xmin;
    bounds[xx * 2 + 1] = xmax;
  }
  return { kk, bounds, ksize };
}

/** One float plane (w×h) to ow×oh: the horizontal pass first, then the vertical, float32 between. */
function resizePlane(src, w, h, ow, oh, H = coeffs(w, ow), V = coeffs(h, oh)) {
  const tmp = new Float32Array(ow * h);
  for (let y = 0; y < h; y++) {
    const row = y * w;
    for (let xx = 0; xx < ow; xx++) {
      const xmin = H.bounds[xx * 2];
      const n = H.bounds[xx * 2 + 1];
      const k = xx * H.ksize;
      let s = 0;
      for (let x = 0; x < n; x++) s += src[row + xmin + x] * H.kk[k + x];
      tmp[y * ow + xx] = s;
    }
  }
  const out = new Float32Array(ow * oh);
  for (let yy = 0; yy < oh; yy++) {
    const ymin = V.bounds[yy * 2];
    const n = V.bounds[yy * 2 + 1];
    const k = yy * V.ksize;
    const o = yy * ow;
    for (let xx = 0; xx < ow; xx++) {
      let s = 0;
      for (let y = 0; y < n; y++) s += tmp[(ymin + y) * ow + xx] * V.kk[k + y];
      out[o + xx] = s;
    }
  }
  return out;
}

// ── pictures in the worker's own form ──────────────────────────────────────
// SDR: RGBA 8-bit (Uint8ClampedArray), as the GPU reads it.
// HDR: three Float32Array planes of linear nits; encoded when sent.

/** An SDR picture reduced in linear light to ow×oh (make_input). */
function reduceSdr(rgba, w, h, ow, oh) {
  const H = coeffs(w, ow);
  const V = coeffs(h, oh);
  const out = new Uint8ClampedArray(ow * oh * 4);
  const plane = new Float32Array(w * h);
  for (let c = 0; c < 3; c++) {
    for (let i = 0; i < w * h; i++) plane[i] = DEC8[rgba[i * 4 + c]];
    const r = resizePlane(plane, w, h, ow, oh, H, V);
    for (let i = 0; i < ow * oh; i++) {
      const l = r[i] < 0 ? 0 : r[i] > 1 ? 1 : r[i];
      out[i * 4 + c] = encode8(l);
    }
  }
  for (let i = 0; i < ow * oh; i++) out[i * 4 + 3] = 255;
  return out;
}

/** HDR planes (nits) reduced in linear light, negatives cut (hdr_mires.reduce_linear). */
function reduceHdr(planes, w, h, ow, oh) {
  const H = coeffs(w, ow);
  const V = coeffs(h, oh);
  return planes.map((p) => {
    const r = resizePlane(p, w, h, ow, oh, H, V);
    for (let i = 0; i < r.length; i++) if (r[i] < 0) r[i] = 0;
    return r;
  });
}

/** HDR planes (nits) to the feed's encoded RGBA, as Float32Array (4 per pixel). */
function encodeHdr(planes, n) {
  const out = new Float32Array(n * 4);
  for (let i = 0; i < n; i++) {
    out[i * 4] = linToEnc(planes[0][i] / REF_WHITE);
    out[i * 4 + 1] = linToEnc(planes[1][i] / REF_WHITE);
    out[i * 4 + 2] = linToEnc(planes[2][i] / REF_WHITE);
    out[i * 4 + 3] = 1;
  }
  return out;
}

// float32 → IEEE half, rounded to nearest even
const F32 = new Float32Array(1);
const U32 = new Uint32Array(F32.buffer);
function toHalf(v) {
  F32[0] = v;
  const x = U32[0];
  const sign = (x >>> 16) & 0x8000;
  let e = ((x >>> 23) & 0xff) - 127 + 15;
  let m = x & 0x7fffff;
  if (e >= 31) return sign | 0x7c00;
  if (e <= 0) {
    if (e < -10) return sign;
    m = (m | 0x800000) >> (1 - e);
    if (m & 0x1000 && m & 0x2fff) m += 0x2000;
    return sign | (m >> 13);
  }
  if (m & 0x1000 && m & 0x2fff) {
    m += 0x2000;
    if (m & 0x800000) {
      m = 0;
      e++;
      if (e >= 31) return sign | 0x7c00;
    }
  }
  return sign | (e << 10) | (m >> 13);
}
const HALF = new Float32Array(65536);
for (let h = 0; h < 65536; h++) {
  const s = h & 0x8000 ? -1 : 1;
  const e = (h >> 10) & 0x1f;
  const m = h & 0x3ff;
  HALF[h] = e === 0 ? s * m * 2 ** -24 : e === 31 ? (m ? NaN : s * Infinity) : s * (1 + m / 1024) * 2 ** (e - 15);
}
const halves = (rgbaF32) => {
  const out = new Uint16Array(rgbaF32.length);
  for (let i = 0; i < rgbaF32.length; i++) out[i] = toHalf(rgbaF32[i]);
  return out;
};
const fromHalves = (u16) => {
  const out = new Float32Array(u16.length);
  for (let i = 0; i < u16.length; i++) out[i] = HALF[u16[i]];
  return out;
};

// ── the 4:2:0 stream ────────────────────────────────────────────────────────
// The chroma of BT.709 (on the encoded values, as a decoder sees them)
// averaged over 2×2 and brought back by bilinear interpolation, the picture an
// encoder and a decoder hand over before the upscaler. `rgba` is either 8-bit
// or float RGBA; the result has the same type.

function to420(rgba, w, h) {
  const n = w * h;
  const Y = new Float32Array(n);
  const Cb = new Float32Array(n);
  const Cr = new Float32Array(n);
  const sc = rgba instanceof Float32Array ? 1 : 1 / 255;
  for (let i = 0; i < n; i++) {
    const r = rgba[i * 4] * sc;
    const g = rgba[i * 4 + 1] * sc;
    const b = rgba[i * 4 + 2] * sc;
    const y = 0.2126 * r + 0.7152 * g + 0.0722 * b;
    Y[i] = y;
    Cb[i] = (b - y) / 1.8556;
    Cr[i] = (r - y) / 1.5748;
  }
  const cw = (w + 1) >> 1;
  const ch = (h + 1) >> 1;
  const sub = (P) => {
    const s = new Float32Array(cw * ch);
    for (let y = 0; y < ch; y++)
      for (let x = 0; x < cw; x++) {
        let t = 0;
        let k = 0;
        for (let dy = 0; dy < 2; dy++)
          for (let dx = 0; dx < 2; dx++) {
            const xx = 2 * x + dx;
            const yy = 2 * y + dy;
            if (xx < w && yy < h) {
              t += P[yy * w + xx];
              k++;
            }
          }
        s[y * cw + x] = t / k;
      }
    return s;
  };
  const up = (s, x, y) => {
    // bilinear, centre-sited: chroma sample (i, j) sits at full-res (2i + 0.5, 2j + 0.5)
    const fx = Math.min(Math.max((x + 0.5) / 2 - 0.5, 0), cw - 1);
    const fy = Math.min(Math.max((y + 0.5) / 2 - 0.5, 0), ch - 1);
    const x0 = Math.floor(fx);
    const y0 = Math.floor(fy);
    const x1 = Math.min(x0 + 1, cw - 1);
    const y1 = Math.min(y0 + 1, ch - 1);
    const ax = fx - x0;
    const ay = fy - y0;
    const a = s[y0 * cw + x0] * (1 - ax) + s[y0 * cw + x1] * ax;
    const b = s[y1 * cw + x0] * (1 - ax) + s[y1 * cw + x1] * ax;
    return a * (1 - ay) + b * ay;
  };
  const sb = sub(Cb);
  const sr = sub(Cr);
  const out = rgba instanceof Float32Array ? new Float32Array(n * 4) : new Uint8ClampedArray(n * 4);
  for (let y = 0; y < h; y++)
    for (let x = 0; x < w; x++) {
      const i = y * w + x;
      const cb = up(sb, x, y);
      const cr = up(sr, x, y);
      const r = Y[i] + 1.5748 * cr;
      const b = Y[i] + 1.8556 * cb;
      const g = (Y[i] - 0.2126 * r - 0.0722 * b) / 0.7152;
      if (sc === 1) {
        out[i * 4] = Math.max(r, 0);
        out[i * 4 + 1] = Math.max(g, 0);
        out[i * 4 + 2] = Math.max(b, 0);
        out[i * 4 + 3] = 1;
      } else {
        out[i * 4] = Math.round(r * 255);
        out[i * 4 + 1] = Math.round(g * 255);
        out[i * 4 + 2] = Math.round(b * 255);
        out[i * 4 + 3] = 255;
      }
    }
  return out;
}

// ── the metrics ─────────────────────────────────────────────────────────────

// scipy.ndimage.gaussian_filter's window at sigma 1.5, truncate 3.5: radius 5
const GW = (() => {
  const r = 5;
  const w = new Float64Array(2 * r + 1);
  let s = 0;
  for (let i = -r; i <= r; i++) s += w[i + r] = Math.exp((-0.5 / (1.5 * 1.5)) * i * i);
  for (let i = 0; i < w.length; i++) w[i] /= s;
  return w;
})();
/**
 * structural_similarity(X, Y, data_range=1, gaussian_weights=True, sigma=1.5,
 * use_sample_covariance=False): the mean over the picture less 5 pixels on each
 * side, the rows where the window never leaves the picture, so the borders'
 * reflection never counts. One row at a time, the window's sums taken in the
 * order gaussian_filter takes them (down the rows, then along them): the same
 * result, and five rows of memory instead of five pictures (a phone's tab).
 */
function ssim(X, Y, w, h) {
  const r = 5;
  const ax = new Float64Array(w);
  const ay = new Float64Array(w);
  const axx = new Float64Array(w);
  const ayy = new Float64Array(w);
  const axy = new Float64Array(w);
  const C1 = 0.01 ** 2;
  const C2 = 0.03 ** 2;
  let sum = 0;
  let cnt = 0;
  for (let y = r; y < h - r; y++) {
    ax.fill(0);
    ay.fill(0);
    axx.fill(0);
    ayy.fill(0);
    axy.fill(0);
    for (let k = -r; k <= r; k++) {
      const g = GW[k + r];
      const row = (y + k) * w;
      for (let x = 0; x < w; x++) {
        const a = X[row + x];
        const b = Y[row + x];
        ax[x] += a * g;
        ay[x] += b * g;
        axx[x] += a * a * g;
        ayy[x] += b * b * g;
        axy[x] += a * b * g;
      }
    }
    for (let x = r; x < w - r; x++) {
      let mx = 0;
      let my = 0;
      let sxx = 0;
      let syy = 0;
      let sxy = 0;
      for (let k = -r; k <= r; k++) {
        const g = GW[k + r];
        mx += ax[x + k] * g;
        my += ay[x + k] * g;
        sxx += axx[x + k] * g;
        syy += ayy[x + k] * g;
        sxy += axy[x + k] * g;
      }
      const vx = sxx - mx * mx;
      const vy = syy - my * my;
      const vxy = sxy - mx * my;
      sum += ((2 * mx * my + C1) * (2 * vxy + C2)) / ((mx * mx + my * my + C1) * (vx + vy + C2));
      cnt++;
    }
  }
  return sum / cnt;
}

// What the SSIM reads. SDR (metrics.measure): the luma, Rec. 709 weights on
// the encoded values in float32, and the three channels for the colour SSIM.
// HDR (metrics_hdr.measure): the feed decoded to BT.2020 nits, each channel PQ
// encoded (0..1 over 0..10000 nits), and the luma as BT.2020 weights on them.
const PQ = { m1: 0.1593017578125, m2: 78.84375, c1: 0.8359375, c2: 18.8515625, c3: 18.6875 };
const pqOetf = (nits) => {
  const y = Math.pow(Math.min(Math.max(nits / 10000, 0), 1), PQ.m1);
  return Math.pow((PQ.c1 + PQ.c2 * y) / (1 + PQ.c3 * y), PQ.m2);
};
// the renderer's P3 → BT.2020 (metrics_hdr.M_P3_TO_2020)
const P3_2020 = [
  [0.7538332633935425, 0.19859767443089404, 0.04757001460650134],
  [0.04574424088440792, 0.9417772785966084, 0.012479468040503256],
  [-0.0012104680694026968, 0.017602139157328445, 0.9836083453037453],
];
/** Encoded HDR RGBA → BT.2020 nits RGBA (alpha unused). */
function nits2020(enc, n) {
  const out = new Float32Array(n * 4);
  for (let i = 0; i < n; i++) {
    const r = encToLin(enc[i * 4]) * REF_WHITE;
    const g = encToLin(enc[i * 4 + 1]) * REF_WHITE;
    const b = encToLin(enc[i * 4 + 2]) * REF_WHITE;
    for (let k = 0; k < 3; k++) {
      const m = P3_2020[k];
      out[i * 4 + k] = Math.max(m[0] * r + m[1] * g + m[2] * b, 0);
    }
  }
  return out;
}
const F8 = Float32Array.from({ length: 256 }, (_, v) => f(v / 255));
function metricPlanes(rgba, n, hdr) {
  const Y = new Float32Array(n);
  const ch = [new Float32Array(n), new Float32Array(n), new Float32Array(n)];
  if (!hdr) {
    for (let i = 0; i < n; i++) {
      const r = F8[rgba[i * 4]];
      const g = F8[rgba[i * 4 + 1]];
      const b = F8[rgba[i * 4 + 2]];
      ch[0][i] = r;
      ch[1][i] = g;
      ch[2][i] = b;
      Y[i] = f(f(f(f(0.2126) * r) + f(f(0.7152) * g)) + f(f(0.0722) * b));
    }
  } else {
    const nits = nits2020(rgba, n);
    for (let i = 0; i < n; i++) {
      for (let k = 0; k < 3; k++) ch[k][i] = pqOetf(nits[i * 4 + k]);
      Y[i] = 0.2627 * ch[0][i] + 0.678 * ch[1][i] + 0.0593 * ch[2][i];
    }
  }
  return { Y, ch };
}
const lumaOf = (rgba, n, hdr) => metricPlanes(rgba, n, hdr).Y;

/** Share of the output pixels beyond the range of their 2×2 input texels.
 *  SDR (metrics.overshoot): by more than 2/255, in its float32 steps, which
 *  decide the pixels that sit exactly 2/255 away. HDR (metrics_hdr.
 *  overshoot_linear): in BT.2020 nits, by more than 1 % + 0.5 nit. */
function overshoot(out, ow, oh, inp, iw, ih, hdr) {
  const tol = f(2 / 255);
  const val = hdr ? (v) => v : (v) => F8[v];
  const above = hdr ? (v, hi) => v > hi * 1.01 + 0.5 : (v, hi) => v > f(hi + tol);
  const below = hdr ? (v, lo) => v < lo * 0.99 - 0.5 : (v, lo) => v < f(lo - tol);
  const sx = iw / ow;
  const sy = ih / oh;
  const X0 = new Int32Array(ow);
  for (let x = 0; x < ow; x++) X0[x] = Math.min(Math.max(Math.floor((x + 0.5) * sx - 0.5), 0), iw - 1);
  let over = 0;
  for (let y = 0; y < oh; y++) {
    const y0 = Math.min(Math.max(Math.floor((y + 0.5) * sy - 0.5), 0), ih - 1);
    const y1 = Math.min(y0 + 1, ih - 1);
    for (let x = 0; x < ow; x++) {
      const x0 = X0[x];
      const x1 = Math.min(x0 + 1, iw - 1);
      const o = (y * ow + x) * 4;
      const a = (y0 * iw + x0) * 4;
      const b = (y0 * iw + x1) * 4;
      const c = (y1 * iw + x0) * 4;
      const d = (y1 * iw + x1) * 4;
      for (let k = 0; k < 3; k++) {
        const lo = val(Math.min(inp[a + k], inp[b + k], inp[c + k], inp[d + k]));
        const hi = val(Math.max(inp[a + k], inp[b + k], inp[c + k], inp[d + k]));
        const v = val(out[o + k]);
        if (above(v, hi) || below(v, lo)) {
          over++;
          break;
        }
      }
    }
  }
  return over / (ow * oh);
}

/** Share of the pixels where a channel differs by more than `tol` (the 16-bit gate). */
function differs(a, b, n, tol, hdr) {
  const sc = hdr ? 1 : 1 / 255;
  let k = 0;
  for (let i = 0; i < n; i++)
    for (let c = 0; c < 3; c++)
      if (Math.abs(a[i * 4 + c] - b[i * 4 + c]) * sc > tol) {
        k++;
        break;
      }
  return k / n;
}

// ── the pictures ────────────────────────────────────────────────────────────

const MAX_W = 3840;
const MAX_H = 2160;

async function decode(blob, keepColours) {
  const bmp = await createImageBitmap(blob, {
    colorSpaceConversion: keepColours ? 'none' : 'default',
    premultiplyAlpha: 'none',
    imageOrientation: 'from-image',
  });
  const cv = new OffscreenCanvas(bmp.width, bmp.height);
  const ctx = cv.getContext('2d', { alpha: false, willReadFrequently: true });
  ctx.drawImage(bmp, 0, 0);
  const { data } = ctx.getImageData(0, 0, bmp.width, bmp.height);
  const r = { rgba: data, w: bmp.width, h: bmp.height };
  bmp.close();
  for (let i = 3; i < r.rgba.length; i += 4) r.rgba[i] = 255;
  return r;
}

// The lab's colour code chart (scaler-lab.js drawCode, `iso`): syntax colours
// of one luma, so that every edge is a chroma edge, the case 4:4:4 is for.
function drawCodeIso(w, h) {
  const cv = new OffscreenCanvas(w, h);
  const ctx = cv.getContext('2d', { alpha: false, willReadFrequently: true });
  ctx.fillStyle = '#786e28';
  ctx.fillRect(0, 0, w, h);
  const px = Math.max(9, Math.round(h / 56));
  ctx.font = px + 'px Consolas, monospace';
  const C = {
    kw: '#287d5a',
    str: '#b45a46',
    cmt: '#5a5fe6',
    id: '#c846b4',
    num: '#3c7882',
    fn: '#96643c',
    pl: '#467378',
  };
  const lines = [
    [
      ['kw', 'const '],
      ['id', 'frame '],
      ['pl', '= '],
      ['kw', 'await '],
      ['fn', 'decode'],
      ['pl', '(chunk, '],
      ['num', '16667'],
      ['pl', ');'],
    ],
    [['cmt', '// the external texture expires with the frame']],
    [
      ['kw', 'for '],
      ['pl', '('],
      ['kw', 'let '],
      ['id', 'i '],
      ['pl', '= '],
      ['num', '0'],
      ['pl', '; i < '],
      ['id', 'n'],
      ['pl', '; i++) {'],
    ],
    [
      ['pl', '    '],
      ['id', 'pass'],
      ['pl', '.'],
      ['fn', 'setBindGroup'],
      ['pl', '('],
      ['num', '0'],
      ['pl', ', '],
      ['fn', 'bind'],
      ['pl', '(ext));'],
    ],
    [
      ['pl', '    '],
      ['id', 'log'],
      ['pl', '('],
      ['str', "'gpu p50 ' "],
      ['pl', '+ '],
      ['id', 't'],
      ['pl', '.'],
      ['fn', 'toFixed'],
      ['pl', '('],
      ['num', '3'],
      ['pl', '));'],
    ],
    [['pl', '}']],
  ];
  const lh = Math.round(px * 1.45);
  let y = px * 2;
  let n = 0;
  while (y < h - px) {
    const line = lines[n % lines.length];
    for (const x0 of [px, Math.round(w * 0.5)]) {
      let x = x0;
      ctx.fillStyle = '#64696e';
      ctx.fillText(String(n + 1).padStart(3), x, y);
      x += px * 3;
      for (const [kind, text] of line) {
        ctx.fillStyle = C[kind];
        ctx.fillText(text, x, y);
        x += ctx.measureText(text).width;
      }
    }
    y += lh;
    n++;
  }
  const { data } = ctx.getImageData(0, 0, w, h);
  return data;
}

// The HDR pictures of the lab (hdr_mires.py), in linear nits.
const M = PQ;
const pqEotf = (e) => {
  const ep = Math.pow(Math.min(Math.max(e, 0), 1), 1 / M.m2);
  return 10000 * Math.pow(Math.max(ep - M.c1, 0) / (M.c2 - M.c3 * ep), 1 / M.m1);
};
const planes3 = (n) => [new Float32Array(n), new Float32Array(n), new Float32Array(n)];

function hdrRamp(w, h) {
  const P = planes3(w * h);
  const set = (x, y, r, g, b) => {
    const i = y * w + x;
    P[0][i] = r;
    P[1][i] = g;
    P[2][i] = b;
  };
  const band = Math.floor(h / 5);
  for (let x = 0; x < w; x++) {
    const v = pqEotf((x + 0.5) / w);
    for (let y = 0; y < band; y++) set(x, y, v, v, v);
    for (let y = band; y < 2 * band; y++) set(x, y, v, 0, 0);
  }
  for (let k = 0; k < 12; k++) {
    const s = pqEotf((k + 1) / 12);
    const xa = Math.floor((k * w) / 12);
    const xb = Math.floor(((k + 1) * w) / 12);
    const xm = Math.floor((xa + xb) / 2);
    for (let y = 2 * band; y < 3 * band; y++)
      for (let x = xa; x < xb; x++) {
        const line =
          y >= 2 * band + Math.floor(band / 4) && y < 3 * band - Math.floor(band / 4) && x >= xm && x < xm + 2;
        const v = line ? 1 : s;
        set(x, y, v, v, v);
      }
  }
  for (let y = 3 * band; y < 4 * band; y++) {
    const fine = y >= 3 * band + Math.floor(band / 2);
    for (let x = 0; x < w; x++) {
      const v = (fine ? Math.floor(x / 2) % 2 : Math.floor(x / 4) % 2) === 0 ? 1000 : 0.5;
      set(x, y, v, v, v);
    }
  }
  for (let y = 4 * band; y < h; y++)
    for (let x = 0; x < w; x++) {
      const t = (x / w + (y - 4 * band) / Math.max(1, h - 4 * band)) / 2;
      const v = pqEotf(0.3 + 0.4 * t);
      set(x, y, v, v, v);
    }
  return P;
}

const LINES = [
  'The quick brown fox jumps over the lazy dog 0123456789',
  'mwsr: one sample on the flat, five on the edges; HDR 203 nits = 1.0',
  'clamp(R, lo, hi) — no pixel beyond the local maximum of the input',
  'ΔE ITP ≤ FSR1 on the same feed; the maximum of the picture kept',
];
function textMask(w, h, px) {
  const cv = new OffscreenCanvas(w, h);
  const ctx = cv.getContext('2d', { alpha: false, willReadFrequently: true });
  ctx.fillStyle = '#000';
  ctx.fillRect(0, 0, w, h);
  ctx.fillStyle = '#fff';
  ctx.font = px + "px Consolas, 'DejaVu Sans Mono', monospace";
  ctx.textBaseline = 'top';
  let y = Math.floor(px / 2);
  let k = 0;
  while (y < h - px) {
    ctx.fillText(LINES[k % LINES.length], Math.floor(px / 2), y);
    y += Math.floor(px * 1.4);
    k++;
  }
  const d = ctx.getImageData(0, 0, w, h).data;
  const m = new Float32Array(w * h);
  for (let i = 0; i < m.length; i++) m[i] = d[i * 4] / 255;
  return m;
}
function hdrText(w, h) {
  const P = planes3(w * h);
  const third = Math.floor(h / 3);
  const px = Math.max(12, Math.floor(h / 60));
  const small = Math.max(9, Math.floor(px / 2));
  const white = [1000, 1000, 1000];
  const blocks = [
    [0, third, [0.5, 0.5, 0.5], white],
    [third, 2 * third, white, [10, 10, 10]],
    [2 * third, h, [500, 0, 0], white],
  ];
  const x0 = Math.floor((2 * w) / 3);
  for (const [ya, yb, fg, bg] of blocks) {
    const bh = yb - ya;
    const big = textMask(w, bh, px);
    const sm = textMask(w - x0, bh, small);
    for (let y = 0; y < bh; y++)
      for (let x = 0; x < w; x++) {
        const m = x < x0 ? big[y * w + x] : sm[y * (w - x0) + (x - x0)];
        const i = (ya + y) * w + x;
        for (let c = 0; c < 3; c++) P[c][i] = bg[c] * (1 - m) + fg[c] * m;
      }
  }
  return P;
}
function hdrGame(rgba, w, h) {
  const P = planes3(w * h);
  for (let i = 0; i < w * h; i++) {
    const r = encToLin(rgba[i * 4] / 255);
    const g = encToLin(rgba[i * 4 + 1] / 255);
    const b = encToLin(rgba[i * 4 + 2] / 255);
    const y = 0.2126 * r + 0.7152 * g + 0.0722 * b;
    const t = Math.min(Math.max((y - 0.5) / 0.5, 0), 1);
    const gain = (1 + 4 * t * t * (3 - 2 * t)) * REF_WHITE;
    P[0][i] = r * gain;
    P[1][i] = g * gain;
    P[2][i] = b * gain;
  }
  return P;
}

// ── the state and the messages ──────────────────────────────────────────────

const cache = new Map(); // picture key → { w, h, hdr, rgba | planes }
let current = null; // the case being measured: native and input, as the metrics read them

async function fetchRgba(url) {
  const r = await fetch(url);
  if (!r.ok) throw new Error(url + ': HTTP ' + r.status);
  return decode(await r.blob(), true);
}

async function picture(p) {
  const key = p.kind + ':' + (p.url || p.name || '');
  if (p.kind !== 'file' && cache.has(key)) return cache.get(key);
  let pic;
  if (p.kind === 'sdr') pic = { ...(await fetchRgba(p.url)), hdr: false };
  else if (p.kind === 'iso') pic = { w: p.w, h: p.h, rgba: drawCodeIso(p.w, p.h), hdr: false };
  else if (p.kind === 'hdr-ramp') pic = { w: p.w, h: p.h, planes: hdrRamp(p.w, p.h), hdr: true };
  else if (p.kind === 'hdr-text') pic = { w: p.w, h: p.h, planes: hdrText(p.w, p.h), hdr: true };
  else if (p.kind === 'hdr-game') {
    const g = await fetchRgba(p.url);
    pic = { w: g.w, h: g.h, planes: hdrGame(g.rgba, g.w, g.h), hdr: true };
  } else if (p.kind === 'file') {
    const d = await decode(p.file, false);
    pic = { ...d, hdr: false, reduced: null };
    if (d.w > MAX_W || d.h > MAX_H) {
      const s = Math.min(MAX_W / d.w, MAX_H / d.h);
      const w = Math.max(1, Math.round(d.w * s));
      const h = Math.max(1, Math.round(d.h * s));
      pic = { rgba: reduceSdr(d.rgba, d.w, d.h, w, h), w, h, hdr: false, reduced: [d.w, d.h] };
    }
  } else throw new Error('picture ' + p.kind + '?');
  if (p.kind !== 'file') {
    cache.set(key, pic);
    // two pictures kept at most: an HDR one weighs 44 MB here
    while (cache.size > 2) cache.delete(cache.keys().next().value);
  }
  return pic;
}

/**
 * The case: the native frame and the input the upscalers read, in the GPU's
 * form. An enlargement (scale > 1) reduces the picture into the input and
 * scores against the picture; at ×1 the input is the picture itself; a
 * reduction (scale < 1) takes the picture as the input and scores against
 * its Lanczos-3 reduction in linear light, the best the output can hold.
 */
// MWSR zones (zones.js, zonetree.js): the native host's codes for its own
// client, from the native frame, the host's capture. Loaded by their versioned
// URLs, once.
let zoneMods = null;
async function hostZones(urls, native, outW, outH, inW, inH) {
  if (!urls) return null;
  zoneMods ||= Promise.all([import(urls.zones), import(urls.tree)]);
  const [Z, T] = await zoneMods;
  // the client reads zones from x1.3 up only
  if (Math.min(outW / inW, outH / inH) < Z.ZONE_MIN_SCALE) return null;
  // the host's scale is the capture's width over the stream's; it reduced in linear light
  const codes = Z.decideZones(Z.zoneCells(native, outW, outH), f(outW / inW), true, T.ZONE_TREE);
  return { cols: Z.ZONES_X, rows: Z.ZONES_Y, codes, texels: Z.zoneTexels(codes) };
}

async function prepare({ pic: p, scale, chroma, role, zoneUrls = null }) {
  const pic = await picture(p);
  const { w, h } = pic;
  let outW, outH, inW, inH;
  if (role === 'input') {
    // the picture is the input: no native frame, nothing to score against
    let s = Math.min(1, MAX_W / (w * scale), MAX_H / (h * scale));
    inW = Math.max(1, Math.round(w * s));
    inH = Math.max(1, Math.round(h * s));
    outW = Math.max(1, Math.round(inW * scale));
    outH = Math.max(1, Math.round(inH * scale));
  } else if (scale >= 1) {
    outW = w;
    outH = h;
    inW = Math.max(1, Math.round(w / scale));
    inH = Math.max(1, Math.round(h / scale));
  } else {
    inW = w;
    inH = h;
    outW = Math.max(1, Math.round(w * scale));
    outH = Math.max(1, Math.round(h * scale));
  }
  const res = { outW, outH, inW, inH, picW: w, picH: h, hdr: pic.hdr, reduced: pic.reduced || null };
  const own = (W, H) => W === w && H === h;
  let native = null;
  let input;
  if (!pic.hdr) {
    const at = (W, H) => (own(W, H) ? pic.rgba : reduceSdr(pic.rgba, w, h, W, H));
    if (role !== 'input') native = at(outW, outH);
    input = at(inW, inH);
    if (chroma === '420') input = to420(input, inW, inH);
    res.native = native && new Uint8Array(native);
    res.input = new Uint8Array(input);
    // SDR with a native frame only: the host takes no zones in HDR
    if (native) res.zones = await hostZones(zoneUrls, native, outW, outH, inW, inH);
  } else {
    const at = (W, H) => encodeHdr(own(W, H) ? pic.planes : reduceHdr(pic.planes, w, h, W, H), W * H);
    native = at(outW, outH);
    input = at(inW, inH);
    if (chroma === '420') input = to420(input, inW, inH);
    // the GPU reads half floats: the metrics read the same values
    res.native = halves(native);
    res.input = halves(input);
    native = fromHalves(res.native);
    input = fromHalves(res.input);
  }
  const ref = native ? metricPlanes(native, outW * outH, pic.hdr) : null;
  current = { ...res, ref, inputData: pic.hdr ? nits2020(input, inW * inH) : input };
  // copies (new Uint8Array(…) and halves(…) copy): the arrays above stay the worker's
  return { result: res, transfer: [res.input.buffer, ...(res.native ? [res.native.buffer] : [])] };
}

function measure({ out }) {
  const c = current;
  if (!c) throw new Error('no case');
  const n = c.outW * c.outH;
  const data = c.hdr ? fromHalves(out) : out;
  const over = c.hdr ? nits2020(data, n) : data;
  const r = { overshoot: overshoot(over, c.outW, c.outH, c.inputData, c.inW, c.inH, c.hdr) };
  if (c.ref) {
    const P = metricPlanes(data, n, c.hdr);
    r.ssimY = ssim(c.ref.Y, P.Y, c.outW, c.outH);
    // the colour SSIM: the mean of the three channels' (channel_axis=2)
    r.ssimRgb = P.ch.reduce((t, p, k) => t + ssim(c.ref.ch[k], p, c.outW, c.outH), 0) / 3;
  }
  return { result: r };
}

function gate({ a, b }) {
  const c = current;
  const A = c.hdr ? fromHalves(a) : a;
  const B = c.hdr ? fromHalves(b) : b;
  return { result: { share: differs(A, B, c.outW * c.outH, 4 / 255, c.hdr) } };
}

const OPS = { prepare, measure, gate };

// One message at a time, in their order: a case prepared while an earlier one
// still decodes must not end before it.
let queue = Promise.resolve();
self.onmessage = (e) => {
  queue = queue.then(async () => {
    const { id, op, args } = e.data;
    try {
      const { result, transfer = [] } = await OPS[op](args);
      self.postMessage({ id, ok: true, result }, transfer);
    } catch (err) {
      self.postMessage({ id, ok: false, error: String((err && err.message) || err) });
    }
  });
};

// for the tests (node): the same functions, without the messages
export const _test = { reduceSdr, ssim, lumaOf, metricPlanes, overshoot, to420, toHalf, HALF };
