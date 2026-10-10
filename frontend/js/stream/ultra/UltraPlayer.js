/*
 * MoonlightWeb — browser-based Sunshine/GameStream client.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * UltraPlayer — the page's half of the POC Ultra stream (U4.4): a PyroWave
 * frame in (the bytes of all its packets, as the audio road delivers them),
 * a VideoFrame out, so that the rest of the page (pacing, Canvas2D presenter,
 * frame log, latency probe) treats it like a frame from WebCodecs.
 *
 * Decode and conversion to RGB run on WebGPU (PyroWaveDecoder.js) into an
 * OffscreenCanvas; the VideoFrame is made from that canvas, which stays on the
 * GPU. A browser with no WebGPU adapter (an Android TV box, older Safari)
 * runs the WebGL2 decoder instead (PyroWaveDecoderGL.js), the same way. One
 * frame in flight at a time: a frame that arrives while the GPU is still busy
 * with the previous one replaces any other waiting frame (the freshest wins),
 * never queues behind it.
 *
 * By slices (WebGPU, bench key mw_ultra_slices=1 on the audio road): the
 * pieces of a frame still on its way come to pushPart(), and the blocks there
 * are dequantized, and the coarse levels transformed, before the frame is
 * whole; its last piece leaves only the rest to do. A piece missing, or a
 * frame by slices waiting for the GPU, and the next frame comes whole.
 *
 * Nudged (WebGPU, bench key mw_ultra_nudge=1 or all): while a frame waits for
 * its work done, empty submits wake Chrome's GPU process up, which otherwise
 * sees the end 2 to 3 ms late (GpuNudge.js).
 *
 * Presented by the player (WebGPU, bench key mw_ultra_present, U3.7 B2.1):
 * the present pass draws on a canvas the page shows, and the page gets a
 * stand-in for its bookkeeping at submit: no VideoFrame, no Canvas2D draw,
 * and nothing waits for the work done but the next frames, PRESENT_AHEAD of
 * them at most on the GPU's queue (no nudge then).
 */

import { GpuNudge, nudgeMode } from './GpuNudge.js';
import { PyroWaveDecoder } from './PyroWaveDecoder.js';
import { PyroWaveDecoderGL, glDecoderSupported } from './PyroWaveDecoderGL.js';

// Per-frame times kept for the breakdown (globalThis.__mwUltraPlayer): the
// last minute or so at 60 fps, enough for a bench pass.
const KEEP = 4096;
// One frame in this many carries GPU timestamps (their read-back costs a map).
const GPU_EVERY = 8;
// Bench switch (localStorage mw_ultra_trace=1, plan "attente" B0): every frame's
// timeline is kept, GPU timestamps on every frame (this many read-backs in
// flight), and after one frame in REF_EVERY, when nothing waits, an empty
// reference pass: what submit → work done costs Chrome with no work at all.
const TRACE_KEEP = 16384;
const TRACE_READS = 6;
const REF_EVERY = 8;
// Traced, by slices: the passes of a frame's pieces carry GPU timestamps too,
// for this many pieces at most (a pair each, after the frame's own four).
const PART_STAMPS = 62;
// A query set resolves at a multiple of 256 bytes.
const RESOLVE_STRIDE = 256;
// Presented by the player: frames submitted and not yet done, at most.
const PRESENT_AHEAD = 2;

// Bench switch (localStorage mw_ultra_idwt=2 or 1): an older inverse wavelet
// shader of the decoder, which gives the same values more slowly, for an A/B.
function idwtVersion() {
    try {
        const v = globalThis.localStorage?.getItem('mw_ultra_idwt');
        return v === '1' ? 1 : v === '2' ? 2 : 3;
    } catch {
        return 3;
    }
}

// Bench switch (localStorage mw_ultra_fp16=0): the decoder keeps all its
// planes in f32 instead of the two finest levels in FP16, for an A/B.
function fp16Storage() {
    try {
        return globalThis.localStorage?.getItem('mw_ultra_fp16') !== '0';
    } catch {
        return true;
    }
}

