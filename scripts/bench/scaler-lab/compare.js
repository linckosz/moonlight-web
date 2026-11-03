// compare.js — the side-by-side viewer of the upscalers (docs/design/
// upscaler-mwsr.md §6.1, item 4), for the eye, on a phone as on a PC: one
// picture, the product's own renderers on it (WebGpuRenderer, WebGlRenderer,
// unchanged, each on an OffscreenCanvas), their outputs in bands under one
// zoom and pan, each band with its distance to the reference and its time per
// frame. compare.html is the shell; the state lives in the URL's query, and
// the last one in localStorage:
//
//   img=<url>          the picture, 2560×1440 (default: /test/test.png, else
//                      /test/00_Background.png — the lab server's C:\Test)
//   case=<id>          1440-1080, 1440-720, 720-1440, 1080-1440, 720-1080
//   algos=a,b,c        natif, mw, fsr1, sgsr1, nis, smooth2d, in display order
//   bands=v|h          vertical or horizontal bands
//   zone=same|cont     every band shows the same region, or the bands cut one
//                      continuous picture
//   api=auto|webgpu|webgl  the shader renderers' API; auto is the product's
//                      choice (plan §9, 22/10/2026): WebGL2, WebGPU on iOS
//   iters=300          draws per algorithm for the time
//   kg=, ft=           MWSR's per-draw values (MwsrShader's mwsrLaw): the
//                      sharpness and the edge vote's threshold; the defaults
//                      are the product's, the sharpness by the law at the
//                      case's sizes (mwsrLaw) unless the link names one
//   soft=1             the WebGPU fallback adapter (SwiftShader), for a test
//   viewer=2d          the bands drawn by Canvas2D drawImage, not WebGL2 (a test)
//
// The reference of an output at 1440p is the picture; at 1080p and 720p, its
// bicubic reduction (compare-worker.js), which is also the input of the
// upscale cases. The time is the lab's `done`: from the draw call to the
// GPU's completion of the frame, averaged over the iterations.

const SRC_W = 2560;
const SRC_H = 1440;
const CASES = [
  {
    id: "1440-1080",
    label: "1440p → 1080p (×0,75)",
    inLevel: 1440,
    outLevel: 1080,
  },
  {
    id: "1440-720",
    label: "1440p → 720p (×0,5)",
    inLevel: 1440,
    outLevel: 720,
  },
  { id: "720-1440", label: "720p → 1440p (×2)", inLevel: 720, outLevel: 1440 },
  {
    id: "1080-1440",
    label: "1080p → 1440p (×1,33)",
    inLevel: 1080,
    outLevel: 1440,
  },
  {
    id: "720-1080",
    label: "720p → 1080p (×1,5)",
    inLevel: 720,
    outLevel: 1080,
  },
];
const ALGOS = [
  {
    id: "natif",
    label: "Natif",
    note: "le bilinéaire du navigateur (drawImage)",
  },
  {
    id: "mw",
    label: "MWSR",
    gpu: "mwsr",
    gl: "gl-mwsr",
    note: "MoonlightWeb",
  },
  {
    id: "fsr1",
    label: "FSR1",
    gpu: "fsr1",
    gl: "gl-fsr1",
    note: "AMD, EASU + RCAS",
  },
  { id: "sgsr1", label: "SGSR1", gpu: "sgsr", gl: "gl-sgsr", note: "Qualcomm" },
  { id: "nis", label: "NIS", gpu: "nis", gl: "gl-nis", note: "NVIDIA" },
  { id: "smooth2d", label: "Smooth2D", note: "Canvas2D, qualité « high »" },
];
const CASE_BY = Object.fromEntries(CASES.map((c) => [c.id, c]));
const ALGO_BY = Object.fromEntries(ALGOS.map((a) => [a.id, a]));
const RENDERER_URLS = {
  webgpu: "/frontend/js/stream/renderers/WebGpuRenderer.js",
  webgl: "/frontend/js/stream/renderers/WebGlRenderer.js",
};
const MWSR_URL = "/frontend/js/stream/renderers/MwsrShader.js";
// MWSR's per-draw values and their product defaults (read from the module at
// boot); the panel's two sliders, applied through the renderers' setMwsrTuning.
const MWSR_DEF = {
  kg: 1,
  ft: 0.005,
};
// MwsrShader's module once loaded (boot): its sharpness law by the scale
let mwsrModule = null;
const KG_RANGE = [0, 3, 0.05];
const FT_RANGE = [0.0025, 0.1, 0.0025];
// The gaze zone (MwsrShader's MWSR_ZONE and its GAZE constant, plan §15.2):
// the green ellipse's centre y and half-axes (fractions of the picture), the
// orange ring's width (a fraction of the half-axes), the vote threshold at the
// centre and its sharpness as a multiple of the law's, which stays the border's.
// The comparator pins the zone's weight to 1, as on a moving picture; the
// product follows the stream's motion (GazeZone), behind a lab key.
const ZONE_DEF = {
  cx: 0.5,
  cy: 0.5,
  rx: 0.2,
  ry: 0.45,
  ring: 0.75,
  ft: 0.005,
  boost: 1.5,
};
// id in the link, label, [min, max, step], key in the zone, decimals
const ZONE_SLIDERS = [
  ["zcy", "centre y", [0.3, 0.7, 0.01], "cy", 2],
  ["zrx", "demi-axe x", [0.05, 0.5, 0.01], "rx", 2],
  ["zry", "demi-axe y", [0.1, 0.6, 0.01], "ry", 2],
  ["zring", "anneau", [0.05, 1.5, 0.05], "ring", 2],
  ["zft", "τ centre", [0.0025, 0.1, 0.0025], "ft", 4],
  ["zboost", "netteté centre (× loi)", [1, 3, 0.05], "boost", 2],
];
const API_LABEL = { webgpu: "WebGPU", webgl: "WebGL2", canvas2d: "Canvas2D" };
const DEFAULT_IMAGES = ["/test/test.png", "/test/00_Background.png"];
const STORE = "mw_compare";
const WARM = 20;
const UA = navigator.userAgent || "";
const IS_IOS =
  /iP(hone|ad|od)/.test(UA) || (/Mac/.test(UA) && navigator.maxTouchPoints > 1);

// ── the state ──────────────────────────────────────────────────────────────

const state = {
  img: "",
  caseId: "720-1440",
  order: ALGOS.map((a) => a.id),
  on: new Set(["natif", "mw", "fsr1", "sgsr1", "nis"]),
  bands: "v",
  zone: "same",
  api: "auto",
  iters: 300,
  kg: null, // null: the product's law (mwsrLaw)
  ft: MWSR_DEF.ft,
  // the gaze zone: off (the product on a still picture), its values, the
  // rings drawn over the bands
  gaze: false,
  gz: { ...ZONE_DEF },
  rings: true,
  soft: false,
};
const shown = () => state.order.filter((a) => state.on.has(a));
// the law's sharpness at this case's sizes (MwsrShader's mwsrLaw)
const lawKg = () => {
  const c = currentCase();
  return mwsrModule && mwsrModule.mwsrLaw && c
    ? mwsrModule.mwsrLaw((c.inLevel * 16) / 9, c.inLevel, (c.outLevel * 16) / 9, c.outLevel).kg
    : MWSR_DEF.kg;
};
const kgNow = () => (state.kg === null ? lawKg() : state.kg);
// no sharpness named: the renderer's mwsrUniforms() takes the law itself
const tuning = () => ({
  ...(state.kg === null ? {} : { kg: state.kg }),
  ft: state.ft,
});
const tuned = () =>
  state.kg !== null ||
  Math.abs(state.ft - MWSR_DEF.ft) > 1e-9 ||
  state.gaze;
const zoneText = () =>
  " · zone " +
  fr(state.gz.rx, 2) +
  "×" +
  fr(state.gz.ry, 2) +
  " +" +
  fr(state.gz.ring, 2) +
  " · τc " +
  fr(state.gz.ft, 3) +
  " · nc ×" +
  fr(state.gz.boost, 2);
const tuningText = () =>
  "netteté " +
  fr(kgNow(), 2) +
  (state.kg === null ? " (loi)" : "") +
  " · τ " +
  fr(state.ft, 3) +
  (state.gaze ? zoneText() : "");
const currentCase = () => CASE_BY[state.caseId];
const sizeOf = (level) => [Math.round((level * 16) / 9), level];

