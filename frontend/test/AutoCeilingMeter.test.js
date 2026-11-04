/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 *
 * Does a stream prove its rung? The promises: a still desktop proves nothing,
 * time at the rate and size asked for does, one signal is enough to call a
 * stream sick for good, and a window that means nothing counts for nothing.
 */
import { describe, it, expect } from 'vitest';
import {
    AutoCeilingMeter,
    DROP_WINDOW_MS,
    GRACE_MS,
    HOST_WINDOW_MS,
    frameMatchesAsked,
} from '../js/stream/AutoCeilingMeter.js';

const TICK = 500;
const FPS = 120;
const MIN = 60000;

/** A meter and its stream: ticks at 120 fps, loaded, until told otherwise. */
function rig(opts = {}) {
    const events = [];
    const meter = new AutoCeilingMeter({
        startedAt: 0,
        fps: FPS,
        limited: true,
        provesRung: true,
        holdMs: 5 * MIN,
        confirmMs: 5 * MIN,
        onEvent: (e) => events.push(e),
        ...opts,
    });
    const r = {
        meter,
        events,
        now: 0,
        frames: 0,
        drops: 0,
        rebuilds: 0,
        types: () => events.map((e) => e.type),
        /** Run @p ms of stream with this sample, `dropsPerTick` added as it goes. */
        run(ms, sample = {}, dropsPerTick = 0) {
            const end = r.now + ms;
            while (r.now < end) {
                r.now += TICK;
                const arrival = sample.arrivalFps === undefined ? FPS : sample.arrivalFps;
                r.frames += (arrival * TICK) / 1000;
                r.drops += dropsPerTick;
                meter.tick(r.now, {
                    arrivalFps: arrival,
                    sizeMatches: true,
                    frames: r.frames,
                    drops: r.drops,
                    rebuilds: r.rebuilds,
                    fpsCap: 0,
                    presentsFps: 0,
                    ...sample,
                });
            }
        },
    };
    return r;
}

describe('AutoCeilingMeter: what proves a rung', () => {
    it('counts nothing in the first seconds', () => {
        const r = rig();
        r.run(GRACE_MS);
        expect(r.meter.loadedMs).toBe(0);
        r.run(1000);
        expect(r.meter.loadedMs).toBe(1000);
    });

    it('confirms the rung and earns the next after five loaded minutes', () => {
        const r = rig();
        r.run(GRACE_MS + 5 * MIN - TICK);
        expect(r.events).toEqual([]);
        r.run(TICK);
        expect(r.types()).toEqual(['held', 'earned']);
        expect(r.events[0].atRung).toBe(true);
    });

    it('says each thing once', () => {
        const r = rig();
        r.run(GRACE_MS + 12 * MIN);
        expect(r.types()).toEqual(['held', 'earned']);
    });

    it('learns nothing from a still desktop: few frames are no proof', () => {
        const r = rig();
        r.run(GRACE_MS + 20 * MIN, { arrivalFps: 30 });
        expect(r.meter.loadedMs).toBe(0);
        expect(r.events).toEqual([]);
    });

    it('learns nothing from a frame smaller than asked', () => {
        const r = rig();
        r.run(GRACE_MS + 20 * MIN, { sizeMatches: false });
        expect(r.meter.loadedMs).toBe(0);
        expect(r.events).toEqual([]);
    });

    it('takes up to its hold time after a past failure, and confirms on the way', () => {
        const r = rig({ holdMs: 10 * MIN });
        r.run(GRACE_MS + 5 * MIN);
        expect(r.types()).toEqual(['held']);
        r.run(5 * MIN);
        expect(r.types()).toEqual(['held', 'earned']);
    });

    it('earns nothing when the ceiling was not what limited this launch', () => {
        const r = rig({ limited: false });
        r.run(GRACE_MS + 20 * MIN);
        expect(r.types()).toEqual(['held']);
    });

    it('proves nothing of a rung the stream does not need', () => {
        const r = rig({ provesRung: false });
        r.run(GRACE_MS + 20 * MIN);
        expect(r.events).toEqual([]);
    });
});

