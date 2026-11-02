/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 *
 * One launch's dealings with the learned ceiling. The promises: a journal-only
 * session writes nothing and spends nothing more; only what was asked within a
 * rung says anything of it; what is earned serves the next launch and never this
 * one; a stream that never came back costs a rung, a page closed does not.
 */
import { describe, it, expect } from 'vitest';
import { AUTO_CEILING_KEY, autoCeilingMemory } from '../js/util/AutoCeiling.js';
import { AutoCeilingSession } from '../js/stream/AutoCeilingSession.js';
import { GRACE_MS } from '../js/stream/AutoCeilingMeter.js';
import { PIXEL_RATE_BUDGET, streamFootprint } from '../js/util/StreamResolution.js';

const HOST = 'host-uuid';
const CODEC = 'hevc';
const MIN = 60000;

function fakeStorage() {
    const map = new Map();
    return {
        getItem: (k) => (map.has(k) ? map.get(k) : null),
        setItem: (k, v) => map.set(k, String(v)),
        written: () => map.has(AUTO_CEILING_KEY),
    };
}

function rig(mode, overrides = {}) {
    const storage = fakeStorage();
    const memory = autoCeilingMemory(storage, () => 1000);
    const log = [];
    const session = () =>
        new AutoCeilingSession({
            host: HOST,
            codec: CODEC,
            native: true,
            device: {},
            mode,
            memory,
            log: (l) => log.push(l),
            ...overrides,
        });
    return { storage, memory, log, session };
}

/** A 4K 120 Hz screen in Auto/Auto: 1080p at the base budget, more above. */
const heights = { 250: 1080, 330: 1240, 450: 1440, 500: 1528, 700: 1808, 1000: 2160 };
const uhdAuto = (budget) =>
    streamFootprint({ height: heights[budget / 1e6], aspect: '16:9' }, 120);

/** Run a meter loaded for @p ms, ticking every 500 ms. */
function runLoaded(meter, ms) {
    for (let t = 500; t <= GRACE_MS + ms; t += 500) {
        meter.tick(t, {
            arrivalFps: 120,
            sizeMatches: true,
            frames: t * 0.12,
            drops: 0,
            rebuilds: 0,
            fpsCap: 0,
            presentsFps: 0,
        });
    }
}

describe('AutoCeilingSession: who learns', () => {
    it('leaves a host other than the native one alone', () => {
        const { session } = rig('on', { native: false });
        const s = session();
        expect(s.learns).toBe(false);
        expect(s.budget).toBe(PIXEL_RATE_BUDGET);
        s.weigh(uhdAuto);
        expect(s.meter(0)).toBeNull();
    });

    it('leaves a TV, a small memory and Power Saving alone', () => {
        for (const device of [{ tv: true }, { lowMemory: true }, { powerSave: true }]) {
            const { session, storage } = rig('on', { device });
            const s = session();
            s.weigh(uhdAuto);
            expect(s.learns).toBe(false);
            expect(s.meter(0)).toBeNull();
            expect(storage.written()).toBe(false);
        }
    });

    it('does nothing when switched off', () => {
        const { session } = rig('off');
        expect(session().learns).toBe(false);
    });
});

describe('AutoCeilingSession: journal only', () => {
    it('measures, writes nothing, and stays on the base budget', () => {
        const { session, storage, log } = rig('log');
        const s = session();
        s.weigh(uhdAuto);
        expect(s.budget).toBe(PIXEL_RATE_BUDGET);
        runLoaded(s.meter(0), 6 * MIN);
        expect(storage.written()).toBe(false);
        expect(log.join('\n')).toContain('330 M earned for the next launch');
        expect(log.join('\n')).toContain('(journal only)');
    });

    it('ignores a rung prepared by hand', () => {
        const { session, memory } = rig('log');
        memory.raise(HOST, CODEC);
        expect(session().budget).toBe(PIXEL_RATE_BUDGET);
    });

    it('says a sick stream too, without touching the memory', () => {
        const { session, storage, log } = rig('log');
        const s = session();
        s.weigh(uhdAuto);
        const m = s.meter(0);
        m.noteSick('worker_crash', true);
        expect(s.sick).toBe('worker_crash');
        expect(storage.written()).toBe(false);
        expect(log.join('\n')).toContain('sick (worker_crash)');
    });
});