function readState() {
  const q = Object.fromEntries(new URLSearchParams(location.search));
  let saved = {};
  try {
    saved = JSON.parse(localStorage.getItem(STORE) || "{}") || {};
  } catch (e) {
    // no storage: the query and the defaults
  }
  const pick = (k) => (q[k] !== undefined ? q[k] : saved[k]);
  if (pick("img") !== undefined) state.img = String(pick("img"));
  if (CASE_BY[pick("case")]) state.caseId = pick("case");
  if (saved.order) {
    const o = String(saved.order)
      .split(",")
      .filter((a) => ALGO_BY[a]);
    state.order = [...o, ...state.order.filter((a) => !o.includes(a))];
  }
  if (pick("algos") !== undefined) {
    const on = String(pick("algos"))
      .split(",")
      .filter((a) => ALGO_BY[a]);
    state.on = new Set(on);
    state.order = [...on, ...state.order.filter((a) => !on.includes(a))];
  }
  const bands = pick("bands");
  state.bands =
    bands === "h" || bands === "v"
      ? bands
      : innerHeight > innerWidth
        ? "h"
        : "v";
  if (pick("zone") === "cont" || pick("zone") === "same")
    state.zone = pick("zone");
  if (["auto", "webgpu", "webgl"].includes(pick("api")))
    state.api = pick("api");
  const it = parseInt(pick("iters"), 10);
  if (it >= 1 && it <= 100000) state.iters = it;
  // the sliders come from the link only: a plain /compare starts at the
  // product's own values, whatever was tried last time
  const kg = parseFloat(q.kg);
  if (kg >= 0 && kg <= 4) state.kg = kg;
  const ft = parseFloat(q.ft);
  if (ft >= 0.001 && ft <= 0.5) state.ft = ft;
  state.gaze = q.gaze === "1";
  for (const [id, , [min, max], key] of ZONE_SLIDERS) {
    const v = parseFloat(q[id]);
    if (v >= min && v <= max) state.gz[key] = v;
  }
  state.rings = q.rings !== "0";
  state.soft = q.soft === "1";
}

function writeState() {
  const p = new URLSearchParams();
  if (state.img) p.set("img", state.img);
  p.set("case", state.caseId);
  p.set("algos", shown().join(","));
  p.set("bands", state.bands);
  p.set("zone", state.zone);
  p.set("api", state.api);
  p.set("iters", String(state.iters));
  if (state.kg !== null) p.set("kg", String(state.kg));
  p.set("ft", String(state.ft));
  if (state.gaze) {
    p.set("gaze", "1");
    for (const [id, , , key] of ZONE_SLIDERS) p.set(id, String(state.gz[key]));
    if (!state.rings) p.set("rings", "0");
  }
  if (state.soft) p.set("soft", "1");
  history.replaceState(null, "", "?" + p.toString());
  try {
    localStorage.setItem(
      STORE,
      JSON.stringify({
        img: state.img,
        case: state.caseId,
        order: state.order.join(","),
        algos: shown().join(","),
        bands: state.bands,
        zone: state.zone,
        api: state.api,
        iters: state.iters,
      }),
    );
  } catch (e) {
    // no storage
  }
}

// ── the worker ─────────────────────────────────────────────────────────────

const worker = new Worker(new URL("./compare-worker.js", import.meta.url));
const pending = new Map();
let rpcId = 0;
worker.onmessage = (e) => {
  const p = pending.get(e.data.id);
  if (!p) return;
  pending.delete(e.data.id);
  if (e.data.error) p.reject(new Error(e.data.error));
  else p.resolve(e.data);
};
worker.onerror = (e) => {
  for (const p of pending.values()) p.reject(new Error("worker: " + e.message));
  pending.clear();
};
function ask(msg, transfer) {
  return new Promise((resolve, reject) => {
    const id = ++rpcId;
    pending.set(id, { resolve, reject });
    worker.postMessage({ ...msg, id }, transfer || []);
  });
}

// ── the picture and the inputs ─────────────────────────────────────────────

const source = {
  name: "",
  w: 0,
  h: 0,
  fitted: false,
  canvases: new Map(),
  frames: new Map(),
};

function dropInputs() {
  for (const f of source.frames.values()) f.close();
  source.frames.clear();
  source.canvases.clear();
}

/** The picture, as 2560×1440 (cover-fitted when it is another size), its
 *  reductions by the worker; every level as a canvas, the frames made later. */
async function loadSource(src) {
  status("Chargement de l'image…");
  let blob;
  let name;
  if (src.file) {
    blob = src.file;
    name = src.file.name;
  } else {
    const r = await fetch(src.url, { cache: "no-store" });
    if (!r.ok) throw new Error(src.url + " : " + r.status);
    blob = await r.blob();
    name = src.url.split("/").pop();
  }
  const bmp = await createImageBitmap(blob);
  const cv = new OffscreenCanvas(SRC_W, SRC_H);
  const ctx = cv.getContext("2d", { alpha: false, willReadFrequently: true });
  const s = Math.max(SRC_W / bmp.width, SRC_H / bmp.height);
  const dw = bmp.width * s;
  const dh = bmp.height * s;
  ctx.imageSmoothingEnabled = true;
  ctx.imageSmoothingQuality = "high";
  ctx.drawImage(bmp, (SRC_W - dw) / 2, (SRC_H - dh) / 2, dw, dh);
  const origW = bmp.width;
  const origH = bmp.height;
  bmp.close();
  status("Réductions bicubiques (1080p, 720p)…");
  const data = ctx.getImageData(0, 0, SRC_W, SRC_H);
  const res = await ask(
    {
      type: "source",
      width: SRC_W,
      height: SRC_H,
      data: data.data.buffer,
      levels: [1080, 720],
    },
    [data.data.buffer],
  );
  dropInputs();
  source.canvases.set(1440, cv);
  for (const lv of [1080, 720]) {
    const [w, h] = sizeOf(lv);
    const c = new OffscreenCanvas(w, h);
    c.getContext("2d", { alpha: false }).putImageData(
      new ImageData(new Uint8ClampedArray(res.levels[lv]), w, h),
      0,
      0,
    );
    source.canvases.set(lv, c);
  }
  source.name = name;
  source.w = origW;
  source.h = origH;
  source.fitted = origW !== SRC_W || origH !== SRC_H;
  status("");
}

/** The VideoFrame of a level, as the renderers take one; made from the canvas,
 *  else from a bitmap of it, else from its bytes. Cloned at every draw. */
async function inputFrame(level) {
  if (source.frames.has(level)) return source.frames.get(level);
  const cv = source.canvases.get(level);
  if (!cv) throw new Error("no picture at " + level + "p");
  let f;
  try {
    f = new VideoFrame(cv, { timestamp: 0 });
  } catch (e1) {
    try {
      const bmp = await createImageBitmap(cv);
      f = new VideoFrame(bmp, { timestamp: 0 });
      bmp.close();
    } catch (e2) {
      const id = cv.getContext("2d").getImageData(0, 0, cv.width, cv.height);
      f = new VideoFrame(id.data.buffer, {
        format: "RGBA",
        codedWidth: cv.width,
        codedHeight: cv.height,
        timestamp: 0,
      });
    }
  }
  source.frames.set(level, f);
  return f;
}

// ── the renderers ──────────────────────────────────────────────────────────
// One product renderer per API, its pass switched with setAlgo as the
// enhancer ladder does; the two Canvas2D "algorithms" are drawImage itself.

const engines = { webgpu: null, webgl: null };
const canvas2d = { cv: new OffscreenCanvas(16, 16), ctx: null };
const scratch = { cv: new OffscreenCanvas(16, 16), ctx: null };

async function engine(api) {
  if (engines[api]) return engines[api];
  try {
    const mod = await import(RENDERER_URLS[api]);
    const cv = new OffscreenCanvas(16, 16);
    let r;
    if (api === "webgpu") {
      if (!navigator.gpu) throw new Error("pas de navigator.gpu");
      r = await mod.WebGpuRenderer.create(cv, {
        algo: "mwsr",
        videoCodec: "h264",
      });
    } else
      r = await mod.WebGlRenderer.create(cv, {
        algo: "gl-mwsr",
        videoCodec: "h264",
      });
    engines[api] = { r, gpu: describeGpu(api, r) };
  } catch (e) {
    engines[api] = { error: e.message || String(e) };
  }
  renderInfo();
  return engines[api];
}

function describeGpu(api, r) {
  try {
    if (api === "webgl") {
      const gl = r.gl;
      const dbg = gl.getExtension("WEBGL_debug_renderer_info");
      return String(
        dbg
          ? gl.getParameter(dbg.UNMASKED_RENDERER_WEBGL)
          : gl.getParameter(gl.RENDERER),
      );
    }
    const info = (r._adapter && r._adapter.info) || {};
    return [info.vendor, info.architecture, info.device, info.description]
      .filter(Boolean)
      .join(" ");
  } catch (e) {
    return "?";
  }
}

/** The API an algorithm runs on now: Canvas2D for the two plain ones, else
 *  the setting, auto being the product's choice; WebGL2 when WebGPU is not
 *  there or refused the renderer. */
function apiFor(algoId) {
  const def = ALGO_BY[algoId];
  if (!def.gpu) return "canvas2d";
  let api = state.api === "auto" ? (IS_IOS ? "webgpu" : "webgl") : state.api;
  if (
    api === "webgpu" &&
    (!navigator.gpu || (engines.webgpu && engines.webgpu.error))
  )
    api = "webgl";
  return api;
}
const outKey = (algoId) =>
  state.caseId +
  "|" +
  apiFor(algoId) +
  "|" +
  algoId +
  (algoId === "mw"
    ? "|s" +
      kgNow() +
      "|t" +
      state.ft +
      (state.gaze
        ? "|g" + ZONE_SLIDERS.map(([, , , k]) => state.gz[k]).join(",")
        : "")
    : "");

