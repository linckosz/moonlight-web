// mwsr on an HDR stream (PQ, BT.2020, 10-bit Y'CbCr frames), through the
// product's WebGpuRenderer, on the software adapter; driven by hdr_check.py.
// Each case of /__dir/hdr-cases.json is drawn on the renderer's two HDR
// readback paths, 'linear' (the rgba16float canvas, mwsr on float
// intermediates) and 'tonemap' (ACES, then mwsr on 8-bit), with:
//   - 'off', the plain copy, and 'mwsr', the product's;
//   - 'mwsr-enc' on a reduction on 'linear': the reduction of before
//     26/10/2026 (pass 0 encoded, the 2×2 grid in gamma-2 light, LINEAR_LIGHT
//     1) beside the product's linear light (LINEAR_LIGHT 3), the comparison;
//   - 'mwsr-f32' where the device has shader-f16: the f32 kernel beside the
//     f16 one the renderer picked.
// The canvas texture is a texture of the canvas' own format, so that the
// picture is read back whole (half floats on 'linear'); it is posted to
// /__bin as RGBA half floats for the driver to measure. The page refuses any
// adapter but SwiftShader before it asks for a device. Its answer is left in
// window.__done, its progress in window.__progress.
//
// ?time=N&vendor=WORD (hdr_check.py --time, on a GPU the driver pinned and
// checked): the speed instead, on 'linear', of 'mwsr' and 'mwsr-enc' on each
// case — N draws each, in ABBA order after a warm-up, every render pass of
// the renderer stamped (timestamp-query) so that pass 0 and the kernel are
// timed apart. The page then refuses SwiftShader and any adapter whose name
// lacks WORD. &gap=MS (hdr_check.py --gap): MS of idle before each block, so
// that the GPU drops its P-state in front of both variants (V10, 28/10/2026:
// at 810 MHz of memory, P5, a 4K reduction takes nine times longer).
//
import { mwsrRegime } from "../../../frontend/js/stream/renderers/MwsrShader.js";
import { WebGpuRenderer } from "../../../frontend/js/stream/renderers/WebGpuRenderer.js";

const T = GPUTextureUsage;
const B = GPUBufferUsage;
const PARAMS = new URLSearchParams(location.search);
const TIME = Math.max(0, parseInt(PARAMS.get("time") || "0", 10) || 0);
const GAP = Math.max(0, parseInt(PARAMS.get("gap") || "0", 10) || 0);

/** The texture's pixels as RGBA half floats (rows packed), 8-bit ones in [0, 1]. */
async function readTarget(device, tex) {
  const w = tex.width;
  const h = tex.height;
  const half = tex.format === "rgba16float";
  const bpp = half ? 8 : 4;
  const row = Math.ceil((w * bpp) / 256) * 256;
  const buf = device.createBuffer({ size: row * h, usage: B.COPY_DST | B.MAP_READ });
  const enc = device.createCommandEncoder();
  enc.copyTextureToBuffer({ texture: tex }, { buffer: buf, bytesPerRow: row }, [w, h]);
  device.queue.submit([enc.finish()]);
  await buf.mapAsync(GPUMapMode.READ);
  const src = new Uint8Array(buf.getMappedRange());
  let out;
  if (half) {
    out = new Uint8Array(w * h * 8);
    for (let y = 0; y < h; y++) out.set(src.subarray(y * row, y * row + w * 8), y * w * 8);
  } else {
    const f = new Float16Array(w * h * 4);
    const [ri, bi] = tex.format.startsWith("bgra") ? [2, 0] : [0, 2];
    for (let y = 0; y < h; y++) {
      for (let x = 0; x < w; x++) {
        const s = y * row + x * 4;
        const d = (y * w + x) * 4;
        f[d] = src[s + ri] / 255;
        f[d + 1] = src[s + 1] / 255;
        f[d + 2] = src[s + bi] / 255;
        f[d + 3] = 1;
      }
    }
    out = new Uint8Array(f.buffer);
  }
  buf.unmap();
  buf.destroy();
  return out;
}

