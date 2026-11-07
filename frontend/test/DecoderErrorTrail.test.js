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
 * DecoderErrorTrail — the context line logged with a "Decoding error".
 */
import { describe, it, expect } from 'vitest';
import { DecoderErrorTrail, GAP_WINDOW_MS } from '../js/stream/DecoderErrorTrail.js';

const decodingError = { name: 'EncodingError', message: 'Decoding error' };

describe('DecoderErrorTrail', () => {
    it('says nothing was seen before the first frame', () => {
        const t = new DecoderErrorTrail();
        expect(t.line(decodingError, 1000, 'hevc')).toBe(
            'Decoder error context: codec=hevc error=EncodingError "Decoding error" ' +
                'queue=0 key=none gap=0ms last=none flush=none',
        );
    });

    it('names the stall, the burst and the chain after a stall with no loss', () => {
        const t = new DecoderErrorTrail();
        // 60 fps, a keyframe, then nine deltas.
        let now = 0;
        for (let i = 0; i < 10; i++) {
            now = i * 16;
            t.noteArrival(now);
            t.noteDecode(i === 0, 1);
        }
        // The link stalls 900 ms; the frames it held come in one burst.
        now += 900;
        for (let i = 0; i < 6; i++) {
            t.noteArrival(now + i);
            t.noteDecode(false, 2 + i);
        }
        const line = t.line(decodingError, now + 10, 'hevc');
        expect(line).toContain(' queue=7 ');
        expect(line).toContain(' key=15 ');
        expect(line).toContain(' gap=900ms ');
        expect(line).toContain(' last=5ms ');
        expect(line).toContain(' flush=none');
    });

    it('forgets a stall older than its window', () => {
        const t = new DecoderErrorTrail();
        t.noteArrival(0);
        t.noteArrival(1500); // a 1.5 s wait, ending at 1500
        expect(t.longestGap(1600)).toBe(1500);
        expect(t.longestGap(1500 + GAP_WINDOW_MS + 1)).toBe(0);
        for (let at = 1516; at < 1500 + GAP_WINDOW_MS + 100; at += 16) t.noteArrival(at);
        expect(t.longestGap(1500 + GAP_WINDOW_MS + 100)).toBe(16);
    });

    it('counts from the keyframe and times the flush', () => {
        const t = new DecoderErrorTrail();
        t.noteDecode(false, 0); // a delta before any keyframe: still none
        expect(t.line(null, 0, 'h264')).toContain(' key=none ');
        t.noteDecode(true, 9);
        t.noteFlush(100);
        t.noteDecode(false, 0);
        const line = t.line({ name: 'OperationError', code: 0, message: 'x' }, 350, 'h264');
        expect(line).toContain('error=OperationError/0 "x"');
        expect(line).toContain(' queue=0 key=1 ');
        expect(line).toContain(' flush=250ms');
    });

    it('starts over on reset', () => {
        const t = new DecoderErrorTrail();
        t.noteArrival(0);
        t.noteArrival(800);
        t.noteDecode(true, 4);
        t.noteFlush(10);
        t.reset();
        expect(t.line(decodingError, 900, 'av1')).toContain(
            'queue=0 key=none gap=0ms last=none flush=none',
        );
    });
});