/**
 * The bench key mw_ultra_present (B2.1), or null: how the player presents on
 * the page's canvas. dom: its WebGPU context; offscreen: an OffscreenCanvas
 * its control is transferred to (Chrome sends that one's frames to the
 * compositor itself, apart from the page's). Not a WebGPU context asked for
 * desynchronized, as the page's Canvas2D is: Chrome then shows a black canvas
 * (lab, 10/10/2026).
 */
export function presentMode() {
    try {
        const v = globalThis.localStorage?.getItem('mw_ultra_present');
        return v === 'dom' || v === 'offscreen' ? v : null;
    } catch {
        return null;
    }
}

function quantiles(xs) {
    if (!xs.length) return null;
    const s = Float64Array.from(xs).sort();
    const at = (p) => Math.round(s[Math.min(s.length - 1, Math.floor(p * s.length))] * 1000) / 1000;
    return { n: s.length, p50: at(0.5), p90: at(0.9), p99: at(0.99) };
}

/**
 * True when this browser may run the decoder: WebGPU, or WebGL2 for the
 * fallback. Whether a GPU answers is known only at init().
 */
export function ultraPlayerSupported() {
    return (
        typeof OffscreenCanvas === 'function' &&
        ((typeof navigator !== 'undefined' && !!navigator.gpu) ||
            typeof WebGL2RenderingContext === 'function')
    );
}

export class UltraPlayer {
    /**
     * @param {number} width picture width (even)
     * @param {number} height picture height (even)
     * @param {(frame: VideoFrame | object, meta: {timestamp: number, backendTs: number, decodeMs: number}) => void} onFrame
     *        receives each VideoFrame (to close) with its chunk timestamp and host stamp;
     *        presented by the player, a stand-in instead: `presented: true`, the
     *        frame's timestamp and size, close()
     * @param {{limited?: boolean, log?: function(string): void, present?: HTMLCanvasElement | null}} [options]
     *        present: the canvas to present on (bench key mw_ultra_present), WebGPU only
     */
    constructor(
        width,
        height,
        onFrame,
        { limited = true, log = console.log, present = null } = {},
    ) {
        this.width = width;
        this.height = height;
        this.onFrame = onFrame;
        this.limited = limited;
        this.log = log;
        this.device = null;
        this.decoder = null;
        this._busy = false;
        this._waiting = null;
        // Presented by the player (B2.1): the canvas and the way, until init()
        // has it configured (presenting); then the frames on the GPU's queue.
        this.presentMode = present ? presentMode() : null;
        this._presentEl = this.presentMode ? present : null;
        this.presenting = false;
        this._inFlight = 0;
        this.stats = { frames: 0, replaced: 0, incomplete: 0, errors: 0, sliced: 0, parts: 0 };
        // The frame coming by slices: its frame id, the bytes of it seen, the
        // tail of a block cut by the transport, and whether it is all in.
        this._slice = null;
        // Where a frame's time goes, ms: waiting for the GPU to finish the
        // previous one, parsing its packets, recording and submitting the
        // work, submit to work done, the VideoFrame made from the canvas, and
        // the GPU's own time for the decode and the present passes (by
        // slices: what was left at the last piece), and a slice's own time
        // on this thread.
        this.times = {
            part: [],
            wait: [],
            parse: [],
            record: [],
            done: [],
            frame: [],
            gpuDecode: [],
            gpuPresent: [],
            spin: [],
        };
        // GPU timestamp read-backs: one normally, TRACE_READS when tracing,
        // each its own slot of the resolve buffer.
        this._reads = [];
        this._slotBytes = RESOLVE_STRIDE;
        // Bench switch (localStorage mw_ultra_early=1): hand the frame over at submit.
        try {
            this.early = globalThis.localStorage?.getItem('mw_ultra_early') === '1';
        } catch {
            this.early = false;
        }
        // Bench switch (mw_ultra_nudge): the GpuNudge built at init(), WebGPU only.
        this.nudgeMode = nudgeMode();
        this._nudge = null;
        // The timeline, when traced: one record per frame decoded, all on
        // performance.now() but the GPU's (ns, its own clock, put on this one
        // by the bench from the bounds submit and work done give it).
        //   frame: {host, ts, at, t0, t1, t2, t3, sliced, gpu: [decode begin,
        //          end, present begin, end] | null, nudge?: [from, first, count],
        //          parts?: [[in, submitted, GPU begin?, GPU end?], ...]}
        //   reference: {ref: true, t1, t2, gpu: [begin, end] | null}
        // at: the frame in; t0: its decode starts; t1: submitted; t2: work
        // done; t3: the VideoFrame made; nudged, the nudges' start planned,
        // their loop's real start (0 if it never ran) and how many. By
        // slices, each piece submitted: when it came, when its work went,
        // and its pass on the GPU (when the frame's read-back had room).
        this.trace = null;
        try {
            if (globalThis.localStorage?.getItem('mw_ultra_trace') === '1') this.trace = [];
        } catch {
            this.trace = null;
        }
        this._refCount = 0;
        globalThis.__mwUltraPlayer = this;
    }