/** A renderer made for mode and variant, drawing into a texture of its canvas' format. */
async function makeRenderer(c, mode, variant) {
  const canvas = new OffscreenCanvas(16, 16);
  const r = await WebGpuRenderer.create(canvas, {
    algo: variant === "off" ? "off" : "mwsr",
    hdr: mode === "linear",
    hdrLinear: mode === "linear",
    hdrTonemap: mode === "tonemap",
    // the SDR picture stands for an ordinary (4:2:0) stream: the vote on the luma
    yuv444: mode !== "sdr" && c.chroma === "444",
    videoCodec: "hevc",
  });
  const errors = [];
  r._device.addEventListener("uncapturederror", (e) => errors.push(String(e.error.message)));
  const cfg = {
    hdr: r._hdr,
    hdrLinear: r._hdrLinear,
    format: r._format,
    inter: r._interFormat,
    srgbViews: r._srgbViews,
    half: r._mwsrHalf,
  };
  if (variant === "mwsr-f32") {
    // the same kernel in f32: its pipelines rebuilt as they are asked for
    r._mwsrHalf = false;
    r._mwsrPipelines = new Map();
    cfg.half = false;
  }
  // the reduction of before: pass 0 encoded, the software light's grid
  if (variant === "mwsr-enc") r._mwsrLinearDown = false;
  // The canvas texture: one of the canvas' format, whole for the readback.
  const out = { target: null };
  r.ctx = {
    getCurrentTexture() {
      if (!out.target || out.target.width !== canvas.width || out.target.height !== canvas.height) {
        if (out.target) out.target.destroy();
        out.target = r._device.createTexture({
          size: [canvas.width, canvas.height],
          format: r._format,
          usage: T.RENDER_ATTACHMENT | T.COPY_SRC,
          viewFormats: r._srgbViews ? [r._format + "-srgb"] : [],
        });
      }
      return out.target;
    },
  };
  r.setOutputSize(c.out[0], c.out[1]);
  return { r, canvas, cfg, errors, out };
}

/** Case c's frame as a software decoder hands it; on 'sdr', its SDR picture
 *  (8-bit RGBA, sRGB). */
function hdrFrame(c, planes, timestamp, mode = "linear") {
  if (mode === "sdr")
    return new VideoFrame(planes, {
      format: "RGBA",
      codedWidth: c.in[0],
      codedHeight: c.in[1],
      timestamp,
      colorSpace: { primaries: "bt709", transfer: "iec61966-2-1", matrix: "rgb", fullRange: true },
    });
  return new VideoFrame(planes, {
    format: c.chroma === "444" ? "I444P10" : "I420P10",
    codedWidth: c.in[0],
    codedHeight: c.in[1],
    timestamp,
    colorSpace: { primaries: "bt2020", transfer: "pq", matrix: "bt2020-ncl", fullRange: false },
  });
}

/** One frame of case c drawn by a renderer made for mode and variant. */
async function drawOnce(c, planes, mode, variant) {
  const { r, canvas, cfg, errors, out } = await makeRenderer(c, mode, variant);
  await r.draw(hdrFrame(c, planes, 1000, mode));
  const [w, h] = c.in;
  const res = {
    case: c.name,
    mode,
    variant,
    cfg,
    path: r.lastDraw ? r.lastDraw.path : null,
    size: [canvas.width, canvas.height],
    // the regime the draw took: the linear grid when pass 0 wrote linear light
    regime: mwsrRegime(w, h, canvas.width, canvas.height, { srgb: r._srgbViews, linear: r._p0Linear }),
    errors,
    failed: r.failed,
    dump: ["hdr", c.name, mode, variant].join("-"),
  };
  if (!out.target) throw new Error("nothing was drawn");
  const bytes = await readTarget(r._device, out.target);
  const post = await fetch("/__bin?name=" + encodeURIComponent(res.dump), { method: "POST", body: bytes });
  if (!post.ok) throw new Error("posting " + res.dump + ": HTTP " + post.status);
  out.target.destroy();
  r.dispose();
  return res;
}

// ── time=N: every render pass of the product's devices stamped ──────────────

const MAX_STAMPS = 16;
const STAMPS = { on: false, q: 0, per: new WeakMap() };

function stampsOf(dev) {
  if (!dev.features.has("timestamp-query")) return null;
  let s = STAMPS.per.get(dev);
  if (!s) {
    s = {
      qs: dev.createQuerySet({ type: "timestamp", count: MAX_STAMPS }),
      resolve: dev.createBuffer({ size: MAX_STAMPS * 8, usage: B.QUERY_RESOLVE | B.COPY_SRC }),
      read: dev.createBuffer({ size: MAX_STAMPS * 8, usage: B.MAP_READ | B.COPY_DST }),
      n: 0,
    };
    STAMPS.per.set(dev, s);
  }
  return s;
}

