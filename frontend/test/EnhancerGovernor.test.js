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
import { describe, it, expect, beforeEach } from 'vitest';
import {
    EnhancerGovernor,
    ladderRung,
    rendererAlgo,
    rememberedRung,
    rememberRung,
} from '../js/stream/EnhancerGovernor.js';

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

/** Same cadence as feed(), with a decode-queue depth on every window. */
function feedQueued(gov, obs, seconds, startMs = 0) {
    const steps = [];
    let now = startMs;
    for (let i = 0; i < seconds * 2; i++) {
        now += 500;
        const algo = gov.update({
            serviceMs: obs.serviceMs,
            arrivalMs: obs.arrivalMs,
            decodeQueue: obs.decodeQueue,
            now,
        });
        if (algo) steps.push({ algo, now });
    }
    return steps;
}

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

/**
 * The failure the draw timing cannot report: a WebGL pass that only queues its
 * work costs nothing to submit while the decoder starves behind it. Numbers
 * from an Arc A380 streaming 1440p120 HEVC with FSR1 on — serviceMs 0.2ms for
 * a 9ms budget, decodeQueueSize 12-14, two thirds of the frames lost.
 */
describe('EnhancerGovernor — a starved decoder behind a cheap draw', () => {
    const STARVED = { serviceMs: 0.2, arrivalMs: 9.0, decodeQueue: 13 };

    it('steps down although the draw says it fits', () => {
        const gov = new EnhancerGovernor('fsr1');
        // Same window without the queue: the old policy saw nothing at all.
        const blind = new EnhancerGovernor('fsr1');
        expect(feed(blind, { serviceMs: 0.2, arrivalMs: 9.0 }, 60)).toEqual([]);
        expect(blind.level).toBe('fsr1');

        const steps = feedQueued(gov, STARVED, 20);
        expect(steps[0].algo).toBe('nis');
        // The queue stays deep, so the ladder keeps walking down to off.
        expect(gov.level).toBe('off');
    });

    it('holds through a spike shorter than the sustain window', () => {
        const gov = new EnhancerGovernor('fsr1');
        expect(feedQueued(gov, STARVED, 1.5)).toEqual([]);
        expect(gov.level).toBe('fsr1');
    });

    it('leaves a queue that is merely busy alone', () => {
        const gov = new EnhancerGovernor('fsr1');
        // decQ 2 with a cheap draw is a healthy 120fps stream, not starvation.
        expect(feedQueued(gov, { serviceMs: 0.2, arrivalMs: 9.0, decodeQueue: 2 }, 60)).toEqual([]);
        expect(gov.level).toBe('fsr1');
    });

    it('does not climb back while the queue is still deep', () => {
        const gov = new EnhancerGovernor('fsr1');
        feedQueued(gov, STARVED, 20);
        expect(gov.level).toBe('off');
        // Draw cost says recover — the decoder says the GPU is still behind.
        expect(
            feedQueued(gov, { serviceMs: 0.2, arrivalMs: 9.0, decodeQueue: 6 }, 300, 20000),
        ).toEqual([]);
        expect(gov.level).toBe('off');
    });

    it('climbs back once the decoder is clear again', () => {
        const gov = new EnhancerGovernor('fsr1');
        feedQueued(gov, STARVED, 20);
        expect(gov.level).toBe('off');
        const steps = feedQueued(
            gov,
            { serviceMs: 0.2, arrivalMs: 9.0, decodeQueue: 0 },
            300,
            20000,
        );
        expect(steps.map((s) => s.algo)).toContain('sgsr');
    });

    it('stays out of it with the enhancer already off', () => {
        const gov = new EnhancerGovernor('off');
        // A backlog with nothing enhancing is not the ladder's problem.
        expect(feedQueued(gov, STARVED, 60)).toEqual([]);
        expect(gov.level).toBe('off');
    });
});

/**
 * The other failure the draw timing cannot report: a GPU that keeps UP with the
 * frame rate but runs BEHIND it. 2-CU Radeon iGPU, 1440p60, FSR1 WebGL: 60 fps
 * in and out, decoder queue empty, submission 0.3ms — and every picture ~29ms
 * late (click-to-photon 59.6ms, 30.8 without the enhancer, 32.6 with SGSR).
 * The fences say it: FSR1's work is still running when the next frame comes.
 */