    _note(key, ms) {
        const a = this.times[key];
        if (a.length >= KEEP) a.shift();
        a.push(ms);
    }

    /** The breakdown, for the bench (pass.py) and the console. */
    summary() {
        const out = {
            api: this.api || null,
            idwt: this.decoder?.idwtVersion ?? null,
            fp16: this.decoder?.fp16 ?? null,
            stats: { ...this.stats },
            gpuTimestamps: !!this._querySet,
            early: this.early,
            present: this.presenting ? this.presentMode : null,
            nudge: this._nudge ? { mode: this._nudge.mode, ...this._nudge.stats } : null,
            traced: this.trace ? this.trace.length : null,
        };
        for (const [k, xs] of Object.entries(this.times)) out[k] = quantiles(xs);
        return out;
    }

    /**
     * Opens the GPU and builds the decoder: WebGPU when an adapter answers,
     * else WebGL2. False when neither can run it.
     */
    async init() {
        if (!ultraPlayerSupported()) return false;
        // Bench switch (localStorage mw_ultra_api=webgl2): the fallback even where WebGPU runs.
        let forceGL = false;
        try {
            forceGL = globalThis.localStorage?.getItem('mw_ultra_api') === 'webgl2';
        } catch {
            forceGL = false;
        }
        const adapter =
            navigator.gpu && !forceGL
                ? await navigator.gpu.requestAdapter({ powerPreference: 'high-performance' })
                : null;
        if (!adapter) return this._initGL();
        const timestamps = adapter.features.has('timestamp-query');
        this.device = await adapter.requestDevice({
            requiredFeatures: timestamps ? ['timestamp-query'] : [],
            requiredLimits: {
                maxStorageBufferBindingSize: adapter.limits.maxStorageBufferBindingSize,
                maxBufferSize: adapter.limits.maxBufferSize,
            },
        });
        this.device.lost.then((info) => this.log('[MW-ULTRA] GPU device lost: ' + info.message));
        this.decoder = new PyroWaveDecoder(this.device, this.width, this.height, {
            idwt: idwtVersion(),
            fp16: fp16Storage(),
        });
        if (this._presentEl) this._presentOnPage();
        if (!this.presenting) {
            this.canvas = new OffscreenCanvas(this.width, this.height);
            this.context = this.canvas.getContext('webgpu');
            this.context.configure({
                device: this.device,
                format: 'rgba8unorm',
                alphaMode: 'opaque',
            });
        }
        if (timestamps) {
            const reads = this.trace ? TRACE_READS : 1;
            const count = this.trace ? 4 + 2 * PART_STAMPS : 4;
            this._slotBytes = Math.ceil((count * 8) / RESOLVE_STRIDE) * RESOLVE_STRIDE;
            this._querySet = this.device.createQuerySet({ type: 'timestamp', count });
            this._queryBuf = this.device.createBuffer({
                size: this._slotBytes * reads,
                usage: GPUBufferUsage.QUERY_RESOLVE | GPUBufferUsage.COPY_SRC,
            });
            for (let i = 0; i < reads; i++) {
                const buf = this.device.createBuffer({
                    size: count * 8,
                    usage: GPUBufferUsage.MAP_READ | GPUBufferUsage.COPY_DST,
                });
                this._reads.push({ buf, slot: i, busy: false });
            }
        }
        // Presented by the player, nothing waits for the work done but the next frames.
        if (this.nudgeMode && !this.presenting && typeof MessageChannel === 'function')
            this._nudge = new GpuNudge(this.device, this.nudgeMode);
        this.api = 'webgpu';
        this.log(
            '[MW-ULTRA] PyroWave decoder ready, ' +
                this.width +
                'x' +
                this.height +
                ', inverse wavelet ' +
                this.decoder.idwtVersion +
                (this.decoder.fp16 ? ', fine levels in FP16' : ', all in f32') +
                (this._nudge ? ', nudged (' + this._nudge.mode + ')' : '') +
                (this.presenting ? ', presented on the page (' + this.presentMode + ')' : ''),
        );
        return true;
    }