/** MWSR's settings onto the renderer, before one of its draws: the two
 *  per-draw values and the gaze zone, its weight pinned to 1: the zone as on a
 *  moving picture. */
function applyTuning(algoId, r) {
  if (algoId !== "mw") return;
  if (r.setMwsrTuning) r.setMwsrTuning(tuning());
  if (r.setMwsrZone) r.setMwsrZone(state.gaze ? { ...state.gz } : null);
  if (r.setMwsrGaze) r.setMwsrGaze(state.gaze ? 1 : null);
}

function pixelsOf(bmp, w, h) {
  const cv = scratch.cv;
  if (cv.width !== w || cv.height !== h) {
    cv.width = w;
    cv.height = h;
  }
  scratch.ctx ||= cv.getContext("2d", {
    alpha: false,
    willReadFrequently: true,
  });
  scratch.ctx.drawImage(bmp, 0, 0);
  return scratch.ctx.getImageData(0, 0, w, h);
}

function looksBlack(px) {
  const d = px.data;
  for (let i = 0; i < d.length; i += 997 * 4)
    if (d[i] || d[i + 1] || d[i + 2]) return false;
  return true;
}

/** The browser's own scaling: drawImage of the level's canvas (a GPU-side
 *  source everywhere, as a decoded frame is; a VideoFrame made from pixels
 *  went through the CPU on Safari, ten times the cost). */
function drawPlain(algoId, src, outW, outH) {
  const cv = canvas2d.cv;
  if (cv.width !== outW || cv.height !== outH) {
    cv.width = outW;
    cv.height = outH;
  }
  canvas2d.ctx ||= cv.getContext("2d", { alpha: false });
  const ctx = canvas2d.ctx;
  ctx.imageSmoothingEnabled = true;
  ctx.imageSmoothingQuality = algoId === "smooth2d" ? "high" : "low";
  ctx.drawImage(src, 0, 0, outW, outH);
}

/** One frame through an algorithm: its output as a bitmap for the viewer and
 *  as pixels for the distance, with the renderer's own path. */
async function renderOnce(algoId, c) {
  const def = ALGO_BY[algoId];
  const api = apiFor(algoId);
  const [outW, outH] = sizeOf(c.outLevel);
  let bmp;
  let path;
  let r = null;
  let frame = null;
  if (api === "canvas2d") {
    drawPlain(algoId, source.canvases.get(c.inLevel), outW, outH);
    bmp = canvas2d.cv.transferToImageBitmap();
    path = "drawImage " + (algoId === "smooth2d" ? "high" : "low");
  } else {
    frame = await inputFrame(c.inLevel);
    const eng = await engine(api);
    if (eng.error) throw new Error(API_LABEL[api] + " : " + eng.error);
    r = eng.r;
    r.setAlgo(api === "webgpu" ? def.gpu : def.gl);
    r.setOutputSize(outW, outH);
    applyTuning(algoId, r);
    await r.draw(frame.clone());
    if (r.failed) throw new Error("le renderer a abandonné");
    path = r.lastDraw.path;
    if (/failed|\+lost$/.test(path))
      throw new Error("aucune image dessinée (" + path + ")");
    if (r.canvas.width !== outW || r.canvas.height !== outH)
      throw new Error("canvas " + r.canvas.width + "×" + r.canvas.height);
    bmp = r.canvas.transferToImageBitmap();
  }
  if (bmp.width !== outW || bmp.height !== outH)
    throw new Error("capture " + bmp.width + "×" + bmp.height);
  let px = pixelsOf(bmp, outW, outH);
  if (r && looksBlack(px)) {
    // the transfer gave nothing (a WebGPU canvas some browser does not hand
    // over that way): the frame drawn again and the canvas read as an image
    bmp.close();
    await r.draw(frame.clone());
    bmp = await createImageBitmap(r.canvas);
    px = pixelsOf(bmp, outW, outH);
    if (looksBlack(px)) path += " (capture noire)";
  }
  return { bmp, px, path, api, w: outW, h: outH };
}

// ── the time ───────────────────────────────────────────────────────────────

function nextTask() {
  return new Promise((res) => {
    const ch = new MessageChannel();
    ch.port1.onmessage = () => res();
    ch.port2.postMessage(0);
  });
}
const sleep = (ms) => new Promise((res) => setTimeout(res, ms));

// The GPU's completion of what WebGL2 was given: a fence polled task by task
// (Chrome's gl.finish() is a flush), as the lab does. On NVIDIA's D3D11
// driver the fence alone waits some 120 ms for a submission that neither the
// draw nor the flush provokes (nor finish, readPixels or more flushes: J37);
// a time query closed after the draw forces it, so timeAlgo wraps the draw in
// one where the extension exists (4.7 ms there, nothing changes elsewhere).
// The query lives until the fence has signalled: on Qualcomm's D3D11 driver
// (Adreno 618) a query deleted before that frees nothing, and the renderer's
// own per-draw fence keeps the wait at some 190 ms; kept, 20 ms (J38).
async function glDone(gl) {
  const sync = gl.fenceSync(gl.SYNC_GPU_COMMANDS_COMPLETE, 0);
  if (!sync) return;
  gl.flush();
  const t = performance.now();
  for (;;) {
    await nextTask();
    if (gl.getSyncParameter(sync, gl.SYNC_STATUS) === gl.SIGNALED) break;
    if (performance.now() - t > 5000) break;
  }
  gl.deleteSync(sync);
}

let busyUntil = 0;
const busy = () => performance.now() < busyUntil || document.hidden;
async function pauseWhileBusy(alive) {
  while (busy() && alive()) await sleep(80);
}

/** `iters` draws of an algorithm, each timed from the call to the GPU's
 *  completion (the WebGPU draw waits for it itself; WebGL2 gets a fence;
 *  Canvas2D a one-pixel read), after a warm-up; paused while the user pans. */
async function timeAlgo(algoId, c, iters, onProgress, alive) {
  const def = ALGO_BY[algoId];
  const api = apiFor(algoId);
  const [outW, outH] = sizeOf(c.outLevel);
  let draw; // one frame of the algorithm, from the level's input
  let sync = null;
  let r = null;
  if (api === "canvas2d") {
    const src = source.canvases.get(c.inLevel);
    draw = () => drawPlain(algoId, src, outW, outH);
    sync = () => canvas2d.ctx.getImageData(0, 0, 1, 1);
  } else {
    const frame = await inputFrame(c.inLevel);
    const eng = await engine(api);
    if (eng.error) throw new Error(API_LABEL[api] + " : " + eng.error);
    r = eng.r;
    r.setAlgo(api === "webgpu" ? def.gpu : def.gl);
    r.setOutputSize(outW, outH);
    applyTuning(algoId, r);
    draw = () => r.draw(frame.clone());
    if (api === "webgl") {
      const gl = r.gl;
      const timer = gl.getExtension("EXT_disjoint_timer_query_webgl2");
      if (timer) {
        const bare = draw;
        let q = null; // the draw's query, deleted once the fence has signalled
        draw = async () => {
          if (q) gl.deleteQuery(q); // a draw that threw left its query behind
          q = gl.createQuery();
          gl.beginQuery(timer.TIME_ELAPSED_EXT, q);
          try {
            await bare();
          } finally {
            gl.endQuery(timer.TIME_ELAPSED_EXT);
          }
        };
        sync = async () => {
          await glDone(gl);
          if (q) gl.deleteQuery(q);
          q = null;
        };
      } else sync = () => glDone(gl);
    }
  }
  for (let i = 0; i < WARM; i++) {
    await draw();
    if (sync) await sync();
    await nextTask();
  }
  if (r && (r.failed || /failed|\+lost$/.test(r.lastDraw.path)))
    throw new Error("aucune image dessinée (" + r.lastDraw.path + ")");
  const times = new Float64Array(iters);
  let total = 0;
  for (let i = 0; i < iters; i++) {
    await pauseWhileBusy(alive);
    if (!alive()) return null;
    const t0 = performance.now();
    await draw();
    if (sync) await sync();
    const dt = performance.now() - t0;
    times[i] = dt;
    total += dt;
    if (i % 25 === 24) onProgress(i + 1);
    // a task between two draws, outside the measure: the page's gestures and
    // frames get the thread (a Canvas2D draw yields nothing by itself, and
    // ten of them between two tasks froze an iPhone's screen for a second)
    await nextTask();
  }
  const sorted = times.slice().sort();
  return {
    mean: total / iters,
    p50: sorted[iters >> 1],
    n: iters,
    path: r ? r.lastDraw.path : "",
  };
}