describe('AutoCeilingMeter: neutral windows', () => {
    it('counts neither time nor drops while neutral', () => {
        const r = rig();
        r.run(GRACE_MS);
        r.run(10 * MIN, { neutral: true }, 50);
        expect(r.meter.loadedMs).toBe(0);
        expect(r.events).toEqual([]);
        r.run(DROP_WINDOW_MS);
        expect(r.events).toEqual([]);
    });

    it('ignores a decode cap, a rebuild or a congestion in a neutral window', () => {
        const r = rig();
        r.run(GRACE_MS);
        r.rebuilds = 3;
        r.run(TICK, { neutral: true, fpsCap: 60 });
        r.meter.noteCongestion();
        r.meter.noteCongestion();
        r.meter.noteSick('enhancer');
        expect(r.events).toEqual([]);
        r.run(TICK);
        expect(r.events).toEqual([]);
    });

    it('still takes a crash from a neutral window', () => {
        const r = rig();
        r.run(TICK, { neutral: true });
        r.meter.noteSick('worker_crash', true);
        expect(r.events[0]).toMatchObject({ type: 'sick', reason: 'worker_crash' });
    });

    it('does not credit a stalled timer with the time it slept', () => {
        const r = rig();
        r.run(GRACE_MS);
        r.now += 30 * MIN;
        r.run(TICK);
        expect(r.meter.loadedMs).toBeLessThanOrEqual(2000);
    });
});

describe('AutoCeilingMeter: a step of the cadence', () => {
    it('earns the next rung on a step kept to the end', () => {
        const r = rig();
        r.run(GRACE_MS, { stepped: true, arrivalFps: 2 * FPS });
        expect(r.meter.loadedMs).toBe(0);
        r.run(5 * MIN, { stepped: true, arrivalFps: 2 * FPS });
        expect(r.types()).toEqual(['held', 'earned']);
        expect(r.events[0].atRung).toBe(true);
    });

    it('holds nothing that happens on a step against the rung', () => {
        const r = rig();
        r.run(GRACE_MS);
        r.rebuilds = 3;
        r.run(TICK, { stepped: true, fpsCap: FPS });
        r.meter.noteCongestion();
        r.meter.noteCongestion();
        r.meter.noteSick('enhancer');
        r.run(DROP_WINDOW_MS, { stepped: true }, 50);
        r.run(HOST_WINDOW_MS, { stepped: true, arrivalFps: 0, presentsFps: FPS });
        expect(r.events).toEqual([]);
    });

    it('takes what comes after the step back again', () => {
        const r = rig();
        r.run(GRACE_MS);
        r.rebuilds = 3;
        r.run(TICK, { stepped: true });
        r.run(TICK);
        expect(r.events).toEqual([]);
        r.rebuilds = 4;
        r.run(TICK);
        expect(r.events[0]).toMatchObject({ type: 'sick', reason: 'decoder_rebuilt' });
    });

    it('counts time only if the frames are there, at the size asked for', () => {
        const r = rig();
        r.run(GRACE_MS);
        r.run(MIN, { stepped: true, arrivalFps: FPS / 2 });
        r.run(MIN, { stepped: true, sizeMatches: false });
        expect(r.meter.loadedMs).toBe(0);
    });

    it('counts for nothing in a window that is neutral anyway', () => {
        const r = rig();
        r.run(GRACE_MS);
        r.run(10 * MIN, { stepped: true, neutral: true, arrivalFps: 2 * FPS });
        expect(r.meter.loadedMs).toBe(0);
        expect(r.events).toEqual([]);
    });

    it('still takes a crash', () => {
        const r = rig();
        r.run(GRACE_MS + TICK, { stepped: true });
        r.meter.noteSick('worker_crash', true);
        expect(r.events[0]).toMatchObject({ type: 'sick', reason: 'worker_crash' });
    });
});

