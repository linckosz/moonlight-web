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
 * CadenceStepper — "Auto" with detection: a step kept when the picture gets
 * younger, given up when it does not, the backoff, the net, and no trial for
 * content no faster than the client.
 */
import { afterEach, describe, it, expect } from 'vitest';
import {
    autostepEnabled,
    BACKOFF_MS,
    BASE_MS,
    CadenceStepper,
    FASTER_STATS,
    FILET_HOLD_MS,
    FILET_WIDE_MIN_MS,
    HELD_MS,
    ladder,
    MEMO_KEY,
    MEMO_TTL_MS,
    MEMO_WAIT_MS,
    QUEUE_HOLD_MS,
    stepMemory,
} from '../js/stream/CadenceStepper.js';

const OFFSET_US = 5e9; // the host's clock, ahead of this one

/**
 * A client and a native host, a quarter of a millisecond at a time. The host
 * answers `fpsstep` 1 ms later (`reply`, applied by default), says its cadence
 * once a second, and sends min(content, step) frames a second; each frame is
 * received `netMs` after its capture and painted `latency(rate)` after it, a
 * `paintShare` of them. Pongs every 500 ms carry the host's exact clock.
 */
function setup(opts = {}) {
    const o = {
        base: 120,
        display: 240,
        presents: 240, // or a function of the time
        latency: () => 20,
        netMs: 5,
        paintShare: 1,
        queue: () => 0,
        reply: (fps) => ({ verdict: 'applied', fps }),
        ...opts,
    };
    let now = 0;
    let hostStep = 0;
    const sent = [];
    const replies = [];
    const stepper = new CadenceStepper({
        baseFps: o.base,
        send: (msg) => {
            sent.push({ at: now, ...msg });
            if (msg.type !== 'fpsstep') return;
            const r = msg.fps > 0 ? o.reply(msg.fps) : { verdict: 'base', fps: 0 };
            if (r.verdict !== 'refused') hostStep = r.fps;
            replies.push({
                at: now + 1,
                msg: {
                    type: 'fpsstep',
                    asked: msg.fps,
                    verdict: r.verdict,
                    why: r.why,
                    fps: r.verdict === 'refused' ? hostStep : r.fps,
                },
            });
        },
        now: () => now,
        log: () => {},
        timers: { every: () => 0, stop: () => {} },
        memory: o.memory,
    });
    stepper.start();
    const presentsAt = (t) => (typeof o.presents === 'function' ? o.presents(t) : o.presents);
    let frames = [];
    let nextFrameAt = 10;
    let nextPong = 100;
    let nextStats = 1000;
    let nextTick = 100;
    let share = 0;
    let q = 0;
    const run = (untilMs) => {
        for (; q <= untilMs * 4; q++) {
            now = q / 4;
            while (replies.length && replies[0].at <= now)
                stepper.noteReply(replies.shift().msg, now);
            if (now >= nextPong) {
                stepper.notePong({ ts: now - 0.4, host: (now - 0.2) * 1000 + OFFSET_US }, now);
                nextPong += 500;
            }
            if (now >= nextStats) {
                stepper.noteStats(
                    { presents: presentsAt(now), base: o.base, step: hostStep, display: o.display },
                    now,
                );
                nextStats += 1000;
            }
            const rate = Math.min(presentsAt(now), hostStep || o.base);
            if (now >= nextFrameAt) {
                if (rate > 0) {
                    frames.push({
                        recvAt: now + o.netMs,
                        paintAt: now + o.latency(rate, now),
                        ts: (now * 1000 + OFFSET_US) / 1000,
                        rate,
                        received: false,
                    });
                    nextFrameAt += 1000 / rate;
                } else {
                    nextFrameAt = now + 10;
                }
            }
            for (const f of frames) {
                if (f.received || f.recvAt > now) continue;
                f.received = true;
                stepper.noteReceived(now);
                stepper.noteDecodeQueue(o.queue(f.rate, now), now);
            }
            const due = frames.filter((f) => f.paintAt <= now);
            if (due.length) {
                frames = frames.filter((f) => f.paintAt > now);
                for (const f of due) {
                    share += o.paintShare;
                    if (share >= 1 - 1e-9) {
                        share -= 1;
                        stepper.notePainted(f.ts, now);
                    }
                }
            }
            if (now >= nextTick) {
                stepper.tick(now);
                nextTick += 100;
            }
        }
    };
    const steps = () => sent.filter((m) => m.type === 'fpsstep');
    return {
        stepper,
        run,
        steps,
        get hostStep() {
            return hostStep;
        },
    };
}

