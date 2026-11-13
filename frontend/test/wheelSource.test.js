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

// Real events from Chrome 126 on a MacBook Air M3: a Logitech M590 notched
// wheel and the built-in trackpad, natural scrolling on.
const RECORDED = JSON.parse(
    readFileSync(
        join(dirname(fileURLToPath(import.meta.url)), 'fixtures', 'wheel-chrome-mac.json'),
        'utf8',
    ),
);

/** A wheel event as the page sees it, from one recorded row. */
function ev([deltaX, deltaY, deltaMode, wheelDeltaX, wheelDeltaY, ms]) {
    return { deltaX, deltaY, deltaMode, wheelDeltaX, wheelDeltaY, timeStamp: ms };
}

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
        'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36',
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

describe('the kind of one recorded event (Chrome 126, macOS)', () => {
    for (const [name, rows] of Object.entries(RECORDED.gestures)) {
        const want = name.startsWith('wheel_') ? WHEEL_NOTCHED : WHEEL_CONTINUOUS;
        it(`reads every event of ${name} as ${want}`, () => {
            const kinds = rows.map((r) => wheelEventKind(ev(r)));
            expect(kinds.filter((k) => k !== want)).toEqual([]);
            expect(kinds.length).toBeGreaterThan(20);
        });
    }

    it('takes a trackpad event that lost 1 to rounding for a continuous one', () => {
        // -3 × -44 = 132; Chromium's 120 × -44 / 40 came out as 131.
        expect(wheelEventKind(ev([9, -44, 0, -27, 131, 0]))).toBe(WHEEL_CONTINUOUS);
    });
});

describe('the kind of one event, at the edges', () => {
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
        expect(wheelEventKind(ev([39.5, 0, 0, -120, 0, 0]))).toBe(WHEEL_NOTCHED);
    });

    it('leaves continuous an event whose wheelDelta is not in whole notches', () => {
        // Fractional deltas, as a page zoom might give a trackpad: never inverted.
        expect(wheelEventKind(ev([0, -11.818, 0, 0, 39, 0]))).toBe(WHEEL_CONTINUOUS);
    });

    it('reads as continuous a notch that lands exactly on the identity', () => {
        // One notch of exactly 40 px with wheelDelta 120 looks continuous on its own.
        expect(wheelEventKind(ev([0, -40, 0, 0, 120, 0]))).toBe(WHEEL_CONTINUOUS);
    });
});

describe('the kind of a gesture', () => {
    it('is decided by its first event and held, so a gesture never changes kind', () => {
        const t = new WheelSourceTracker();
        // A notched burst opens on 4 px; its 40 px event alone would read continuous.
        expect(t.kindOf(ev([0, -4, 0, 0, 120, 0]), 0)).toBe(WHEEL_NOTCHED);
        expect(t.kindOf(ev([0, -40, 0, 0, 120, 0]), 30)).toBe(WHEEL_NOTCHED);
        expect(t.kindOf(ev([0, -146.2, 0, 0, 360, 0]), 60)).toBe(WHEEL_NOTCHED);
    });

    it('starts again after a pause, so the trackpad after the wheel reads continuous', () => {
        const t = new WheelSourceTracker();
        expect(t.kindOf(ev([0, -4, 0, 0, 120, 0]), 0)).toBe(WHEEL_NOTCHED);
        const later = WHEEL_GESTURE_GAP_MS + 1;
        expect(t.kindOf(ev([0, -1, 0, 0, 3, 0]), later)).toBe(WHEEL_CONTINUOUS);
        expect(t.kindOf(ev([2, -12, 0, -6, 36, 0]), later + 16)).toBe(WHEEL_CONTINUOUS);
    });

    it('holds through a trackpad inertia tail, events 16 ms apart', () => {
        const t = new WheelSourceTracker();
        const kinds = RECORDED.gestures.trackpad_down.map((r) => t.kindOf(ev(r), r[5]));
        expect(new Set(kinds)).toEqual(new Set([WHEEL_CONTINUOUS]));
    });

    it('reads every recorded gesture right, one tracker for the whole session', () => {
        const t = new WheelSourceTracker();
        // The gestures were recorded one after the other, seconds apart.
        let offset = 0;
        for (const [name, rows] of Object.entries(RECORDED.gestures)) {
            const want = name.startsWith('wheel_') ? WHEEL_NOTCHED : WHEEL_CONTINUOUS;
            for (const r of rows) expect(t.kindOf(ev(r), offset + r[5])).toBe(want);
            offset += rows[rows.length - 1][5] + 10_000;
        }
    });

    it('waits for a readable event before deciding', () => {
        const t = new WheelSourceTracker();
        expect(t.kindOf({ deltaX: 0, deltaY: -4, deltaMode: 0 }, 0)).toBeNull();
        expect(t.kindOf(ev([0, -4, 0, 0, 120, 0]), 20)).toBe(WHEEL_NOTCHED);
    });
});
