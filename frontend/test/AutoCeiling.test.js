/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 *
 * What a browser has earned from a host. The promises: nothing learned means
 * the base budget, one rung at a time up and down, a lost rung is hard to
 * regain but never out of reach for good, and a broken memory costs nothing.
 */
import { describe, it, expect } from 'vitest';
import {
    AUTO_CEILING_KEY,
    AUTO_LEARN_KEY,
    CEILING_RUNGS,
    ENTRIES_KEPT,
    FAIL_TTL_MS,
    HANDHELD_MAX_CEILING,
    HOLD_BASE_MS,
    HOLD_MAX_MS,
    autoCeilingKey,
    autoCeilingMemory,
    autoLearnMode,
    deviceMaxCeiling,
    holdMsFor,
    previewCeiling,
    rungAbove,
    rungBelow,
} from '../js/util/AutoCeiling.js';
import { PIXEL_RATE_BUDGET } from '../js/util/StreamResolution.js';

/** A Storage that answers like the browser's, kept in a Map. */
function fakeStorage(initial) {
    const map = new Map(Object.entries(initial || {}));
    return {
        getItem: (k) => (map.has(k) ? map.get(k) : null),
        setItem: (k, v) => map.set(k, String(v)),
        removeItem: (k) => map.delete(k),
        raw: () => JSON.parse(map.get(AUTO_CEILING_KEY)),
    };
}

const HOST = 'host-uuid';
const CODEC = 'hevc';

function memory(initial) {
    const storage = fakeStorage(initial);
    const clock = { now: 1000000 };
    return { storage, clock, mem: autoCeilingMemory(storage, () => clock.now) };
}

describe('the rungs', () => {
    it('start on the budget, end at 4K120, and never jump more than ×1.43', () => {
        expect(CEILING_RUNGS[0]).toBe(PIXEL_RATE_BUDGET);
        expect(CEILING_RUNGS[CEILING_RUNGS.length - 1]).toBeGreaterThanOrEqual(3840 * 2160 * 120);
        for (let i = 1; i < CEILING_RUNGS.length; i++)
            expect(CEILING_RUNGS[i] / CEILING_RUNGS[i - 1]).toBeLessThanOrEqual(1.43);
    });

    it('name their neighbours, and stop at both ends', () => {
        expect(rungAbove(PIXEL_RATE_BUDGET)).toBe(330e6);
        expect(rungBelow(330e6)).toBe(PIXEL_RATE_BUDGET);
        expect(rungBelow(PIXEL_RATE_BUDGET)).toBe(0);
        expect(rungAbove(1000e6)).toBe(0);
    });
});

describe('deviceMaxCeiling', () => {
    it('lets a computer reach the top', () => {
        expect(deviceMaxCeiling({})).toBe(1000e6);
    });

    it('stops a phone or a tablet at 450 M', () => {
        expect(deviceMaxCeiling({ handheld: true })).toBe(HANDHELD_MAX_CEILING);
        expect(HANDHELD_MAX_CEILING).toBe(450e6);
    });

    it('holds a TV, a small memory and Power Saving at the base budget', () => {
        for (const d of [{ tv: true }, { lowMemory: true }, { powerSave: true }])
            expect(deviceMaxCeiling(d)).toBe(PIXEL_RATE_BUDGET);
        expect(deviceMaxCeiling({ handheld: true, powerSave: true })).toBe(PIXEL_RATE_BUDGET);
    });
});

describe('autoLearnMode', () => {
    it('reads the switch, and learns when it says nothing', () => {
        expect(autoLearnMode(fakeStorage())).toBe('on');
        expect(autoLearnMode(fakeStorage({ [AUTO_LEARN_KEY]: '0' }))).toBe('off');
        expect(autoLearnMode(fakeStorage({ [AUTO_LEARN_KEY]: '1' }))).toBe('on');
        expect(autoLearnMode(fakeStorage({ [AUTO_LEARN_KEY]: 'log' }))).toBe('log');
        expect(autoLearnMode(null)).toBe('on');
    });
});

