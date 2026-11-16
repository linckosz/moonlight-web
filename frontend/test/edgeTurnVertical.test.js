/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
/* global document, window */
import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import { StreamView } from '../js/ui/StreamView.js';

/**
 * VERTFIX: iPadOS takes the top and bottom edges (the notification centre,
 * the home bar), so the pointer never rests on them. In full screen only, a
 * band before them turns the camera up or down once the pointer has rested
 * there (V1-V7).
 */
let now = 0;
let frames = [];
let fullscreen = null;
function tick(ms) {
    now += ms;
    const run = frames;
    frames = [];
    for (const f of run) f();
}
function ticks(n, ms = 16) {
    for (let i = 0; i < n; i++) tick(ms);
}

beforeEach(() => {
    now = 1000;
    frames = [];
    fullscreen = document.createElement('div');
    Object.defineProperty(document, 'fullscreenElement', {
        configurable: true,
        get: () => fullscreen,
    });
    vi.spyOn(performance, 'now').mockImplementation(() => now);
    vi.stubGlobal('requestAnimationFrame', (f) => frames.push(f));
    vi.stubGlobal('cancelAnimationFrame', () => {
        frames = [];
    });
    vi.spyOn(document, 'hasFocus').mockReturnValue(true);
    vi.spyOn(console, 'log').mockImplementation(() => {});
});
afterEach(() => {
    delete document.fullscreenElement;
    vi.restoreAllMocks();
    vi.unstubAllGlobals();
});

function view() {
    const inputEl = document.createElement('div');
    const sent = [];
    const v = Object.create(StreamView.prototype);
    Object.assign(v, {
        inputEl,
        _gamingMode: true,
        _mouseAllowed: true,
        _mouseFocused: false,
        pointerLocked: false,
        _pointerLockRefused: true,
        _lockFreeLook: true,
        _lockRefusalFinal: true,
        _mouseSensitivity: 1,
        _mouseCarryX: 0,
        _mouseCarryY: 0,
        _hostCursorScale: 1,
        // A game that hides the pointer: lock-free look sends deltas.
        _hostDrawsCursor: true,
        _hostCursorVisible: false,
        _clientCursorPos: null,
        _clientCursorEl: null,
        _clientCursorActive: () => true,
        _pictureWidth: () => 1920,
        _pictureHeight: () => 1080,
        _mediaRect: () => ({ left: 0, top: 0, width: 960, height: 540 }),
        _sendToHost: (m) => sent.push(m),
        _noteMouseSent() {},
        _applyLocalCursor() {},
        _disarmPointerCapture() {},
        _lockPointerOnClick() {},
    });
    v._bindGamingEvents();
    return { v, sent };
}
const move = (v, clientX, clientY, movementX, movementY) =>
    v._onGamingMouseMove({ clientX, clientY, movementX, movementY, buttons: 0 });
const vertical = (sent) => sent.filter((m) => 'dy' in m).reduce((a, m) => a + m.dy, 0);
const sideways = (sent) => sent.filter((m) => 'dx' in m).reduce((a, m) => a + m.dx, 0);
const bottom = () => window.innerHeight;

/** Up into the top band at 6 px per 10 ms (600 px/s), ending 46 px from the top. */
function approachTop(v, x = 500) {
    for (let i = 0; i < 10; i++) {
        now += 10;
        move(v, x, 100 - (i + 1) * 6 + 6, 0, -6);
    }
}
/** The same towards the bottom edge. */
function approachBottom(v, x = 500) {
    for (let i = 0; i < 10; i++) {
        now += 10;
        move(v, x, bottom() - 100 + (i + 1) * 6 - 6, 0, 6);
    }
}
/** Past the rest, and a few frames of turning. */
function restThenTurn(n = 10) {
    ticks(Math.ceil(StreamView.EDGE_TURN_REST_MS / 16) + 1);
    ticks(n);
}