describe('AutoCeilingSession: learning', () => {
    it('earns the next rung for the next launch, never this one', () => {
        const { session } = rig('on');
        const s = session();
        s.weigh(uhdAuto);
        runLoaded(s.meter(0), 5 * MIN);
        expect(s.budget).toBe(PIXEL_RATE_BUDGET);
        const next = session();
        expect(next.budget).toBe(330e6);
        expect(next.trial).toBe(true);
    });

    it('does not climb past what the device may reach', () => {
        const { session, memory } = rig('on', { device: { handheld: true } });
        memory.raise(HOST, CODEC);
        memory.raise(HOST, CODEC);
        const s = session();
        expect(s.budget).toBe(450e6);
        s.weigh(uhdAuto);
        runLoaded(s.meter(0), 20 * MIN);
        s.end();
        expect(session().budget).toBe(450e6);
    });

    it('confirms a trial without climbing when the screen is what stopped it', () => {
        const { session, memory } = rig('on');
        memory.raise(HOST, CODEC);
        memory.raise(HOST, CODEC);
        const s = session();
        // A 1440p 120 Hz screen: 450 M is all it asks, and 500 M would add nothing.
        s.weigh(() => streamFootprint({ height: 1440, aspect: '16:9' }, 120));
        runLoaded(s.meter(0), 20 * MIN);
        s.end();
        const next = session();
        expect(next.budget).toBe(450e6);
        expect(next.trial).toBe(false);
    });

    it('costs a rung to a sick stream that ran at it', () => {
        const { session, memory } = rig('on');
        memory.raise(HOST, CODEC);
        memory.raise(HOST, CODEC);
        const s = session();
        s.weigh(uhdAuto);
        s.meter(0).noteSick('worker_crash', true);
        expect(session().budget).toBe(330e6);
    });

    it('costs nothing to a sick stream that asked less than the rung below', () => {
        const { session, memory } = rig('on');
        memory.raise(HOST, CODEC);
        memory.raise(HOST, CODEC);
        const s = session();
        s.weigh(() => streamFootprint({ height: 720, aspect: '16:9' }, 60));
        s.meter(0).noteSick('worker_crash', true);
        expect(session().budget).toBe(450e6);
    });

    it('costs nothing to a stream a named half took past the rung', () => {
        const { session, memory } = rig('on');
        memory.raise(HOST, CODEC);
        const s = session();
        s.weigh(() => streamFootprint({ height: 2160, aspect: '16:9' }, 120));
        s.meter(0).noteSick('worker_crash', true);
        expect(session().budget).toBe(330e6);
    });

    it('builds no meter for a stream already sick: a relaunch proves nothing', () => {
        const { session } = rig('on');
        const s = session();
        s.weigh(uhdAuto);
        s.meter(0).noteSick('codec_fallback', true);
        expect(s.meter(0)).toBeNull();
    });
});

describe('AutoCeilingSession: a stream that never came back', () => {
    const atRung = () => {
        const r = rig('on');
        r.memory.raise(HOST, CODEC);
        r.memory.raise(HOST, CODEC);
        return r;
    };

    it('costs a rung at the next launch, and says so', () => {
        const { session, log } = atRung();
        const s = session();
        s.weigh(uhdAuto);
        s.meter(0);
        const next = session();
        expect(next.budget).toBe(330e6);
        expect(log.join('\n')).toContain('never came back');
    });

    it('costs nothing when the host refused the launch: no stream ever came up', () => {
        const { session } = atRung();
        session().weigh(uhdAuto);
        expect(session().budget).toBe(450e6);
    });

    it('costs nothing when the stream ended', () => {
        const { session } = atRung();
        const s = session();
        s.weigh(uhdAuto);
        s.meter(0);
        s.end();
        expect(session().budget).toBe(450e6);
    });

    it('costs nothing when the page went away, or out of sight and was killed there', () => {
        const { session } = atRung();
        const s = session();
        s.weigh(uhdAuto);
        s.meter(0);
        s.pause();
        expect(session().budget).toBe(450e6);
    });

    it('counts again once the page is back', () => {
        const { session } = atRung();
        const s = session();
        s.weigh(uhdAuto);
        s.meter(0);
        s.pause();
        s.resume();
        expect(session().budget).toBe(330e6);
    });

    it('opens nothing for a stream that says nothing of its rung', () => {
        const { session } = atRung();
        const s = session();
        s.weigh(() => streamFootprint({ height: 720, aspect: '16:9' }, 60));
        s.meter(0);
        expect(session().budget).toBe(450e6);
    });
});
