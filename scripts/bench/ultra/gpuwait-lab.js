// Where a PyroWave frame waits on the page's GPU, away from any stream (POC
// Ultra U3.7, the "attente" plan's B1). The same submissions as UltraPlayer,
// timed the same way, so that scripts/bench/clickpath/gpuwait.py reads the
// result as it reads a pass's <tag>.ultratrace.json.
//
// Driven by gpuwait-lab.html (main thread) or as a module worker (thread=
// worker: the page posts the parameters, the worker posts the result back).
//
//   mode   empty  an empty command buffer, submit → work done
//          stamp  one empty timestamped pass (UltraPlayer's reference)
//          decode a real frame: dequantization, inverse transform and the
//                 present pass into a WebGPU OffscreenCanvas, then a
//                 VideoFrame made from it (UltraPlayer._run), with an empty
//                 reference after one frame in `ref` when nothing waits
//   pace   back   the next one as soon as the last is done
//          tick   one every `period` ms, as frames come off the network; a
//                 tick that finds the GPU busy waits for it
//          raf    one per animation frame (a shown window only)
//   wait   done   onSubmittedWorkDone(), as UltraPlayer does
//          map    mapAsync() of a buffer the submission writes
//   n      how many (references not counted)
//   draw   1: each VideoFrame is drawn on a shown Canvas2D (main thread)
//   clip, mbps, enc  the corpus stream decoded (game10-1080p, 170, 0x10de)
//   nudge  while a submission waits, a cheap command now and then, so that
//          Chrome's GPU process looks at its fences again: submit (an empty
//          queue.submit), write (4 bytes by queue.writeBuffer), done (one
//          more onSubmittedWorkDone); from nudgeAfter ms after the submit,
//          every nudgeEvery ms (0.25), on a MessageChannel loop; nudgeTimer=1:
//          by timeouts instead (Chrome's ~1 ms steps, no spinning), each set
//          from a message so that nested timeouts are never held to 4 ms;
//          player: UltraPlayer's own GpuNudge (B2.0), nudgeMode auto or all
import { GpuNudge } from "/frontend/js/stream/ultra/GpuNudge.js";
import { PyroWaveDecoder } from "/frontend/js/stream/ultra/PyroWaveDecoder.js";

const RESOLVE_STRIDE = 256;

export async function bytes(url) {
  const r = await fetch(url);
  if (!r.ok) throw new Error(`${url}: ${r.status}`);
  return new Uint8Array(await r.arrayBuffer());
}

// A corpus stream's frames, each the bytes of its packets back to back (what
// the audio road hands UltraPlayer).
export function pwvFrames(pwv, max) {
  const dv = new DataView(pwv.buffer, pwv.byteOffset);
  const w = dv.getUint32(4, true);
  const h = dv.getUint32(8, true);
  const count = Math.min(dv.getUint32(20, true), max);
  const frames = [];
  let pos = 24;
  for (let f = 0; f < count; f++) {
    const n = dv.getUint32(pos, true);
    pos += 4;
    const parts = [];
    let size = 0;
    for (let k = 0; k < n; k++) {
      const len = dv.getUint32(pos, true);
      parts.push(pwv.subarray(pos + 4, pos + 4 + len));
      size += len;
      pos += 4 + len;
    }
    const all = new Uint8Array(size);
    let at = 0;
    for (const p of parts) {
      all.set(p, at);
      at += p.length;
    }
    frames.push(all);
  }
  return { w, h, frames };
}