    // B2.1: the page's canvas at the frame's size, then its context, in the
    // canvas's own format. On any failure, the VideoFrame road instead.
    _presentOnPage() {
        const el = this._presentEl;
        try {
            // A placeholder's size cannot change once its control is transferred.
            el.width = this.width;
            el.height = this.height;
            const target = this.presentMode === 'offscreen' ? el.transferControlToOffscreen() : el;
            const ctx = target.getContext('webgpu');
            if (!ctx) throw new Error('no WebGPU context on the canvas');
            ctx.configure({
                device: this.device,
                format: navigator.gpu.getPreferredCanvasFormat(),
                alphaMode: 'opaque',
            });
            this.context = ctx;
            this.presenting = true;
        } catch (e) {
            this.log('[MW-ULTRA] presenting on the page failed (' + e.message + '): by VideoFrame');
        }
    }

    _initGL() {
        if (typeof WebGL2RenderingContext !== 'function') return false;
        this.canvas = new OffscreenCanvas(this.width, this.height);
        const gl = this.canvas.getContext('webgl2', {
            alpha: false,
            antialias: false,
            depth: false,
            stencil: false,
            powerPreference: 'high-performance',
        });
        if (!glDecoderSupported(gl)) return false;
        try {
            this.decoder = new PyroWaveDecoderGL(gl, this.width, this.height);
        } catch (e) {
            this.log('[MW-ULTRA] WebGL2 decoder failed to build: ' + e.message);
            return false;
        }
        this.gl = gl;
        this.api = 'webgl2';
        this.log(
            '[MW-ULTRA] PyroWave decoder ready on WebGL2 (no WebGPU adapter), ' +
                this.width +
                'x' +
                this.height,
        );
        return true;
    }

    /**
     * A piece of a frame still on its way (WebGPU only): @param {number} fid
     * the host's frame id @param {number} off where the piece starts in the
     * frame @param {Uint8Array} bytes the piece. Its whole blocks go to the
     * GPU at once; push() brings the rest.
     */
    pushPart(fid, off, bytes) {
        const dec = this.decoder;
        if (!dec || !this.device) return;
        let s = this._slice;
        // A frame by slices still waits for the GPU: this one comes whole.
        if (s && s.whole) return;
        if (off === 0) {
            s = this._slice = {
                fid,
                off: 0,
                carry: null,
                whole: false,
                parts: this.trace ? [] : null,
            };
            dec.clear();
            dec.startSlices();
        } else if (!s || s.fid !== fid || s.off !== off) {
            // A piece missing (a chunk asked again, a frame given up on): whole, then.
            this._slice = null;
            return;
        }
        const t0 = performance.now();
        if (!this._feed(s, bytes)) {
            this.stats.errors++;
            return;
        }
        const enc = this.device.createCommandEncoder();
        // Traced: the piece's pass timestamped, in the next pair after the frame's four.
        const k = s.parts && this._querySet && s.parts.length < PART_STAMPS ? s.parts.length : -1;
        const tw =
            k < 0
                ? undefined
                : {
                      querySet: this._querySet,
                      beginningOfPassWriteIndex: 4 + 2 * k,
                      endOfPassWriteIndex: 5 + 2 * k,
                  };
        if (dec.decodeSlice(enc, false, tw)) {
            this.device.queue.submit([enc.finish()]);
            s.parts?.push([t0, performance.now()]);
        }
        this.stats.parts++;
        this._note('part', performance.now() - t0);
    }

