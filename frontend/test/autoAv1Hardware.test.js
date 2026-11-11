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
    /** One reading every half second for @p seconds, the queue as @p queueAt says. */
    function run(watch, seconds, queueAt, from = 0) {
        const verdicts = [];
        for (let t = from; t <= from + seconds * 1000; t += 500) {
            const v = watch.sample(queueAt(t), t);
            if (v) verdicts.push([v, t]);
        }
        return verdicts;
    }

    it('says failed once the queue was full for five seconds of the first minute', () => {
        const verdicts = run(new Av1Watch(), 200, () => 8);
        expect(verdicts).toEqual([['failed', 5000]]);
    });

    it('adds up the time in pieces', () => {
        // Full one second in five: five seconds of it after 25.
        const verdicts = run(new Av1Watch(), 200, (t) => (t % 5000 < 1000 ? AV1_WATCH_QUEUE : 0));
        expect(verdicts).toHaveLength(1);
        expect(verdicts[0][0]).toBe('failed');
        expect(verdicts[0][1]).toBeGreaterThan(20000);
        expect(verdicts[0][1]).toBeLessThan(30000);
    });

    it('says proven after two minutes that never added up to that', () => {
        expect(run(new Av1Watch(), 200, () => 0.4)).toEqual([['proven', 120000]]);
        // A burst now and then is not a decoder that does not keep up.
        expect(run(new Av1Watch(), 200, (t) => (t % 60000 < 1500 ? 8 : 1))).toEqual([
            ['proven', 120000],
        ]);
    });

    it('says nothing of a stream that got slow after its first minute', () => {
        expect(run(new Av1Watch(), 300, (t) => (t > 70000 ? 8 : 0))).toEqual([]);
    });

    it('says nothing of a stream that ended early', () => {
        expect(run(new Av1Watch(), 100, () => 0)).toEqual([]);
    });

    it('does not count the time a page was put away', () => {
        const watch = new Av1Watch();
        watch.sample(8, 0);
        // One reading after half a minute away: two seconds of it at most.
        expect(watch.sample(8, 30000)).toBeNull();
        expect(watch.sample(8, 30500)).toBeNull();
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

    /** A view of that stream, ticked every half second for @p seconds. */
    function stream(view, seconds, queueAvg) {
        for (let t = 0; t <= seconds * 1000; t += 500)
            StreamView.prototype._tickAv1Watch.call(view, { decodeQueueAvg: queueAvg }, t);
        return view;
    }
    const av1 = (more = {}) => ({ videoCodec: 'av1', _codecAuto: true, ...more });

    it('notes an AV1 that does not keep up under Auto: H.264 at the next launch', () => {
        noteHevcFailed();
        stream(av1(), 20, 8);
        expect(av1FailedHere()).toBe(true);
        expect(autoLaunchCodec(HOST_AV1, undefined, undefined, undefined, { hardware: true })).toBe(
            'h264',
        );
    });

    it('notes nothing against an AV1 chosen by hand', () => {
        stream(av1({ _codecAuto: false }), 20, 8);
        expect(av1FailedHere()).toBe(false);
    });

    it('remembers an AV1 that ran smoothly, chosen by hand or by Auto', () => {
        stream(av1({ _codecAuto: false }), 121, 0.5);
        expect(av1ProvenHere()).toBe(true);
    });

    it('leaves the other streams alone: another codec, HDR, 4:4:4', () => {
        stream({ videoCodec: 'hevc', _codecAuto: true }, 130, 8);
        stream(av1({ _hdrEnabled: true }), 130, 8);
        stream(av1({ _yuv444: true }), 130, 0);
        expect(av1FailedHere()).toBe(false);
        expect(av1ProvenHere()).toBe(false);
    });
});