/** The product's devices get timestamp-query; while STAMPS.on, each pass a draw
 *  begins is stamped and its command buffer resolves the stamps (scaler-lab.js's
 *  hooks, one draw at a time). */
function installStamps() {
  const origDev = GPUAdapter.prototype.requestDevice;
  GPUAdapter.prototype.requestDevice = function (desc) {
    const d = { ...(desc || {}) };
    if (this.features.has("timestamp-query"))
      d.requiredFeatures = [...new Set([...(d.requiredFeatures || []), "timestamp-query"])];
    return origDev.call(this, d);
  };
  const origEnc = GPUDevice.prototype.createCommandEncoder;
  GPUDevice.prototype.createCommandEncoder = function (d) {
    const e = origEnc.call(this, d);
    e.__dev = this;
    return e;
  };
  const origPass = GPUCommandEncoder.prototype.beginRenderPass;
  GPUCommandEncoder.prototype.beginRenderPass = function (desc) {
    const s = STAMPS.on && this.__dev ? stampsOf(this.__dev) : null;
    if (s && !desc.timestampWrites && STAMPS.q + 2 <= MAX_STAMPS) {
      desc = {
        ...desc,
        timestampWrites: {
          querySet: s.qs,
          beginningOfPassWriteIndex: STAMPS.q,
          endOfPassWriteIndex: STAMPS.q + 1,
        },
      };
      STAMPS.q += 2;
      this.__stamped = s;
    }
    return origPass.call(this, desc);
  };
  const origFinish = GPUCommandEncoder.prototype.finish;
  GPUCommandEncoder.prototype.finish = function (d) {
    const s = this.__stamped;
    if (s) {
      this.resolveQuerySet(s.qs, 0, STAMPS.q, s.resolve, 0);
      this.copyBufferToBuffer(s.resolve, 0, s.read, 0, STAMPS.q * 8);
      s.n = STAMPS.q;
      this.__stamped = null;
    }
    return origFinish.call(this, d);
  };
}

/** n draws of case c by renderer r; each one's pass times (ms), in order, and
 *  into `walls` each draw's start (ms since the epoch), to set a tail against
 *  the GPU's clocks read beside the page. */
async function timedDraws(r, c, planes, n, ts, mode, walls = []) {
  const out = [];
  for (let i = 0; i < n; i++) {
    walls.push(Math.round(performance.timeOrigin + performance.now()));
    STAMPS.q = 0;
    STAMPS.on = true;
    await r.draw(hdrFrame(c, planes, ts.next++ * 16667, mode));
    STAMPS.on = false;
    const s = STAMPS.per.get(r._device);
    if (!s || !s.n) throw new Error("no pass was stamped (timestamp-query?)");
    await s.read.mapAsync(GPUMapMode.READ, 0, s.n * 8);
    const t = new BigUint64Array(s.read.getMappedRange(0, s.n * 8)).slice();
    s.read.unmap();
    const passes = [];
    for (let k = 0; k + 1 < s.n; k += 2) passes.push(Number(t[k + 1] - t[k]) / 1e6);
    s.n = 0;
    out.push(passes);
  }
  return out;
}

function stats(xs) {
  const s = xs.slice().sort((a, b) => a - b);
  const at = (q) => s[Math.min(s.length - 1, Math.floor(q * s.length))];
  return { p50: at(0.5), p99: at(0.99), mean: s.reduce((a, b) => a + b, 0) / s.length, n: s.length };
}

/** 'mwsr' and 'mwsr-enc' on case c by
 *  one renderer, switched by blocks (the same device, textures and pipelines
 *  for both: two renderers, two devices, put 13 % between two identical
 *  paths, 26/10/2026), n draws each after a warm-up of 100, in four ABBA
 *  rounds (the GPU's clocks, which no one pins at their maximum, move). */