describe('holdMsFor', () => {
    it('doubles with each failure, up to its maximum', () => {
        expect(holdMsFor(0)).toBe(HOLD_BASE_MS);
        expect(holdMsFor(1)).toBe(2 * HOLD_BASE_MS);
        expect(holdMsFor(2)).toBe(4 * HOLD_BASE_MS);
        expect(holdMsFor(3)).toBe(HOLD_MAX_MS);
        expect(holdMsFor(9)).toBe(HOLD_MAX_MS);
    });
});

describe('autoCeilingMemory', () => {
    it('knows nothing of a new host: the base budget, no trial', () => {
        const { mem } = memory();
        expect(mem.entry(HOST, CODEC)).toEqual({
            ceiling: PIXEL_RATE_BUDGET,
            trial: false,
            crashed: false,
            holdMs: HOLD_BASE_MS,
        });
    });

    it('climbs one rung at a time, each one a trial until held', () => {
        const { mem } = memory();
        expect(mem.raise(HOST, CODEC)).toBe(330e6);
        expect(mem.entry(HOST, CODEC)).toMatchObject({ ceiling: 330e6, trial: true });
        mem.hold(HOST, CODEC);
        expect(mem.entry(HOST, CODEC).trial).toBe(false);
        expect(mem.raise(HOST, CODEC)).toBe(450e6);
        expect(mem.entry(HOST, CODEC)).toMatchObject({ ceiling: 450e6, trial: true });
    });

    it('stops at the top, and at what this device may reach', () => {
        const { mem } = memory();
        for (const r of CEILING_RUNGS.slice(1)) expect(mem.raise(HOST, CODEC)).toBe(r);
        expect(mem.raise(HOST, CODEC)).toBe(0);
        const phone = memory().mem;
        expect(phone.raise(HOST, CODEC, 450e6)).toBe(330e6);
        expect(phone.raise(HOST, CODEC, 450e6)).toBe(450e6);
        expect(phone.raise(HOST, CODEC, 450e6)).toBe(0);
    });

    it('answers under what this device may reach, whatever was learned', () => {
        const { mem } = memory();
        for (let i = 0; i < 3; i++) mem.raise(HOST, CODEC);
        expect(mem.entry(HOST, CODEC).ceiling).toBe(500e6);
        expect(mem.entry(HOST, CODEC, 450e6).ceiling).toBe(450e6);
        expect(mem.entry(HOST, CODEC, PIXEL_RATE_BUDGET)).toMatchObject({
            ceiling: PIXEL_RATE_BUDGET,
            trial: false,
        });
    });

    it('keeps each host and each codec apart', () => {
        const { mem } = memory();
        mem.raise(HOST, 'hevc');
        expect(mem.entry(HOST, 'hevc').ceiling).toBe(330e6);
        expect(mem.entry(HOST, 'h264').ceiling).toBe(PIXEL_RATE_BUDGET);
        expect(mem.entry('other', 'hevc').ceiling).toBe(PIXEL_RATE_BUDGET);
    });

    it('falls one rung on a failure, and never under the base budget', () => {
        const { mem } = memory();
        mem.raise(HOST, CODEC);
        mem.raise(HOST, CODEC);
        expect(mem.fail(HOST, CODEC)).toBe(330e6);
        // What it fell back on is what held: not a trial.
        expect(mem.entry(HOST, CODEC)).toMatchObject({ ceiling: 330e6, trial: false });
        expect(mem.fail(HOST, CODEC)).toBe(PIXEL_RATE_BUDGET);
        expect(mem.fail(HOST, CODEC)).toBe(0);
        expect(mem.entry(HOST, CODEC).ceiling).toBe(PIXEL_RATE_BUDGET);
    });

    it('asks twice the hold to regain a lost rung, each time, up to its maximum', () => {
        const { mem } = memory();
        const holds = [];
        for (let i = 0; i < 5; i++) {
            mem.raise(HOST, CODEC);
            mem.fail(HOST, CODEC);
            holds.push(mem.entry(HOST, CODEC).holdMs);
        }
        expect(holds).toEqual([
            2 * HOLD_BASE_MS,
            4 * HOLD_BASE_MS,
            HOLD_MAX_MS,
            HOLD_MAX_MS,
            HOLD_MAX_MS,
        ]);
    });

    it('forgets a failure after a week: the memory never sticks for good', () => {
        const { mem, clock } = memory();
        mem.raise(HOST, CODEC);
        mem.fail(HOST, CODEC);
        expect(mem.entry(HOST, CODEC).holdMs).toBe(2 * HOLD_BASE_MS);
        clock.now += FAIL_TTL_MS;
        expect(mem.entry(HOST, CODEC).holdMs).toBe(HOLD_BASE_MS);
    });

    it('forgets the failures of a rung once it has held', () => {
        const { mem } = memory();
        mem.raise(HOST, CODEC);
        mem.fail(HOST, CODEC);
        mem.raise(HOST, CODEC);
        mem.hold(HOST, CODEC);
        mem.fail(HOST, CODEC);
        // One failure on record: the first was wiped when the rung held.
        expect(mem.entry(HOST, CODEC).holdMs).toBe(2 * HOLD_BASE_MS);
    });

    it('lets the host crash once at a rung, and counts the second there', () => {
        const { mem } = memory();
        expect(mem.hostCrash(HOST, CODEC, 450e6)).toBe(false);
        expect(mem.hostCrash(HOST, CODEC, 330e6)).toBe(false);
        expect(mem.hostCrash(HOST, CODEC, 450e6)).toBe(true);
        // The count starts over at that rung; the other one still waits.
        expect(mem.hostCrash(HOST, CODEC, 450e6)).toBe(false);
        expect(mem.hostCrash(HOST, CODEC, 330e6)).toBe(true);
        // Remembering costs nothing by itself.
        expect(mem.entry(HOST, CODEC).holdMs).toBe(HOLD_BASE_MS);
    });

    it('forgets a host crash after a week, and keeps none at the base budget', () => {
        const { mem, clock, storage } = memory();
        mem.hostCrash(HOST, CODEC, 450e6);
        clock.now += FAIL_TTL_MS;
        expect(mem.hostCrash(HOST, CODEC, 450e6)).toBe(false);
        expect(mem.hostCrash(HOST, CODEC, PIXEL_RATE_BUDGET)).toBe(false);
        expect(mem.hostCrash(HOST, CODEC, PIXEL_RATE_BUDGET)).toBe(false);
        expect(storage.raw().hosts[HOST + '|' + CODEC].crashes).toEqual({
            [450e6]: clock.now,
        });
    });

    it('stores no list of crashes once none is left', () => {
        const { mem, storage } = memory();
        mem.hostCrash(HOST, CODEC, 450e6);
        mem.hostCrash(HOST, CODEC, 450e6);
        expect(storage.raw().hosts[HOST + '|' + CODEC]).not.toHaveProperty('crashes');
    });

    it('climbs once on one proof, even from two views of the same session', () => {
        const { mem } = memory();
        expect(mem.raise(HOST, CODEC, undefined, PIXEL_RATE_BUDGET)).toBe(330e6);
        expect(mem.raise(HOST, CODEC, undefined, PIXEL_RATE_BUDGET)).toBe(0);
        expect(mem.entry(HOST, CODEC).ceiling).toBe(330e6);
    });

    it('blames the rung the stream ran at, not the one it had just earned', () => {
        const { mem } = memory();
        mem.raise(HOST, CODEC);
        mem.hold(HOST, CODEC);
        // At 330 M the stream earns 450 M, then falls ill: 330 M failed.
        expect(mem.raise(HOST, CODEC, undefined, 330e6)).toBe(450e6);
        expect(mem.fail(HOST, CODEC, 330e6)).toBe(PIXEL_RATE_BUDGET);
        expect(mem.entry(HOST, CODEC)).toMatchObject({
            ceiling: PIXEL_RATE_BUDGET,
            trial: false,
            holdMs: 2 * HOLD_BASE_MS,
        });
    });

    it('leaves a trial it did not run open when an older rung holds', () => {
        const { mem } = memory();
        mem.raise(HOST, CODEC);
        mem.hold(HOST, CODEC, PIXEL_RATE_BUDGET);
        expect(mem.entry(HOST, CODEC).trial).toBe(true);
    });

    it('tells a stream that never came back from one that closed', () => {
        const { mem } = memory();
        mem.open(HOST, CODEC);
        expect(mem.entry(HOST, CODEC).crashed).toBe(true);
        mem.close(HOST, CODEC);
        expect(mem.entry(HOST, CODEC).crashed).toBe(false);
    });

    it('remembers the host launched last, for a preview that knows no host', () => {
        const { mem } = memory();
        expect(mem.lastCeiling()).toBe(PIXEL_RATE_BUDGET);
        mem.raise(HOST, CODEC);
        mem.open(HOST, CODEC);
        expect(mem.lastCeiling()).toBe(330e6);
        expect(mem.lastCeiling(PIXEL_RATE_BUDGET)).toBe(PIXEL_RATE_BUDGET);
        mem.noteLast('other', CODEC);
        expect(mem.lastCeiling()).toBe(PIXEL_RATE_BUDGET);
    });

    it('keeps the hosts used last, and no more than it says', () => {
        const { mem, clock } = memory();
        for (let i = 0; i < ENTRIES_KEPT + 3; i++) {
            clock.now += 1000;
            mem.raise('host-' + i, CODEC);
        }
        expect(mem.entry('host-0', CODEC).ceiling).toBe(PIXEL_RATE_BUDGET);
        expect(mem.entry('host-' + (ENTRIES_KEPT + 2), CODEC).ceiling).toBe(330e6);
    });

    it('stores what the plan says, readable on a bench', () => {
        const { mem, storage } = memory();
        mem.raise(HOST, CODEC);
        mem.open(HOST, CODEC);
        const raw = storage.raw();
        expect(raw.v).toBe(1);
        expect(raw.last).toBe(HOST + '|' + CODEC);
        expect(raw.hosts[HOST + '|' + CODEC]).toMatchObject({ ceiling: 330e6, trial: true });
    });

    it('reads a rung prepared by hand, snapped under what it says', () => {
        const prepared = {
            v: 1,
            last: '',
            hosts: { [HOST + '|' + CODEC]: { ceiling: 460000000, trial: false, openAt: 0 } },
        };
        const { mem } = memory({ [AUTO_CEILING_KEY]: JSON.stringify(prepared) });
        expect(mem.entry(HOST, CODEC).ceiling).toBe(450e6);
    });

    it('takes a broken memory or another version for an empty one', () => {
        for (const raw of ['{not json', '42', 'null', JSON.stringify({ v: 99, hosts: {} })]) {
            const { mem } = memory({ [AUTO_CEILING_KEY]: raw });
            expect(mem.entry(HOST, CODEC).ceiling).toBe(PIXEL_RATE_BUDGET);
            expect(mem.raise(HOST, CODEC)).toBe(330e6);
        }
    });

    it('answers the base budget when storage is refused', () => {
        const refused = {
            getItem() {
                throw new Error('denied');
            },
            setItem() {
                throw new Error('denied');
            },
        };
        const mem = autoCeilingMemory(refused, () => 1);
        expect(mem.entry(HOST, CODEC).ceiling).toBe(PIXEL_RATE_BUDGET);
        expect(mem.raise(HOST, CODEC)).toBe(330e6);
        expect(mem.entry(HOST, CODEC).ceiling).toBe(PIXEL_RATE_BUDGET);
        const none = autoCeilingMemory(null, () => 1);
        expect(none.entry(HOST, CODEC).ceiling).toBe(PIXEL_RATE_BUDGET);
    });
});

