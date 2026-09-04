/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import { WebRtcDataChannel } from '../js/api/WebRtcDataChannel.js';

// A fixed clock, well past zero: the transport keeps "last seen" times as
// performance.now() values and treats 0 as "never", which a fresh process's
// small real clock would trip over.
const NOW = 100000;
beforeEach(() => {
    vi.spyOn(performance, 'now').mockReturnValue(NOW);
});
afterEach(() => {
    vi.restoreAllMocks();
});

// A transport that never connects: the recovery logic under test lives in
// _assembleFrame / _cleanupStaleFrames / _requestIdrFrame and only needs an
// open-looking input channel to talk to.
function transport() {
    const t = new WebRtcDataChannel('wss://host.invalid/signal');
    t.dataChannels.input = { readyState: 'open', send: vi.fn() };
    // Past the "initial quiet period" the starvation check skips, silent for
    // 5 s, and past the request throttle.
    t.stats.framesAssembled = 10;
    t._lastAssembledTime = NOW - 5000;
    t._lastIdrRequestTime = -1e6;
    return t;
}

function completeEntry(keyframe = false) {
    return {
        chunks: [new Uint8Array([1, 2, 3])],
        total: 1,
        received: 1,
        keyframe,
        firstChunkTime: performance.now(),
        completed: false,
        backendTs: 1,
    };
}

function idrRequests(t) {
    return t.dataChannels.input.send.mock.calls.filter((c) => c[0].includes('requestidr')).length;
}

describe('WebRtcDataChannel recovery — a host that heals by invalidation', () => {
    it('asks for no keyframe on its own, whatever the reason', () => {
        const t = transport();
        t.healsByInvalidation = true;
        t._requestIdrFrame('incomplete frame #7');
        t._requestIdrFrame('stale frames (2 dropped)');
        expect(idrRequests(t)).toBe(0);
    });

    it('counts a silence as a stall and requests nothing', () => {
        const t = transport();
        t.healsByInvalidation = true;
        t._cleanupStaleFrames();
        expect(t.stats.stalls).toBe(1);
        expect(idrRequests(t)).toBe(0);
        // Same episode: counted once.
        t._cleanupStaleFrames();
        expect(t.stats.stalls).toBe(1);
        // The stream resumes: the episode closes, the next silence is a new one.
        t._assembleFrame(1, completeEntry());
        expect(t._stalledSince).toBe(0);
        t._lastAssembledTime = NOW - 5000;
        t._cleanupStaleFrames();
        expect(t.stats.stalls).toBe(2);
    });

    it('still requests a keyframe on starvation for every other host', () => {
        const t = transport();
        t._cleanupStaleFrames();
        expect(t.stats.stalls).toBe(1);
        expect(idrRequests(t)).toBe(1);
    });

    it('declares a frame lost the moment a newer one completes (ordered channel)', () => {
        const t = transport();
        t.healsByInvalidation = true;
        const lost = [];
        t.onFrameLoss = (id) => lost.push(id);
        t._reassembly.set(5, { ...completeEntry(), total: 3, received: 1 });
        t._assembleFrame(6, completeEntry());
        expect(lost).toEqual([5]);
        expect(t._reassembly.has(5)).toBe(false);
        expect(t.stats.framesDropped).toBe(1);
    });

    it('leaves an incomplete older frame to the 500 ms clock on other hosts', () => {
        const t = transport();
        const lost = [];
        t.onFrameLoss = (id) => lost.push(id);
        t._reassembly.set(5, { ...completeEntry(), total: 3, received: 1 });
        t._assembleFrame(6, completeEntry());
        expect(lost).toEqual([]);
        expect(t._reassembly.has(5)).toBe(true);
    });

    it('forgets a silence the page itself caused', () => {
        const t = transport();
        t.healsByInvalidation = true;
        t.noteResumed();
        t._cleanupStaleFrames();
        expect(t.stats.stalls).toBe(0);
    });
});

describe('WebRtcDataChannel ride-out watchdog', () => {
    it('gives the wave its period in frames received, not 2.5 s', () => {
        const t = transport();
        t.rideOutLoss = true;
        t.rideOutFrames = 100;
        t._requestIdrFrame('incomplete frame #3');
        expect(t._rideOutSince).toBeGreaterThan(0);
        expect(idrRequests(t)).toBe(0);
        // 2.5 s later on the clock but only half a wave in: still riding it out.
        t._rideOutSince = performance.now() - 2600;
        for (let i = 1; i <= 50; i++) t._assembleFrame(i * 2, completeEntry());
        t._requestIdrFrame('incomplete frame #103');
        expect(idrRequests(t)).toBe(0);
        expect(t.stats.rideOutFailed).toBe(0);
        // 1.2 periods received without the stream running in order: give up.
        for (let i = 51; i <= 120; i++) t._assembleFrame(i * 2, completeEntry());
        t._requestIdrFrame('incomplete frame #241');
        expect(t.stats.rideOutFailed).toBe(1);
        expect(t._rideOutFailed).toBe(true);
        expect(idrRequests(t)).toBe(1);
    });

    it('keeps the clock when the host said nothing about its wave', () => {
        const t = transport();
        t.rideOutLoss = true;
        t._requestIdrFrame('incomplete frame #3');
        t._rideOutSince = performance.now() - 2600;
        t._requestIdrFrame('incomplete frame #9');
        expect(t.stats.rideOutFailed).toBe(1);
        expect(idrRequests(t)).toBe(1);
    });

    it('does not wait forever for frames that never come', () => {
        const t = transport();
        t.rideOutLoss = true;
        t.rideOutFrames = 1000;
        t._requestIdrFrame('incomplete frame #3');
        t._rideOutSince = performance.now() - t.RIDE_OUT_HARD_MAX_MS - 1;
        t._requestIdrFrame('starvation');
        expect(t.stats.rideOutFailed).toBe(1);
        expect(idrRequests(t)).toBe(1);
    });

    it('closes the ride-out once two frames arrive in order', () => {
        const t = transport();
        t.rideOutLoss = true;
        t.rideOutFrames = 100;
        t._requestIdrFrame('incomplete frame #3');
        t._assembleFrame(10, completeEntry());
        t._assembleFrame(11, completeEntry());
        expect(t._rideOutSince).toBe(0);
    });
});