// ── the run ────────────────────────────────────────────────────────────────

const outputs = new Map(); // key → {bmp, w, h, path, api} | {error}, the current case only
const results = new Map(); // key → {metrics, metricsError, time, timeError, timing}
const res = (key) => {
  if (!results.has(key)) results.set(key, {});
  return results.get(key);
};
let gen = 0;
let current = Promise.resolve();
let running = false;
let idleResolvers = [];

function dropOutputs() {
  for (const o of outputs.values()) if (o.bmp) o.bmp.close();
  outputs.clear();
  if (gl && !gl.isContextLost())
    for (const tx of textures.values()) gl.deleteTexture(tx.tex);
  textures.clear();
}

function clearAll() {
  dropOutputs();
  results.clear();
}

/** The current case, shown algorithm after shown algorithm: the picture first
 *  (every output rendered once, its distance asked of the worker), then the
 *  time of each. A newer refresh ends this one at its next step. */
function refresh() {
  const g = ++gen;
  const prev = current;
  current = (async () => {
    try {
      await prev;
    } catch (e) {
      // the previous run's failure is its own
    }
    if (g !== gen) return;
    running = true;
    try {
      await run(g);
    } catch (e) {
      status("Erreur : " + (e.message || e));
      console.error(e);
    } finally {
      if (g === gen) {
        running = false;
        for (const r of idleResolvers) r();
        idleResolvers = [];
      }
    }
  })();
  return current;
}

async function run(g) {
  const c = currentCase();
  if (!source.canvases.size) return;
  const alive = () => g === gen;
  for (const a of shown()) {
    const key = outKey(a);
    if (outputs.has(key)) continue;
    let out;
    try {
      out = await renderOnce(a, c);
    } catch (e) {
      out = { error: e.message || String(e) };
    }
    if (!alive()) {
      if (out.bmp) out.bmp.close();
      return;
    }
    outputs.set(key, out);
    viewerTake(key, out);
    res(key).path = out.path;
    requestDraw();
    updateChips();
    if (out.px) {
      const px = out.px;
      out.px = null;
      ask(
        {
          type: "metrics",
          level: c.outLevel,
          width: out.w,
          height: out.h,
          data: px.data.buffer,
        },
        [px.data.buffer],
      )
        .then(
          (m) => {
            res(key).metrics = m;
          },
          (e) => {
            res(key).metricsError = e.message;
          },
        )
        .then(() => {
          updateChips();
          renderTable();
        });
    }
  }
  status("");
  for (const a of shown()) {
    const key = outKey(a);
    const out = outputs.get(key);
    if (!out || out.error) continue;
    const rs = res(key);
    if (rs.time || rs.timeError) continue;
    rs.timing = { done: 0, n: state.iters };
    updateChips();
    try {
      const t = await timeAlgo(
        a,
        c,
        state.iters,
        (done) => {
          rs.timing = { done, n: state.iters };
          updateChips();
        },
        alive,
      );
      if (t) rs.time = t;
    } catch (e) {
      rs.timeError = e.message || String(e);
    }
    rs.timing = null;
    if (!alive()) return;
    updateChips();
    renderTable();
  }
}

// ── the viewer ─────────────────────────────────────────────────────────────
// WebGL2 draws the bands: each output is a texture uploaded once (the same
// bytes the distance reads), a frame is one clear and one quad per band, the
// same cost on every device. Safari on an iPhone spent seconds on the 2D
// drawImage of 1440p bitmaps that did it before; that path stays as the
// fallback without WebGL2, and on ?viewer=2d.

let view = document.getElementById("view");
// The gaze zone's rings, a 2D canvas over the viewer (under the labels):
// the green ellipse and the orange one, 1 + ring times it, on every band.
const rings = document.createElement("canvas");
rings.id = "rings";
Object.assign(rings.style, {
  position: "fixed",
  inset: "0",
  width: "100%",
  height: "100%",
  pointerEvents: "none",
});
view.after(rings);
const bandsEl = document.getElementById("bands");
const zoomEl = document.getElementById("zoom");
const statusEl = document.getElementById("status");
const t = { zoom: 1, x: 0, y: 0 }; // image origin, in device px of the band (same) or the page (cont)
let dpr = 1;
let W = 0;
let H = 0;
let rects = [];
const ZOOM_MIN = 0.05;
const ZOOM_MAX = 64;
const BG = [16 / 255, 16 / 255, 20 / 255]; // #101014

let gl = null;
let vctx = null;
let glProg = null;
let glU = null;
const textures = new Map(); // output key → {tex, w, h}

const VIEW_VS = `#version 300 es
void main() {
  vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}`;
const VIEW_FS = `#version 300 es
precision highp float;
uniform sampler2D u_tex;
uniform vec2 u_origin; // the picture's origin on the page, device px, y down
uniform float u_zoom;
uniform vec2 u_size; // the picture, px
uniform float u_height; // the canvas, px
uniform vec3 u_bg;
uniform int u_nearest; // 1 from 1:1 up: the texel under the pixel's centre
out vec4 o;
void main() {
  vec2 p = vec2(gl_FragCoord.x, u_height - gl_FragCoord.y);
  vec2 q = (p - u_origin) / u_zoom;
  if (any(lessThan(q, vec2(0.0))) || any(greaterThanEqual(q, u_size))) {
    o = vec4(u_bg, 1.0);
    return;
  }
  // texelFetch, not a normalised coordinate: at 1:1 a sample on a texel's
  // edge would fall on either side from one column to the next
  if (u_nearest == 1) o = vec4(texelFetch(u_tex, ivec2(q), 0).rgb, 1.0);
  else o = vec4(texture(u_tex, q / u_size).rgb, 1.0);
}`;

function compileGl(g) {
  const sh = (type, src) => {
    const s = g.createShader(type);
    g.shaderSource(s, src);
    g.compileShader(s);
    if (!g.getShaderParameter(s, g.COMPILE_STATUS))
      throw new Error(g.getShaderInfoLog(s) || "shader");
    return s;
  };
  const prog = g.createProgram();
  g.attachShader(prog, sh(g.VERTEX_SHADER, VIEW_VS));
  g.attachShader(prog, sh(g.FRAGMENT_SHADER, VIEW_FS));
  g.linkProgram(prog);
  if (!g.getProgramParameter(prog, g.LINK_STATUS))
    throw new Error(g.getProgramInfoLog(prog) || "program");
  glProg = prog;
  glU = {};
  for (const n of [
    "u_tex",
    "u_origin",
    "u_zoom",
    "u_size",
    "u_height",
    "u_bg",
    "u_nearest",
  ])
    glU[n] = g.getUniformLocation(prog, n);
  g.useProgram(prog);
  g.uniform1i(glU.u_tex, 0);
  g.uniform3f(glU.u_bg, BG[0], BG[1], BG[2]);
  g.pixelStorei(g.UNPACK_FLIP_Y_WEBGL, false);
  g.pixelStorei(g.UNPACK_PREMULTIPLY_ALPHA_WEBGL, false);
  g.pixelStorei(g.UNPACK_COLORSPACE_CONVERSION_WEBGL, g.NONE);
}

function initViewer() {
  const want2d = new URLSearchParams(location.search).get("viewer") === "2d";
  if (!want2d) {
    try {
      gl = view.getContext("webgl2", {
        alpha: false,
        antialias: false,
        depth: false,
        stencil: false,
        preserveDrawingBuffer: false,
      });
      if (gl) compileGl(gl);
    } catch (e) {
      console.warn("afficheur WebGL2 refusé, Canvas2D :", e);
      // the canvas is taken by its WebGL2 context: a fresh one for 2D
      const fresh = view.cloneNode(false);
      view.replaceWith(fresh);
      view = fresh;
      gl = null;
    }
  }
  if (gl) {
    view.addEventListener("webglcontextlost", (e) => {
      e.preventDefault();
      textures.clear();
    });
    view.addEventListener("webglcontextrestored", () => {
      compileGl(gl);
      dropOutputs();
      refresh();
    });
  } else vctx = view.getContext("2d", { alpha: false });
}
initViewer();

/** An output into the viewer: its pixels as a mipmapped texture (the bitmap
 *  is then let go), or the bitmap kept for the 2D path. */
function viewerTake(key, out) {
  if (!gl || !out.px || gl.isContextLost()) return;
  const old = textures.get(key);
  if (old) gl.deleteTexture(old.tex);
  const tex = gl.createTexture();
  gl.bindTexture(gl.TEXTURE_2D, tex);
  const d = out.px.data;
  gl.texImage2D(
    gl.TEXTURE_2D,
    0,
    gl.RGBA8,
    out.w,
    out.h,
    0,
    gl.RGBA,
    gl.UNSIGNED_BYTE,
    new Uint8Array(d.buffer, d.byteOffset, d.byteLength),
  );
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
  gl.generateMipmap(gl.TEXTURE_2D);
  textures.set(key, { tex, w: out.w, h: out.h });
  if (out.bmp) {
    out.bmp.close();
    out.bmp = null;
  }
}

