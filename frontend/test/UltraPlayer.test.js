/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { afterEach, describe, expect, it, vi } from 'vitest';

// The GPU half is stubbed: what is tested is the player's flow.
vi.mock('../js/stream/ultra/PyroWaveDecoder.js', () => ({
    PyroWaveDecoder: class {
        constructor() {
            this.pushed = [];
            this.decoded = 0;
        }
        pushPacket(bytes) {
            this.pushed.push(bytes[0]);
            return bytes[0] !== 0xee;
        }
        // A piece: every byte is a whole "block", but a last 0xcc is one cut short.
        pushPiece(bytes) {
            this.pieces = (this.pieces || []).concat([...bytes]);
            return bytes[bytes.length - 1] === 0xcc ? bytes.length - 1 : bytes.length;
        }
        clear() {
            this.cleared = (this.cleared || 0) + 1;
        }
        startSlices() {
            this.slices = [];
        }
        decodeSlice(enc, last = false, timestampWrites) {
            this.slices.push(last ? 'last' : 'some');
            this.stamps = (this.stamps || []).concat([timestampWrites]);
            return true;
        }
        isReady() {
            return true;
        }
        decode() {
            this.decoded++;
        }
        present() {}
        destroy() {}
    },
}));

const { UltraPlayer } = await import('../js/stream/ultra/UltraPlayer.js');

// Handed over at the work done unless early (the player's default is early).
function player({ early = false } = {}) {
    const frames = [];
    const p = new UltraPlayer(64, 64, (frame, meta) => frames.push(meta.timestamp), {
        log: () => {},
    });
    p.early = early;
    let release = null;
    p.device = {
        createCommandEncoder: () => ({ finish: () => ({}) }),
        queue: {
            submit() {},
            onSubmittedWorkDone: () =>
                new Promise((resolve) => {
                    release = resolve;
                }),
        },
    };
    return { p, frames, done: () => release() };
}

afterEach(() => {
    vi.unstubAllGlobals();
});