    // The next bytes of the frame by slices, after the tail of a block cut
    // last time. False (the frame then comes whole) when malformed.
    _feed(s, bytes) {
        let data = bytes;
        if (s.carry) {
            data = new Uint8Array(s.carry.length + bytes.length);
            data.set(s.carry);
            data.set(bytes, s.carry.length);
        }
        const used = this.decoder.pushPiece(data);
        if (used < 0) {
            this._slice = null;
            return false;
        }
        s.carry = used < data.length ? data.slice(used) : null;
        s.off += bytes.length;
        return true;
    }

    /**
     * One frame from the host. @param {Uint8Array} bytes its packets, back to
     * back @param {number} timestamp the chunk timestamp the page tracks it by
     * @param {number} backendTs the host's capture stamp @param {number} [fid]
     * the host's frame id, which pairs the frame with its pieces
     */
    push(bytes, timestamp, backendTs, fid) {
        if (!this.decoder) return;
        const job = { bytes, timestamp, backendTs, at: performance.now() };
        const s = this._slice;
        if (s && !s.whole && fid !== undefined && s.fid === fid && s.off <= bytes.length) {
            // The rest of a frame by slices: only what it left is still to do.
            const tp = performance.now();
            const ok = this._feed(s, bytes.subarray(s.off));
            this._note('parse', performance.now() - tp);
            if (!ok || s.carry) {
                this._slice = null;
                this.stats.errors++;
                return;
            }
            s.whole = true;
            job.slice = s;
            job.bytes = null;
        }
        if (this._busy) {
            const old = this._waiting;
            if (old) {
                this.stats.replaced++;
                // A frame by slices replaced: its blocks are no longer the ones wanted.
                if (old.slice && this._slice === old.slice) this._slice = null;
            }
            this._waiting = job;
            return;
        }
        this._note('wait', 0);
        this._run(job);
    }

    _run(job) {
        const dec = this.decoder;
        if (job.slice) {
            this.stats.sliced++;
            this._slice = null;
        } else {
            // A whole frame: what a frame by slices had parsed goes.
            this._slice = null;
            dec.clear();
            const tp = performance.now();
            const parsed = dec.pushPacket(job.bytes);
            this._note('parse', performance.now() - tp);
            if (!parsed) {
                this.stats.errors++;
                return this._next();
            }
        }
        const { timestamp, backendTs } = job;
        // The whole frame came at once, so a frame short of blocks is one the
        // host sent so (packets lost before the road): decoded as it is.
        if (!dec.isReady(true)) {
            this.stats.incomplete++;
            return this._next();
        }
        this._busy = true;
        const t0 = performance.now();
        if (this.gl) return this._runGL(timestamp, backendTs, t0);
        const enc = this.device.createCommandEncoder();
        // Now and then (every frame, traced), GPU timestamps around both
        // passes, when the adapter has them and a read-back is free.
        const read = this.trace || this.stats.frames % GPU_EVERY === 0 ? this._freeRead() : null;
        const q = read ? this._querySet : null;
        const tw = (a, b) =>
            q ? { querySet: q, beginningOfPassWriteIndex: a, endOfPassWriteIndex: b } : undefined;
        // Dequantization and the inverse transform only: the present pass
        // reads the f32 planes, the 8-bit packing is the lab's. By slices,
        // what the last piece left of them.
        if (job.slice) dec.decodeSlice(enc, true, tw(0, 1));
        else dec.decode(enc, tw(0, 1), ['dequant', 'idwt']);
        dec.present(enc, this.context, this.limited, tw(2, 3));
        // Traced by slices, the pieces' pairs come back with the frame's four.
        const parts = job.slice?.parts || null;
        const count = 4 + 2 * (parts ? Math.min(parts.length, PART_STAMPS) : 0);
        if (read) this._resolveInto(enc, read, count);
        this.device.queue.submit([enc.finish()]);
        const t1 = performance.now();
        this._note('record', t1 - t0);
        const rec = this.trace
            ? {
                  host: backendTs,
                  ts: timestamp,
                  at: job.at,
                  t0,
                  t1,
                  t2: null,
                  t3: null,
                  sliced: !!job.slice,
                  gpu: null,
                  ...(parts ? { parts } : {}),
              }
            : null;
        if (rec) this._keep(rec);
        if (read) this._readGpuTimes(read, count, rec);
        if (this.presenting) return this._presented(timestamp, backendTs, t0, t1, rec);
        this._nudge?.begin(t1);
        const emit = (from) => {
            let frame = null;
            try {
                frame = new VideoFrame(this.canvas, { timestamp });
            } catch (e) {
                this.stats.errors++;
                this.log('[MW-ULTRA] VideoFrame from the canvas failed: ' + e.message);
            }
            const t3 = performance.now();
            this._note('frame', t3 - from);
            if (rec) rec.t3 = t3;
            if (frame) this.onFrame(frame, { timestamp, backendTs, decodeMs: t3 - t0 });
        };
        // Early: the frame goes to the page as soon as the work is submitted;
        // Chrome's own fences hold its draw until the GPU is done, without
        // the callback's round trip. The next decode still waits for this one.
        if (this.early) emit(t1);
        this.device.queue.onSubmittedWorkDone().then(
            () => {
                this._busy = false;
                this.stats.frames++;
                const t2 = performance.now();
                this._note('done', t2 - t1);
                if (rec) rec.t2 = t2;
                const nw = this._nudge?.end(t2);
                if (nw) {
                    this._note('spin', nw.spin);
                    if (rec) rec.nudge = [nw.from, nw.first, nw.nudges];
                }
                if (!this.early) emit(t2);
                // Traced: an empty reference now and then, on an idle GPU.
                if (this.trace && !this._waiting && ++this._refCount % REF_EVERY === 0)
                    this._reference();
                this._next();
            },
            () => {
                this._busy = false;
                this.stats.errors++;
                this._nudge?.end();
            },
        );
    }