function status(s) {
  statusEl.textContent = s;
  statusEl.style.display = s ? "block" : "none";
}

function layout() {
  dpr = window.devicePixelRatio || 1;
  W = Math.max(1, Math.round(view.clientWidth * dpr));
  H = Math.max(1, Math.round(view.clientHeight * dpr));
  if (view.width !== W || view.height !== H) {
    view.width = W;
    view.height = H;
  }
  if (rings.width !== W || rings.height !== H) {
    rings.width = W;
    rings.height = H;
  }
  const n = Math.max(1, shown().length);
  rects = [];
  for (let i = 0; i < n; i++) {
    if (state.bands === "v") {
      const x0 = Math.round((i * W) / n);
      const x1 = Math.round(((i + 1) * W) / n);
      rects.push({ x: x0, y: 0, w: x1 - x0, h: H });
    } else {
      const y0 = Math.round((i * H) / n);
      const y1 = Math.round(((i + 1) * H) / n);
      rects.push({ x: 0, y: y0, w: W, h: y1 - y0 });
    }
  }
  layoutChips();
  requestDraw();
}

const frameRect = () =>
  state.zone === "same"
    ? rects[0] || { x: 0, y: 0, w: W, h: H }
    : { x: 0, y: 0, w: W, h: H };
const outSize = () => sizeOf(currentCase().outLevel);

function centerFrac() {
  const r = frameRect();
  const [ow, oh] = outSize();
  return {
    cx: (r.w / 2 - t.x) / t.zoom / ow,
    cy: (r.h / 2 - t.y) / t.zoom / oh,
  };
}
function applyCenter(f) {
  const r = frameRect();
  const [ow, oh] = outSize();
  t.x = r.w / 2 - f.cx * ow * t.zoom;
  t.y = r.h / 2 - f.cy * oh * t.zoom;
  constrain();
}
/** The picture's edges never pass the frame's centre: at most half a band of
 *  black on either axis (Bruno, 23/10), whatever the zoom. */
function constrain() {
  const r = frameRect();
  const [ow, oh] = outSize();
  t.x = Math.min(r.w / 2, Math.max(r.w / 2 - ow * t.zoom, t.x));
  t.y = Math.min(r.h / 2, Math.max(r.h / 2 - oh * t.zoom, t.y));
}
/** A change of layout or case with the same picture point kept at the centre. */
function keepCenter(fn) {
  const f = centerFrac();
  fn();
  applyCenter(f);
  requestDraw();
  updateZoomChip();
}
function initialView() {
  t.zoom = 1;
  applyCenter({ cx: 0.5, cy: 0.5 });
  requestDraw();
  updateZoomChip();
}
function fitView() {
  const r = frameRect();
  const [ow, oh] = outSize();
  t.zoom = clampZoom(Math.min(r.w / ow, r.h / oh));
  applyCenter({ cx: 0.5, cy: 0.5 });
  requestDraw();
  updateZoomChip();
}
const clampZoom = (z) => Math.min(ZOOM_MAX, Math.max(ZOOM_MIN, z));

/** The origin the transform is relative to, for a page point: its band's
 *  corner (same zone), or the page's (one continuous picture). */
function originAt(p) {
  if (state.zone !== "same") return { x: 0, y: 0 };
  const rc =
    rects.find(
      (r) => p.x >= r.x && p.x < r.x + r.w && p.y >= r.y && p.y < r.y + r.h,
    ) || rects[0];
  return rc ? { x: rc.x, y: rc.y } : { x: 0, y: 0 };
}
function zoomAt(p, z) {
  z = clampZoom(z);
  const o = originAt(p);
  const px = (p.x - o.x - t.x) / t.zoom;
  const py = (p.y - o.y - t.y) / t.zoom;
  t.zoom = z;
  t.x = p.x - o.x - px * z;
  t.y = p.y - o.y - py * z;
  constrain();
  requestDraw();
  updateZoomChip();
}

let drawQueued = false;
function requestDraw() {
  constrain();
  if (drawQueued) return;
  drawQueued = true;
  requestAnimationFrame(() => {
    drawQueued = false;
    draw();
  });
}
function draw() {
  if (gl) drawGl();
  else draw2d();
  drawRings();
}
/** The zone's two ellipses over each band, in the band's own transform. */
function drawRings() {
  const c = rings.getContext("2d");
  c.setTransform(1, 0, 0, 1, 0, 0);
  c.clearRect(0, 0, W, H);
  if (!state.gaze || !state.rings) return;
  const [ow, oh] = outSize();
  const z = t.zoom;
  const zn = state.gz;
  c.lineWidth = Math.max(1.5, 1.5 * dpr);
  shown().forEach((a, i) => {
    const rc = rects[i];
    if (!rc) return;
    const ox = (state.zone === "same" ? rc.x : 0) + t.x;
    const oy = (state.zone === "same" ? rc.y : 0) + t.y;
    const cx = ox + zn.cx * ow * z;
    const cy = oy + zn.cy * oh * z;
    c.save();
    c.beginPath();
    c.rect(rc.x, rc.y, rc.w, rc.h);
    c.clip();
    for (const [k, color] of [
      [1, "rgba(46, 204, 64, 0.9)"],
      [1 + zn.ring, "rgba(255, 133, 27, 0.9)"],
    ]) {
      c.beginPath();
      c.ellipse(
        cx,
        cy,
        zn.rx * ow * z * k,
        zn.ry * oh * z * k,
        0,
        0,
        2 * Math.PI,
      );
      c.strokeStyle = color;
      c.stroke();
    }
    c.restore();
  });
}
function drawGl() {
  if (gl.isContextLost()) return;
  const z = t.zoom;
  gl.viewport(0, 0, W, H);
  gl.disable(gl.SCISSOR_TEST);
  gl.clearColor(BG[0], BG[1], BG[2], 1);
  gl.clear(gl.COLOR_BUFFER_BIT);
  gl.useProgram(glProg);
  gl.uniform1f(glU.u_height, H);
  gl.uniform1f(glU.u_zoom, z);
  gl.uniform1i(glU.u_nearest, z < 1 ? 0 : 1);
  gl.enable(gl.SCISSOR_TEST);
  // the pixels themselves from 1:1 up; below, a mipmapped reduction
  const minF = z < 1 ? gl.LINEAR_MIPMAP_LINEAR : gl.NEAREST;
  const magF = z < 1 ? gl.LINEAR : gl.NEAREST;
  shown().forEach((a, i) => {
    const rc = rects[i];
    const tx = textures.get(outKey(a));
    if (!rc || !tx) return;
    gl.scissor(rc.x, H - rc.y - rc.h, rc.w, rc.h);
    gl.bindTexture(gl.TEXTURE_2D, tx.tex);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, minF);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, magF);
    gl.uniform2f(
      glU.u_origin,
      (state.zone === "same" ? rc.x : 0) + t.x,
      (state.zone === "same" ? rc.y : 0) + t.y,
    );
    gl.uniform2f(glU.u_size, tx.w, tx.h);
    gl.drawArrays(gl.TRIANGLES, 0, 3);
  });
}
function draw2d() {
  vctx.setTransform(1, 0, 0, 1, 0, 0);
  vctx.fillStyle = "#101014";
  vctx.fillRect(0, 0, W, H);
  const z = t.zoom;
  vctx.imageSmoothingEnabled = z < 1;
  vctx.imageSmoothingQuality = "high";
  shown().forEach((a, i) => {
    const rc = rects[i];
    const out = outputs.get(outKey(a));
    if (!rc || !out || !out.bmp) return;
    const ox = (state.zone === "same" ? rc.x : 0) + t.x;
    const oy = (state.zone === "same" ? rc.y : 0) + t.y;
    const ix0 = Math.max(0, Math.floor((rc.x - ox) / z));
    const iy0 = Math.max(0, Math.floor((rc.y - oy) / z));
    const ix1 = Math.min(out.w, Math.ceil((rc.x + rc.w - ox) / z));
    const iy1 = Math.min(out.h, Math.ceil((rc.y + rc.h - oy) / z));
    if (ix1 <= ix0 || iy1 <= iy0) return;
    vctx.save();
    vctx.beginPath();
    vctx.rect(rc.x, rc.y, rc.w, rc.h);
    vctx.clip();
    vctx.drawImage(
      out.bmp,
      ix0,
      iy0,
      ix1 - ix0,
      iy1 - iy0,
      ox + ix0 * z,
      oy + iy0 * z,
      (ix1 - ix0) * z,
      (iy1 - iy0) * z,
    );
    vctx.restore();
  });
}

// ── the bands' labels ──────────────────────────────────────────────────────

const fr = (x, d) =>
  Number.isFinite(x)
    ? x.toFixed(d).replace(".", ",")
    : x === Infinity
      ? "∞"
      : "—";
