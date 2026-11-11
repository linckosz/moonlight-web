/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 *
 * One launch's dealings with the learned ceiling. The promises: a journal-only
 * session writes nothing and spends nothing more; only what was asked within a
 * rung says anything of it; what is earned serves the next launch and never this
 * one; a stream that never came back costs a rung, a page closed does not; the
 * host's first crash at a rung costs nothing, its second there does.
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
const uhdAuto = (budget) => streamFootprint({ height: heights[budget / 1e6], aspect: '16:9' }, 120);

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
        s.meter(0).noteSick('decoder_silent', true);
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

describe('AutoCeilingSession: a host that crashed', () => {
    /** A rig whose host and codec stand at 450 M. */
    function at450() {
        const r = rig('on');
        r.memory.raise(HOST, CODEC);
        r.memory.raise(HOST, CODEC);
        return r;
    }

    /** One stream whose host's worker crashes. */
    function crashed(r) {
        const s = r.session();
        s.weigh(uhdAuto);
        s.meter(0).noteSick('worker_crash', true);
        s.end();
        return s;
    }

    it('costs nothing the first time at a rung, and says so', () => {
        const r = at450();
        const s = crashed(r);
        expect(s.sick).toBe('worker_crash');
        expect(r.session().budget).toBe(450e6);
        expect(r.log.join('\n')).toContain(
            "sick (worker_crash), 0 s loaded, at 450 M — the host's first crash at it, not counted",
        );
    });

    it('costs the rung on a second crash at it, in another stream', () => {
        const r = at450();
        crashed(r);
        crashed(r);
        expect(r.session().budget).toBe(330e6);
        expect(r.log.join('\n')).toContain(
            "at 450 M — down to 330 M, the host's second crash at it",
        );
    });

    it('takes two views of one stream for one crash', () => {
        const r = at450();
        const s = r.session();
        s.weigh(uhdAuto);
        const a = s.meter(0);
        const b = s.meter(0);
        a.noteSick('worker_crash', true);
        b.noteSick('worker_crash', true);
        expect(r.session().budget).toBe(450e6);
    });

    it('starts the count over once a crash has cost the rung', () => {
        const r = at450();
        crashed(r);
        crashed(r);
        r.memory.raise(HOST, CODEC);
        crashed(r);
        expect(r.session().budget).toBe(450e6);
    });

    it('says nothing of a first crash at the base budget, where nothing is lost', () => {
        const r = rig('on');
        crashed(r);
        expect(r.session().budget).toBe(PIXEL_RATE_BUDGET);
        expect(r.log.join('\n')).not.toContain('first crash');
    });

    it('still costs a rung to what the client itself reports', () => {
        const r = at450();
        const s = r.session();
        s.weigh(uhdAuto);
        s.meter(0).noteSick('renderer_failed', true);
        expect(r.session().budget).toBe(330e6);
    });
});

