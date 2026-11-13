/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect } from 'vitest';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

import {
    WHEEL_CONTINUOUS,
    WHEEL_GESTURE_GAP_MS,
    WHEEL_NOTCHED,
    WheelSourceTracker,
    wheelEventKind,
    wheelSourceSupport,
} from '../js/stream/WheelSource.js';

function fixture(name) {
    return JSON.parse(
        readFileSync(join(dirname(fileURLToPath(import.meta.url)), 'fixtures', name), 'utf8'),
    );
}

// Real events from Chrome 126 on a MacBook Air M3, an M590 notched wheel and
// the trackpad, at page zoom 100 % and 110 %; and from Chrome 157 on Windows,
// a Logitech M331 wheel.
const MAC = fixture('wheel-chrome-mac.json').recordings;
const WINDOWS = fixture('wheel-chrome-windows.json').events;

/** A wheel event as the page sees it, from one recorded row. */
function ev([deltaX, deltaY, deltaMode, wheelDeltaX, wheelDeltaY, ms]) {
    return { deltaX, deltaY, deltaMode, wheelDeltaX, wheelDeltaY, timeStamp: ms };
}

/** The kind of every event of a recording, through one tracker. */
function trackAll(rows) {
    const t = new WheelSourceTracker();
    return rows.map((r) => t.kindOf(ev(r), r[5]));
}

/** The rows that open a gesture: the first, and each one after a pause. */
function gestureStarts(rows) {
    return rows.filter((r, i) => i === 0 || r[5] - rows[i - 1][5] > WHEEL_GESTURE_GAP_MS);
}

const KIND = { wheel: WHEEL_NOTCHED, trackpad: WHEEL_CONTINUOUS };

const UA = {
    chromeMac:
        'Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36',
    edgeMac:
        'Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36 Edg/131.0.0.0',
    safariMac:
        'Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/18.1 Safari/605.1.15',
    firefoxMac:
        'Mozilla/5.0 (Macintosh; Intel Mac OS X 10.15; rv:132.0) Gecko/20100101 Firefox/132.0',
    chromeWindows:
        'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/157.0.0.0 Safari/537.36',
    chromeLinux:
        'Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36',
};

describe('where the kind of a wheel event can be read', () => {
    it('is Chromium on macOS, Edge included', () => {
        expect(wheelSourceSupport(UA.chromeMac, 0)).toBe('chromium-mac');
        expect(wheelSourceSupport(UA.edgeMac, 0)).toBe('chromium-mac');
    });

    it('is not Safari, Firefox, nor Chromium on Windows or Linux', () => {
        expect(wheelSourceSupport(UA.safariMac, 0)).toBeNull();
        expect(wheelSourceSupport(UA.firefoxMac, 0)).toBeNull();
        expect(wheelSourceSupport(UA.chromeWindows, 0)).toBeNull();
        expect(wheelSourceSupport(UA.chromeLinux, 0)).toBeNull();
    });

    it('is not an iPad asking for the desktop site, which reads like a Mac', () => {
        expect(wheelSourceSupport(UA.chromeMac, 5)).toBeNull();
    });
});

describe('the recorded gestures, Chrome 126 on macOS', () => {
    for (const rec of MAC) {
        const want = KIND[rec.device];

        it(`reads every event of "${rec.name}" (zoom ${rec.zoom}) as ${want}`, () => {
            const kinds = trackAll(rec.events);
            expect(kinds.filter((k) => k !== want)).toEqual([]);
            expect(kinds.length).toBeGreaterThan(20);
        });

        it(`reads the first event of each gesture of "${rec.name}" on its own as ${want}`, () => {
            const starts = gestureStarts(rec.events);
            expect(starts.map((r) => wheelEventKind(ev(r)))).toEqual(starts.map(() => want));
        });
    }

    it('reads every trackpad event on its own as continuous, at 100 % and 110 %', () => {
        const rows = MAC.filter((r) => r.device === 'trackpad').flatMap((r) => r.events);
        expect(rows.length).toBeGreaterThan(1400);
        expect(rows.filter((r) => wheelEventKind(ev(r)) !== WHEEL_CONTINUOUS)).toEqual([]);
    });

    it('covers a page zoom of 110 %, where a notch reads 109 and deltas are fractional', () => {
        const zoomed = MAC.filter((r) => r.zoom === 1.1);
        expect(zoomed.map((r) => r.device).sort()).toEqual(['trackpad', 'wheel']);
        expect(zoomed.find((r) => r.device === 'wheel').events[0][4]).toBe(-109);
    });
});

