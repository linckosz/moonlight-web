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
 * EnhancerGovernor — observable behaviour: which algo the ladder asks for after
 * a sequence of observation windows. The first suite replays the five real
 * measurements this policy was calibrated on (4K client, FSR1, HEVC), so a
 * threshold change that would start degrading a healthy stream fails here.
 */
import { describe, it, expect } from 'vitest';
import { EnhancerGovernor } from '../js/stream/EnhancerGovernor.js';

/** Feed the same window repeatedly, 500ms apart (the real posting cadence). */
function feed(gov, obs, seconds, startMs = 0) {
    const steps = [];
    let now = startMs;
    for (let i = 0; i < seconds * 2; i++) {
        now += 500;
        const algo = gov.update({ serviceMs: obs.serviceMs, arrivalMs: obs.arrivalMs, now });
        if (algo) steps.push({ algo, now });
    }
    return steps;
}

/**
 * Same, but the service time follows the level actually running — which is the
 * whole point of the ladder: stepping down is supposed to make the draw
 * cheaper. Feeding a fixed cost would model a ladder that changes nothing.
 */
function run(gov, costByLevel, seconds, startMs = 0, arrivalMs = 16.7) {
    const steps = [];
    let now = startMs;
    for (let i = 0; i < seconds * 2; i++) {
        now += 500;
        const algo = gov.update({ serviceMs: costByLevel[gov.level], arrivalMs, now });
        if (algo) steps.push({ algo, now });
    }
    return { steps, now };
}

// Cost of one draw per level, in ms, down the ladder fsr1 → nis → sgsr → off.
// Under a call the GPU is contended and FSR1 (3 passes) no longer fits a
// 16.7ms frame; NIS (one pass, heavier taps) does, and so does SGSR below it.
const CONTENDED = { fsr1: 19.5, nis: 12, sgsr: 9, off: 3 };
// Recovery needs the running level under 0.4 × 16.7 = 6.7ms.
const IDLE = { fsr1: 10.3, nis: 6, sgsr: 5, off: 2 };

describe('EnhancerGovernor — measured scenarios', () => {
    // Each: [name, render service time, arrival avg] from the captured [perf] lines.
    const healthy = [
        ['video paused, 27fps', 10.1, 38.0],
        ['video playing, 60fps', 10.3, 16.8],
        ['mouse moving, 57fps', 10.9, 17.9],
        ['Teams call, static 22fps', 10.2, 47.0],
    ];

    for (const [name, serviceMs, arrivalMs] of healthy) {
        it(`holds FSR1 — ${name}`, () => {
            const gov = new EnhancerGovernor('fsr1');
            expect(feed(gov, { serviceMs, arrivalMs }, 60)).toEqual([]);
            expect(gov.level).toBe('fsr1');
            expect(gov.degraded).toBe(false);
        });
    }

    it('steps down under a Teams call at 60fps (19.5ms wait for a 16.7ms budget)', () => {
        const gov = new EnhancerGovernor('fsr1');
        const steps = feed(gov, { serviceMs: 19.5, arrivalMs: 16.7 }, 10);
        expect(steps[0].algo).toBe('nis');
        expect(gov.degraded).toBe(true);
    });
});