    // B2.1: the frame is on the page's canvas at the end of this task. The
    // page's bookkeeping now, by a stand-in; the next frame may go at once,
    // unless PRESENT_AHEAD are still on the GPU's queue: the freshest then
    // waits for one of them to be done.
    _presented(timestamp, backendTs, t0, t1, rec) {
        this._inFlight++;
        this._busy = this._inFlight >= PRESENT_AHEAD;
        const t3 = performance.now();
        this._note('frame', t3 - t1);
        if (rec) rec.t3 = t3;
        const { width, height } = this;
        this.onFrame(
            {
                presented: true,
                timestamp,
                codedWidth: width,
                codedHeight: height,
                displayWidth: width,
                displayHeight: height,
                close() {},
            },
            { timestamp, backendTs, decodeMs: t3 - t0 },
        );
        this.device.queue.onSubmittedWorkDone().then(
            () => {
                this._inFlight--;
                this._busy = this._inFlight >= PRESENT_AHEAD;
                this.stats.frames++;
                const t2 = performance.now();
                this._note('done', t2 - t1);
                if (rec) rec.t2 = t2;
                // Traced: the empty reference only on a GPU with nothing else queued.
                if (
                    this.trace &&
                    !this._waiting &&
                    !this._inFlight &&
                    ++this._refCount % REF_EVERY === 0
                )
                    this._reference();
                this._next();
            },
            () => {
                this._inFlight--;
                this._busy = this._inFlight >= PRESENT_AHEAD;
                this.stats.errors++;
            },
        );
    }

    // WebGL2: the frame goes to the page as soon as the work is issued (the
    // VideoFrame is a snapshot of the canvas, Chrome orders it after the
    // draws); a fence, polled, holds the next decode until the GPU is done.
    _runGL(timestamp, backendTs, t0) {
        const gl = this.gl;
        this.decoder.decode();
        this.decoder.present(this.limited);
        const sync = gl.fenceSync(gl.SYNC_GPU_COMMANDS_COMPLETE, 0);
        gl.flush();
        const t1 = performance.now();
        this._note('record', t1 - t0);
        let frame = null;
        try {
            frame = new VideoFrame(this.canvas, { timestamp });
        } catch (e) {
            this.stats.errors++;
            this.log('[MW-ULTRA] VideoFrame from the canvas failed: ' + e.message);
        }
        const t2 = performance.now();
        this._note('frame', t2 - t1);
        if (frame) this.onFrame(frame, { timestamp, backendTs, decodeMs: t2 - t0 });
        const poll = () => {
            if (!this.gl) return;
            const s = gl.clientWaitSync(sync, 0, 0);
            if (s === gl.TIMEOUT_EXPIRED) {
                // A message, not setTimeout(0): nested timeouts are held to
                // 4 ms, which made the next decode wait (9.2 ms submit to
                // done against 4.3 on WebGPU, a tenth of the frames replaced).
                this._yield(poll);
                return;
            }
            gl.deleteSync(sync);
            this._busy = false;
            if (s === gl.WAIT_FAILED) this.stats.errors++;
            else this.stats.frames++;
            this._note('done', performance.now() - t1);
            this._next();
        };
        poll();
    }

