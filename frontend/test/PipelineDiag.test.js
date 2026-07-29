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
 * PipelineDiag — observable behaviour only: what a snapshot reports for a given
 * sequence of pipeline events, and what the formatted line contains.
 */
import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';
import { PipelineDiag, formatDiag } from '../js/stream/PipelineDiag.js';

/** Drive performance.now() by hand so the sliding window is deterministic. */
let clock = 0;

beforeEach(() => {
    clock = 1000;
    vi.spyOn(performance, 'now').mockImplementation(() => clock);
});

afterEach(() => {
    vi.restoreAllMocks();
});

describe('PipelineDiag', () => {
    it('reports zeros before any observation', () => {
        const snap = new PipelineDiag().snapshot();
        expect(snap.arrivalAvgMs).toBe(0);
        expect(snap.decodeQueueMax).toBe(0);
        expect(snap.renderPath).toBe('');
        expect(snap.dropStale).toBe(0);
    });

    it('measures the interval between arrivals, not the first one', () => {
        const d = new PipelineDiag();
        d.noteArrival(); // no interval yet
        clock += 16;
        d.noteArrival();
        clock += 20;
        d.noteArrival();

        const snap = d.snapshot();
        expect(snap.arrivalAvgMs).toBe(18);
        expect(snap.arrivalMaxMs).toBe(20);
    });

    it('records encoded frame sizes, which have no millisecond bound', () => {
        const d = new PipelineDiag();
        d.noteArrival(1000);
        d.noteArrival(180000); // full-size frame: well past the ms sanity bound
        d.noteArrival(); // size unknown — not a zero-byte sample

        const snap = d.snapshot();
        expect(snap.frameBytesAvg).toBe(90500);
        expect(snap.frameBytesMax).toBe(180000);
    });

    it('reports avg and max queue depths', () => {
        const d = new PipelineDiag();
        d.noteDecodeQueue(1);
        d.noteDecodeQueue(7);
        d.noteFrameQueue(0);
        d.noteFrameQueue(2);

        const snap = d.snapshot();
        expect(snap.decodeQueueAvg).toBe(4);
        expect(snap.decodeQueueMax).toBe(7);
        expect(snap.frameQueueAvg).toBe(1);
        expect(snap.frameQueueMax).toBe(2);
    });

    it('splits the draw into submit and wait, and keeps the last path', () => {
        const d = new PipelineDiag();
        d.noteDraw({ submitMs: 1, waitMs: 20, path: 'sgsr' });
        d.noteDraw({ submitMs: 3, waitMs: 10, path: 'off' });
        d.noteDraw(null); // renderer not ready — ignored, no throw

        const snap = d.snapshot();
        expect(snap.renderSubmitMs).toBe(2);
        expect(snap.renderWaitMs).toBe(15);
        expect(snap.renderPath).toBe('off');
    });

    it('counts drops per cause and ignores unknown ones', () => {
        const d = new PipelineDiag();
        d.noteDrop('stale');
        d.noteDrop('stale');
        d.noteDrop('queueFull');
        d.noteDrop('nonsense');

        const snap = d.snapshot();
        expect(snap.dropStale).toBe(2);
        expect(snap.dropQueueFull).toBe(1);
        expect(snap.dropBackpressure).toBe(0);
    });

    it('forgets samples older than the window (drop counters stay cumulative)', () => {
        const d = new PipelineDiag(2000);
        d.noteDecodeQueue(8);
        d.noteDrop('stale');
        clock += 2500;
        d.noteDecodeQueue(1);

        const snap = d.snapshot();
        expect(snap.decodeQueueAvg).toBe(1);
        expect(snap.decodeQueueMax).toBe(1);
        expect(snap.dropStale).toBe(1);
    });

    it('rejects out-of-range samples instead of poisoning the window', () => {
        const d = new PipelineDiag();
        d.noteArrival();
        clock += 60000; // stream stalled a minute — not a cadence measurement
        d.noteArrival();
        d.noteDecodeQueue(-1);

        const snap = d.snapshot();
        expect(snap.arrivalAvgMs).toBe(0);
        expect(snap.decodeQueueAvg).toBe(0);
    });
});

describe('formatDiag', () => {
    it('returns an empty string without a snapshot', () => {
        expect(formatDiag(null, {})).toBe('');
    });

    it('shows the decoded-vs-presented gap and the draw split', () => {
        const d = new PipelineDiag();
        d.noteDraw({ submitMs: 1.5, waitMs: 22.25, path: 'drawImage' });
        d.noteDrop('stale');

        const line = formatDiag(d.snapshot(), {
            decodedFps: 60,
            presentedFps: 41.6,
            dropsPerSec: 18.4,
        });

        expect(line).toContain('fps in/out 60/42');
        expect(line).toContain('frame 0.0/0.0KB');
        expect(line).toContain('draw 1.5+22.3ms');
        expect(line).toContain('[drawImage]');
        expect(line).toContain('stale 1');
    });

    it('tolerates missing rates', () => {
        const line = formatDiag(new PipelineDiag().snapshot(), {});
        expect(line).toContain('fps in/out 0/0');
    });
});
