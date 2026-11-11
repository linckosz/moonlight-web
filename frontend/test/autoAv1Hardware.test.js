/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 *
 * « Auto » and an AV1 the hardware does not decode (11/11/2026).
 *
 * Where HEVC had failed, Auto asked AV1 of any host that encodes it. An AV1
 * decoded in software on a weak client does not keep up, and raises no error:
 * a N95 on 1440p60 topped at 62 frames a second, half a second behind, where
 * its hardware H.264 took 1 ms a frame — and nothing ever marked it as failed,
 * so it stayed that way for thirty days.
 *
 * Pinned down: AV1 only where the browser says the hardware decodes it; in
 * Firefox, also where an AV1 stream already ran smoothly; H.264 otherwise. How
 * each browser is asked. And the watch on a running AV1 stream: a decoder
 * queue that stays full in the first minute is a failure under Auto, two
 * minutes without it are remembered as smooth.
 *
 * The first real streams showed two holes in that watch (11/11/2026, evening).
 * A desktop that stood still sent two pictures a second, and two minutes of it
 * were remembered as smooth. And the AV1 path never told the diagnostic of its
 * decoder's queue: on a N95 decoding 1440p60 in software, 39 pictures came out
 * of the 60 a second, a thousand were waiting after 50 s, the overlay read an
 * empty queue — and that stream was remembered as smooth. Pinned down: the
 * two minutes count in moving pictures only, a host asked for fewer pictures
 * is a decoder that does not keep up, and the AV1 path reports its queue.
 *
 * And Auto's AV1 that does not keep up comes back in H.264 at once, the way
 * an HEVC that fails does (Bruno, 11/11/2026): left to run, that stream only
 * fell further behind, and H.264 came at the next launch.
 */
import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import { StreamView } from '../js/ui/StreamView.js';
import {
    AV1_WATCH_QUEUE,
    Av1Watch,
    autoLaunchCodec,
    av1FailedHere,
    av1HardwareHere,
    av1ProvenHere,
    noteAv1Failed,
    noteAv1Proven,
    noteHevcFailed,
    resetAv1HardwareHere,
} from '../js/util/CodecAuto.js';

const HOST_AV1 = 0x1 | 0x100 | 0x200 | 0x10000 | 0x20000;
const DAY = 24 * 3600 * 1000;
const NOW = 1000 * DAY;
const UA = 'Mozilla/5.0 (X11; Linux x86_64) Chrome/157.0.0.0 Safari/537.36';

function memoryStore() {
    const m = new Map();
    return {
        getItem: (k) => (m.has(k) ? m.get(k) : null),
        setItem: (k, v) => m.set(k, String(v)),
    };
}

/** A browser whose HEVC failed, on a host that encodes AV1. */
function hevcFailed() {
    const s = memoryStore();
    noteHevcFailed(UA, s, NOW);
    return s;
}
const ask = (s, av1) => autoLaunchCodec(HOST_AV1, UA, s, NOW, av1);

describe('Auto, where HEVC failed: AV1 only where it is decoded by the hardware', () => {
    it('asks AV1 of a browser that says so, H.264 of one that does not', () => {
        const s = hevcFailed();
        expect(ask(s, { hardware: true })).toBe('av1');
        expect(ask(s, { hardware: false })).toBe('h264');
        expect(ask(s, {})).toBe('h264');
        expect(ask(s)).toBe('h264');
    });

    it('still asks HEVC first, whatever the AV1 decoder', () => {
        const s = memoryStore();
        expect(ask(s, { hardware: true })).toBe('hevc');
        expect(ask(s, { hardware: false })).toBe('hevc');
    });

    it('asks H.264 once AV1 failed, hardware or not', () => {
        const s = hevcFailed();
        noteAv1Failed(UA, s, NOW);
        expect(ask(s, { hardware: true })).toBe('h264');
        expect(ask(s, { hardware: true, gecko: true })).toBe('h264');
    });
});

describe('Auto in Firefox: AV1 where it already ran smoothly', () => {
    it('asks AV1 once a stream proved it, without the hardware saying so', () => {
        const s = hevcFailed();
        expect(ask(s, { hardware: false, gecko: true })).toBe('h264');
        noteAv1Proven(UA, s, NOW);
        expect(av1ProvenHere(UA, s, NOW)).toBe(true);
        expect(ask(s, { hardware: false, gecko: true })).toBe('av1');
        // Thirty days, like the failures.
        expect(ask(s, { hardware: false, gecko: true })).toBe('av1');
        expect(autoLaunchCodec(HOST_AV1, UA, s, NOW + 31 * DAY, { gecko: true })).toBe('hevc');
    });

    it('tries AV1 where Firefox says the hardware decodes it', () => {
        expect(ask(hevcFailed(), { hardware: true, gecko: true })).toBe('av1');
    });

    it('keeps the proof for Firefox: elsewhere the hardware answer is the rule', () => {
        const s = hevcFailed();
        noteAv1Proven(UA, s, NOW);
        expect(ask(s, { hardware: false, gecko: false })).toBe('h264');
    });

    it('lets the later of a proof and a failure stand', () => {
        const s = hevcFailed();
        noteAv1Proven(UA, s, NOW);
        noteAv1Failed(UA, s, NOW);
        expect(av1ProvenHere(UA, s, NOW)).toBe(false);
        expect(ask(s, { hardware: true, gecko: true })).toBe('h264');
        noteAv1Proven(UA, s, NOW);
        expect(av1FailedHere(UA, s, NOW)).toBe(false);
        expect(ask(s, { hardware: false, gecko: true })).toBe('av1');
    });
});