const esc = (s) =>
  String(s).replace(
    /[&<>"]/g,
    (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" })[c],
  );

function metricsText(rs) {
  if (rs.metrics) {
    const m = rs.metrics;
    return (
      "SSIM " +
      fr(m.ssim, 3) +
      " · PSNR " +
      (m.psnr === Infinity ? "∞" : fr(m.psnr, 1) + " dB") +
      (m.black ? ' · <span class="err">capture noire</span>' : "")
    );
  }
  if (rs.metricsError)
    return '<span class="err">qualité : ' + esc(rs.metricsError) + "</span>";
  return "qualité…";
}
function timeText(rs) {
  if (rs.time) return fr(rs.time.mean, 2) + " ms / image (" + rs.time.n + ")";
  if (rs.timeError)
    return '<span class="err">chrono : ' + esc(rs.timeError) + "</span>";
  if (rs.timing) return "chrono " + rs.timing.done + "/" + rs.timing.n + "…";
  return "chrono en attente";
}
function chipHtml(algoId) {
  const def = ALGO_BY[algoId];
  const key = outKey(algoId);
  const out = outputs.get(key);
  const rs = results.get(key) || {};
  const head =
    "<b>" +
    esc(def.label) +
    "</b> · " +
    API_LABEL[apiFor(algoId)] +
    (algoId === "mw" && tuned() ? " · " + esc(tuningText()) : "");
  if (out && out.error)
    return head + '<br><span class="err">' + esc(out.error) + "</span>";
  if (!out) return head + "<br>rendu…";
  return head + "<br>" + metricsText(rs) + "<br>" + timeText(rs);
}

function layoutChips() {
  const list = shown();
  while (bandsEl.children.length > list.length) bandsEl.lastChild.remove();
  while (bandsEl.children.length < list.length) {
    const d = document.createElement("div");
    d.className = "band";
    d.innerHTML = '<div class="chip"></div>';
    bandsEl.appendChild(d);
  }
  list.forEach((a, i) => {
    const rc = rects[i];
    const d = bandsEl.children[i];
    d.style.left = rc.x / dpr + "px";
    d.style.top = rc.y / dpr + "px";
    d.style.width = rc.w / dpr + "px";
    d.style.height = rc.h / dpr + "px";
    const last = i === list.length - 1;
    d.style.borderRightWidth = state.bands === "v" && !last ? "1px" : "0";
    d.style.borderBottomWidth = state.bands === "h" && !last ? "1px" : "0";
  });
  updateChips();
}
function updateChips() {
  shown().forEach((a, i) => {
    const el = bandsEl.children[i];
    if (el) el.firstChild.innerHTML = chipHtml(a);
  });
  updateZoomChip();
}
function updateZoomChip() {
  const z = t.zoom;
  zoomEl.textContent =
    "×" +
    fr(z, 2) +
    (Math.abs(z - 1) < 1e-3
      ? " · 1:1"
      : z < 1
        ? " · réduit par l'afficheur"
        : "") +
    " · " +
    currentCase().label;
}

// ── the gestures ───────────────────────────────────────────────────────────
// One finger pans, two pinch around their middle, a double tap goes 1:1 at
// that point or fits the picture; the wheel zooms around the pointer.

const ptrs = new Map();
let gesture = null;
let lastTap = { at: 0, x: 0, y: 0 };
const pt = (e) => ({ x: e.clientX * dpr, y: e.clientY * dpr });

function startGesture() {
  const ps = [...ptrs.values()];
  if (ps.length === 1)
    gesture = {
      kind: "pan",
      x: ps[0].x,
      y: ps[0].y,
      tx: t.x,
      ty: t.y,
      moved: 0,
    };
  else if (ps.length >= 2) {
    const [a, b] = ps;
    const mid = { x: (a.x + b.x) / 2, y: (a.y + b.y) / 2 };
    gesture = {
      kind: "pinch",
      dist: Math.max(1, Math.hypot(b.x - a.x, b.y - a.y)),
      mid,
      origin: originAt(mid),
    };
  } else gesture = null;
}

view.addEventListener("pointerdown", (e) => {
  if (panel.classList.contains("open")) closePanel();
  view.setPointerCapture(e.pointerId);
  ptrs.set(e.pointerId, pt(e));
  startGesture();
  busyUntil = Infinity;
  view.classList.add("dragging");
  e.preventDefault();
});
view.addEventListener("pointermove", (e) => {
  if (!ptrs.has(e.pointerId)) return;
  const p = pt(e);
  ptrs.set(e.pointerId, p);
  if (!gesture) return;
  if (gesture.kind === "pan" && ptrs.size === 1) {
    const dx = p.x - gesture.x;
    const dy = p.y - gesture.y;
    gesture.moved = Math.max(gesture.moved, Math.hypot(dx, dy));
    t.x = gesture.tx + dx;
    t.y = gesture.ty + dy;
    constrain();
    // against a limit the gesture follows the picture: a finger coming back
    // moves it at once, not after the way it went past
    gesture.tx = t.x - dx;
    gesture.ty = t.y - dy;
    requestDraw();
  } else if (gesture.kind === "pinch" && ptrs.size >= 2) {
    // step by step from the last event: the picture point under the fingers'
    // previous middle goes under their middle now, at the new zoom
    const [a, b] = [...ptrs.values()];
    const dist = Math.max(1, Math.hypot(b.x - a.x, b.y - a.y));
    const mid = { x: (a.x + b.x) / 2, y: (a.y + b.y) / 2 };
    const z = clampZoom((t.zoom * dist) / gesture.dist);
    const o = gesture.origin;
    const px = (gesture.mid.x - o.x - t.x) / t.zoom;
    const py = (gesture.mid.y - o.y - t.y) / t.zoom;
    t.zoom = z;
    t.x = mid.x - o.x - px * z;
    t.y = mid.y - o.y - py * z;
    constrain();
    gesture.dist = dist;
    gesture.mid = mid;
    requestDraw();
    updateZoomChip();
  }
});
function endPointer(e) {
  if (!ptrs.has(e.pointerId)) return;
  const p = pt(e);
  const tap =
    gesture &&
    gesture.kind === "pan" &&
    gesture.moved < 12 * dpr &&
    e.type === "pointerup";
  ptrs.delete(e.pointerId);
  if (tap) {
    const now = performance.now();
    if (
      now - lastTap.at < 350 &&
      Math.hypot(p.x - lastTap.x, p.y - lastTap.y) < 40 * dpr
    ) {
      lastTap = { at: 0, x: 0, y: 0 };
      if (Math.abs(t.zoom - 1) > 1e-3) zoomAt(p, 1);
      else fitView();
    } else lastTap = { at: now, x: p.x, y: p.y };
  }
  startGesture();
  if (!ptrs.size) {
    busyUntil = performance.now() + 300;
    view.classList.remove("dragging");
  }
}
view.addEventListener("pointerup", endPointer);
view.addEventListener("pointercancel", endPointer);
view.addEventListener(
  "wheel",
  (e) => {
    e.preventDefault();
    const dy =
      e.deltaMode === 1
        ? e.deltaY * 16
        : e.deltaMode === 2
          ? e.deltaY * 400
          : e.deltaY;
    zoomAt(pt(e), t.zoom * Math.exp(-dy * 0.002));
    busyUntil = performance.now() + 300;
  },
  { passive: false },
);
// Safari's own pinch of the page, and the scroll a touch could start
document.addEventListener("gesturestart", (e) => e.preventDefault());
view.addEventListener("touchmove", (e) => e.preventDefault(), {
  passive: false,
});
view.addEventListener("dblclick", (e) => e.preventDefault());
view.addEventListener("contextmenu", (e) => e.preventDefault());

window.addEventListener("keydown", (e) => {
  if (
    e.target instanceof HTMLInputElement ||
    e.target instanceof HTMLSelectElement
  )
    return;
  const c = { x: W / 2, y: H / 2 };
  if (e.key === "+" || e.key === "=") zoomAt(c, t.zoom * 1.25);
  else if (e.key === "-") zoomAt(c, t.zoom / 1.25);
  else if (e.key === "0") zoomAt(c, 1);
  else if (e.key === "f") fitView();
  else if (e.key === "Escape") closePanel();
  else return;
  e.preventDefault();
});

// ── the settings ───────────────────────────────────────────────────────────

const panel = document.getElementById("panel");
const gear = document.getElementById("gear");
const fileInput = document.getElementById("file");
function openPanel() {
  panel.classList.add("open");
  gear.setAttribute("aria-expanded", "true");
  syncPanel();
}
function closePanel() {
  panel.classList.remove("open");
  gear.setAttribute("aria-expanded", "false");
}
gear.addEventListener("click", () =>
  panel.classList.contains("open") ? closePanel() : openPanel(),
);
document.getElementById("close").addEventListener("click", closePanel);

const el = (tag, attrs, ...children) => {
  const n = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs || {})) {
    if (k === "class") n.className = v;
    else if (k.startsWith("on")) n.addEventListener(k.slice(2), v);
    else if (v !== false && v != null) n.setAttribute(k, v === true ? "" : v);
  }
  for (const c of children) if (c != null) n.append(c);
  return n;
};
const radio = (name, value, label, checked, onchange) =>
  el(
    "label",
    { class: "row" },
    el("input", { type: "radio", name, value, checked, onchange }),
    el("span", null, label),
  );