describe('autostepEnabled', () => {
    const set = (v) => localStorage.setItem('mw_autostep', v);
    afterEach(() => localStorage.removeItem('mw_autostep'));

    it("is on by default, and mw_autostep = '0' turns it off", () => {
        expect(autostepEnabled()).toBe(true);
        set('0');
        expect(autostepEnabled()).toBe(false);
        set('1');
        expect(autostepEnabled()).toBe(true);
    });

    it("is off on a TV unless mw_autostep = '1'", () => {
        expect(autostepEnabled(true)).toBe(false);
        set('1');
        expect(autostepEnabled(true)).toBe(true);
        set('0');
        expect(autostepEnabled(true)).toBe(false);
    });
});

describe('ladder', () => {
    it("is the stream's rate, twice it, then the display's refresh", () => {
        expect(ladder(60, 240)).toEqual([60, 120, 240]);
        expect(ladder(120, 240)).toEqual([120, 240]);
        expect(ladder(144, 240)).toEqual([144, 240]);
        expect(ladder(120, 144)).toEqual([120, 144]);
    });

    it('has nothing above a display no faster than the stream', () => {
        expect(ladder(60, 60)).toEqual([60]);
        expect(ladder(240, 240)).toEqual([240]);
    });

    it('doubles without a cap while the display is unknown', () => {
        expect(ladder(144, 0)).toEqual([144, 288]);
        // Under Auto's learned ceiling: 4K120 on a 240 Hz screen does not double.
        expect(ladder(120, 240, 120)).toEqual([120]);
        expect(ladder(60, 240, 120)).toEqual([60, 120]);
        expect(ladder(120, 240, 241)).toEqual([120, 240]);
        expect(ladder(0, 240)).toEqual([]);
    });
});