describe('asking the browser about its AV1 decoder', () => {
    beforeEach(() => resetAv1HardwareHere());

    const decoder = (answer) => ({ isConfigSupported: vi.fn(async () => answer) });
    const capabilities = (answer) => ({ decodingInfo: vi.fn(async () => answer) });

    it('asks Chrome for a hardware decoder, and takes its word', async () => {
        const vd = decoder({ supported: true });
        const mc = capabilities({ supported: true, powerEfficient: false });
        expect(await av1HardwareHere({ decoder: vd, capabilities: mc })).toBe(true);
        expect(vd.isConfigSupported).toHaveBeenCalledWith(
            expect.objectContaining({ hardwareAcceleration: 'prefer-hardware' }),
        );
        expect(mc.decodingInfo).not.toHaveBeenCalled();

        resetAv1HardwareHere();
        expect(await av1HardwareHere({ decoder: decoder({ supported: false }) })).toBe(false);
    });

    it('asks Firefox whether the decoding is power efficient: its other answer is always yes', async () => {
        const vd = decoder({ supported: true });
        const off = capabilities({ supported: true, smooth: true, powerEfficient: false });
        expect(await av1HardwareHere({ gecko: true, decoder: vd, capabilities: off })).toBe(false);
        expect(vd.isConfigSupported).not.toHaveBeenCalled();
        expect(off.decodingInfo).toHaveBeenCalledWith(expect.objectContaining({ type: 'webrtc' }));

        resetAv1HardwareHere();
        const on = capabilities({ supported: true, smooth: true, powerEfficient: true });
        expect(await av1HardwareHere({ gecko: true, decoder: vd, capabilities: on })).toBe(true);
    });

    it('asks a browser without WebCodecs the same way as Firefox', async () => {
        const on = capabilities({ supported: true, powerEfficient: true });
        expect(await av1HardwareHere({ decoder: null, capabilities: on })).toBe(true);
    });

    it('takes no answer, an error or a late answer for a no', async () => {
        expect(await av1HardwareHere({ decoder: null, capabilities: null })).toBe(false);

        resetAv1HardwareHere();
        const throwing = {
            isConfigSupported: async () => {
                throw new Error('no');
            },
        };
        expect(await av1HardwareHere({ decoder: throwing })).toBe(false);

        resetAv1HardwareHere();
        const never = { isConfigSupported: () => new Promise(() => {}) };
        expect(await av1HardwareHere({ decoder: never, timeoutMs: 20 })).toBe(false);
    });

    it('asks once per page', async () => {
        const vd = decoder({ supported: true });
        await av1HardwareHere({ decoder: vd });
        await av1HardwareHere({ decoder: vd });
        expect(vd.isConfigSupported).toHaveBeenCalledTimes(1);
    });
});

