/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect } from 'vitest';
import { SilentDecoderWatch } from '../js/stream/SilentDecoderWatch.js';

/**
 * A decoder that takes every chunk and gives nothing back says nothing about
 * it: the watch reads it off the two counters, received and decoded.
 */
describe('SilentDecoderWatch', () => {
    /** Drive the watch on a 500 ms tick, `rate` chunks a second, none decoded. */
    function silentFor(watch, ms, rate = 50, start = { rx: 100, dec: 40, t: 0 }) {
        let hit = null;
        for (let t = 0; t <= ms; t += 500) {
            const r = watch.observe(
                start.rx + Math.round((t / 1000) * rate),
                start.dec,
                start.t + t,
            );
            if (r) hit = hit || { ...r, at: t };
        }
        return hit;
    }

    it('answers once the decoder has given nothing back for long enough while chunks went in', () => {
        const hit = silentFor(new SilentDecoderWatch(), 3000);
        expect(hit).not.toBe(null);
        expect(hit.at).toBe(1500);
        expect(hit.chunks).toBe(75);
        expect(hit.ms).toBe(1500);
    });

    it('gives a still host its time: few chunks are not a silence yet', () => {
        // Two frames a second: 30 chunks take 15 s, and only then does it count.
        const hit = silentFor(new SilentDecoderWatch(), 20000, 2);
        expect(hit.at).toBe(15000);
    });

    it('does not mistake a short hiccup for a decoder gone', () => {
        const w = new SilentDecoderWatch();
        expect(w.observe(0, 0, 0)).toBe(null);
        expect(w.observe(40, 0, 1000)).toBe(null);
        // A picture at 1.2 s: the count starts over from there.
        expect(w.observe(60, 1, 1200)).toBe(null);
        expect(w.observe(100, 1, 2500)).toBe(null);
        expect(w.observe(140, 1, 2800)).not.toBe(null);
    });

    it('answers once per stretch, and again only after as much again', () => {
        const w = new SilentDecoderWatch();
        w.observe(0, 5, 0);
        expect(w.observe(80, 5, 1600)).not.toBe(null);
        expect(w.observe(90, 5, 1800)).toBe(null);
        expect(w.observe(170, 5, 3200)).not.toBe(null);
    });

    it('says how often the decoder failed in the stretch', () => {
        const w = new SilentDecoderWatch();
        w.observe(0, 9, 0, 2);
        expect(w.observe(80, 9, 1600, 2)).toEqual({ chunks: 80, ms: 1600, faults: 0 });
        expect(w.observe(170, 9, 3200, 5)).toEqual({ chunks: 90, ms: 1600, faults: 3 });
        // Counted from the stretch it answered: none since.
        expect(w.observe(250, 9, 4800, 5)).toEqual({ chunks: 80, ms: 1600, faults: 0 });
    });

    describe('a stall of the link is not a silence of the decoder', () => {
        // The overlay's 500 ms tick; the last picture is read at t = 0. The
        // link then holds the frames for 1502 ms and lets 91 go at once at
        // 1491, and the tick at 1500 reads them (MacBook Air M3, Chrome, HEVC,
        // 13/11/2026 08:05:10: "91 chunks in, no picture for 1507 ms").
        const RX = 5000;
        const DEC = 4990;

        it('keeps quiet when the burst after a 1.5 s stall is read before its first picture', () => {
            const w = new SilentDecoderWatch();
            expect(w.observe(RX, DEC, 0)).toBe(null);
            expect(w.observe(RX, DEC, 500)).toBe(null);
            expect(w.observe(RX, DEC, 1000)).toBe(null);
            expect(w.observe(RX + 91, DEC, 1500)).toBe(null);
            // The decoder answers the burst: the count starts over.
            expect(w.observe(RX + 121, DEC + 95, 2000)).toBe(null);
        });

        it('keeps quiet too when the decoder still held a couple of frames as the link stalled', () => {
            const w = new SilentDecoderWatch();
            expect(w.observe(RX, DEC, 0)).toBe(null);
            // Two frames went in just after the last picture and wait for more.
            expect(w.observe(RX + 2, DEC, 500)).toBe(null);
            expect(w.observe(RX + 2, DEC, 1000)).toBe(null);
            expect(w.observe(RX + 93, DEC, 1500)).toBe(null);
        });

        it('still answers a decoder that gives nothing back once the frames flow again', () => {
            const w = new SilentDecoderWatch();
            w.observe(RX, DEC, 0);
            w.observe(RX, DEC, 500);
            w.observe(RX, DEC, 1000);
            expect(w.observe(RX + 91, DEC, 1500)).toBe(null);
            expect(w.observe(RX + 121, DEC, 2000)).toBe(null);
            // 60 frames a second go in, and nothing comes out of them.
            expect(w.observe(RX + 151, DEC, 2500)).toEqual({ chunks: 151, ms: 1500, faults: 0 });
        });

        it('pauses the count for a stall in a silence, without starting it over', () => {
            const w = new SilentDecoderWatch();
            w.observe(RX, DEC, 0);
            // 1 s of frames taken without a picture...
            expect(w.observe(RX + 30, DEC, 500)).toBe(null);
            expect(w.observe(RX + 60, DEC, 1000)).toBe(null);
            // ...then the link holds them for 1.5 s...
            expect(w.observe(RX + 60, DEC, 1500)).toBe(null);
            expect(w.observe(RX + 60, DEC, 2000)).toBe(null);
            expect(w.observe(RX + 60, DEC, 2500)).toBe(null);
            // ...and the first look with frames in again makes 1.5 s of them.
            expect(w.observe(RX + 150, DEC, 3000)).toEqual({ chunks: 150, ms: 1500, faults: 0 });
        });
    });

    it('starts over when the counters do (a new decoder), or when told to', () => {
        const w = new SilentDecoderWatch();
        w.observe(500, 200, 0);
        expect(w.observe(10, 200, 1600)).toBe(null);
        expect(w.observe(90, 200, 3200)).not.toBe(null);
        w.reset();
        expect(w.observe(200, 200, 3300)).toBe(null);
        expect(w.observe(240, 200, 4000)).toBe(null);
    });
});