describe('UltraPlayer', () => {
    it('keeps one frame in flight and lets the freshest waiting one win', async () => {
        vi.stubGlobal(
            'VideoFrame',
            class {
                constructor(src, { timestamp }) {
                    this.timestamp = timestamp;
                }
            },
        );
        const { PyroWaveDecoder } = await import('../js/stream/ultra/PyroWaveDecoder.js');
        const { p, frames, done } = player();
        p.decoder = new PyroWaveDecoder();
        p.canvas = {};
        p.context = {};
        p.push(new Uint8Array([1]), 1, 10);
        p.push(new Uint8Array([2]), 2, 11); // waits
        p.push(new Uint8Array([3]), 3, 12); // replaces frame 2
        expect(p.decoder.pushed).toEqual([1]);
        done();
        await Promise.resolve();
        await Promise.resolve();
        expect(frames).toEqual([1]);
        expect(p.decoder.pushed).toEqual([1, 3]);
        expect(p.stats.replaced).toBe(1);
        done();
        await Promise.resolve();
        await Promise.resolve();
        expect(frames).toEqual([1, 3]);
    });

    it('is early and nudged by default, each off by its key', () => {
        const keys = {};
        vi.stubGlobal('localStorage', { getItem: (k) => (k in keys ? keys[k] : null) });
        const make = () => new UltraPlayer(64, 64, () => {}, { log: () => {} });
        let p = make();
        expect(p.early).toBe(true);
        expect(p.nudgeMode).toBe('auto');
        for (const off of ['0', '']) {
            keys.mw_ultra_early = off;
            keys.mw_ultra_nudge = off;
            p = make();
            expect(p.early).toBe(false);
            expect(p.nudgeMode).toBe(null);
        }
        keys.mw_ultra_early = '1';
        keys.mw_ultra_nudge = 'all';
        p = make();
        expect(p.early).toBe(true);
        expect(p.nudgeMode).toBe('all');
    });

    it('early: the frame goes at submit, the next still waits for the work done', async () => {
        vi.stubGlobal(
            'VideoFrame',
            class {
                constructor(src, { timestamp }) {
                    this.timestamp = timestamp;
                }
            },
        );
        const { PyroWaveDecoder } = await import('../js/stream/ultra/PyroWaveDecoder.js');
        const { p, frames, done } = player({ early: true });
        p.decoder = new PyroWaveDecoder();
        p.canvas = {};
        p.context = {};
        p.push(new Uint8Array([1]), 1, 10);
        expect(frames).toEqual([1]);
        p.push(new Uint8Array([2]), 2, 11); // waits
        expect(p.decoder.pushed).toEqual([1]);
        expect(p.stats.frames).toBe(0);
        done();
        await Promise.resolve();
        await Promise.resolve();
        expect(p.stats.frames).toBe(1);
        expect(frames).toEqual([1, 2]);
        expect(p.decoder.pushed).toEqual([1, 2]);
    });

    it('drops a malformed frame without stalling the next', async () => {
        vi.stubGlobal(
            'VideoFrame',
            class {
                constructor(src, { timestamp }) {
                    this.timestamp = timestamp;
                }
            },
        );
        const { PyroWaveDecoder } = await import('../js/stream/ultra/PyroWaveDecoder.js');
        const { p, frames, done } = player();
        p.decoder = new PyroWaveDecoder();
        p.canvas = {};
        p.context = {};
        p.push(new Uint8Array([0xee]), 1, 10);
        expect(p.stats.errors).toBe(1);
        p.push(new Uint8Array([4]), 2, 11);
        done();
        await Promise.resolve();
        await Promise.resolve();
        expect(frames).toEqual([2]);
    });

    it('on WebGL2, hands each frame over at once and holds the next until the fence', async () => {
        vi.useFakeTimers();
        // The fence is polled at the next task: a message, or here a timeout.
        vi.stubGlobal('MessageChannel', undefined);
        try {
            vi.stubGlobal(
                'VideoFrame',
                class {
                    constructor(src, { timestamp }) {
                        this.timestamp = timestamp;
                    }
                },
            );
            const { PyroWaveDecoder } = await import('../js/stream/ultra/PyroWaveDecoder.js');
            const frames = [];
            const p = new UltraPlayer(64, 64, (frame, meta) => frames.push(meta.timestamp), {
                log: () => {},
            });
            let signaled = false;
            p.gl = {
                TIMEOUT_EXPIRED: 1,
                WAIT_FAILED: 2,
                SYNC_GPU_COMMANDS_COMPLETE: 3,
                fenceSync: () => ({}),
                flush() {},
                clientWaitSync: () => (signaled ? 0 : 1),
                deleteSync() {},
            };
            p.decoder = new PyroWaveDecoder();
            p.canvas = {};
            p.push(new Uint8Array([1]), 1, 10);
            expect(frames).toEqual([1]); // at once, not at the fence
            p.push(new Uint8Array([2]), 2, 11); // waits
            p.push(new Uint8Array([3]), 3, 12); // replaces frame 2
            vi.advanceTimersByTime(5);
            expect(p.decoder.pushed).toEqual([1]);
            signaled = true;
            vi.advanceTimersByTime(1);
            expect(p.decoder.pushed).toEqual([1, 3]);
            expect(frames).toEqual([1, 3]);
            expect(p.stats.replaced).toBe(1);
            expect(p.stats.frames).toBe(2);
        } finally {
            vi.useRealTimers();
        }
    });

    it('by slices: pieces go to the GPU as they come, the frame finishes the decode', async () => {
        vi.stubGlobal(
            'VideoFrame',
            class {
                constructor(src, { timestamp }) {
                    this.timestamp = timestamp;
                }
            },
        );
        const { PyroWaveDecoder } = await import('../js/stream/ultra/PyroWaveDecoder.js');
        const { p, frames, done } = player();
        let submits = 0;
        p.device.queue.submit = () => submits++;
        p.decoder = new PyroWaveDecoder();
        p.canvas = {};
        p.context = {};
        p.pushPart(7, 0, new Uint8Array([1, 2, 0xcc]));
        p.pushPart(7, 3, new Uint8Array([3, 4]));
        expect(submits).toBe(2);
        // The cut tail comes again in front of the next piece.
        expect(p.decoder.pieces).toEqual([1, 2, 0xcc, 0xcc, 3, 4]);
        p.push(new Uint8Array([1, 2, 0xcc, 3, 4, 5]), 1, 10, 7);
        expect(p.decoder.pieces.slice(-1)).toEqual([5]); // only the rest
        expect(p.decoder.slices).toEqual(['some', 'some', 'last']);
        expect(p.decoder.pushed).toEqual([]); // never parsed whole
        done();
        await Promise.resolve();
        await Promise.resolve();
        expect(frames).toEqual([1]);
        expect(p.stats.sliced).toBe(1);
    });

    it('traced: every frame keeps its timeline and GPU times, and an empty reference follows', async () => {
        vi.stubGlobal(
            'VideoFrame',
            class {
                constructor(src, { timestamp }) {
                    this.timestamp = timestamp;
                }
            },
        );
        vi.stubGlobal('GPUMapMode', { READ: 1 });
        const { PyroWaveDecoder } = await import('../js/stream/ultra/PyroWaveDecoder.js');
        const { p, frames, done } = player();
        p.trace = [];
        p.decoder = new PyroWaveDecoder();
        p.canvas = {};
        p.context = {};
        const passes = [];
        p.device.createCommandEncoder = () => ({
            finish: () => ({}),
            resolveQuerySet() {},
            copyBufferToBuffer() {},
            beginComputePass: (d) => {
                passes.push(d.timestampWrites);
                return { end() {} };
            },
        });
        p._querySet = {};
        p._queryBuf = {};
        // Each read-back answers [1000, 3000, 3500, 4000] ns, or [10, 20] for a reference.
        const read = (slot) => ({
            slot,
            busy: false,
            buf: {
                mapAsync: () => Promise.resolve(),
                getMappedRange: (off, size) =>
                    new BigUint64Array(size === 32 ? [1000n, 3000n, 3500n, 4000n] : [10n, 20n])
                        .buffer,
                unmap() {},
            },
        });
        p._reads = [read(0), read(1)];
        for (let i = 1; i <= 8; i++) {
            p.push(new Uint8Array([i]), i, 100 + i);
            done();
            for (let k = 0; k < 4; k++) await Promise.resolve();
        }
        expect(frames).toEqual([1, 2, 3, 4, 5, 6, 7, 8]);
        const kept = p.trace.filter((r) => !r.ref);
        expect(kept.map((r) => r.host)).toEqual([101, 102, 103, 104, 105, 106, 107, 108]);
        for (const r of kept) {
            expect(r.t1).toBeGreaterThanOrEqual(r.t0);
            expect(r.t2).toBeGreaterThanOrEqual(r.t1);
            expect(r.t3).toBeGreaterThanOrEqual(r.t2);
            expect(r.gpu).toEqual([1000, 3000, 3500, 4000]);
        }
        // The eighth frame's done ran an empty pass, timestamped.
        const refs = p.trace.filter((r) => r.ref);
        expect(refs.length).toBe(1);
        expect(passes.length).toBe(1);
        expect(passes[0].beginningOfPassWriteIndex).toBe(0);
        done();
        for (let k = 0; k < 4; k++) await Promise.resolve();
        expect(refs[0].gpu).toEqual([10, 20]);
        expect(refs[0].t2).toBeGreaterThanOrEqual(refs[0].t1);
        expect(p.summary().traced).toBe(9);
    });

    it("traced by slices: each piece's pass is timestamped, read back with the frame's", async () => {
        vi.stubGlobal(
            'VideoFrame',
            class {
                constructor(src, { timestamp }) {
                    this.timestamp = timestamp;
                }
            },
        );
        vi.stubGlobal('GPUMapMode', { READ: 1 });
        const { PyroWaveDecoder } = await import('../js/stream/ultra/PyroWaveDecoder.js');
        const { p, frames, done } = player();
        p.trace = [];
        p.decoder = new PyroWaveDecoder();
        p.canvas = {};
        p.context = {};
        let resolved = 0;
        p.device.createCommandEncoder = () => ({
            finish: () => ({}),
            resolveQuerySet: (set, first, count) => {
                resolved = count;
            },
            copyBufferToBuffer() {},
        });
        p._querySet = {};
        p._queryBuf = {};
        // The read-back answers 1000, 2000, ... ns, one per query.
        p._reads = [
            {
                slot: 0,
                busy: false,
                buf: {
                    mapAsync: () => Promise.resolve(),
                    getMappedRange: (off, size) =>
                        new BigUint64Array(size / 8).map((v, i) => BigInt(1000 * (i + 1))).buffer,
                    unmap() {},
                },
            },
        ];
        p.pushPart(7, 0, new Uint8Array([1, 2]));
        p.pushPart(7, 2, new Uint8Array([3]));
        p.push(new Uint8Array([1, 2, 3, 4]), 1, 10, 7);
        // The pieces in the pairs after the frame's four, the last piece in 0-1.
        expect(p.decoder.stamps.map((tw) => tw.beginningOfPassWriteIndex)).toEqual([4, 6, 0]);
        expect(resolved).toBe(8);
        done();
        for (let k = 0; k < 4; k++) await Promise.resolve();
        expect(frames).toEqual([1]);
        const [rec] = p.trace;
        expect(rec.sliced).toBe(true);
        expect(rec.gpu).toEqual([1000, 2000, 3000, 4000]);
        expect(rec.parts.map((x) => x.slice(2))).toEqual([
            [5000, 6000],
            [7000, 8000],
        ]);
        expect(rec.parts[1][1]).toBeGreaterThanOrEqual(rec.parts[0][1]);
    });

    it('nudged: empty submits while the frame waits, none once it is done', async () => {
        vi.stubGlobal(
            'VideoFrame',
            class {
                constructor(src, { timestamp }) {
                    this.timestamp = timestamp;
                }
            },
        );
        const { PyroWaveDecoder } = await import('../js/stream/ultra/PyroWaveDecoder.js');
        const { GpuNudge } = await import('../js/stream/ultra/GpuNudge.js');
        const { p, frames, done } = player();
        let submits = 0;
        p.device.queue.submit = () => submits++;
        p.decoder = new PyroWaveDecoder();
        p.canvas = {};
        p.context = {};
        p.trace = [];
        p._nudge = new GpuNudge(p.device, 'all');
        p.push(new Uint8Array([1]), 1, 10);
        expect(submits).toBe(1); // the frame's own
        await new Promise((r) => globalThis.setTimeout(r, 5));
        expect(submits).toBeGreaterThan(1);
        done();
        await Promise.resolve();
        await Promise.resolve();
        expect(frames).toEqual([1]);
        const [rec] = p.trace;
        expect(rec.nudge[2]).toBe(submits - 1);
        expect(p.summary().nudge).toMatchObject({ mode: 'all', waits: 1, nudges: submits - 1 });
        const after = submits;
        await new Promise((r) => globalThis.setTimeout(r, 5));
        expect(submits).toBe(after);
        p._nudge.destroy();
    });

    it('presented by the player: a stand-in at submit, two frames ahead at most, the freshest waits', async () => {
        const { PyroWaveDecoder } = await import('../js/stream/ultra/PyroWaveDecoder.js');
        const got = [];
        const p = new UltraPlayer(64, 48, (frame, meta) => got.push([frame, meta.timestamp]), {
            log: () => {},
        });
        const releases = [];
        p.device = {
            createCommandEncoder: () => ({ finish: () => ({}) }),
            queue: {
                submit() {},
                onSubmittedWorkDone: () => new Promise((resolve) => releases.push(resolve)),
            },
        };
        p.decoder = new PyroWaveDecoder();
        p.context = {};
        p.presenting = true;
        p.push(new Uint8Array([1]), 1, 10);
        p.push(new Uint8Array([2]), 2, 11); // goes too: one ahead
        p.push(new Uint8Array([3]), 3, 12); // waits: two on the queue
        p.push(new Uint8Array([4]), 4, 13); // replaces frame 3
        expect(p.decoder.pushed).toEqual([1, 2]);
        expect(got.map(([, ts]) => ts)).toEqual([1, 2]); // at submit, not at the work done
        const [frame] = got[0];
        expect(frame).toMatchObject({ presented: true, timestamp: 1, displayWidth: 64 });
        expect(frame.displayHeight).toBe(48);
        frame.close(); // the page closes what it is given
        releases.shift()();
        for (let k = 0; k < 3; k++) await Promise.resolve();
        expect(p.decoder.pushed).toEqual([1, 2, 4]);
        expect(got.map(([, ts]) => ts)).toEqual([1, 2, 4]);
        expect(p.stats).toMatchObject({ frames: 1, replaced: 1 });
        releases.shift()();
        releases.shift()();
        for (let k = 0; k < 3; k++) await Promise.resolve();
        expect(p.stats.frames).toBe(3);
        expect(p._busy).toBe(false);
    });

    it("presented by the player: the page's canvas at the frame's size, by the way the key asks", async () => {
        vi.stubGlobal('navigator', { gpu: { getPreferredCanvasFormat: () => 'bgra8unorm' } });
        let mode = 'dom';
        vi.stubGlobal('localStorage', {
            getItem: (k) => (k === 'mw_ultra_present' ? mode : null),
        });
        const canvas = () => {
            const ctx = { configure: vi.fn() };
            const el = { width: 300, height: 150, getContext: vi.fn(() => ctx), ctx };
            el.transferControlToOffscreen = vi.fn(() => ({ getContext: vi.fn(() => ctx) }));
            return el;
        };
        const open = (el) => {
            const p = new UltraPlayer(64, 48, () => {}, { log: () => {}, present: el });
            p.device = {};
            p._presentOnPage();
            return p;
        };
        let el = canvas();
        let p = open(el);
        expect(p.presenting).toBe(true);
        expect([el.width, el.height]).toEqual([64, 48]);
        expect(el.getContext).toHaveBeenCalledWith('webgpu');
        expect(el.ctx.configure).toHaveBeenCalledWith(
            expect.objectContaining({ format: 'bgra8unorm', alphaMode: 'opaque' }),
        );
        expect(p.summary().present).toBe('dom');
        mode = 'offscreen';
        el = canvas();
        p = open(el);
        expect(el.transferControlToOffscreen).toHaveBeenCalled();
        expect(el.getContext).not.toHaveBeenCalled();
        expect(p.presenting).toBe(true);
        // A way the player does not know (desync showed a black canvas): none.
        mode = 'desync';
        expect(new UltraPlayer(64, 48, () => {}, { present: canvas() }).presentMode).toBe(null);
        // A canvas that will not give a WebGPU context: the VideoFrame road.
        mode = 'dom';
        el = canvas();
        el.getContext = () => null;
        p = open(el);
        expect(p.presenting).toBe(false);
        expect(p.summary().present).toBe(null);
        // No key, no canvas taken.
        mode = null;
        el = canvas();
        p = new UltraPlayer(64, 48, () => {}, { log: () => {}, present: el });
        expect(p.presentMode).toBe(null);
    });

    it('by slices: a piece missing, and the frame decodes whole', async () => {
        vi.stubGlobal(
            'VideoFrame',
            class {
                constructor(src, { timestamp }) {
                    this.timestamp = timestamp;
                }
            },
        );
        const { PyroWaveDecoder } = await import('../js/stream/ultra/PyroWaveDecoder.js');
        const { p, frames, done } = player();
        p.decoder = new PyroWaveDecoder();
        p.canvas = {};
        p.context = {};
        p.pushPart(8, 0, new Uint8Array([1, 2]));
        p.pushPart(8, 5, new Uint8Array([6])); // 2..4 never came as a piece
        p.push(new Uint8Array([9, 2, 3, 4, 5, 6, 7]), 1, 10, 8);
        expect(p.decoder.pushed).toEqual([9]);
        done();
        await Promise.resolve();
        await Promise.resolve();
        expect(frames).toEqual([1]);
        expect(p.stats.sliced).toBe(0);
    });
});
