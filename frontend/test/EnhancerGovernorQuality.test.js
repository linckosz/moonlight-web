/**
 * EnhancerGovernor — the rung `mwsr-q`, mwsr's quality mode (the kernel and a
 * sharpening pass after it): the ceiling of a session on a computer with a
 * dedicated graphics card. Its step down is `mwsr`, the kernel alone, so a
 * card that cannot hold the second pass keeps the upscaler; and it is a rung
 * of those sessions only.
 */
import { describe, it, expect, beforeEach } from 'vitest';
import {
    EnhancerGovernor,
    ENHANCER_LADDER,
    ladderRung,
    rendererAlgo,
    rememberedRung,
    rememberRung,
} from '../js/stream/EnhancerGovernor.js';

/** The service time follows the level running, a window every 500 ms. */
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

describe("EnhancerGovernor — mwsr's quality rung", () => {
    beforeEach(() => localStorage.clear());

    it('sits one rung above the kernel alone, in both flavours', () => {
        expect(ENHANCER_LADDER.indexOf('mwsr-q')).toBe(ENHANCER_LADDER.indexOf('mwsr') - 1);
        expect(ladderRung('gl-mwsr-q')).toBe('mwsr-q');
        expect(ladderRung('mwsr-q')).toBe('mwsr-q');
        expect(rendererAlgo('mwsr-q', 'webgl')).toBe('gl-mwsr-q');
        expect(rendererAlgo('mwsr-q', 'webgpu')).toBe('mwsr-q');
        const gov = new EnhancerGovernor('mwsr-q');
        expect(gov.level).toBe('mwsr-q');
        expect(gov.ceiling).toBe('mwsr-q');
        expect(gov.degraded).toBe(false);
    });

    it('steps down to the kernel alone and stays there when that fits', () => {
        // the second pass does not fit a 16.7 ms frame on this card; the kernel does
        const gov = new EnhancerGovernor('mwsr-q');
        const { steps } = run(gov, { 'mwsr-q': 15, mwsr: 9, off: 3 }, 10);
        expect(steps.map((s) => s.algo)).toEqual(['mwsr']);
        expect(gov.level).toBe('mwsr');
        expect(gov.degraded).toBe(true);
    });

    it('goes on to off when the kernel does not fit either, and climbs back rung by rung', () => {
        const gov = new EnhancerGovernor('mwsr-q');
        const down = run(gov, { 'mwsr-q': 19, mwsr: 15, off: 3 }, 20);
        expect(down.steps.map((s) => s.algo)).toEqual(['mwsr', 'off']);
        // the pressure gone: back to the kernel, then to quality
        const up = run(gov, { 'mwsr-q': 4, mwsr: 3, off: 2 }, 120, down.now);
        expect(up.steps.map((s) => s.algo)).toEqual(['mwsr', 'mwsr-q']);
        expect(gov.degraded).toBe(false);
    });

    it('is no rung of a session that comes down from another setting', () => {
        for (const [ceiling, want] of [
            ['fsr1', ['nis', 'sgsr', 'mwsr', 'off']],
            ['sgsr', ['mwsr', 'off']],
        ]) {
            const gov = new EnhancerGovernor(ceiling);
            const cost = { fsr1: 40, nis: 40, sgsr: 40, 'mwsr-q': 40, mwsr: 40, off: 40 };
            const down = run(gov, cost, 60);
            expect(
                down.steps.map((s) => s.algo),
                ceiling,
            ).toEqual(want);
            // and climbs back past it
            const idle = { fsr1: 2, nis: 2, sgsr: 2, 'mwsr-q': 2, mwsr: 2, off: 2 };
            const up = run(gov, idle, 600, down.now);
            expect(
                up.steps.map((s) => s.algo),
                ceiling,
            ).toEqual([...want.slice(0, -1).reverse(), ceiling]);
        }
        // a session whose setting is the kernel alone never climbs to quality
        const gov = new EnhancerGovernor('mwsr');
        const down = run(gov, { mwsr: 40, off: 40 }, 20);
        expect(down.steps.map((s) => s.algo)).toEqual(['off']);
        const up = run(gov, { mwsr: 2, off: 2 }, 120, down.now);
        expect(up.steps.map((s) => s.algo)).toEqual(['mwsr']);
    });

    it('remembers its verdict under its own ceiling only', () => {
        rememberRung('webgl', 'mwsr-q', 'mwsr', 15000);
        expect(rememberedRung('webgl', 'mwsr-q')).toEqual({ rung: 'mwsr', recoverAfterMs: 15000 });
        // a session whose setting is the kernel alone reads nothing of it
        expect(rememberedRung('webgl', 'mwsr')).toBeNull();
        // and a verdict kept under the kernel's ceiling says nothing to a quality session
        rememberRung('webgl', 'mwsr', 'off', 15000);
        expect(rememberedRung('webgl', 'mwsr-q')).toBeNull();
        // starting where the last session settled: degraded from the first frame
        const gov = new EnhancerGovernor('mwsr-q', { startRung: 'mwsr' });
        expect(gov.level).toBe('mwsr');
        expect(gov.degraded).toBe(true);
    });
});