function changed(what) {
  writeState();
  if (what === "image") {
    clearAll();
    initialView();
  } else if (what === "case") {
    dropOutputs();
    keepCenter(() => {});
  } else if (what === "layout") keepCenter(layout);
  else if (what === "api") dropOutputs();
  else if (what === "iters") {
    for (const rs of results.values()) {
      rs.time = null;
      rs.timeError = null;
    }
  } else if (what === "mwsr") {
    // a new key for MWSR: its output and its figures are made again, the
    // other bands keep theirs
  } else if (what === "redo") {
    for (const a of shown()) {
      const rs = res(outKey(a));
      rs.time = null;
      rs.timeError = null;
    }
  }
  layout();
  syncPanel();
  refresh();
}

function buildPanel() {
  const body = document.getElementById("body");
  body.replaceChildren();
  // the picture
  body.append(el("h3", null, "Image"));
  body.append(
    el(
      "div",
      { class: "row" },
      el("span", { id: "imgname", class: "grow" }, "…"),
      el(
        "button",
        { type: "button", onclick: () => fileInput.click() },
        "Choisir…",
      ),
    ),
  );
  body.append(
    el(
      "p",
      { class: "note" },
      "Référence : l'image à 1440p ; à 1080p et 720p, sa réduction bicubique (Catmull-Rom), qui est aussi l'entrée des agrandissements.",
    ),
  );
  // the case
  body.append(el("h3", null, "Cas"));
  for (const c of CASES)
    body.append(
      radio("case", c.id, c.label, state.caseId === c.id, () => {
        state.caseId = c.id;
        changed("case");
      }),
    );
  // the algorithms, in order
  body.append(el("h3", null, "Algorithmes, dans l'ordre des bandes"));
  body.append(el("div", { id: "algos" }));
  // the bands
  body.append(el("h3", null, "Bandes"));
  body.append(
    radio("bands", "v", "verticales", state.bands === "v", () => {
      state.bands = "v";
      changed("layout");
    }),
    radio("bands", "h", "horizontales", state.bands === "h", () => {
      state.bands = "h";
      changed("layout");
    }),
    radio(
      "zone",
      "same",
      "la même zone dans chaque bande",
      state.zone === "same",
      () => {
        state.zone = "same";
        changed("layout");
      },
    ),
    radio(
      "zone",
      "cont",
      "une image continue, coupée en bandes",
      state.zone === "cont",
      () => {
        state.zone = "cont";
        changed("layout");
      },
    ),
  );
  body.append(
    el(
      "p",
      { class: "note" },
      "Un doigt déplace, deux doigts zooment, un double appui passe à 1:1 ou ajuste l'image ; à la souris : glisser, molette, double clic ; touches + − 0 f.",
    ),
  );
  // the API
  body.append(el("h3", null, "API des shaders"));
  body.append(
    radio(
      "api",
      "auto",
      "auto — WebGL2 ; WebGPU sur iPhone et iPad",
      state.api === "auto",
      () => {
        state.api = "auto";
        changed("api");
      },
    ),
    radio("api", "webgpu", "WebGPU", state.api === "webgpu", () => {
      state.api = "webgpu";
      changed("api");
    }),
    radio("api", "webgl", "WebGL2", state.api === "webgl", () => {
      state.api = "webgl";
      changed("api");
    }),
  );
  // the time
  body.append(el("h3", null, "Temps"));
  const sel = el("select", {
    onchange: () => {
      state.iters = parseInt(sel.value, 10);
      changed("iters");
    },
  });
  for (const n of [100, 300, 1000, 3000])
    sel.append(
      el("option", { value: n, selected: state.iters === n }, String(n)),
    );
  if (![100, 300, 1000, 3000].includes(state.iters))
    sel.append(
      el("option", { value: state.iters, selected: true }, String(state.iters)),
    );
  body.append(
    el(
      "div",
      { class: "row" },
      sel,
      el("span", { class: "grow" }, "itérations"),
      el(
        "button",
        { type: "button", onclick: () => changed("redo") },
        "Refaire la mesure",
      ),
    ),
  );
  body.append(
    el(
      "p",
      { class: "note" },
      "Moyenne du temps d'une image, de l'appel du dessin à la fin du travail du GPU (le « done » du labo), après 20 images de chauffe ; en pause pendant les gestes.",
    ),
  );
  // MWSR's two per-draw values
  body.append(el("h3", null, "Réglage MWSR"));
  const slider = (id, label, range, key, dec) => {
    const [min, max, step] = range;
    const cur = () => (key === "kg" ? kgNow() : state[key]);
    const val = el("span", { id: id + "v", class: "val" }, fr(cur(), dec));
    const input = el("input", {
      id,
      type: "range",
      min,
      max,
      step,
      value: cur(),
      oninput: () => {
        val.textContent = fr(parseFloat(input.value), dec);
      },
      onchange: () => {
        state[key] = parseFloat(input.value);
        changed("mwsr");
      },
    });
    body.append(
      el("div", { class: "row" }, el("span", null, label), input, val),
    );
  };
  slider("kg", "netteté", KG_RANGE, "kg", 2);
  // back to the product's law (MwsrShader's mwsrLaw): the slider follows
  // the case's sizes again
  body.append(
    el(
      "div",
      { class: "row" },
      el(
        "button",
        {
          type: "button",
          onclick: () => {
            state.kg = null;
            changed("mwsr");
          },
        },
        "Netteté par la loi d'échelle (défaut du produit)",
      ),
    ),
  );
  slider("ft", "seuil τ", FT_RANGE, "ft", 4);
  body.append(
    el(
      "div",
      { class: "row" },
      el(
        "button",
        {
          type: "button",
          onclick: () => {
            state.kg = null;
            state.ft = MWSR_DEF.ft;
            state.gaze = false;
            state.gz = { ...ZONE_DEF };
            changed("mwsr");
          },
        },
        "Défauts du produit (netteté par l'échelle · " + fr(MWSR_DEF.ft, 3) + ")",
      ),
    ),
  );
  body.append(
    el(
      "p",
      { class: "note" },
      "Netteté : ce que le noyau ajoute du détail mesuré entre le cubique et les deux lectures prises en travers du contour. Seuil τ : la pente de luma par texel d'entrée sous laquelle un quad de 2×2 pixels garde sa lecture unique.",
    ),
  );
  // the gaze zone: on or off, its six values, the rings
  body.append(el("h3", null, "Zone du regard"));
  const gazeSel = el("select", {
    id: "gaze",
    onchange: () => {
      state.gaze = gazeSel.value === "1";
      changed("mwsr");
    },
  });
  gazeSel.append(
    el("option", { value: "0", selected: !state.gaze }, "non (image fixe)"),
    el(
      "option",
      { value: "1", selected: state.gaze },
      "oui (comme sur une image en mouvement)",
    ),
  );
  body.append(el("div", { class: "row" }, el("span", null, "zone"), gazeSel));
  for (const [id, label, range, key, dec] of ZONE_SLIDERS) {
    const [min, max, step] = range;
    const val = el(
      "span",
      { id: id + "v", class: "val" },
      fr(state.gz[key], dec),
    );
    const input = el("input", {
      id,
      type: "range",
      min,
      max,
      step,
      value: state.gz[key],
      oninput: () => {
        val.textContent = fr(parseFloat(input.value), dec);
      },
      onchange: () => {
        state.gz[key] = parseFloat(input.value);
        changed("mwsr");
      },
    });
    body.append(
      el("div", { class: "row" }, el("span", null, label), input, val),
    );
  }
  const check = (id, label, key, onchange) =>
    el(
      "label",
      { class: "row" },
      el("input", {
        type: "checkbox",
        id,
        checked: state[key],
        onchange: (e) => {
          state[key] = e.target.checked;
          onchange();
        },
      }),
      el("span", null, label),
    );
  body.append(
    check("rings", "anneaux dessinés sur les bandes", "rings", () => {
      writeState();
      requestDraw();
    }),
  );
  body.append(
    el(
      "p",
      { class: "note" },
      "Zone du regard (plan §15.2) : dans l'ellipse verte, le seuil τ et la netteté glissent vers les valeurs « centre » ; sur l'anneau orange, retour progressif (smoothstep) aux valeurs de la table ; dehors, la table. Ici le poids de la zone est tenu à 1 ; dans le produit il suit le mouvement de l'image (taille des images encodées), et reste à 0 sur une image fixe ou un client de bureau.",
    ),
  );
  // the results
  body.append(el("h3", { id: "restitle" }, "Résultats"));
  body.append(el("div", { id: "table" }));
  // the device
  body.append(el("h3", null, "Appareil"));
  body.append(el("div", { id: "info", class: "note" }));
  // the link
  body.append(el("h3", null, "Lien de cet état"));
  const link = el("input", { id: "link", type: "text", readonly: true });
  body.append(
    el(
      "div",
      { class: "row" },
      link,
      el(
        "button",
        {
          type: "button",
          onclick: async () => {
            try {
              await navigator.clipboard.writeText(location.href);
            } catch (e) {
              link.select();
            }
          },
        },
        "Copier",
      ),
    ),
  );
  syncPanel();
}