describe('CadenceStepper', () => {
    it('keeps a step that makes the picture younger, within ten seconds', () => {
        // 120 Hz client, 240 Hz content: same capture → painted, half the wait.
        const s = setup();
        s.run(10000);
        expect(s.steps().map((m) => m.fps)).toEqual([240]);
        // Asked once the host has said FASTER_STATS times, a second apart,
        // that the content is faster.
        expect(s.steps()[0].at).toBeGreaterThanOrEqual(FASTER_STATS * 1000);
        expect(s.steps()[0].at).toBeLessThan(FASTER_STATS * 1000 + 200);
        expect(s.stepper.stepFps).toBe(240);
        expect(s.stepper.kept).toBe(1);
        expect(s.hostStep).toBe(240);
        const kept = s.stepper.events.find((e) => e.what === 'kept');
        expect(kept.at).toBeLessThan(10000);
        expect(kept.gain).toBeCloseTo(1000 / 120 / 2 - 1000 / 240 / 2, 0);
        // And it stays.
        s.run(60000);
        expect(s.steps()).toHaveLength(1);
        expect(s.stepper.stepFps).toBe(240);
    });

    it('climbs 60 → 120 → 240 on a client that keeps up', () => {
        const s = setup({ base: 60 });
        s.run(12000);
        expect(s.steps().map((m) => m.fps)).toEqual([120, 240]);
        expect(s.stepper.stepFps).toBe(240);
        expect(s.stepper.kept).toBe(2);
    });

    it('gives up a step that makes the picture older, and backs off', () => {
        // At 240 each frame takes 3.5 ms longer to be painted: more than the
        // 2.1 ms of waiting it saves, less than the net's half refresh.
        const s = setup({ latency: (rate) => (rate > 120 ? 23.5 : 20) });
        s.run(9000);
        expect(s.steps().map((m) => m.fps)).toEqual([240, 0]);
        expect(s.stepper.stepFps).toBe(0);
        expect(s.stepper.rejected).toBe(1);
        expect(s.stepper.trips).toBe(0);
        const givenUp = s.steps()[1].at;
        // The next trial waits 30 s, then a base window.
        s.run(givenUp + BACKOFF_MS[0] - 1);
        expect(s.steps()).toHaveLength(2);
        s.run(givenUp + BACKOFF_MS[0] + BASE_MS + 500);
        expect(s.steps().map((m) => m.fps)).toEqual([240, 0, 240]);
        // Given up again: a minute this time.
        s.run(givenUp + BACKOFF_MS[0] + 8000);
        expect(s.steps().map((m) => m.fps)).toEqual([240, 0, 240, 0]);
        const again = s.steps()[3].at;
        s.run(again + BACKOFF_MS[1] - 1);
        expect(s.steps()).toHaveLength(4);
        s.run(again + BACKOFF_MS[1] + BASE_MS + 500);
        expect(s.steps()).toHaveLength(5);
    });

    it('gives up a step when the client stops painting what it receives', () => {
        // Every frame painted at 120, four in five at 240: younger, but not
        // shown.
        const s = setup();
        let n = 0;
        const painted = s.stepper.notePainted.bind(s.stepper);
        s.stepper.notePainted = (ts, at) => {
            if (s.hostStep > 120 && ++n % 5 === 0) return;
            painted(ts, at);
        };
        s.run(9000);
        expect(s.steps().map((m) => m.fps)).toEqual([240, 0]);
        const rejected = s.stepper.events.find((e) => e.what === 'rejected');
        expect(rejected.ratio).toBeLessThan(0.9);
    });

    it('comes back at once when the client drowns (the net)', () => {
        const s = setup({ latency: (rate) => (rate > 120 ? 60 : 20) });
        s.run(9000);
        const steps = s.steps();
        expect(steps.map((m) => m.fps)).toEqual([240, 0]);
        expect(s.stepper.trips).toBe(1);
        // Once the late frames are half of the net's window — 60 ms for the
        // first of them to be painted, ~70 more for them to outnumber the
        // ones painted at 120 — and have stayed so FILET_HOLD_MS: a stall of
        // a few frames does not trip it.
        expect(steps[1].at - steps[0].at).toBeLessThan(200 + FILET_HOLD_MS);
        expect(s.stepper.stepFps).toBe(0);
    });

    it("rides out a spike of the link shorter than the net's hold", () => {
        // 240 kept, then 300 ms of frames painted 60 ms late, once: the link,
        // not the step (a Mac on Wi-Fi).
        let spikeAt = Infinity;
        const s = setup({
            latency: (rate, t) => (t >= spikeAt && t < spikeAt + 300 ? 60 : 20),
        });
        s.run(10000);
        expect(s.stepper.stepFps).toBe(240);
        spikeAt = 12000;
        s.run(20000);
        expect(s.stepper.trips).toBe(0);
        expect(s.stepper.stepFps).toBe(240);
        expect(s.steps().map((m) => m.fps)).toEqual([240]);
    });

    it('waits the longest backoff after two trips of the net in a row', () => {
        // A client that drowns at every step: each trial trips the net.
        const s = setup({ latency: (rate) => (rate > 120 ? 60 : 20) });
        s.run(9000);
        expect(s.stepper.trips).toBe(1);
        expect(s.stepper.summary.nextTrialInMs).toBeLessThanOrEqual(BACKOFF_MS[0]);
        const first = s.steps()[1].at;
        s.run(first + BACKOFF_MS[0] + BASE_MS + 2000);
        expect(s.steps().map((m) => m.fps)).toEqual([240, 0, 240, 0]);
        expect(s.stepper.trips).toBe(2);
        // Not the second backoff: the longest.
        expect(s.stepper.summary.nextTrialInMs).toBeGreaterThan(BACKOFF_MS.at(-2));
        s.run(first + BACKOFF_MS[0] + BACKOFF_MS[1] + 2 * BASE_MS + 4000);
        expect(s.steps()).toHaveLength(4);
    });

    it('keeps that wait when the content changes meanwhile', () => {
        let presents = 240;
        const s = setup({ presents: () => presents, latency: (rate) => (rate > 120 ? 60 : 20) });
        s.run(9000);
        const first = s.steps()[1].at;
        s.run(first + BACKOFF_MS[0] + BASE_MS + 2000);
        expect(s.stepper.trips).toBe(2);
        const second = s.steps()[3].at;
        // The content stops, then comes back: a new band, the same link.
        presents = 0;
        s.run(second + 4000);
        presents = 240;
        s.run(second + 20000);
        expect(s.stepper.events.some((e) => e.what === 'content' && e.at > second)).toBe(true);
        expect(s.steps()).toHaveLength(4);
        expect(s.stepper.summary.nextTrialInMs).toBeGreaterThan(BACKOFF_MS.at(-2));
    });

    it('forgets its trips once a kept step has held', () => {
        // The first trial drowns; the second holds; then, much later, one
        // spike of the link long enough for the net.
        let spikeAt = Infinity;
        const s = setup({
            latency: (rate, t) =>
                rate > 120 && (t < 20000 || (t >= spikeAt && t < spikeAt + 1000)) ? 60 : 20,
        });
        s.run(60000);
        expect(s.stepper.trips).toBe(1);
        expect(s.stepper.stepFps).toBe(240);
        const kept = s.stepper.events.find((e) => e.what === 'kept');
        spikeAt = kept.at + HELD_MS + 5000;
        s.run(spikeAt + 3000);
        expect(s.stepper.trips).toBe(2);
        expect(s.stepper.summary.strikes).toBe(1);
        // The first backoff again, not the longest.
        expect(s.stepper.summary.nextTrialInMs).toBeLessThanOrEqual(BACKOFF_MS[0]);
    });

    it('widens its net for a link that rises at any rate, and keeps the step', () => {
        // A Mac on Wi-Fi: 2 s of frames painted 60 ms late every 5 s, at 120
        // as at 240. The first trial trips on a rise; the second holds through
        // them, and is judged on the lower quartile.
        const rise = (t) => t > 3000 && t % 5000 < 2000;
        const s = setup({ latency: (rate, t) => (rise(t) ? 80 : 20) });
        s.run(120000);
        const trials = s.stepper.events.filter((e) => e.what === 'trial');
        expect(trials[0].netMs).toBe(FILET_HOLD_MS);
        expect(trials.at(-1).netMs).toBeGreaterThan(2000);
        expect(s.stepper.stepFps).toBe(240);
        const kept = s.stepper.events.find((e) => e.what === 'kept');
        expect(kept.measure).toBe('lower quartile');
        // And it stays through the rises that follow.
        expect(s.stepper.events.filter((e) => e.what === 'fallback' && e.at > kept.at)).toEqual([]);
    });

    it('holds a widened step through rises longer than the ones seen before it', () => {
        // The Mac at 240: rises of 0.8 s at its own rate, then of 2.5 s at
        // the step. A net of the longest seen × 1.2 would drop the step.
        const s = setup({
            latency: (rate, t) => {
                if (rate > 120) return t % 5000 < 2500 ? 80 : 20;
                return t > 3000 && t % 5000 < 800 ? 80 : 20;
            },
        });
        s.run(120000);
        const trials = s.stepper.events.filter((e) => e.what === 'trial');
        expect(trials.at(-1).netMs).toBe(FILET_WIDE_MIN_MS);
        const kept = s.stepper.events.find((e) => e.what === 'kept');
        expect(kept).toBeDefined();
        expect(s.stepper.events.filter((e) => e.what === 'fallback' && e.at > kept.at)).toEqual([]);
        expect(s.stepper.stepFps).toBe(240);
    });

    it('widens its first trial when the link already rose, and keeps the step within ten seconds', () => {
        // The same Mac, its first rise before the content asks for a step.
        const rise = (t) => t % 4000 >= 1000 && t % 4000 < 2500;
        const s = setup({ latency: (rate, t) => (rise(t) ? 80 : 20) });
        s.run(60000);
        const trials = s.stepper.events.filter((e) => e.what === 'trial');
        expect(trials[0].netMs).toBeGreaterThan(1500);
        const kept = s.stepper.events.find((e) => e.what === 'kept');
        expect(kept.at).toBeLessThan(10000);
        expect(s.stepper.trips).toBe(0);
        expect(s.stepper.stepFps).toBe(240);
    });

    it('narrows the next trial after a widened one failed, then waits the longest', () => {
        // An N95 whose link rose before its first trial: widened, it drowns;
        // the second is narrow and trips at once.
        const rise = (t) => t % 4000 >= 1000 && t % 4000 < 2500;
        const s = setup({ latency: (rate, t) => (rate > 120 ? 120 : rise(t) ? 80 : 20) });
        s.run(60000);
        const trials = s.stepper.events.filter((e) => e.what === 'trial');
        expect(trials).toHaveLength(2);
        expect(trials[0].netMs).toBeGreaterThan(FILET_HOLD_MS);
        expect(trials[1].netMs).toBe(FILET_HOLD_MS);
        expect(s.stepper.summary.strikes).toBe(2);
        expect(s.stepper.summary.nextTrialInMs).toBeGreaterThan(BACKOFF_MS.at(-2));
    });

    it('gives a widened trial back on a link that drowns at the step, and waits the longest', () => {
        // An N95 on Wi-Fi: the same rises at its own rate, and at 240 every
        // frame 100 ms late.
        const rise = (t) => t > 3000 && t % 5000 < 2000;
        const s = setup({ latency: (rate, t) => (rate > 120 ? 120 : rise(t) ? 80 : 20) });
        s.run(60000);
        const trials = s.stepper.events.filter((e) => e.what === 'trial');
        expect(trials).toHaveLength(2);
        expect(trials[1].netMs).toBeGreaterThan(FILET_HOLD_MS);
        expect(s.stepper.stepFps).toBe(0);
        expect(s.stepper.summary.strikes).toBe(2);
        expect(s.stepper.summary.nextTrialInMs).toBeGreaterThan(BACKOFF_MS.at(-2));
    });

    it('comes back at once on a decode queue that holds', () => {
        const s = setup({ queue: (rate) => (rate > 120 ? 3 : 0) });
        s.run(9000);
        const steps = s.steps();
        expect(steps.map((m) => m.fps)).toEqual([240, 0]);
        expect(s.stepper.trips).toBe(1);
        expect(steps[1].at - steps[0].at).toBeLessThan(QUEUE_HOLD_MS + 50);
    });

    it('tries nothing for content no faster than the client', () => {
        for (const presents of [53, 60, 66]) {
            const s = setup({ base: 60, presents });
            s.run(60000);
            expect(s.steps()).toEqual([]);
            expect(s.stepper.trials).toBe(0);
        }
    });

    it('tries nothing for a few faster seconds over a slower game', () => {
        // The bench, 01/10/2026: a kiosk opening over a 50-frame page — 169
        // presents a second, a pause, 84 for four reports, then the game's 51.
        const reports = [169, 169, 0, 0, 84, 84, 84, 84];
        const s = setup({
            base: 60,
            presents: (t) => reports[Math.floor(t / 1000) - 1] ?? 51,
        });
        s.run(60000);
        expect(s.steps()).toEqual([]);
        expect(s.stepper.trials).toBe(0);
    });

    it('takes a trial back without blame when the content slows under it', () => {
        let presents = 240;
        const s = setup({ base: 60, presents: () => presents });
        s.run(FASTER_STATS * 1000 + 200);
        expect(s.steps().map((m) => m.fps)).toEqual([120]);
        presents = 50;
        s.run(FASTER_STATS * 1000 + 1500);
        expect(s.steps().map((m) => m.fps)).toEqual([120, 0]);
        expect(s.stepper.events.at(-1)).toMatchObject({
            what: 'inconclusive',
            why: 'the content slowed',
        });
        expect(s.stepper.rejected).toBe(0);
        expect(s.stepper.trips).toBe(0);
        expect(s.stepper.stepFps).toBe(0);
        // No backoff: faster again, tried again as soon as it has been long
        // enough.
        presents = 240;
        s.run(FASTER_STATS * 1000 + 1500 + (FASTER_STATS + 1) * 1000);
        expect(s.steps().map((m) => m.fps)).toEqual([120, 0, 120]);
    });

    it('lets a kept step go when the content stops using it', () => {
        let presents = 240;
        const s = setup({ base: 60, presents: () => presents });
        s.run(13000);
        expect(s.stepper.stepFps).toBe(240);
        // The game drops to 100 frames a second: 120 is enough.
        presents = 100;
        s.run(15600);
        expect(s.steps().map((m) => m.fps)).toEqual([120, 240, 120]);
        expect(s.stepper.stepFps).toBe(120);
        // Then to 50: the client's own rate.
        presents = 50;
        s.run(17600);
        expect(s.steps().map((m) => m.fps)).toEqual([120, 240, 120, 0]);
        expect(s.stepper.stepFps).toBe(0);
        expect(s.stepper.trips).toBe(0);
        expect(s.stepper.rejected).toBe(0);
        expect(s.stepper.events.filter((e) => e.what === 'stepdown')).toHaveLength(2);
        // And it stays there while the content is slow.
        s.run(60000);
        expect(s.steps()).toHaveLength(4);
    });

    it('a game at 80 fps on a 60 Hz client: 120, and no further', () => {
        const s = setup({ base: 60, presents: 80 });
        s.run(30000);
        expect(s.steps().map((m) => m.fps)).toEqual([120]);
        expect(s.stepper.stepFps).toBe(120);
    });

    it('stays where it is when the host refuses, and asks again later', () => {
        const s = setup({
            reply: () => ({ verdict: 'refused', fps: 0, why: 'the encoder takes longer' }),
        });
        s.run(9000);
        expect(s.steps().map((m) => m.fps)).toEqual([240]);
        expect(s.stepper.refused).toBe(1);
        expect(s.stepper.stepFps).toBe(0);
        const asked = s.steps()[0].at;
        s.run(asked + BACKOFF_MS[0] - 1);
        expect(s.steps()).toHaveLength(1);
        s.run(asked + BACKOFF_MS[0] + BASE_MS + 500);
        expect(s.steps()).toHaveLength(2);
    });

    it("takes the host's cap as the step", () => {
        // The display's refresh unknown to the client: twice 144 is asked,
        // the host caps it at its 240.
        const s = setup({
            base: 144,
            display: 0,
            reply: (fps) =>
                fps > 240 ? { verdict: 'capped', fps: 240 } : { verdict: 'applied', fps },
        });
        s.run(10000);
        expect(s.steps().map((m) => m.fps)).toEqual([288]);
        expect(s.stepper.stepFps).toBe(240);
        expect(s.hostStep).toBe(240);
    });

    it('forgets the backoff when the content changes', () => {
        // Given up at first; then the content stops and starts again.
        let presents = 240;
        const s = setup({
            presents: () => presents,
            latency: (rate) => (rate > 120 ? 23.5 : 20),
        });
        s.run(9000);
        expect(s.steps().map((m) => m.fps)).toEqual([240, 0]);
        const givenUp = s.steps()[1].at;
        presents = 0;
        s.run(givenUp + 4000);
        presents = 240;
        s.run(givenUp + 14000);
        // Well before the 30 s the backoff asked for.
        expect(s.steps().map((m) => m.fps)).toEqual([240, 0, 240, 0]);
        expect(s.steps()[2].at).toBeLessThan(givenUp + BACKOFF_MS[0]);
    });

    it('lets a step go when the decoder asks for fewer frames', () => {
        const s = setup();
        s.run(10000);
        expect(s.stepper.stepFps).toBe(240);
        s.stepper.setDecoderCapped(true, 10000);
        expect(s.steps().map((m) => m.fps)).toEqual([240, 0]);
        expect(s.stepper.stepFps).toBe(0);
        s.run(60000);
        expect(s.steps()).toHaveLength(2);
    });

    it('follows a host that dropped the step', () => {
        const s = setup();
        s.run(10000);
        expect(s.stepper.stepFps).toBe(240);
        // The host lifts it on its own (a decoder cap it heard first).
        s.stepper.noteStats({ presents: 240, base: 120, step: 0, display: 240 }, 10001);
        s.stepper.noteStats({ presents: 240, base: 120, step: 0, display: 240 }, 10002);
        expect(s.stepper.stepFps).toBe(0);
    });

    it('takes the step back when it stops, and starts over when the screen changes', () => {
        const s = setup();
        s.run(10000);
        expect(s.stepper.stepFps).toBe(240);
        s.stepper.linkChanged('the screen changed', 10000);
        expect(s.steps().map((m) => m.fps)).toEqual([240, 0]);
        // A new trial after the warm-up, no backoff.
        s.run(20000);
        expect(s.steps().map((m) => m.fps)).toEqual([240, 0, 240]);
        s.stepper.stop();
        expect(s.steps().map((m) => m.fps)).toEqual([240, 0, 240, 0]);
    });
});

