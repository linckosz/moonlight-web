/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { afterEach, describe, expect, it, vi } from 'vitest';
import { GpuNudge, nudgeMode } from '../js/stream/ultra/GpuNudge.js';

const sleep = (ms) => new Promise((r) => globalThis.setTimeout(r, ms));

function device() {
    const d = { submits: 0, queue: { submit: () => d.submits++ } };
    return d;
}

afterEach(() => {
    globalThis.localStorage.removeItem('mw_ultra_nudge');
});

describe('GpuNudge', () => {
    it('is auto by default, off by its key', () => {
        expect(nudgeMode()).toBe('auto');
        globalThis.localStorage.setItem('mw_ultra_nudge', '1');
        expect(nudgeMode()).toBe('auto');
        globalThis.localStorage.setItem('mw_ultra_nudge', 'auto');
        expect(nudgeMode()).toBe('auto');
        globalThis.localStorage.setItem('mw_ultra_nudge', 'all');
        expect(nudgeMode()).toBe('all');
        for (const off of ['0', 'off', '']) {
            globalThis.localStorage.setItem('mw_ultra_nudge', off);
            expect(nudgeMode()).toBe(null);
        }
    });

    it('submits empty work while a frame waits, and stops at its end', async () => {
        const d = device();
        const n = new GpuNudge(d, 'all');
        const t1 = globalThis.performance.now();
        const w = n.begin(t1);
        await sleep(5);
        expect(d.submits).toBeGreaterThan(0);
        const t2 = globalThis.performance.now();
        expect(n.end(t2)).toBe(w);
        expect(w.nudges).toBe(d.submits);
        expect(w.first).toBeGreaterThanOrEqual(t1);
        expect(w.spin).toBeGreaterThan(0);
        const after = d.submits;
        await sleep(5);
        expect(d.submits).toBe(after);
        expect(n.stats).toMatchObject({ waits: 1, nudges: after });
        n.destroy();
    });

    it('auto: starts a little before the end the last frames saw', () => {
        const n = new GpuNudge(device(), 'auto');
        // Too few frames known: from the submit.
        expect(n.lead()).toBe(0);
        for (const ms of [6, 5, 7, 6, 6, 8, 5.5, 6]) {
            n.begin(0);
            n.end(ms);
        }
        // The 10th percentile of submit → done (5), less the lead (1).
        expect(n.lead()).toBe(4);
        expect(new GpuNudge(device(), 'all').lead()).toBe(0);
        n.destroy();
    });

    it('a message of a wait already over does not start the next one early', async () => {
        const d = device();
        const n = new GpuNudge(d, 'auto');
        // Waits that end at once, their first message still on its way.
        for (let i = 0; i < 8; i++) {
            n.begin(0);
            n.end(10);
        }
        // Then a wait whose start is 9 ms away: nothing before its timer.
        n.begin(globalThis.performance.now());
        await sleep(3);
        expect(d.submits).toBe(0);
        n.end();
        n.destroy();
    });

    it('auto: a start further away than a timer can be late waits on a timer', async () => {
        vi.useFakeTimers({ toFake: ['setTimeout', 'clearTimeout', 'performance'] });
        try {
            const d = device();
            const n = new GpuNudge(d, 'auto');
            for (let i = 0; i < 8; i++) {
                n.begin(0);
                n.end(10);
            }
            const timers = n.stats.timers;
            const t1 = globalThis.performance.now();
            const w = n.begin(t1);
            expect(w.from).toBe(t1 + 9);
            expect(n.stats.timers).toBe(timers + 1);
            // A failed wait (no end time) stops the timer and is not learned.
            expect(n.end()).toBe(w);
            expect(n.stats.waits).toBe(8);
            n.destroy();
        } finally {
            vi.useRealTimers();
        }
    });
});
