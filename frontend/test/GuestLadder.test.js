/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 *
 * An invited player's congestion ladder (issue #67, step 2). The host works
 * every step's picture out (server/GuestProfile degraded()) and sends the
 * page the whole ladder; the page decides when to step and what to say. One
 * table holds both sides, fixtures/guest-profile-cases.json:
 * backend/tests/test_guest_profile.cpp holds the C++ to it, and here the
 * table is held to the owner's ladder rule on the page's own Auto bitrate
 * (util/AutoBitrate.js), and the page to reading it.
 */
import { describe, it, expect, beforeEach } from 'vitest';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
import { computeAutoBitrate } from '../js/util/AutoBitrate.js';
import {
    GuestLadder,
    STEP_GAP_MS,
    STABLE_MS,
    MAX_STABLE_MS,
    FLAP_MS,
} from '../js/stream/GuestLadder.js';

const cases = JSON.parse(
    readFileSync(
        join(dirname(fileURLToPath(import.meta.url)), 'fixtures', 'guest-profile-cases.json'),
        'utf8',
    ),
);

/** The table's whole ladder for one base, as the host would send it. */
function ladderOf(height, kbps, aspect, lan) {
    const rows = cases.degrade
        .filter(
            (c) => c.height === height && c.kbps === kbps && c.aspect === aspect && c.lan === lan,
        )
        .sort((a, b) => a.n - b.n);
    return rows.map((c) => ({ height: c.want[0], bitrate_kbps: c.want[1] }));
}

describe("the guest's ladder, on the shared table", () => {
    it('has a whole ladder for at least one base', () => {
        const steps = ladderOf(1440, 15000, '16:9', false);
        expect(steps).toHaveLength(9);
        expect(steps[0]).toEqual({ height: 1440, bitrate_kbps: 15000 });
    });

    // Each row from the one before it, by the owner's rule (app.js
    // _onStreamCongested): the bitrate halved at the first step, -30 % at each
    // next one, 2 Mbps at the least; one rung down while above 720, the bitrate
    // then no more than Auto's for the new height.
    it.each(cases.degrade.filter((c) => c.n > 0))(
        '$height p $kbps kbps LAN $lan, step $n: the owner rule',
        (c) => {
            let h = c.height;
            let k = c.kbps;
            for (let step = 1; step <= Math.min(c.n, 8); step++) {
                let next = Math.max(2000, Math.round(k * (step === 1 ? 0.5 : 0.7)));
                if (h > 720) {
                    h = [1440, 1080, 720].find((r) => r < h);
                    next = Math.max(
                        2000,
                        Math.min(next, computeAutoBitrate(h, c.aspect, false, false, c.lan) * 1000),
                    );
                }
                k = Math.min(k, next);
            }
            expect([h, k]).toEqual(c.want);
        },
    );
});