describe('the kind of one event, at the edges', () => {
    it('takes a trackpad event that lost 1 to rounding for a continuous one', () => {
        // -3 × -44 = 132; Chromium's 120 × -44 / 40 came out as 131.
        expect(wheelEventKind(ev([9, -44, 0, -27, 131, 0]))).toBe(WHEEL_CONTINUOUS);
    });

    it('cannot tell without wheelDelta, nor in lines or pages', () => {
        expect(wheelEventKind({ deltaX: 0, deltaY: 4, deltaMode: 0 })).toBeNull();
        expect(
            wheelEventKind({
                deltaX: 0,
                deltaY: 3,
                deltaMode: 1,
                wheelDeltaX: 0,
                wheelDeltaY: -120,
            }),
        ).toBeNull();
        expect(
            wheelEventKind({
                deltaX: 0,
                deltaY: 1,
                deltaMode: 2,
                wheelDeltaX: 0,
                wheelDeltaY: -120,
            }),
        ).toBeNull();
    });

    it('cannot tell from an event that does not move', () => {
        expect(wheelEventKind(ev([0, 0, 0, 0, 0, 0]))).toBeNull();
    });

    it('reads a tilt wheel, notched on the horizontal axis only', () => {
        expect(wheelEventKind(ev([4, 0, 0, -120, 0, 0]))).toBe(WHEEL_NOTCHED);
    });

    it('reads a notch that lands on the identity as continuous, on its own', () => {
        // Recorded: one notch of 39.976 px, wheelDelta -120, inside a burst.
        expect(wheelEventKind(ev([0, 39.976, 0, 0, -120, 0]))).toBe(WHEEL_CONTINUOUS);
    });
});

describe('the kind of a gesture', () => {
    it('is decided by its first event and held, so a gesture never changes kind', () => {
        const t = new WheelSourceTracker();
        expect(t.kindOf(ev([0, 4, 0, 0, -120, 0]), 0)).toBe(WHEEL_NOTCHED);
        expect(t.kindOf(ev([0, 39.976, 0, 0, -120, 0]), 27)).toBe(WHEEL_NOTCHED);
        expect(t.kindOf(ev([0, 146.213, 0, 0, -360, 0]), 60)).toBe(WHEEL_NOTCHED);
    });

    it('starts again after a pause, so the trackpad after the wheel reads continuous', () => {
        const t = new WheelSourceTracker();
        expect(t.kindOf(ev([0, -4, 0, 0, 120, 0]), 0)).toBe(WHEEL_NOTCHED);
        const later = WHEEL_GESTURE_GAP_MS + 1;
        expect(t.kindOf(ev([0, -1, 0, 0, 3, 0]), later)).toBe(WHEEL_CONTINUOUS);
        expect(t.kindOf(ev([2, -12, 0, -6, 36, 0]), later + 16)).toBe(WHEEL_CONTINUOUS);
    });

    it('waits for a readable event before deciding', () => {
        const t = new WheelSourceTracker();
        expect(t.kindOf({ deltaX: 0, deltaY: -4, deltaMode: 0 }, 0)).toBeNull();
        expect(t.kindOf(ev([0, -4, 0, 0, 120, 0]), 20)).toBe(WHEEL_NOTCHED);
    });

    it('gives a notch that bounces back its gesture kind, never a kind of its own', () => {
        // Chrome 157 on Windows, a Logitech M331 turned fast: some notches go
        // the other way in the middle of a turn (9275, 14213, 14229, 15080 ms).
        const bounces = WINDOWS.filter((r, i) => {
            const prev = WINDOWS[i - 1];
            return (
                prev &&
                r[5] - prev[5] <= WHEEL_GESTURE_GAP_MS &&
                Math.sign(r[1]) !== Math.sign(prev[1])
            );
        });
        expect(bounces.map((r) => r[5])).toEqual(
            expect.arrayContaining([9275, 14213, 14229, 15080]),
        );
        const kinds = trackAll(WINDOWS);
        expect(new Set(kinds)).toEqual(new Set([WHEEL_NOTCHED]));
    });
});