/** A device's memory of the steps it failed at, as CadenceStepper sees it. */
function fakeMemory(failed = []) {
    const m = {
        steps: new Set(failed),
        calls: [],
        failed: () => [...m.steps],
        fail: (fps) => {
            m.steps.add(fps);
            m.calls.push(['fail', fps]);
        },
        clear: (fps) => {
            m.steps.delete(fps);
            m.calls.push(['clear', fps]);
        },
    };
    return m;
}

describe('stepMemory', () => {
    /** A Storage of its own, and a clock to move. */
    function store() {
        const data = new Map();
        return {
            getItem: (k) => (data.has(k) ? data.get(k) : null),
            setItem: (k, v) => data.set(k, String(v)),
            data,
        };
    }

    it('remembers a step per host and resolution, and forgets it when kept', () => {
        const st = store();
        let t = 1000;
        const a = stepMemory('host-a|1920×1080', st, () => t);
        const b = stepMemory('host-a|2560×1440', st, () => t);
        a.fail(118);
        a.fail(240);
        expect(a.failed().sort()).toEqual([118, 240]);
        expect(b.failed()).toEqual([]);
        a.clear(118);
        expect(a.failed()).toEqual([240]);
        a.clear(240);
        expect(JSON.parse(st.getItem(MEMO_KEY))).toEqual({});
    });

    it('lets a remembered step expire', () => {
        const st = store();
        let t = 1000;
        const m = stepMemory('h|1080', st, () => t);
        m.fail(118);
        t += MEMO_TTL_MS - 1;
        expect(m.failed()).toEqual([118]);
        t += 1;
        expect(m.failed()).toEqual([]);
        // …and the next write sweeps it out.
        stepMemory('other|1080', st, () => t).fail(240);
        expect(Object.keys(JSON.parse(st.getItem(MEMO_KEY)))).toEqual(['other|1080']);
    });

    it('keeps the latest hosts and resolutions only', () => {
        const st = store();
        let t = 1000;
        for (let i = 0; i < 40; i++) {
            t += 1;
            stepMemory('h' + i + '|1080', st, () => t).fail(118);
        }
        const kept = Object.keys(JSON.parse(st.getItem(MEMO_KEY)));
        expect(kept).toHaveLength(32);
        expect(kept).toContain('h39|1080');
        expect(kept).not.toContain('h0|1080');
    });

    it('remembers nothing without a working storage', () => {
        const broken = {
            getItem: () => {
                throw new Error('denied');
            },
            setItem: () => {
                throw new Error('denied');
            },
        };
        const m = stepMemory('h|1080', broken);
        expect(() => m.fail(118)).not.toThrow();
        expect(m.failed()).toEqual([]);
        expect(stepMemory('h|1080', null).failed()).toEqual([]);
    });
});