describe('EnhancerGovernor', () => {
    it('never climbs above the user setting', () => {
        const gov = new EnhancerGovernor('sgsr');
        expect(feed(gov, { serviceMs: 0.5, arrivalMs: 16.7 }, 120)).toEqual([]);
        expect(gov.level).toBe('sgsr');
    });

    it('walks the whole ladder down but stops at off', () => {
        const gov = new EnhancerGovernor('fsr1');
        const steps = feed(gov, { serviceMs: 40, arrivalMs: 16.7 }, 60);
        expect(steps.map((s) => s.algo)).toEqual(['nis', 'sgsr', 'off']);
        expect(gov.level).toBe('off');
    });

    it('needs the verdict to hold — a single busy window changes nothing', () => {
        const gov = new EnhancerGovernor('fsr1');
        let now = 0;
        expect(gov.update({ serviceMs: 40, arrivalMs: 16.7, now: (now += 500) })).toBe(null);
        expect(gov.update({ serviceMs: 5, arrivalMs: 16.7, now: (now += 500) })).toBe(null);
        expect(gov.update({ serviceMs: 40, arrivalMs: 16.7, now: (now += 500) })).toBe(null);
        expect(gov.level).toBe('fsr1');
    });

    it('stops at the first level that fits, instead of falling to off', () => {
        const gov = new EnhancerGovernor('fsr1');
        const { steps } = run(gov, CONTENDED, 120);
        expect(steps.map((s) => s.algo)).toEqual(['nis']);
        expect(gov.level).toBe('nis');
    });

    it('climbs back to the setting once the pressure is gone', () => {
        const gov = new EnhancerGovernor('fsr1');
        const call = run(gov, CONTENDED, 60);
        expect(gov.level).toBe('nis');

        const after = run(gov, IDLE, 60, call.now);
        expect(after.steps.map((s) => s.algo)).toEqual(['fsr1']);
        expect(gov.degraded).toBe(false);
    });

    it('backs off instead of flip-flopping when the level above never fits', () => {
        // Pathological: NIS looks cheap enough to justify a restore, FSR1 is
        // always too expensive — every restore is immediately undone.
        const gov = new EnhancerGovernor('fsr1');
        const { steps } = run(gov, { fsr1: 19.5, nis: 6, sgsr: 6, off: 2 }, 300);
        const restores = steps.filter((s) => s.algo === 'fsr1');
        // A fixed 15s retry would have burnt ~19 restores over five minutes.
        expect(restores.length).toBeLessThanOrEqual(4);
        // …and each attempt waits longer than the previous one.
        const gaps = restores.slice(1).map((s, i) => s.now - restores[i].now);
        for (let i = 1; i < gaps.length; i++) expect(gaps[i]).toBeGreaterThan(gaps[i - 1]);
        expect(gov.level).toBe('nis');
    });

    it('ignores the window right after a switch (it describes the old level)', () => {
        const gov = new EnhancerGovernor('fsr1');
        // One expensive burst, long enough to degrade once and no more: the
        // stale samples that follow must not walk the ladder down again.
        const steps = feed(gov, { serviceMs: 40, arrivalMs: 16.7 }, 3);
        expect(steps.map((s) => s.algo)).toEqual(['nis']);
    });

    it('decides nothing without a usable frame budget', () => {
        const gov = new EnhancerGovernor('fsr1');
        expect(feed(gov, { serviceMs: 40, arrivalMs: 0 }, 60)).toEqual([]);
        expect(feed(gov, { serviceMs: 40, arrivalMs: 5000 }, 60)).toEqual([]);
        expect(gov.level).toBe('fsr1');
    });

    it('treats an unknown setting as the top of the ladder', () => {
        const gov = new EnhancerGovernor(undefined);
        expect(gov.level).toBe('fsr1');
    });
});

/**
 * The profile an invited player streams under. Their session is fixed at 60fps
 * and they chose none of it, so the promise is a flat cost cap rather than a
 * share of a frame budget that depends on their own link.
 */
describe('EnhancerGovernor — invited player profile', () => {
    const PLAYER = { fixedBudgetMs: 8, warmupMs: 10000, noRecovery: true };

    it('ignores the first 10 seconds, however expensive', () => {
        const gov = new EnhancerGovernor('sgsr', PLAYER);
        // 20ms a frame is way over the cap, but this is shader compilation and
        // a cold pipeline — not the steady cost.
        expect(feed(gov, { serviceMs: 20, arrivalMs: 16.7 }, 9)).toEqual([]);
        expect(gov.level).toBe('sgsr');
    });

    it('drops SGSR once it costs more than 8ms past the warm-up', () => {
        const gov = new EnhancerGovernor('sgsr', PLAYER);
        const steps = feed(gov, { serviceMs: 9, arrivalMs: 16.7 }, 20);
        expect(steps.map((s) => s.algo)).toEqual(['off']);
        expect(gov.degraded).toBe(true);
    });

    it('holds SGSR at a cost the owner ladder would also accept', () => {
        const gov = new EnhancerGovernor('sgsr', PLAYER);
        // 6ms fits the 8ms cap; the adaptive rule (0.8 x 16.7 = 13.4ms) would
        // hold it too — this is the case where the two agree.
        expect(feed(gov, { serviceMs: 6, arrivalMs: 16.7 }, 60)).toEqual([]);
        expect(gov.level).toBe('sgsr');
    });

    it('applies the cap where the adaptive budget would not have', () => {
        const gov = new EnhancerGovernor('sgsr', PLAYER);
        // 11ms is under 0.8 x 16.7, so the owner's governor holds — but the
        // player was promised 8ms.
        const steps = feed(gov, { serviceMs: 11, arrivalMs: 16.7 }, 20);
        expect(steps.map((s) => s.algo)).toEqual(['off']);

        const owner = new EnhancerGovernor('sgsr');
        expect(feed(owner, { serviceMs: 11, arrivalMs: 16.7 }, 20)).toEqual([]);
    });

    it('never comes back once dropped', () => {
        const gov = new EnhancerGovernor('sgsr', PLAYER);
        feed(gov, { serviceMs: 12, arrivalMs: 16.7 }, 20);
        expect(gov.level).toBe('off');
        // Idle GPU for five minutes: predictable beats optimal for a guest.
        expect(feed(gov, { serviceMs: 0.5, arrivalMs: 16.7 }, 300, 40000)).toEqual([]);
        expect(gov.level).toBe('off');
    });
});