describe('AutoCeilingMeter: what makes a stream sick', () => {
    const startLoaded = () => {
        const r = rig();
        r.run(GRACE_MS);
        return r;
    };

    it('takes drops over half a percent, over a minute of frames', () => {
        const r = startLoaded();
        // 120 fps, one tick is 60 frames: one drop every tick is 1.7 %.
        r.run(DROP_WINDOW_MS - TICK, {}, 1);
        expect(r.events).toEqual([]);
        r.run(TICK, {}, 1);
        expect(r.events[0]).toMatchObject({ type: 'sick', reason: 'drops', atRung: true });
    });

    it('lets a share under the threshold through', () => {
        const r = startLoaded();
        // One drop every 4 ticks: 240 frames, 0.42 %.
        for (let i = 0; i < 240; i++) {
            r.run(3 * TICK);
            r.run(TICK, {}, 1);
        }
        expect(r.types()).not.toContain('sick');
    });

    it('does not weigh drops on too few frames: one in a hundred of a still desktop', () => {
        const r = startLoaded();
        r.run(DROP_WINDOW_MS, { arrivalFps: 2, sizeMatches: true }, 0);
        r.drops += 1;
        r.run(TICK, { arrivalFps: 2 });
        expect(r.events).toEqual([]);
    });

    it('takes a decode cap asked of the host', () => {
        const r = startLoaded();
        r.run(TICK, { fpsCap: 60 });
        expect(r.events[0]).toMatchObject({ type: 'sick', reason: 'decode_rate' });
    });

    it('takes a rebuilt decoder', () => {
        const r = startLoaded();
        r.rebuilds = 1;
        r.run(TICK);
        expect(r.events[0]).toMatchObject({ type: 'sick', reason: 'decoder_rebuilt' });
    });

    it('takes two sustained congestions, not one', () => {
        const r = startLoaded();
        r.run(TICK);
        r.meter.noteCongestion();
        expect(r.events).toEqual([]);
        r.meter.noteCongestion();
        expect(r.events[0]).toMatchObject({ type: 'sick', reason: 'congestion' });
    });

    it('takes a host whose display keeps up while its frames do not arrive', () => {
        const r = startLoaded();
        r.run(HOST_WINDOW_MS, { presentsFps: 120, arrivalFps: 100 });
        expect(r.events[0]).toMatchObject({ type: 'sick', reason: 'host_behind' });
    });

    it('leaves a host alone when its display itself is slow, or its frames arrive', () => {
        const slow = startLoaded();
        slow.run(3 * HOST_WINDOW_MS, { presentsFps: 30, arrivalFps: 30 });
        expect(slow.events).toEqual([]);
        const fine = startLoaded();
        fine.run(3 * HOST_WINDOW_MS, { presentsFps: 120, arrivalFps: 115 });
        expect(fine.types()).not.toContain('sick');
    });

    it('stays sick to its end: nothing is earned after', () => {
        const r = startLoaded();
        r.run(TICK);
        r.meter.noteSick('enhancer');
        r.run(20 * MIN);
        expect(r.types()).toEqual(['sick']);
        expect(r.meter.sickReason).toBe('enhancer');
    });

    it('can fall ill after having earned the next rung', () => {
        const r = startLoaded();
        r.run(5 * MIN);
        r.meter.noteSick('enhancer');
        expect(r.types()).toEqual(['held', 'earned', 'sick']);
    });

    it('does not blame the rung for a stream that does not need it', () => {
        const r = rig({ provesRung: false });
        r.run(GRACE_MS + TICK);
        r.meter.noteSick('enhancer');
        expect(r.events[0]).toMatchObject({ type: 'sick', atRung: false });
    });

    it('does not blame the rung for a stream that never reached its size', () => {
        const r = rig();
        r.run(GRACE_MS + TICK, { sizeMatches: false });
        r.meter.noteSick('enhancer');
        expect(r.events[0]).toMatchObject({ type: 'sick', atRung: false });
    });

    it('blames the rung when no frame has said its size yet: a failure to start', () => {
        const r = rig();
        r.run(TICK, { sizeMatches: null });
        r.meter.noteSick('decoder_silent', true);
        expect(r.events[0]).toMatchObject({ type: 'sick', atRung: true });
    });
});

describe('frameMatchesAsked', () => {
    const asked = { width: 2560, height: 1440, fps: 120, below: 330e6 };

    it('cannot tell before the first frame, or for a size it was not told', () => {
        expect(frameMatchesAsked('', asked)).toBeNull();
        expect(frameMatchesAsked('2560×1440', null)).toBeNull();
        expect(frameMatchesAsked('2560×1440', { ...asked, height: 0 })).toBeNull();
    });

    it('takes the size asked for, and a host fitting its own shape inside the box', () => {
        expect(frameMatchesAsked('2560×1440', asked)).toBe(true);
        expect(frameMatchesAsked('2304×1440', asked)).toBe(true);
    });

    it('refuses a host much smaller than the box', () => {
        expect(frameMatchesAsked('1920×1080', asked)).toBe(false);
    });

    it('refuses a frame the rung below would have allowed anyway', () => {
        // A 1080p host in a 1240p box at 330 M: 249 M is the base budget's.
        const box = { width: 2204, height: 1240, fps: 120, below: 250e6 };
        expect(frameMatchesAsked('1920×1080', box)).toBe(false);
    });
});
