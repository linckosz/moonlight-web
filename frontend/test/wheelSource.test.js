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
    WHEEL_PENDING,
    WheelInverter,
    WheelSourceTracker,
    wheelEventKind,
    wheelSourceSupport,
} from '../js/stream/WheelSource.js';

function fixture(name) {
    return JSON.parse(
        readFileSync(join(dirname(fileURLToPath(import.meta.url)), 'fixtures', name), 'utf8'),
    );
}

// Real events from a MacBook Air M3, an M590 notched wheel and the trackpad:
// Chrome 126 at page zoom 100 % and 110 %, Safari 27 at 100 % and 115 %; and
// from Chrome 157 on Windows, a Logitech M331 wheel.
const MAC = fixture('wheel-chrome-mac.json').recordings;
const SAFARI = fixture('wheel-safari-mac.json').recordings;
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

/** The rows of a recording, cut into gestures at each pause. */
function gestures(rows) {
    const out = [];
    rows.forEach((r, i) => {
        if (i === 0 || r[5] - rows[i - 1][5] > WHEEL_GESTURE_GAP_MS) out.push([]);
        out[out.length - 1].push(r);
    });
    return out;
}

/** A plain move, as WheelInverter gives it back. */
const move = (dx, dy) => ({ deltaX: dx, deltaY: dy, deltaMode: 0 });

/** A recorded row's move, turned the other way when `inverted`. */
const rowMove = (r, inverted) => (inverted ? move(-r[0] || 0, -r[1] || 0) : move(r[0], r[1]));

/**
 * What a recording sends through an inverter, flushed at each pause as the
 * stream's timer would, with whether each push left travel held back.
 */
