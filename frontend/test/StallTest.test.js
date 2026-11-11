/*
 * MoonlightWeb — browser-based Sunshine/GameStream client.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 */

/**
 * StallTest — a stall of the link played by the page (debug only).
 */
import { describe, it, expect } from 'vitest';
import {
    StallTest,
    parseStallTest,
    STALL_MIN_HOLD_MS,
    STALL_MAX_HOLD_MS,
} from '../js/stream/StallTest.js';

/** Frames at 60 fps from @p fromMs to @p toMs, admitted one by one. */
function play(t, fromMs, toMs, first = 0) {
    const handed = [];
    let id = first;
    for (let now = fromMs; now < toMs; now += 16) handed.push(...t.admit(id++, now));
    return { handed, next: id };
}

describe('parseStallTest', () => {
    it('is off when the setting is absent', () => {
        expect(parseStallTest(null)).toBe(null);
        expect(parseStallTest(undefined)).toBe(null);
        expect(parseStallTest('')).toBe(null);
    });

    it('reads "everyMs,holdMs"', () => {
        expect(parseStallTest('10000,1500')).toEqual({ everyMs: 10000, holdMs: 1500 });
        expect(parseStallTest(' 8000 , 900 ')).toEqual({ everyMs: 8000, holdMs: 900 });
    });

    it('refuses what makes no sense', () => {
        expect(parseStallTest('10000')).toBe(null);
        expect(parseStallTest('a,b')).toBe(null);
        expect(parseStallTest('10000,1500,3')).toBe(null);
        expect(parseStallTest('100000,' + (STALL_MIN_HOLD_MS - 1))).toBe(null);
        expect(parseStallTest('100000,' + (STALL_MAX_HOLD_MS + 1))).toBe(null);
        // A stall every 2 s that lasts 1.5 s leaves no stream between two.
        expect(parseStallTest('2000,1500')).toBe(null);
    });
});

describe('StallTest', () => {
    it('hands every frame on at once until the first stall is due', () => {
        const t = new StallTest({ everyMs: 1000, holdMs: 300 });
        const { handed } = play(t, 0, 990);
        expect(handed).toEqual([...Array(handed.length).keys()]);
        expect(t.stalls).toBe(0);
        expect(t.holding).toBe(false);
    });

    it('holds the frames for the stall, then hands them all on in order, none lost', () => {
        const t = new StallTest({ everyMs: 1000, holdMs: 300 });
        const before = play(t, 0, 1000);
        // From 1000 ms on, the frames are held.
        const during = play(t, 1000, 1300, before.next);
        expect(t.holding).toBe(true);
        expect(t.stalls).toBe(1);
        expect(during.handed).toEqual([]);
        // The first frame after the stall brings the whole burst with it.
        const burst = t.admit(during.next, 1310);
        const ids = [];
        for (let id = before.next; id <= during.next; id++) ids.push(id);
        expect(burst).toEqual(ids);
        expect(t.lastBurst.frames).toBe(burst.length);
        expect(t.lastBurst.heldMs).toBeGreaterThanOrEqual(300);
        expect(t.holding).toBe(false);
    });

    it('plays the next stall a full period after the burst', () => {
        const t = new StallTest({ everyMs: 1000, holdMs: 300 });
        play(t, 0, 1000);
        play(t, 1000, 1300);
        t.admit(-1, 1310);
        // Free again until 2310.
        const free = play(t, 1320, 2300, 100);
        expect(free.handed.length).toBe(free.next - 100);
        expect(t.stalls).toBe(1);
        expect(t.admit(999, 2320)).toEqual([]);
        expect(t.stalls).toBe(2);
    });

    it('ends a stall early rather than hold more than its ceiling', () => {
        const t = new StallTest({ everyMs: 1000, holdMs: 5000, maxHeld: 10 });
        play(t, 0, 1000);
        const handed = [];
        for (let i = 0; i < 10; i++) handed.push(...t.admit(i, 1000 + i));
        expect(handed).toEqual([]);
        const burst = t.admit(10, 1010);
        expect(burst).toEqual([0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10]);
        expect(t.holding).toBe(false);
    });

    it('keeps the frames themselves, untouched', () => {
        const t = new StallTest({ everyMs: 100, holdMs: 50 });
        const a = { data: new Uint8Array([1]), isKeyframe: false };
        const b = { data: new Uint8Array([2]), isKeyframe: true };
        t.admit({}, 0);
        expect(t.admit(a, 100)).toEqual([]);
        const out = t.admit(b, 160);
        expect(out[0]).toBe(a);
        expect(out[1]).toBe(b);
    });
});