    // Runs fn at the next task, without the clamp of nested timeouts.
    _yield(fn) {
        if (typeof MessageChannel !== 'function') return setTimeout(fn, 0);
        if (!this._channel) {
            this._channel = new MessageChannel();
            this._channel.port1.onmessage = () => {
                const f = this._yielded;
                this._yielded = null;
                f?.();
            };
        }
        this._yielded = fn;
        this._channel.port2.postMessage(0);
    }

    // A read-back not in use, or null: then this frame goes without timestamps.
    _freeRead() {
        if (!this._querySet) return null;
        return this._reads.find((r) => !r.busy) || null;
    }

    // The first @p count timestamps of the set, resolved into @p read's slot
    // and copied to it.
    _resolveInto(enc, read, count) {
        const off = read.slot * this._slotBytes;
        enc.resolveQuerySet(this._querySet, 0, count, this._queryBuf, off);
        enc.copyBufferToBuffer(this._queryBuf, off, read.buf, 0, count * 8);
        read.busy = true;
    }

    // The trace keeps its last TRACE_KEEP records.
    _keep(rec) {
        if (this.trace.length >= TRACE_KEEP) this.trace.shift();
        this.trace.push(rec);
    }

    // An empty pass, timestamped, then submit → work done: Chrome's own cost
    // of the round trip (Dawn's GPU process, the fence), with no work in it.
    _reference() {
        const rec = { ref: true, t1: 0, t2: null, gpu: null };
        const enc = this.device.createCommandEncoder();
        const read = this._freeRead();
        if (read) {
            enc.beginComputePass({
                timestampWrites: {
                    querySet: this._querySet,
                    beginningOfPassWriteIndex: 0,
                    endOfPassWriteIndex: 1,
                },
            }).end();
            this._resolveInto(enc, read, 2);
        }
        this.device.queue.submit([enc.finish()]);
        rec.t1 = performance.now();
        this._keep(rec);
        if (read) this._readGpuTimes(read, 2, rec);
        this.device.queue.onSubmittedWorkDone().then(
            () => {
                rec.t2 = performance.now();
            },
            () => {},
        );
    }

    _readGpuTimes(read, count, rec = null) {
        const buf = read.buf;
        buf.mapAsync(GPUMapMode.READ, 0, count * 8).then(
            () => {
                const t = new BigUint64Array(buf.getMappedRange(0, count * 8));
                if (count >= 4) {
                    // Nanoseconds; a pair out of order (a quantized or reset clock) is skipped.
                    if (t[1] > t[0]) this._note('gpuDecode', Number(t[1] - t[0]) / 1e6);
                    if (t[3] > t[2]) this._note('gpuPresent', Number(t[3] - t[2]) / 1e6);
                }
                if (rec) {
                    rec.gpu = Array.from(t.subarray(0, Math.min(count, 4)), Number);
                    // The pieces' pairs, after the frame's four.
                    for (let i = 0; 6 + 2 * i <= count; i++)
                        rec.parts[i].push(Number(t[4 + 2 * i]), Number(t[5 + 2 * i]));
                }
                buf.unmap();
                read.busy = false;
            },
            () => {
                read.busy = false;
            },
        );
    }

    _next() {
        const w = this._waiting;
        if (!w) return;
        this._waiting = null;
        this._note('wait', performance.now() - w.at);
        this._run(w);
    }

    destroy() {
        this._nudge?.destroy();
        this._nudge = null;
        this.decoder?.destroy();
        this.device?.destroy();
        this.gl?.getExtension('WEBGL_lose_context')?.loseContext();
        this._channel?.port1.close();
        this.decoder = null;
        this.device = null;
        this.gl = null;
    }
}