describe('the watch on an AV1 stream', () => {
    /** One reading every half second for @p seconds, as @p readingAt says. */
    function run(watch, seconds, readingAt, from = 0) {
        const verdicts = [];
        for (let t = from; t <= from + seconds * 1000; t += 500) {
            const v = watch.sample(readingAt(t), t);
            if (v) verdicts.push([v, t]);
        }
        return verdicts;
    }
    /** A stream that moves, 60 pictures a second, its decoder queue at @p queue. */
    const moving = (queue) => ({ queue, fps: 60 });

    it('says failed once the queue was full for five seconds of the first minute', () => {
        const verdicts = run(new Av1Watch(), 200, () => moving(8));
        expect(verdicts).toEqual([['failed', 5000]]);
    });

    it('adds up the time in pieces', () => {
        // Full one second in five: five seconds of it after 25.
        const verdicts = run(new Av1Watch(), 200, (t) =>
            moving(t % 5000 < 1000 ? AV1_WATCH_QUEUE : 0),
        );
        expect(verdicts).toHaveLength(1);
        expect(verdicts[0][0]).toBe('failed');
        expect(verdicts[0][1]).toBeGreaterThan(20000);
        expect(verdicts[0][1]).toBeLessThan(30000);
    });

    it('says proven after two minutes that never added up to that', () => {
        expect(run(new Av1Watch(), 200, () => moving(0.4))).toEqual([['proven', 120000]]);
        // A burst now and then is not a decoder that does not keep up.
        expect(run(new Av1Watch(), 200, (t) => moving(t % 60000 < 1500 ? 8 : 1))).toEqual([
            ['proven', 120000],
        ]);
    });

    it('says nothing of a stream that got slow after its first minute', () => {
        expect(run(new Av1Watch(), 300, (t) => moving(t > 70000 ? 8 : 0))).toEqual([]);
    });

    it('says nothing of a stream that ended early', () => {
        expect(run(new Av1Watch(), 100, () => moving(0))).toEqual([]);
    });

    it('does not count the time a page was put away', () => {
        const watch = new Av1Watch();
        watch.sample(moving(8), 0);
        // One reading after half a minute away: two seconds of it at most.
        expect(watch.sample(moving(8), 30000)).toBeNull();
        expect(watch.sample(moving(8), 30500)).toBeNull();
    });

    it('says nothing of a desktop that stands still: two pictures a second prove no decoder', () => {
        expect(run(new Av1Watch(), 900, () => ({ queue: 0, fps: 2 }))).toEqual([]);
        expect(run(new Av1Watch(), 900, () => ({ queue: 0 }))).toEqual([]);
    });

    it('counts the two minutes in moving pictures only', () => {
        // Still for a hundred seconds, then a film at 24 pictures a second.
        const verdicts = run(new Av1Watch(), 400, (t) => ({ queue: 0, fps: t < 100000 ? 2 : 24 }));
        expect(verdicts).toHaveLength(1);
        expect(verdicts[0][0]).toBe('proven');
        expect(verdicts[0][1]).toBeGreaterThan(219000);
        expect(verdicts[0][1]).toBeLessThan(221000);
    });

    it('counts the first minute in moving pictures too: a game started late still fails', () => {
        // Five minutes of a still desktop, then the game: the queue fills.
        const verdicts = run(new Av1Watch(), 400, (t) =>
            t < 300000 ? { queue: 0, fps: 2 } : moving(9),
        );
        expect(verdicts).toHaveLength(1);
        expect(verdicts[0][0]).toBe('failed');
        expect(verdicts[0][1]).toBeLessThan(306000);
    });

    it('takes a host asked for fewer pictures for a decoder that does not keep up', () => {
        // The governor emptied the queue by slowing the host down: 40 a second.
        const capped = { queue: 0, fps: 40, capped: true };
        expect(run(new Av1Watch(), 200, () => capped)).toEqual([['failed', 5000]]);
        // Ten seconds for the governor to act, the queue full all the while.
        expect(run(new Av1Watch(), 200, (t) => (t < 10000 ? moving(30) : capped))).toEqual([
            ['failed', 5000],
        ]);
    });

    it('fails a decoder behind however few pictures arrive', () => {
        expect(run(new Av1Watch(), 200, () => ({ queue: 12, fps: 10 }))).toEqual([
            ['failed', 5000],
        ]);
    });
});