describe('CadenceStepper, what this device remembers', () => {
    it('remembers a step that tripped the net twice in a stream, never kept', () => {
        const memory = fakeMemory();
        const s = setup({ memory, latency: (rate) => (rate > 120 ? 60 : 20) });
        s.run(9000);
        expect(memory.calls).toEqual([]);
        const first = s.steps()[1].at;
        s.run(first + BACKOFF_MS[0] + BASE_MS + 2000);
        expect(s.stepper.trips).toBe(2);
        expect(memory.calls).toEqual([['fail', 240]]);
    });

    it('does not try a remembered step early in the next stream', () => {
        const memory = fakeMemory([240]);
        const s = setup({ memory });
        s.run(60000);
        expect(s.steps()).toEqual([]);
        expect(s.stepper.events.filter((e) => e.what === 'remembered')).toHaveLength(1);
        expect(s.stepper.summary.remembered).toEqual([240]);
        // Later in a long stream, it is tried again — and kept here: forgotten.
        s.run(MEMO_WAIT_MS + 10000);
        expect(s.steps().map((m) => m.fps)).toEqual([240]);
        expect(s.stepper.stepFps).toBe(240);
        expect(memory.calls).toEqual([['clear', 240]]);
    });

    it('tries a remembered step again on another link', () => {
        const memory = fakeMemory([240]);
        const s = setup({ memory });
        s.run(20000);
        expect(s.steps()).toEqual([]);
        s.stepper.linkChanged('the link changed', 20000);
        s.run(32000);
        expect(s.stepper.stepFps).toBe(240);
        expect(memory.calls).toEqual([['clear', 240]]);
    });

    it('forgets nothing for a trial given up for no gain', () => {
        // 3.5 ms more at 240: older by less than half a refresh — it did not cost.
        const memory = fakeMemory();
        const s = setup({ memory, latency: (rate) => (rate > 120 ? 23.5 : 20) });
        s.run(9000 + BACKOFF_MS[0] + 8000);
        expect(s.stepper.rejected).toBe(2);
        expect(memory.calls).toEqual([]);
    });

    it('does not remember a step kept earlier in the stream', () => {
        // Kept, then the link drowns it twice later on.
        let drown = false;
        const memory = fakeMemory();
        const s = setup({ memory, latency: (rate) => (drown && rate > 120 ? 60 : 20) });
        s.run(10000);
        expect(s.stepper.stepFps).toBe(240);
        drown = true;
        s.run(10000 + 2 * BACKOFF_MS[0] + BACKOFF_MS[1] + 20000);
        expect(s.stepper.trips).toBeGreaterThanOrEqual(2);
        expect(memory.calls).toEqual([]);
    });
});