describe('previewCeiling', () => {
    const learned = (mode) => {
        const storage = fakeStorage(mode ? { [AUTO_LEARN_KEY]: mode } : {});
        const mem = autoCeilingMemory(storage, () => 1000);
        mem.raise(HOST, CODEC);
        mem.raise(HOST, CODEC);
        mem.noteLast(HOST, CODEC);
        return storage;
    };

    it('is the base budget when learning is off, or only measured', () => {
        expect(previewCeiling({}, learned())).toBe(450e6);
        expect(previewCeiling({}, learned('0'))).toBe(PIXEL_RATE_BUDGET);
        expect(previewCeiling({}, learned('log'))).toBe(PIXEL_RATE_BUDGET);
    });

    it('reads the host launched last, under what this device may reach', () => {
        expect(previewCeiling({}, learned('1'))).toBe(450e6);
        expect(previewCeiling({ handheld: true }, learned('1'))).toBe(450e6);
        expect(previewCeiling({ tv: true }, learned('1'))).toBe(PIXEL_RATE_BUDGET);
        expect(previewCeiling({ powerSave: true }, learned('1'))).toBe(PIXEL_RATE_BUDGET);
    });

    it('is the base budget when nothing was launched yet', () => {
        expect(previewCeiling({}, fakeStorage({ [AUTO_LEARN_KEY]: '1' }))).toBe(PIXEL_RATE_BUDGET);
    });
});