function invertAll(support, rows) {
    const inv = new WheelInverter(support);
    const sent = [];
    const held = [];
    rows.forEach((r, i) => {
        const next = rows[i + 1];
        sent.push(...inv.push(ev(r), r[5]));
        held.push(inv.holding);
        if (!next || next[5] - r[5] > WHEEL_GESTURE_GAP_MS) sent.push(...inv.flush());
    });
    return { sent, held };
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

    it('is Safari on macOS', () => {
        expect(wheelSourceSupport(UA.safariMac, 0)).toBe('safari-mac');
    });

    it('is not Firefox, nor Chromium on Windows or Linux', () => {
        expect(wheelSourceSupport(UA.firefoxMac, 0)).toBeNull();
        expect(wheelSourceSupport(UA.chromeWindows, 0)).toBeNull();
        expect(wheelSourceSupport(UA.chromeLinux, 0)).toBeNull();
    });

    it('is not an iPad asking for the desktop site, which reads like a Mac', () => {
        expect(wheelSourceSupport(UA.chromeMac, 5)).toBeNull();
        expect(wheelSourceSupport(UA.safariMac, 5)).toBeNull();
    });

    it('is not another Mac browser on WebKit, which was not measured', () => {
        const webView =
            'Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko)';
        expect(wheelSourceSupport(webView, 0)).toBeNull();
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

describe('the recorded gestures, Safari 27 on macOS', () => {
    for (const rec of SAFARI) {
        const wheel = rec.device === 'wheel';

        it(`${wheel ? 'inverts' : 'leaves'} every event of "${rec.name}"`, () => {
            const { sent } = invertAll('safari-mac', rec.events);
            expect(sent).toEqual(rec.events.map((r) => rowMove(r, wheel)));
            expect(sent.length).toBeGreaterThan(60);
        });
    }

    it('holds nothing back from the trackpad, at 100 % and 115 %', () => {
        const pads = SAFARI.filter((r) => r.device === 'trackpad');
        expect(pads.map((r) => r.zoom).sort()).toEqual([1, 1.15]);
        for (const rec of pads) {
            expect(invertAll('safari-mac', rec.events).held).not.toContain(true);
        }
    });

    it('reads the opening event of every trackpad gesture on its own as continuous', () => {
        const starts = SAFARI.filter((r) => r.device === 'trackpad').flatMap((r) =>
            gestureStarts(r.events),
        );
        expect(starts.length).toBeGreaterThan(20);
        expect(starts.map((r) => wheelEventKind(ev(r), 'safari-mac'))).toEqual(
            starts.map(() => WHEEL_CONTINUOUS),
        );
    });

    it('holds back no more than the two opening steps of a wheel gesture, 8 px', () => {
        // Rounded to 3 decimals, as the fixture keeps them, the opening steps
        // read exactly 4 px: the hold is what the recordings exercise here.
        for (const rec of SAFARI.filter((r) => r.device === 'wheel')) {
            const { held } = invertAll('safari-mac', rec.events);
            let at = 0;
            for (const g of gestures(rec.events)) {
                const h = held.slice(at, at + g.length);
                at += g.length;
                const waited = h.indexOf(false) === -1 ? h.length : h.indexOf(false);
                expect(waited).toBeGreaterThanOrEqual(1);
                expect(waited).toBeLessThanOrEqual(2);
                expect(h.slice(waited)).not.toContain(true);
                expect(g.slice(0, waited).map((r) => Math.abs(r[1]))).toEqual(
                    g.slice(0, waited).map(() => 4),
                );
            }
        }
    });
});

describe('the kind of one Safari event', () => {
    const kind = (dx, dy) => wheelEventKind(move(dx, dy), 'safari-mac');

    it('reads a fraction as notched, an opening notch before rounding included', () => {
        expect(kind(0, 23.605)).toBe(WHEEL_NOTCHED);
        expect(kind(0, 4.000244140625)).toBe(WHEEL_NOTCHED);
        expect(kind(0.5, 3)).toBe(WHEEL_NOTCHED);
    });

    it('waits on an exact 4 px step along one axis', () => {
        expect(kind(0, 4)).toBe(WHEEL_PENDING);
        expect(kind(0, -4)).toBe(WHEEL_PENDING);
        expect(kind(-4, 0)).toBe(WHEEL_PENDING);
    });

    it('reads any other whole number as continuous', () => {
        expect(kind(0, 1)).toBe(WHEEL_CONTINUOUS);
        expect(kind(4, 4)).toBe(WHEEL_CONTINUOUS);
        expect(kind(0, 8)).toBe(WHEEL_CONTINUOUS);
        expect(kind(-1, 5)).toBe(WHEEL_CONTINUOUS);
    });

    it('cannot tell in lines, nor from an event that does not move', () => {
        expect(wheelEventKind({ deltaX: 0, deltaY: 3, deltaMode: 1 }, 'safari-mac')).toBeNull();
        expect(kind(0, 0)).toBeNull();
    });
});

describe('the kind of a Safari gesture', () => {
    it('is decided by the event after an opening step, and held', () => {
        const t = new WheelSourceTracker('safari-mac');
        expect(t.kindOf(move(0, 4), 0)).toBe(WHEEL_PENDING);
        expect(t.kindOf(move(0, 23.605), 58)).toBe(WHEEL_NOTCHED);
        expect(t.kindOf(move(0, 4), 216)).toBe(WHEEL_NOTCHED);
    });

    it('is continuous when whole pixels follow the step', () => {
        const t = new WheelSourceTracker('safari-mac');
        expect(t.kindOf(move(0, 4), 0)).toBe(WHEEL_PENDING);
        expect(t.kindOf(move(1, 5), 16)).toBe(WHEEL_CONTINUOUS);
    });

    it('is notched on a third step in a row: a wheel turned slowly', () => {
        const t = new WheelSourceTracker('safari-mac');
        expect(t.kindOf(move(0, -4), 0)).toBe(WHEEL_PENDING);
        expect(t.kindOf(move(0, -4), 164)).toBe(WHEEL_PENDING);
        expect(t.kindOf(move(0, -4), 328)).toBe(WHEEL_NOTCHED);
    });

    it('keeps waiting through an event that cannot tell', () => {
        const t = new WheelSourceTracker('safari-mac');
        expect(t.kindOf(move(0, 0), 0)).toBeNull();
        expect(t.kindOf(move(0, 4), 10)).toBe(WHEEL_PENDING);
        expect(t.kindOf(move(0, 0), 20)).toBe(WHEEL_PENDING);
        expect(t.kindOf(move(0, 17.5), 30)).toBe(WHEEL_NOTCHED);
    });
});

describe('inverting the notched wheel', () => {
    it('inverts the Chrome wheel and leaves its trackpad, holding nothing back', () => {
        for (const rec of MAC) {
            const { sent, held } = invertAll('chromium-mac', rec.events);
            expect(sent).toEqual(rec.events.map((r) => rowMove(r, rec.device === 'wheel')));
            expect(held).not.toContain(true);
        }
    });

    it('sends a held step with the event that tells, in order', () => {
        const inv = new WheelInverter('safari-mac');
        expect(inv.push(move(0, 4), 0)).toEqual([]);
        expect(inv.holding).toBe(true);
        expect(inv.push(move(0, 23.605), 58)).toEqual([move(0, -4), move(0, -23.605)]);
        expect(inv.holding).toBe(false);
    });

    it('sends a held step unchanged when whole pixels follow', () => {
        const inv = new WheelInverter('safari-mac');
        expect(inv.push(move(0, 4), 0)).toEqual([]);
        expect(inv.push(move(1, 5), 16)).toEqual([move(0, 4), move(1, 5)]);
    });

    it('sends a lone step as a notch once the gesture ends', () => {
        const inv = new WheelInverter('safari-mac');
        expect(inv.push(move(0, -4), 0)).toEqual([]);
        expect(inv.flush()).toEqual([move(0, 4)]);
        expect(inv.holding).toBe(false);
        expect(inv.flush()).toEqual([]);
    });

    it('keeps the gesture notched after a flush, should the timer beat the next event', () => {
        const inv = new WheelInverter('safari-mac');
        inv.push(move(0, 4), 0);
        inv.flush();
        expect(inv.push(move(0, 6), 100)).toEqual([move(0, -6)]);
    });

    it('sends a step a late timer left behind before the next gesture', () => {
        const inv = new WheelInverter('safari-mac');
        inv.push(move(0, 4), 0);
        const later = WHEEL_GESTURE_GAP_MS + 1;
        expect(inv.push(move(0, 1), later)).toEqual([move(0, -4), move(0, 1)]);
    });

    it('never sends a negative zero on the idle axis', () => {
        const inv = new WheelInverter('chromium-mac');
        const [d] = inv.push(ev([0, 4, 0, 0, -120, 0]), 0);
        expect(Object.is(d.deltaX, 0)).toBe(true);
        expect(d.deltaY).toBe(-4);
    });

    it('leaves alone an event whose kind cannot be read', () => {
        const inv = new WheelInverter('chromium-mac');
        const lines = { deltaX: 0, deltaY: 3, deltaMode: 1 };
        expect(inv.push(lines, 0)).toEqual([lines]);
    });
});