describe('the stream view and its AV1', () => {
    let store;
    beforeEach(() => {
        const m = new Map();
        store = {
            getItem: (k) => (m.has(k) ? m.get(k) : null),
            setItem: (k, v) => m.set(k, String(v)),
            removeItem: (k) => m.delete(k),
        };
        vi.stubGlobal('localStorage', store);
        vi.spyOn(console, 'log').mockImplementation(() => {});
        vi.spyOn(console, 'warn').mockImplementation(() => {});
    });
    afterEach(() => {
        vi.restoreAllMocks();
        vi.unstubAllGlobals();
    });

    /** A view of that stream, ticked every half second for @p seconds: the
     *  decoder queue at @p queueAvg, @p fps pictures arriving a second. */
    function stream(view, seconds, queueAvg, fps = 60) {
        const diag = { decodeQueueAvg: queueAvg, arrivalAvgMs: 1000 / fps };
        for (let t = 0; t <= seconds * 1000; t += 500)
            StreamView.prototype._tickAv1Watch.call(view, diag, t);
        return view;
    }
    const av1 = (more = {}) => ({
        videoCodec: 'av1',
        _codecAuto: true,
        _decodeRateCap: 0,
        _requestCodecFallback: vi.fn(),
        ...more,
    });

    it('notes an AV1 that does not keep up under Auto: H.264 at the next launch', () => {
        noteHevcFailed();
        stream(av1(), 20, 8);
        expect(av1FailedHere()).toBe(true);
        expect(autoLaunchCodec(HOST_AV1, undefined, undefined, undefined, { hardware: true })).toBe(
            'h264',
        );
    });

    it('brings that stream back in H.264 at once, as an HEVC that fails is', () => {
        const P = StreamView.prototype;
        const view = av1({
            _hdrEnabled: false,
            _yuv444: false,
            _quitting: false,
            _manualQuitting: false,
            _codecFallbackRequested: false,
            _codecFallback: null,
            webrtc: null,
            quit: vi.fn(),
            _computeCodecFallbackTarget: P._computeCodecFallbackTarget,
            _requestCodecFallback: P._requestCodecFallback,
        });
        stream(view, 4, 8);
        expect(view.quit).not.toHaveBeenCalled();
        stream(view, 20, 8);
        // What the app relaunches from (MoonlightApp._onStreamingQuit).
        expect(view._codecFallbackRequested).toBe(true);
        expect(view._codecFallback).toEqual({ codec: 'h264', hdr: false });
        expect(view.quit).toHaveBeenCalledOnce();
    });

    it('leaves a guest where it is: the verdict, no relaunch of the feed', () => {
        const guest = av1({ _playerMode: true });
        stream(guest, 20, 8);
        expect(av1FailedHere()).toBe(true);
        expect(guest._requestCodecFallback).not.toHaveBeenCalled();
    });

    it('notes nothing against an AV1 chosen by hand, and leaves it running', () => {
        const byHand = av1({ _codecAuto: false });
        stream(byHand, 20, 8);
        expect(av1FailedHere()).toBe(false);
        expect(byHand._requestCodecFallback).not.toHaveBeenCalled();
    });

    it('relaunches nothing that ran smoothly', () => {
        const smooth = av1();
        stream(smooth, 130, 0.5);
        expect(av1ProvenHere()).toBe(true);
        expect(smooth._requestCodecFallback).not.toHaveBeenCalled();
    });

    it('remembers an AV1 that ran smoothly, chosen by hand or by Auto', () => {
        stream(av1({ _codecAuto: false }), 121, 0.5);
        expect(av1ProvenHere()).toBe(true);
    });

    it('remembers nothing of a desktop that stood still', () => {
        // What a native host sends of a screen nothing moves on.
        stream(av1({ _codecAuto: false }), 600, 0, 2);
        expect(av1ProvenHere()).toBe(false);
        expect(av1FailedHere()).toBe(false);
    });

    it('notes an AV1 the host had to slow down for: no proof, a failure under Auto', () => {
        noteHevcFailed();
        stream(av1({ _codecAuto: false, _decodeRateCap: 40 }), 130, 0, 40);
        expect(av1ProvenHere()).toBe(false);
        expect(av1FailedHere()).toBe(false);
        stream(av1({ _decodeRateCap: 40 }), 130, 0, 40);
        expect(av1FailedHere()).toBe(true);
    });

    it('has the AV1 path report its decoder queue, like the other codecs', () => {
        vi.stubGlobal(
            'EncodedVideoChunk',
            class {
                constructor(init) {
                    Object.assign(this, init);
                }
            },
        );
        const view = {
            decoderConfigured: true,
            decoder: { decodeQueueSize: 7, decode: vi.fn() },
            frameCount: 0,
            _referenceValid: true,
            _diag: { noteDecodeQueue: vi.fn(), noteDrop: vi.fn(), arrivalAvgMs: 16.7 },
            DECODE_QUEUE_MAX: 8,
            QUEUE_STALL_MS: 200,
            QUEUE_RESET_MS: 1000,
            _queueStallStart: 0,
            _lastQueueFlushMs: -Infinity,
            _renderer: null,
            _decoderTrail: null,
            _trackChunkSubmit: vi.fn(),
            _tellDecodeQueue: vi.fn(),
            _requestIdr: vi.fn(),
            _handleDecoderError: vi.fn(),
            _flushDecodeQueue: vi.fn(),
            stats: { received: 0, dropped: 0 },
        };
        // One frame OBU with its size field: nothing to strip.
        const picture = new Uint8Array([0x32, 0x01, 0x00]);
        StreamView.prototype.decodeAv1Frame.call(view, picture, true);
        view.decoder.decodeQueueSize = 9;
        StreamView.prototype.decodeAv1Frame.call(view, picture, false);
        expect(view._diag.noteDecodeQueue.mock.calls).toEqual([[7], [9]]);
        expect(view.decoder.decode).toHaveBeenCalledTimes(2);
    });

    it('leaves the other streams alone: another codec, HDR, 4:4:4', () => {
        stream({ videoCodec: 'hevc', _codecAuto: true, _decodeRateCap: 0 }, 130, 8);
        stream(av1({ _hdrEnabled: true }), 130, 8);
        stream(av1({ _yuv444: true }), 130, 0);
        expect(av1FailedHere()).toBe(false);
        expect(av1ProvenHere()).toBe(false);
    });
});