describe('vertical edge turn in lock-free look (full screen)', () => {
    it('arms on the way up and turns up only after the rest', () => {
        const { v, sent } = view();
        approachTop(v);
        expect(v._edgeTurn?.axis).toBe('y');
        sent.length = 0;
        ticks(Math.floor((StreamView.EDGE_TURN_REST_MS - 1) / 16));
        expect(sent).toEqual([]);
        restThenTurn();
        expect(vertical(sent)).toBeLessThan(0);
        expect(sideways(sent)).toBe(0);
    });

    it('turns down in the bottom band, at the speed the hand had', () => {
        const { v, sent } = view();
        approachBottom(v);
        sent.length = 0;
        ticks(Math.ceil(StreamView.EDGE_TURN_REST_MS / 16) + 1);
        sent.length = 0;
        ticks(10, 10);
        // 100 ms at 600 px/s.
        expect(vertical(sent)).toBe(60);
    });

    it('never arms outside full screen', () => {
        fullscreen = null;
        const { v, sent } = view();
        approachTop(v);
        sent.length = 0;
        restThenTurn(30);
        expect(sent).toEqual([]);
        expect(v._edgeTurn ?? null).toBe(null);
    });

    it('stops when the page leaves full screen', () => {
        const { v, sent } = view();
        approachTop(v);
        restThenTurn();
        fullscreen = null;
        sent.length = 0;
        ticks(5);
        expect(sent).toEqual([]);
        expect(v._edgeTurn ?? null).toBe(null);
    });

    it('stops when the window loses focus or the page is hidden', () => {
        const { v, sent } = view();
        approachTop(v);
        restThenTurn();
        document.hasFocus.mockReturnValue(false);
        sent.length = 0;
        ticks(5);
        expect(sent).toEqual([]);
        expect(v._edgeTurn ?? null).toBe(null);
    });

    it('stops at the first touch of the edge, and arms again only after leaving the band', () => {
        const { v, sent } = view();
        approachTop(v);
        restThenTurn();
        move(v, 500, 1, 0, -2);
        expect(v._edgeTurn ?? null).toBe(null);
        // Back into the band towards the edge: still nothing.
        move(v, 500, 20, 0, 4);
        move(v, 500, 10, 0, -10);
        sent.length = 0;
        restThenTurn(20);
        expect(sent).toEqual([]);
        // Out of the band, and in again: armed.
        move(v, 500, 120, 0, 110);
        now += 200;
        approachTop(v);
        sent.length = 0;
        restThenTurn();
        expect(vertical(sent)).toBeLessThan(0);
    });

    it('rests again after a real move towards the edge, and starts again after the same rest', () => {
        const { v, sent } = view();
        approachTop(v);
        restThenTurn();
        now += 10;
        move(v, 500, 40, 0, -6);
        expect(v._edgeTurn?.active).toBe(false);
        sent.length = 0;
        ticks(Math.floor((StreamView.EDGE_TURN_REST_MS - 1) / 16));
        expect(vertical(sent)).toBe(0);
        restThenTurn();
        expect(vertical(sent)).toBeLessThan(0);
    });

    it('is disarmed by aiming sideways in the band (R1)', () => {
        const { v, sent } = view();
        approachTop(v);
        restThenTurn();
        move(v, 510, 40, 10, 0);
        expect(v._edgeTurn ?? null).toBe(null);
        sent.length = 0;
        tick(300);
        ticks(20);
        expect(vertical(sent)).toBe(0);
        // Armed again by the next move towards the edge only.
        now += 200;
        approachTop(v);
        sent.length = 0;
        restThenTurn();
        expect(vertical(sent)).toBeLessThan(0);
    });

    it("is disarmed by a resting pointer's sideways move before it ever turned", () => {
        const { v, sent } = view();
        approachTop(v);
        move(v, 510, 46, 10, 0);
        expect(v._edgeTurn ?? null).toBe(null);
        sent.length = 0;
        restThenTurn(20);
        expect(vertical(sent)).toBe(0);
    });

    it('is disarmed by a real move away from the edge', () => {
        const { v, sent } = view();
        approachTop(v);
        restThenTurn();
        move(v, 500, 40, 0, 6);
        expect(v._edgeTurn ?? null).toBe(null);
        sent.length = 0;
        restThenTurn(20);
        expect(sent).toEqual([]);
    });

    it("never sends the hand's small dy while it turns", () => {
        const { v, sent } = view();
        approachTop(v);
        restThenTurn();
        sent.length = 0;
        move(v, 500, 40, 0, -1);
        move(v, 500, 40, 0, 2);
        expect(sent).toEqual([]);
        expect(v._edgeTurn?.active).toBe(true);
    });

    it('arms nothing for a pointer that slides into the band sideways', () => {
        const { v, sent } = view();
        move(v, 300, 30, 0, 0);
        for (let i = 0; i < 10; i++) {
            now += 10;
            move(v, 300 + i * 10, 30, 10, i % 2 ? -1 : 1);
        }
        sent.length = 0;
        restThenTurn(30);
        expect(vertical(sent)).toBe(0);
        expect(v._edgeTurn ?? null).toBe(null);
    });

    it('gives up after its cap, until the pointer leaves the band', () => {
        const { v, sent } = view();
        approachTop(v);
        ticks(Math.ceil(StreamView.EDGE_TURN_REST_MS / 16) + 1);
        for (let t = 0; t <= StreamView.EDGE_TURN_VERT_MAX_MS; t += 50) tick(50);
        tick(50);
        expect(v._edgeTurn ?? null).toBe(null);
        move(v, 500, 30, 0, -4);
        sent.length = 0;
        restThenTurn(20);
        expect(sent).toEqual([]);
    });

    it('turns sideways only in a corner', () => {
        const { v, sent } = view();
        const right = window.innerWidth - 1;
        approachTop(v, right - 100);
        sent.length = 0;
        restThenTurn();
        expect(vertical(sent)).toBeLessThan(0);
        // The side edge too: the vertical turn gives way.
        for (let i = 0; i < 10; i++) {
            now += 10;
            move(v, right - 90 + i * 10, 40, 10, 0);
        }
        move(v, right, 40, 0, 0);
        expect(v._edgeTurn?.axis).toBe('x');
        sent.length = 0;
        for (let i = 0; i < 10; i++) {
            tick(10);
            move(v, right, 40, 0, 0);
        }
        expect(sideways(sent)).toBeGreaterThan(0);
        expect(vertical(sent)).toBe(0);
    });

    it('sends nothing for a guest whose mouse is taken back', () => {
        const { v, sent } = view();
        approachTop(v);
        restThenTurn();
        v._mouseAllowed = false;
        sent.length = 0;
        ticks(5);
        expect(sent).toEqual([]);
        expect(v._edgeTurn ?? null).toBe(null);
        // Nor arms again without the right.
        approachTop(v);
        expect(v._edgeTurn ?? null).toBe(null);
        sent.length = 0;
        restThenTurn(10);
        expect(vertical(sent)).toBe(0);
    });

    it('stops when the host shows its pointer (absolute mode)', () => {
        const { v, sent } = view();
        approachTop(v);
        restThenTurn();
        v._hostCursorVisible = true;
        move(v, 500, 40, 0, 0);
        sent.length = 0;
        ticks(5);
        expect(vertical(sent)).toBe(0);
        expect(v._edgeTurn ?? null).toBe(null);
    });
});