function syncPanel() {
  const name = document.getElementById("imgname");
  if (name)
    name.textContent = source.name
      ? source.name +
        " · " +
        source.w +
        "×" +
        source.h +
        (source.fitted ? " (ajustée à 2560×1440)" : "")
      : "aucune image";
  const algos = document.getElementById("algos");
  if (algos) {
    algos.replaceChildren();
    state.order.forEach((id, i) => {
      const def = ALGO_BY[id];
      algos.append(
        el(
          "div",
          { class: "row algo" },
          el(
            "label",
            { class: "grow" },
            el("input", {
              type: "checkbox",
              checked: state.on.has(id),
              onchange: (e) => {
                if (e.target.checked) state.on.add(id);
                else state.on.delete(id);
                changed("layout");
              },
            }),
            el(
              "span",
              null,
              el("b", null, def.label),
              def.note ? el("small", null, " — " + def.note) : null,
            ),
          ),
          el(
            "button",
            {
              type: "button",
              title: "monter",
              disabled: i === 0,
              onclick: () => {
                state.order.splice(
                  i - 1,
                  2,
                  state.order[i],
                  state.order[i - 1],
                );
                changed("layout");
              },
            },
            "▲",
          ),
          el(
            "button",
            {
              type: "button",
              title: "descendre",
              disabled: i === state.order.length - 1,
              onclick: () => {
                state.order.splice(i, 2, state.order[i + 1], state.order[i]);
                changed("layout");
              },
            },
            "▼",
          ),
        ),
      );
    });
  }
  for (const input of panel.querySelectorAll("input[type=radio]")) {
    const v = {
      case: state.caseId,
      bands: state.bands,
      zone: state.zone,
      api: state.api,
    }[input.name];
    if (v !== undefined) input.checked = input.value === v;
  }
  for (const [id, key, dec] of [
    ["kg", "kg", 2],
    ["ft", "ft", 4],
  ]) {
    const input = document.getElementById(id);
    const val = document.getElementById(id + "v");
    const v = key === "kg" ? kgNow() : state[key];
    if (input) input.value = v;
    if (val) val.textContent = fr(v, dec);
  }
  const gazeSel = document.getElementById("gaze");
  if (gazeSel) gazeSel.value = state.gaze ? "1" : "0";
  for (const [id, , , key, dec] of ZONE_SLIDERS) {
    const input = document.getElementById(id);
    const val = document.getElementById(id + "v");
    if (input) input.value = state.gz[key];
    if (val) val.textContent = fr(state.gz[key], dec);
  }
  for (const key of ["rings"]) {
    const box = document.getElementById(key);
    if (box) box.checked = state[key];
  }
  const link = document.getElementById("link");
  if (link) link.value = location.href;
  renderTable();
  renderInfo();
}

function renderTable() {
  const box = document.getElementById("table");
  const title = document.getElementById("restitle");
  if (!box) return;
  title.textContent = "Résultats — " + currentCase().label;
  const table = el("table");
  table.append(
    el(
      "tr",
      null,
      el("th", null, "algo"),
      el("th", null, "API"),
      el("th", null, "SSIM"),
      el("th", null, "PSNR"),
      el("th", null, "ms / image"),
    ),
  );
  for (const a of shown()) {
    const key = outKey(a);
    const rs = results.get(key) || {};
    const out = outputs.get(key);
    const m = rs.metrics;
    table.append(
      el(
        "tr",
        { title: rs.path || "" },
        el(
          "td",
          null,
          ALGO_BY[a].label +
            (a === "mw" && tuned() ? " (" + tuningText() + ")" : ""),
        ),
        el("td", null, API_LABEL[apiFor(a)]),
        el("td", null, out && out.error ? "—" : m ? fr(m.ssim, 4) : "…"),
        el(
          "td",
          null,
          out && out.error
            ? "—"
            : m
              ? m.psnr === Infinity
                ? "∞"
                : fr(m.psnr, 2)
              : "…",
        ),
        el(
          "td",
          null,
          rs.time
            ? fr(rs.time.mean, 3) + " (p50 " + fr(rs.time.p50, 3) + ")"
            : rs.timeError || (out && out.error)
              ? "—"
              : rs.timing
                ? rs.timing.done + "/" + rs.timing.n
                : "…",
        ),
      ),
    );
  }
  box.replaceChildren(table);
}

function renderInfo() {
  const box = document.getElementById("info");
  if (!box) return;
  const lines = [
    UA.replace(/^Mozilla\/5\.0 /, "").slice(0, 160),
    "DPR " +
      fr(dpr, 2) +
      " · fenêtre " +
      view.clientWidth +
      "×" +
      view.clientHeight +
      " css, " +
      W +
      "×" +
      H +
      " px",
    "WebGPU : " +
      (navigator.gpu
        ? engines.webgpu
          ? engines.webgpu.error || engines.webgpu.gpu
          : "présent" + (state.soft ? " (adaptateur logiciel demandé)" : "")
        : "absent"),
    "WebGL2 : " +
      (engines.webgl ? engines.webgl.error || engines.webgl.gpu : "—"),
    "afficheur : " + (gl ? "WebGL2" : "Canvas2D"),
  ];
  const paths = shown()
    .map((a) => {
      const rs = results.get(outKey(a));
      return rs && rs.path ? ALGO_BY[a].label + " : " + rs.path : null;
    })
    .filter(Boolean);
  if (paths.length) lines.push("voies : " + paths.join(" · "));
  box.replaceChildren(...lines.map((s) => el("div", null, s)));
}

fileInput.addEventListener("change", async () => {
  const f = fileInput.files && fileInput.files[0];
  if (!f) return;
  try {
    await loadSource({ file: f });
    state.img = "";
    changed("image");
  } catch (e) {
    status("Image refusée : " + (e.message || e));
  }
});

// ── boot ───────────────────────────────────────────────────────────────────

async function loadDefaultSource() {
  const urls = state.img ? [state.img] : DEFAULT_IMAGES;
  let last = null;
  for (const u of urls) {
    try {
      await loadSource({ url: u });
      return;
    } catch (e) {
      last = e;
    }
  }
  throw last || new Error("aucune image");
}

async function boot() {
  try {
    // the product's defaults of MWSR's two values, so that the sliders
    // start from them and the "defaults" button means the product's
    const m = await import(MWSR_URL);
    mwsrModule = m;
    Object.assign(MWSR_DEF, m.mwsrLaw(1280, 720, 2560, 1440));
    if (m.MWSR_ZONE) Object.assign(ZONE_DEF, m.MWSR_ZONE);
    state.kg = null;
    state.ft = MWSR_DEF.ft;
    state.gz = { ...ZONE_DEF };
  } catch (e) {
    console.warn("MwsrShader.js :", e);
  }
  readState();
  writeState();
  if (state.soft && navigator.gpu) {
    // the product's renderer asks for the default adapter: the software one here
    const orig = GPU.prototype.requestAdapter;
    GPU.prototype.requestAdapter = function (opts) {
      return orig.call(this, { ...(opts || {}), forceFallbackAdapter: true });
    };
  }
  buildPanel();
  layout();
  window.addEventListener("resize", () => keepCenter(layout));
  try {
    await loadDefaultSource();
  } catch (e) {
    status(
      "Image introuvable (" + (e.message || e) + ") : choisissez-en une par ⚙.",
    );
    syncPanel();
    return;
  }
  syncPanel();
  initialView();
  refresh();
}

// For a driver (the lab's tests): the state, the figures, and when it rests.
window.mwCompare = {
  state,
  view: t,
  results,
  outputs,
  refresh,
  get idle() {
    return !running;
  },
  get viewer() {
    return gl ? "webgl2" : "canvas2d";
  },
  whenIdle: () =>
    running ? new Promise((r) => idleResolvers.push(r)) : Promise.resolve(),
  table: () =>
    shown().map((a) => {
      const key = outKey(a);
      const rs = results.get(key) || {};
      const out = outputs.get(key);
      return {
        algo: a,
        api: apiFor(a),
        error: (out && out.error) || null,
        path: rs.path || null,
        ssim: rs.metrics ? rs.metrics.ssim : null,
        psnr: rs.metrics ? rs.metrics.psnr : null,
        black: rs.metrics ? rs.metrics.black : null,
        metricsError: rs.metricsError || null,
        ms: rs.time ? rs.time.mean : null,
        p50: rs.time ? rs.time.p50 : null,
        timeError: rs.timeError || null,
      };
    }),
};

boot();