describe('AutoCeilingSession: no codec stored', () => {
    it('is HEVC: the line says so, and the memory is the one HEVC earned', () => {
        const { session, memory, log } = rig('on', { codec: undefined });
        memory.raise(HOST, CODEC);
        const s = session();
        expect(s.budget).toBe(330e6);
        s.weigh(uhdAuto);
        expect(log.join('\n')).toContain('on hevc');
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

describe('AutoCeilingSession: the guard', () => {
    const atTop = () => {
        const r = rig('on');
        for (let i = 0; i < 5; i++) r.memory.raise(HOST, CODEC);
        return r;
    };

    it('comes down a rung for this launch on a "no", and keeps what was learned', async () => {
        const { session } = atTop();
        const s = session();
        const asked = [];
        await s.guard(
            uhdAuto,
            async (ask) => {
                asked.push(ask.height);
                return ask.height > 1440;
            },
            300,
        );
        expect(asked).toEqual([2160, 1808, 1528, 1440]);
        expect(s.budget).toBe(450e6);
        expect(s.trial).toBe(false);
        expect(session().budget).toBe(1000e6);
    });

    it('stays where it is on anything but a "no"', async () => {
        const { session } = atTop();
        const s = session();
        await s.guard(uhdAuto, async () => false, 300);
        expect(s.budget).toBe(1000e6);
    });

    it('never goes under the base budget, and never asks there', async () => {
        const { session } = rig('on');
        const s = session();
        let asked = 0;
        await s.guard(
            uhdAuto,
            async () => {
                asked++;
                return true;
            },
            300,
        );
        expect(asked).toBe(0);
        expect(s.budget).toBe(PIXEL_RATE_BUDGET);
    });

    it('does not ask about a stream the rung below would allow anyway', async () => {
        const { session } = atTop();
        const s = session();
        let asked = 0;
        await s.guard(
            () => streamFootprint({ height: 1080, aspect: '16:9' }, 60),
            async () => {
                asked++;
                return true;
            },
            300,
        );
        expect(asked).toBe(0);
        expect(s.budget).toBe(1000e6);
    });

    it('stops asking once the launch has waited long enough', async () => {
        const { session } = atTop();
        const s = session();
        let now = 0;
        const asked = [];
        await s.guard(
            uhdAuto,
            async (ask, left) => {
                asked.push(left);
                now += 200;
                return true;
            },
            300,
            () => now,
        );
        expect(asked).toEqual([300, 100]);
        expect(s.budget).toBe(500e6);
    });

    it('asks nothing of a session that does not learn', async () => {
        const { session } = rig('log');
        let asked = 0;
        await session().guard(
            uhdAuto,
            async () => {
                asked++;
                return true;
            },
            300,
        );
        expect(asked).toBe(0);
    });
});

describe('AutoCeilingSession: a launch the host refused', () => {
    it('costs a rung on trial, and asks for one more try', () => {
        const { session, memory } = rig('on');
        memory.raise(HOST, CODEC);
        expect(session().refused()).toBe(true);
        expect(session().budget).toBe(PIXEL_RATE_BUDGET);
    });

    it('costs nothing at a rung already held', () => {
        const { session, memory } = rig('on');
        memory.raise(HOST, CODEC);
        memory.hold(HOST, CODEC);
        expect(session().refused()).toBe(false);
        expect(session().budget).toBe(330e6);
    });

    it('costs nothing in journal mode', () => {
        const { session, storage } = rig('log');
        expect(session().refused()).toBe(false);
        expect(storage.written()).toBe(false);
    });
});

describe('AutoCeilingSession: what it tells the view', () => {
    it('bounds the cadence at twice the rung, and at the top rung', () => {
        const { session, memory } = rig('on');
        expect(session().stepCeiling).toBe(500e6);
        for (let i = 0; i < 5; i++) memory.raise(HOST, CODEC);
        expect(session().stepCeiling).toBe(1000e6);
    });

    it('bounds nothing while it does not learn', () => {
        expect(rig('log').session().stepCeiling).toBe(0);
    });

    it('is on trial only for a rung yet to be held', () => {
        const { session, memory } = rig('on');
        expect(session().onTrial).toBe(false);
        memory.raise(HOST, CODEC);
        expect(session().onTrial).toBe(true);
        memory.hold(HOST, CODEC);
        expect(session().onTrial).toBe(false);
        expect(rig('log').session().onTrial).toBe(false);
    });
});

describe('AutoCeilingSession: the line on the stats card', () => {
    const at1440 = () => streamFootprint({ height: 1440, aspect: '16:9' }, 120);

    it('names what Auto chose, on trial until the rung has held', () => {
        const { session, memory } = rig('on');
        memory.raise(HOST, CODEC);
        memory.raise(HOST, CODEC);
        const s = session();
        s.weigh(at1440, true);
        expect(s.statsLine).toEqual({ size: '1440p120', trial: true });
        memory.hold(HOST, CODEC);
        const held = session();
        held.weigh(at1440, true);
        expect(held.statsLine).toEqual({ size: '1440p120', trial: false });
    });

    it('turns to held during the stream that proved the rung', () => {
        const { session, memory } = rig('on');
        memory.raise(HOST, CODEC);
        memory.raise(HOST, CODEC);
        const s = session();
        s.weigh(uhdAuto, true);
        runLoaded(s.meter(0), 5 * MIN);
        expect(s.statsLine.trial).toBe(false);
    });

    it('stays away at the base budget, when the viewer named both, and in journal mode', () => {
        const base = rig('on').session();
        base.weigh(at1440, true);
        expect(base.statsLine).toBeNull();

        const named = rig('on');
        named.memory.raise(HOST, CODEC);
        const s = named.session();
        s.weigh(at1440, false);
        expect(s.statsLine).toBeNull();

        const journal = rig('log');
        journal.memory.raise(HOST, CODEC);
        const j = journal.session();
        j.weigh(at1440, true);
        expect(j.statsLine).toBeNull();
    });

    it('stays away when the guard has brought this launch back to the base budget', async () => {
        const { session, memory } = rig('on');
        memory.raise(HOST, CODEC);
        const s = session();
        await s.guard(uhdAuto, async () => true, 300);
        s.weigh(uhdAuto, true);
        expect(s.statsLine).toBeNull();
    });
});