describe('GuestLadder', () => {
    let ladder;
    const steps = ladderOf(1440, 15000, '16:9', false);

    beforeEach(() => {
        ladder = new GuestLadder();
        ladder.adopt({ degrade: 0, steps });
    });

    it('steps down the table, saying what each step changes', () => {
        let now = 100000;
        const toasts = [];
        for (let i = 0; i < 9; i++) {
            const move = ladder.down(now);
            if (!move) break;
            toasts.push(move.toast);
            // The host's answer to the relaunch.
            ladder.adopt({ degrade: move.level, steps });
            now += STEP_GAP_MS;
        }
        expect(toasts[0]).toEqual({ key: 'stream.degradeResolution', params: { res: '1080' } });
        expect(toasts[1]).toEqual({ key: 'stream.degradeResolution', params: { res: '720' } });
        expect(toasts[2]).toEqual({
            key: 'stream.degradeBitrate',
            params: { mbps: (steps[3].bitrate_kbps / 1000).toFixed(1) },
        });
        // Down to 720p at 2 Mbps, where nothing is left to lower.
        expect(toasts).toHaveLength(5);
        expect(ladder.level).toBe(5);
        expect(ladder.down(now + STEP_GAP_MS)).toBeNull();
    });

    it('waits out the gap between two steps, either way', () => {
        expect(ladder.down(100000)?.level).toBe(1);
        ladder.adopt({ degrade: 1, steps });
        expect(ladder.down(100000 + STEP_GAP_MS - 1)).toBeNull();
        expect(ladder.up(100000 + STEP_GAP_MS - 1)).toBeNull();
        expect(ladder.down(100000 + STEP_GAP_MS)?.level).toBe(2);
    });

    it('waits longer than the host does', () => {
        // ShareManager::kLadderStepGapMs: a page that waits this never sees a 429.
        expect(STEP_GAP_MS).toBeGreaterThan(60000);
    });

    it('climbs back one step once the link has been quiet', () => {
        ladder.down(100000);
        ladder.adopt({ degrade: 1, steps });
        ladder.noteSignal(110000);
        expect(ladder.up(110000 + STABLE_MS - 1)).toBeNull();
        const move = ladder.up(110000 + STABLE_MS);
        expect(move).toEqual({
            level: 0,
            toast: { key: 'stream.upgradeResolution', params: { res: '1440' } },
        });
    });

    it('says a bitrate when the step up keeps the height', () => {
        ladder.adopt({ degrade: 4, steps });
        const move = ladder.up(1e9);
        expect(move?.level).toBe(3);
        expect(move?.toast).toEqual({
            key: 'stream.upgradeBitrate',
            params: { mbps: (steps[3].bitrate_kbps / 1000).toFixed(1) },
        });
    });

    it('never climbs above the top', () => {
        expect(ladder.up(1e9)).toBeNull();
    });

    it('waits twice as long after a flap, ten minutes at most', () => {
        let now = 1000000;
        ladder.adopt({ degrade: 1, steps });
        const waits = [];
        for (let i = 0; i < 8; i++) {
            now += MAX_STABLE_MS + STEP_GAP_MS;
            expect(ladder.up(now)?.level).toBe(0);
            ladder.adopt({ degrade: 0, steps });
            // Choking again as soon as it may: a flap.
            now += STEP_GAP_MS;
            expect(STEP_GAP_MS).toBeLessThan(FLAP_MS);
            expect(ladder.down(now)?.level).toBe(1);
            ladder.adopt({ degrade: 1, steps });
            waits.push(ladder.stableMs);
        }
        expect(waits[0]).toBe(STABLE_MS * 2);
        expect(waits[1]).toBe(STABLE_MS * 4);
        expect(ladder.stableMs).toBe(MAX_STABLE_MS);
    });

    it('takes congestion long after a step up for a new one, not a flap', () => {
        ladder.adopt({ degrade: 1, steps });
        expect(ladder.up(1e7)?.level).toBe(0);
        ladder.adopt({ degrade: 0, steps });
        expect(ladder.down(1e7 + FLAP_MS)?.level).toBe(1);
        expect(ladder.stableMs).toBe(STABLE_MS);
    });

    it('has no ladder from a malformed answer, and steps nothing', () => {
        for (const bad of [
            null,
            undefined,
            {},
            { steps: 'x' },
            { steps: [{ height: 1080 }] },
            {
                steps: [
                    { height: -1, bitrate_kbps: 5 },
                    { height: 720, bitrate_kbps: 5 },
                ],
            },
            { degrade: 0, steps: [{ height: 1080, bitrate_kbps: 10000 }] },
        ]) {
            const l = new GuestLadder();
            l.adopt(bad);
            expect(l.usable).toBe(false);
            expect(l.down(1e9)).toBeNull();
            expect(l.up(1e9)).toBeNull();
        }
    });

    it('holds a step the host names to its own ladder', () => {
        ladder.adopt({ degrade: 99, steps });
        expect(ladder.level).toBe(8);
        ladder.adopt({ degrade: -3, steps });
        expect(ladder.level).toBe(0);
        ladder.adopt({ degrade: '2', steps });
        expect(ladder.level).toBe(0);
    });

    it('starts afresh on reset', () => {
        ladder.down(100000);
        ladder.reset();
        expect(ladder.usable).toBe(false);
        expect(ladder.level).toBe(0);
        expect(ladder.stableMs).toBe(STABLE_MS);
    });
});
