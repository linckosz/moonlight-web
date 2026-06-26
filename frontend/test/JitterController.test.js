/*
 * Moonlight-Web — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect } from 'vitest';
import { JitterController } from '../js/stream/JitterController.js';

// A clean tick: no loss, no freeze, no jitter. Cumulative counters held flat.
function calm(extra = {}) {
    return { packetsLost: 0, packetsReceived: 1000, freezeCount: 0, jitterMs: 0, rttMs: 20, ...extra };
}

describe('JitterController', () => {
    it('primes on the first tick and returns null', () => {
        const jc = new JitterController();
        expect(jc.update(calm())).toBeNull();
        expect(jc.targetMs).toBe(0);
        expect(jc.lastSignals).toBeNull();
    });

    it('stays at the floor and commands nothing on a clean link', () => {
        const jc = new JitterController();
        jc.update(calm());
        expect(jc.update(calm())).toBeNull();
        expect(jc.targetMs).toBe(0);
    });

    it('bumps the target immediately on a freeze (underrun)', () => {
        const jc = new JitterController();
        jc.update(calm({ freezeCount: 0 }));
        const cmd = jc.update(calm({ freezeCount: 1 }));
        expect(cmd).toBe(50); // BUMP_FREEZE from 0
        expect(jc.targetMs).toBe(50);
        expect(jc.lastSignals).toMatchObject({ freezes: 1 });
    });

    it('bumps on a sustained loss spike above LOSS_HI', () => {
        const jc = new JitterController();
        jc.update(calm());
        // 50 lost out of 1050 received-delta => ~4.7% loss > LOSS_HI(2%)
        const cmd = jc.update({ packetsLost: 50, packetsReceived: 2000, freezeCount: 0, jitterMs: 0, rttMs: 40 });
        expect(cmd).toBeGreaterThanOrEqual(30); // at least BUMP_LOSS or RTT_K*rtt
        expect(jc.targetMs).toBeGreaterThan(0);
    });

    it('clamps the target to MAX', () => {
        const jc = new JitterController({ MAX: 100 });
        jc.update(calm());
        // huge jitter drives desired well above MAX
        jc.update(calm({ jitterMs: 1000 }));
        expect(jc.targetMs).toBeLessThanOrEqual(100);
    });

    it('decays back to the floor after sustained calm, commanding the settle', () => {
        const jc = new JitterController({ CLEAN_HOLD: 2, DECAY_STEP: 100, DEADBAND: 15 });
        jc.update(calm());
        jc.update(calm({ freezeCount: 1 })); // target -> 50
        expect(jc.targetMs).toBe(50);
        // sustained calm: cleanTicks accrue, then decay
        jc.update(calm()); // cleanTicks=1
        const settle = jc.update(calm()); // cleanTicks=2 -> decay by 100 -> floor 0
        expect(jc.targetMs).toBe(0);
        expect(settle).toBe(0); // settledToMin forces a command even within deadband
    });

    it('honors the deadband (no command on a sub-threshold change)', () => {
        const jc = new JitterController({ DEADBAND: 100, JITTER_K: 1, EWMA_ALPHA: 1 });
        jc.update(calm());
        // desired = 1 * 10ms = 10ms rise, below the 100ms deadband
        const cmd = jc.update(calm({ jitterMs: 10 }));
        expect(jc.targetMs).toBe(10); // internal target moved
        expect(cmd).toBeNull(); // but nothing commanded
    });

    it('reset() restores the adaptation state and re-primes', () => {
        const jc = new JitterController();
        jc.update(calm());
        jc.update(calm({ freezeCount: 1 }));
        jc.reset();
        expect(jc.targetMs).toBe(0);
        expect(jc.update(calm())).toBeNull(); // primes again after reset
        expect(jc.update(calm())).toBeNull(); // still at the floor
    });
});