describe('EnhancerGovernor — a GPU that keeps up, frames late', () => {
    // Share of draws still on the GPU at the next one, per running level.
    const BEHIND = { fsr1: 1.0, nis: 0.9, sgsr: 0.0, off: 0.0 };

    function runGpu(gov, behindByLevel, seconds, startMs = 0) {
        const steps = [];
        let now = startMs;
        for (let i = 0; i < seconds * 2; i++) {
            now += 500;
            const algo = gov.update({
                serviceMs: 0.3,
                arrivalMs: 16.7,
                decodeQueue: 0,
                gpuBehind: behindByLevel[gov.level],
                now,
            });
            if (algo) steps.push({ algo, now });
        }
        return { steps, now };
    }

    it('steps down to the pass the GPU finishes in time', () => {
        const gov = new EnhancerGovernor('fsr1');
        const { steps } = runGpu(gov, BEHIND, 10);
        expect(steps.map((s) => s.algo)).toEqual(['nis', 'sgsr']);
        expect(gov.level).toBe('sgsr');
    });

    it('undoes each bet on the heavier pass, and waits longer before the next', () => {
        const gov = new EnhancerGovernor('fsr1');
        const { steps } = runGpu(gov, BEHIND, 300);
        const algos = steps.map((s) => s.algo);
        expect(algos.slice(0, 2)).toEqual(['nis', 'sgsr']);
        // Every climb back to NIS finds the fences late again and is undone.
        const bets = steps.filter((s, i) => i >= 2 && s.algo === 'nis').map((s) => s.now);
        expect(bets.length).toBeGreaterThan(1);
        for (let i = 2; i < algos.length; i += 2) expect(algos[i + 1] ?? 'sgsr').toBe('sgsr');
        // ...and the wait between bets grows.
        const gaps = bets.slice(1).map((t, i) => t - bets[i]);
        for (let i = 1; i < gaps.length; i++) expect(gaps[i]).toBeGreaterThan(gaps[i - 1]);
    });

    it('leaves a GPU that is only sometimes late alone', () => {
        const gov = new EnhancerGovernor('fsr1');
        const { steps } = runGpu(gov, { fsr1: 0.3, nis: 0.3, sgsr: 0.3, off: 0 }, 60);
        expect(steps).toEqual([]);
        expect(gov.level).toBe('fsr1');
    });

    it('treats a renderer that cannot tell (null) as never behind', () => {
        const gov = new EnhancerGovernor('fsr1');
        const { steps } = runGpu(gov, { fsr1: null, nis: null, sgsr: null, off: null }, 60);
        expect(steps).toEqual([]);
        expect(gov.level).toBe('fsr1');
    });

    it('does not bet on the heavier pass again while the GPU still lags', () => {
        const gov = new EnhancerGovernor('fsr1');
        runGpu(gov, BEHIND, 10);
        expect(gov.level).toBe('sgsr');
        // Draw and queue say recover; the fences of SGSR itself are not clear.
        const { steps } = runGpu(gov, { ...BEHIND, sgsr: 0.2 }, 300, 10000);
        expect(steps).toEqual([]);
        expect(gov.level).toBe('sgsr');
    });
});

describe('EnhancerGovernor — WebGL2 passes and the memory of a verdict', () => {
    const STARVED = { serviceMs: 0.2, arrivalMs: 9.0, decodeQueue: 13 };
    const CLEAR = { serviceMs: 0.2, arrivalMs: 9.0, decodeQueue: 0 };

    beforeEach(() => localStorage.clear());

    it('names the same rung on both renderers', () => {
        expect(ladderRung('gl-sgsr')).toBe('sgsr');
        expect(ladderRung('sgsr')).toBe('sgsr');
        expect(ladderRung('off')).toBe('off');
        expect(rendererAlgo('sgsr', 'webgl')).toBe('gl-sgsr');
        expect(rendererAlgo('off', 'webgl')).toBe('off');
        expect(rendererAlgo('nis', 'webgpu')).toBe('nis');
    });

    it('starts where the last session left it, under the same ceiling', () => {
        const gov = new EnhancerGovernor('sgsr', { startRung: 'off' });
        expect(gov.level).toBe('off');
        expect(gov.degraded).toBe(true);
        expect(gov.ceiling).toBe('sgsr');
        // A start above the ceiling is not a start: the setting wins.
        expect(new EnhancerGovernor('sgsr', { startRung: 'fsr1' }).level).toBe('sgsr');
    });

    it('climbs back from a remembered start when the decoder is clear', () => {
        const gov = new EnhancerGovernor('sgsr', { startRung: 'off' });
        const steps = feedQueued(gov, CLEAR, 30);
        expect(steps.map((s) => s.algo)).toEqual(['sgsr']);
        expect(gov.degraded).toBe(false);
    });

    it("keeps the previous session's wait before its first bet", () => {
        const gov = new EnhancerGovernor('sgsr', { startRung: 'off', recoverAfterMs: 60000 });
        expect(gov.recoverAfterMs).toBe(60000);
        expect(feedQueued(gov, CLEAR, 50)).toEqual([]);
        expect(feedQueued(gov, CLEAR, 20, 50000).map((s) => s.algo)).toEqual(['sgsr']);
    });

    it('remembers a step down and forgets it back at the setting', () => {
        expect(rememberedRung('webgl', 'sgsr')).toBeNull();
        const gov = new EnhancerGovernor('sgsr');
        const down = feedQueued(gov, STARVED, 10);
        expect(down.map((s) => s.algo)).toEqual(['off']);
        rememberRung('webgl', gov.ceiling, gov.level, gov.recoverAfterMs);
        expect(rememberedRung('webgl', 'sgsr')).toEqual({ rung: 'off', recoverAfterMs: 15000 });
        // Another renderer, another setting: nothing carried over.
        expect(rememberedRung('webgpu', 'sgsr')).toBeNull();
        expect(rememberedRung('webgl', 'fsr1')).toBeNull();
        rememberRung('webgl', 'sgsr', 'sgsr', 15000);
        expect(rememberedRung('webgl', 'sgsr')).toBeNull();
    });

    it('reads a damaged memory as nothing', () => {
        localStorage.setItem('mw_enhancer_rung', '{not json');
        expect(rememberedRung('webgl', 'sgsr')).toBeNull();
        localStorage.setItem(
            'mw_enhancer_rung',
            JSON.stringify({ webgl: { ceiling: 'sgsr', rung: 'x' } }),
        );
        expect(rememberedRung('webgl', 'sgsr')).toBeNull();
    });
});