async function timeCase(c, planes, n, ts, mode = "linear") {
  const { r, canvas, errors, out } = await makeRenderer(c, mode, "mwsr");
  const [A, Bv] = ["mwsr", "mwsr-enc"];
  const draws = { [A]: [], [Bv]: [] };
  const regimes = {};
  // every draw in order, warm-up included, for the tails (hdr-time.json): the
  // variant (1 = A), its total, pass 0, the kernel (ms), its start (epoch ms)
  const seq = [];
  const run = async (v, k) => {
    r._mwsrLinearDown = v === "mwsr";
    if (GAP) await new Promise((done) => setTimeout(done, GAP));
    const walls = [];
    const d = await timedDraws(r, c, planes, k, ts, mode, walls);
    d.forEach((p, i) =>
      seq.push([
        v === A ? 1 : 0,
        +p.reduce((a, b) => a + b, 0).toFixed(4),
        +p[0].toFixed(4),
        +p[p.length - 1].toFixed(4),
        walls[i],
      ]),
    );
    regimes[v] = mwsrRegime(c.in[0], c.in[1], canvas.width, canvas.height, {
      srgb: r._srgbViews,
      linear: r._p0Linear,
    });
    return d;
  };
  await run(A, 50);
  await run(Bv, 50);
  const block = Math.max(1, Math.round(n / 8));
  for (let round = 0; round < 4; round++) for (const v of [A, Bv, Bv, A]) draws[v].push(...(await run(v, block)));
  const res = Object.entries(draws).map(([v, d]) => ({
    case: c.name,
    mode,
    variant: v,
    path: r.lastDraw ? r.lastDraw.path : null,
    regime: regimes[v],
    passes: Math.max(...d.map((p) => p.length)),
    pass0: stats(d.map((p) => p[0])),
    kernel: stats(d.map((p) => p[p.length - 1])),
    total: stats(d.map((p) => p.reduce((a, b) => a + b, 0))),
    errors,
    failed: r.failed,
    ...(v === A ? { seq } : {}),
  }));
  if (out.target) out.target.destroy();
  r.dispose();
  return res;
}

async function main() {
  if (typeof Float16Array !== "function") throw new Error("no Float16Array in this browser");
  if (TIME) installStamps();
  const adapter = await navigator.gpu.requestAdapter();
  if (!adapter) throw new Error("no adapter");
  const info = adapter.info || {};
  const name = [info.vendor, info.architecture, info.device, info.description].join(" ");
  // dry=1: the timing's code on the software adapter, its figures worth nothing
  if (TIME && PARAMS.get("dry") === "1") {
    if (!/swiftshader/i.test(name)) throw new Error("dry=1: not the software adapter: " + name);
  } else if (TIME) {
    const want = (PARAMS.get("vendor") || "").toLowerCase();
    if (/swiftshader/i.test(name)) throw new Error("time=: no speed on the software adapter");
    if (!want || !name.toLowerCase().includes(want)) throw new Error("time=: the adapter is not " + want + ": " + name);
    if (!adapter.features.has("timestamp-query")) throw new Error("time=: no timestamp-query on " + name);
  } else if (!/swiftshader/i.test(name)) throw new Error("not the software adapter: " + name);
  const f16 = adapter.features.has("shader-f16");

  const cases = await (await fetch("/__dir/hdr-cases.json")).json();
  const results = [];
  const ts = { next: 1 };
  let n = 0;
  for (const c of cases) {
    n++;
    const planes = new Uint8Array(await (await fetch("/__dir/" + c.yuv)).arrayBuffer());
    if (TIME) {
      window.__progress = `case ${n}/${cases.length} ${c.name} linear timed`;
      try {
        results.push(...(await timeCase(c, planes, TIME, ts, "linear")));
      } catch (e) {
        results.push({ case: c.name, mode: "linear", error: String((e && e.stack) || e) });
      }
      continue;
    }
    const down = c.out[0] < c.in[0];
    for (const mode of ["linear", "tonemap"]) {
      const variants = ["off", "mwsr"];
      if (mode === "linear" && down) variants.push("mwsr-enc");
      for (let i = 0; i < variants.length; i++) {
        const variant = variants[i];
        window.__progress = `case ${n}/${cases.length} ${c.name} ${mode} ${variant}`;
        let res;
        try {
          res = await drawOnce(c, planes, mode, variant);
        } catch (e) {
          res = { case: c.name, mode, variant, error: String((e && e.stack) || e) };
        }
        results.push(res);
        // the f32 kernel beside an f16 one
        if (variant === "mwsr" && mode === "linear" && res.cfg && res.cfg.half) variants.splice(i + 1, 0, "mwsr-f32");
      }
    }
  }
  return { ok: true, adapter: name, f16, time: TIME, results };
}

main()
  .then((r) => {
    window.__done = r;
  })
  .catch((e) => {
    window.__done = { ok: false, error: String((e && e.stack) || e) };
  });