describe('one memory per machine behind the rendezvous', () => {
    // Every native host answers to one uuid, and the rendezvous gives them one origin.
    const machines = () => {
        const storage = fakeStorage({ [AUTO_LEARN_KEY]: '1' });
        return {
            storage,
            a: autoCeilingMemory(storage, () => 1000, 'machine-a'),
            b: autoCeilingMemory(storage, () => 1000, 'machine-b'),
        };
    };

    it('does not lend one machine what another has earned', () => {
        const { a, b } = machines();
        a.raise(HOST, CODEC);
        expect(a.entry(HOST, CODEC).ceiling).toBe(330e6);
        expect(b.entry(HOST, CODEC).ceiling).toBe(PIXEL_RATE_BUDGET);
    });

    it('does not make one machine pay for what another has failed', () => {
        const { a, b } = machines();
        a.raise(HOST, CODEC);
        b.raise(HOST, CODEC);
        b.fail(HOST, CODEC);
        expect(a.entry(HOST, CODEC).ceiling).toBe(330e6);
        expect(b.entry(HOST, CODEC).ceiling).toBe(PIXEL_RATE_BUDGET);
    });

    it('does not take a crash for another machine', () => {
        const { a, b } = machines();
        a.open(HOST, CODEC);
        expect(a.entry(HOST, CODEC).crashed).toBe(true);
        expect(b.entry(HOST, CODEC).crashed).toBe(false);
    });

    it('previews what was learned on the machine this page is on', () => {
        const { storage, a, b } = machines();
        a.raise(HOST, CODEC);
        a.noteLast(HOST, CODEC);
        expect(previewCeiling({}, storage, 'machine-a')).toBe(330e6);
        expect(previewCeiling({}, storage, 'machine-b')).toBe(PIXEL_RATE_BUDGET);
        b.noteLast(HOST, CODEC);
        expect(previewCeiling({}, storage, 'machine-a')).toBe(330e6);
    });

    it('keeps a direct address where it was, apart from the rest', () => {
        const { storage, a } = machines();
        const direct = autoCeilingMemory(storage, () => 1000, null);
        direct.raise(HOST, CODEC);
        expect(storage.raw().hosts[HOST + '|' + CODEC].ceiling).toBe(330e6);
        expect(a.entry(HOST, CODEC).ceiling).toBe(PIXEL_RATE_BUDGET);
        expect(autoCeilingKey('machine-a')).toBe(AUTO_CEILING_KEY + ':machine-a');
        expect(autoCeilingKey(null)).toBe(AUTO_CEILING_KEY);
    });
});