export async function runLab(p, log = () => {}, drawCanvas = null) {
  const mode = p.mode || "stamp";
  const pace = p.pace || "back";
  const wait = p.wait || "done";
  const n = Number(p.n || 1000);
  const period = Number(p.period || 1000 / 120);
  const refEvery = Number(p.ref ?? 8);
  const adapter = await navigator.gpu.requestAdapter({
    powerPreference: p.power || "high-performance",
  });
  if (!adapter) throw new Error("no WebGPU adapter");
  const info = adapter.info || {};
  const hasTs = adapter.features.has("timestamp-query");
  const device = await adapter.requestDevice({
    requiredFeatures: hasTs ? ["timestamp-query"] : [],
    requiredLimits: {
      maxStorageBufferBindingSize: adapter.limits.maxStorageBufferBindingSize,
      maxBufferSize: adapter.limits.maxBufferSize,
    },
  });
  const errors = [];
  device.onuncapturederror = (e) => errors.push(e.error.message);
  const adapterName =
    `${info.vendor} ${info.architecture} ${info.description || ""}`.trim();
  log(`adapter: ${adapterName} ts=${hasTs}`);

  // GPU timestamps: as UltraPlayer when traced, six read-backs in flight.
  const reads = [];
  let querySet = null;
  let queryBuf = null;
  if (hasTs) {
    querySet = device.createQuerySet({ type: "timestamp", count: 4 });
    queryBuf = device.createBuffer({
      size: RESOLVE_STRIDE * 6,
      usage: GPUBufferUsage.QUERY_RESOLVE | GPUBufferUsage.COPY_SRC,
    });
    for (let i = 0; i < 6; i++)
      reads.push({
        buf: device.createBuffer({
          size: 32,
          usage: GPUBufferUsage.MAP_READ | GPUBufferUsage.COPY_DST,
        }),
        slot: i,
        busy: false,
      });
  }
  const freeRead = () => reads.find((r) => !r.busy) || null;
  const resolveInto = (enc, read, count) => {
    const off = read.slot * RESOLVE_STRIDE;
    enc.resolveQuerySet(querySet, 0, count, queryBuf, off);
    enc.copyBufferToBuffer(queryBuf, off, read.buf, 0, count * 8);
    read.busy = true;
  };
  // The GPU's timestamps into rec.gpu; resolves when mapped (wait=map waits on it).
  const readGpu = (read, count, rec) =>
    read.buf.mapAsync(GPUMapMode.READ, 0, count * 8).then(
      () => {
        rec.gpu = Array.from(
          new BigUint64Array(read.buf.getMappedRange(0, count * 8)),
          Number,
        );
        read.buf.unmap();
        read.busy = false;
      },
      () => {
        read.busy = false;
      },
    );
  // wait=map without timestamps (mode empty): a word copied, then mapped.
  const word = device.createBuffer({ size: 4, usage: GPUBufferUsage.COPY_SRC });
  const wordRead = device.createBuffer({
    size: 4,
    usage: GPUBufferUsage.MAP_READ | GPUBufferUsage.COPY_DST,
  });

  let dec = null;
  let canvas = null;
  let ctx = null;
  let frames = null;
  if (mode === "decode") {
    const clip = p.clip || "game10-1080p";
    const pv = pwvFrames(
      await bytes(
        `/corpus/${clip}-${p.mbps || "170"}-${p.enc || "0x10de"}.pwv`,
      ),
      60,
    );
    frames = pv.frames;
    dec = new PyroWaveDecoder(device, pv.w, pv.h);
    canvas = new OffscreenCanvas(pv.w, pv.h);
    ctx = canvas.getContext("webgpu");
    ctx.configure({ device, format: "rgba8unorm", alphaMode: "opaque" });
    log(
      `decode: ${clip}, ${pv.w}x${pv.h}, ${frames.length} frames of ~${Math.round(frames[0].length / 1024)} KiB`,
    );
  }
  const draw2d = drawCanvas
    ? drawCanvas.getContext("2d", { alpha: false, desynchronized: true })
    : null;

  // The nudges: a loop of messages (no timer clamp) until the wait ends.
  const nudgeKind = p.nudge || "";
  const nudgeAfter = Number(p.nudgeAfter || 0);
  const nudgeEvery = Number(p.nudgeEvery || 0.25);
  const nudgeBuf =
    nudgeKind === "write"
      ? device.createBuffer({ size: 4, usage: GPUBufferUsage.COPY_DST })
      : null;
  const nudgeWord = new Uint32Array(1);
  const nudgeTimer = p.nudgeTimer === "1";
  let nudges = 0;
  let spins = 0;
  const channel = new MessageChannel();
  let nudging = null;
  const nudgeNow = () => {
    if (nudgeKind === "submit") device.queue.submit([]);
    else if (nudgeKind === "write")
      device.queue.writeBuffer(nudgeBuf, 0, nudgeWord);
    else if (nudgeKind === "done") device.queue.onSubmittedWorkDone();
    nudges++;
  };
  channel.port1.onmessage = () => {
    const s = nudging;
    if (!s || s.over) return;
    spins++;
    if (nudgeTimer) {
      // From a message: the timeout's nesting level starts again at 1.
      setTimeout(() => {
        if (s.over) return;
        nudgeNow();
        channel.port2.postMessage(0);
      }, nudgeEvery);
      return;
    }
    const now = performance.now();
    if (now >= s.next) {
      nudgeNow();
      s.next = now + nudgeEvery;
    }
    channel.port2.postMessage(0);
  };
  const player =
    nudgeKind === "player"
      ? new GpuNudge(device, p.nudgeMode === "all" ? "all" : "auto")
      : null;
  const nudgeUntil = (t1, done, rec) => {
    if (player) {
      player.begin(t1);
      done.then(() => {
        const w = player.end(performance.now());
        if (w) {
          rec.nudge = [w.from, w.first, w.nudges];
          rec.spin = w.spin;
          nudges += w.nudges;
        }
      });
      return;
    }
    if (!nudgeKind) return;
    const s = { next: t1 + nudgeAfter, over: false };
    nudging = s;
    done.then(() => {
      s.over = true;
    });
    if (nudgeTimer)
      setTimeout(
        () => {
          if (s.over) return;
          nudgeNow();
          channel.port2.postMessage(0);
        },
        Math.max(0, nudgeAfter),
      );
    else channel.port2.postMessage(0);
  };

  const trace = [];
  let drawnMs = [];

  // One submission, its record, and a promise for its end.
  function submitOne(at, fi) {
    const t0 = performance.now();
    const enc = device.createCommandEncoder();
    let read = null;
    let rec;
    if (mode === "empty") {
      if (wait === "map") enc.copyBufferToBuffer(word, 0, wordRead, 0, 4);
      rec = { ref: true, t1: 0, t2: null, gpu: null };
    } else if (mode === "stamp") {
      read = freeRead();
      if (read) {
        enc
          .beginComputePass({
            timestampWrites: {
              querySet,
              beginningOfPassWriteIndex: 0,
              endOfPassWriteIndex: 1,
            },
          })
          .end();
        resolveInto(enc, read, 2);
      }
      rec = { ref: true, t1: 0, t2: null, gpu: null };
    } else {
      dec.clear();
      if (!dec.pushPacket(frames[fi % frames.length]))
        throw new Error(`frame ${fi}: malformed`);
      read = freeRead();
      const tw = (a, b) =>
        read
          ? { querySet, beginningOfPassWriteIndex: a, endOfPassWriteIndex: b }
          : undefined;
      dec.decode(enc, tw(0, 1), ["dequant", "idwt"]);
      dec.present(enc, ctx, true, tw(2, 3));
      if (read) resolveInto(enc, read, 4);
      rec = {
        host: at,
        ts: fi,
        at,
        t0,
        t1: 0,
        t2: null,
        t3: null,
        sliced: false,
        gpu: null,
      };
    }
    device.queue.submit([enc.finish()]);
    const t1 = performance.now();
    rec.t1 = t1;
    trace.push(rec);
    const mapped = read ? readGpu(read, mode === "decode" ? 4 : 2, rec) : null;
    let done;
    if (wait === "map") {
      if (mapped) done = mapped;
      else if (mode === "empty")
        done = wordRead.mapAsync(GPUMapMode.READ).then(() => wordRead.unmap());
      else done = device.queue.onSubmittedWorkDone();
    } else done = device.queue.onSubmittedWorkDone();
    nudgeUntil(t1, done, rec);
    return done.then(() => {
      rec.t2 = performance.now();
      if (mode === "decode") {
        const vf = new VideoFrame(canvas, { timestamp: fi });
        rec.t3 = performance.now();
        if (draw2d) {
          draw2d.drawImage(vf, 0, 0, drawCanvas.width, drawCanvas.height);
          drawnMs.push(performance.now() - rec.t3);
        }
        vf.close();
      }
      return rec;
    });
  }

  // UltraPlayer's empty reference: after a frame, when nothing waits.
  function reference() {
    const rec = { ref: true, t1: 0, t2: null, gpu: null };
    const enc = device.createCommandEncoder();
    const read = freeRead();
    if (read) {
      enc
        .beginComputePass({
          timestampWrites: {
            querySet,
            beginningOfPassWriteIndex: 0,
            endOfPassWriteIndex: 1,
          },
        })
        .end();
      resolveInto(enc, read, 2);
    }
    device.queue.submit([enc.finish()]);
    rec.t1 = performance.now();
    trace.push(rec);
    if (read) readGpu(read, 2, rec);
    device.queue.onSubmittedWorkDone().then(() => {
      rec.t2 = performance.now();
    });
  }

  // Warm-up, untimed: shaders compiled, buffers placed.
  for (let i = 0; i < 20; i++) await submitOne(performance.now(), i);
  trace.length = 0;
  drawnMs = [];

  const start = performance.now();
  if (pace === "back") {
    for (let i = 0; i < n; i++) {
      await submitOne(performance.now(), i);
      if (mode === "decode" && refEvery > 0 && (i + 1) % refEvery === 0)
        reference();
    }
  } else if (pace === "tick" || pace === "raf") {
    await new Promise((resolve, reject) => {
      let i = 0;
      let busy = false;
      let pending = null; // a tick that came while the GPU was busy (the freshest)
      const run = (at) => {
        busy = true;
        submitOne(at, i).then(() => {
          busy = false;
          i++;
          if (
            mode === "decode" &&
            refEvery > 0 &&
            i % refEvery === 0 &&
            pending === null
          )
            reference();
          if (i >= n) return resolve();
          if (pending !== null) {
            const a = pending;
            pending = null;
            run(a);
          }
        }, reject);
      };
      const arrive = (at) => {
        if (i >= n) return;
        if (busy) pending = at;
        else run(at);
      };
      if (pace === "raf") {
        const frame = () => {
          if (i >= n) return;
          arrive(performance.now());
          requestAnimationFrame(frame);
        };
        requestAnimationFrame(frame);
      } else {
        let k = 0;
        const tick = () => {
          if (i >= n) return;
          arrive(performance.now());
          k++;
          setTimeout(tick, Math.max(0, start + k * period - performance.now()));
        };
        tick();
      }
    });
  } else throw new Error(`pace ${pace}?`);
  const elapsed = performance.now() - start;
  // The last references' callbacks and read-backs.
  await device.queue.onSubmittedWorkDone();
  await new Promise((r) => setTimeout(r, 50));

  const q = (xs, f) => {
    const s = xs
      .filter((x) => x !== null && !Number.isNaN(x))
      .sort((a, b) => a - b);
    return s.length
      ? Math.round(s[Math.min(s.length - 1, Math.floor(f * s.length))] * 1000) /
          1000
      : null;
  };
  const main = trace.filter(
    (r) => (mode === "decode" ? !r.ref : true) && r.t2 !== null,
  );
  const sd = main.map((r) => r.t2 - r.t1);
  const res = {
    adapter: adapterName,
    params: {
      mode,
      pace,
      wait,
      n,
      period,
      ref: refEvery,
      draw: !!draw2d,
      power: p.power || "high-performance",
      nudge: nudgeKind
        ? {
            kind: nudgeKind,
            after: nudgeAfter,
            every: nudgeEvery,
            timer: nudgeTimer,
            count: nudges,
            spins,
            mode: player ? player.mode : null,
            timers: player ? player.stats.timers : null,
          }
        : null,
    },
    isolated: !!globalThis.crossOriginIsolated,
    perSecond: Math.round((main.length / elapsed) * 10000) / 10,
    submitToDone: {
      p10: q(sd, 0.1),
      p50: q(sd, 0.5),
      p90: q(sd, 0.9),
      p99: q(sd, 0.99),
    },
    drawn: draw2d ? { p50: q(drawnMs, 0.5), p90: q(drawnMs, 0.9) } : null,
    errors,
    trace,
  };
  device.destroy();
  return res;
}

// As a module worker: the parameters in, the result out.
if (
  typeof WorkerGlobalScope !== "undefined" &&
  self instanceof WorkerGlobalScope
) {
  self.onmessage = (e) =>
    runLab(e.data).then(
      (r) => self.postMessage(r),
      (err) =>
        self.postMessage({ error: String(err && err.stack ? err.stack : err) }),
    );
}
