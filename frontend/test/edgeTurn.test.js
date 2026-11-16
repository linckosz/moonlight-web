/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
/* global document, window */
import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import { StreamView } from '../js/ui/StreamView.js';

/**
 * IPADFPS: without the pointer lock, Safari's pointer stops at the page's
 * edge and so does movementX. While it rests there and the hand keeps
 * pushing, lock-free look goes on turning at the speed the hand had.
 */
let now = 0;
let frames = [];
function tick(ms) {
    now += ms;
    const run = frames;
    frames = [];
    for (const f of run) f();
}

beforeEach(() => {
    now = 1000;
    frames = [];
    vi.spyOn(performance, 'now').mockImplementation(() => now);
    vi.stubGlobal('requestAnimationFrame', (f) => frames.push(f));
    vi.stubGlobal('cancelAnimationFrame', () => {
        frames = [];
    });
    vi.spyOn(document, 'hasFocus').mockReturnValue(true);
    vi.spyOn(console, 'log').mockImplementation(() => {});
});
afterEach(() => {
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
const right = () => window.innerWidth - 1;
const move = (v, clientX, movementX, movementY = 0) =>
    v._onGamingMouseMove({ clientX, clientY: 300, movementX, movementY, buttons: 0 });

/** A push to the right edge at 10 px per 10 ms (1000 px/s), then the edge. */
function pushToRightEdge(v) {
    for (let i = 0; i < 10; i++) {
        now += 10;
        move(v, right() - 100 + i * 10, 10);
    }
    move(v, right(), 0);
}
const turned = (sent) => sent.filter((m) => 'dx' in m).reduce((a, m) => a + m.dx, 0);

describe('edge turn in lock-free look', () => {
    it('keeps turning at the speed the hand reached the edge with', () => {
        const { v, sent } = view();
        pushToRightEdge(v);
        sent.length = 0;
        for (let i = 0; i < 10; i++) {
            tick(10);
            move(v, right(), 0); // still pushing: Safari reports nothing to move
        }
        // 100 ms at 1000 px/s.
        expect(turned(sent)).toBe(100);
        expect(sent.every((m) => m.dy === 0)).toBe(true);
    });

    it('turns no slower than the floor after a slow approach', () => {
        const { v, sent } = view();
        for (let i = 0; i < 10; i++) {
            now += 10;
            move(v, right() - 10 + i, 1); // 100 px/s
        }
        move(v, right(), 0);
        sent.length = 0;
        for (let i = 0; i < 10; i++) {
            tick(10);
            move(v, right(), 0);
        }
        // 100 ms at the floor.
        expect(turned(sent)).toBe(StreamView.EDGE_TURN_MIN_SPEED / 10);
    });

    it('stops when the hand stops pushing', () => {
        const { v, sent } = view();
        pushToRightEdge(v);
        tick(16);
        tick(16);
        sent.length = 0;
        tick(StreamView.EDGE_TURN_IDLE_MS);
        tick(16);
        expect(sent).toEqual([]);
        expect(v._edgeTurn).toBe(null);
    });

    it('stops when the pointer leaves the edge', () => {
        const { v, sent } = view();
        pushToRightEdge(v);
        move(v, right() - 5, -5);
        sent.length = 0;
        tick(16);
        expect(sent).toEqual([]);
    });

    it('sends nothing more once the mouse is taken from this viewer', () => {
        const { v, sent } = view();
        pushToRightEdge(v);
        tick(16);
        v._mouseAllowed = false;
        sent.length = 0;
        tick(16);
        expect(sent).toEqual([]);
        expect(v._edgeTurn).toBe(null);
    });

    it('stops when the host shows its pointer again', () => {
        const { v, sent } = view();
        pushToRightEdge(v);
        v._hostCursorVisible = true;
        move(v, right(), 0);
        sent.length = 0;
        tick(16);
        expect(sent.filter((m) => 'dx' in m)).toEqual([]);
    });

    it('turns left at the left edge, never at the top or bottom', () => {
        const { v, sent } = view();
        for (let i = 0; i < 10; i++) {
            now += 10;
            move(v, 100 - i * 10, -10);
        }
        move(v, 0, 0);
        sent.length = 0;
        tick(16);
        move(v, 0, 0);
        expect(turned(sent)).toBeLessThan(0);
        v._stopEdgeTurn();
        v._onGamingMouseMove({
            clientX: 500,
            clientY: 0,
            movementX: 0,
            movementY: -10,
            buttons: 0,
        });
        sent.length = 0;
        tick(16);
        expect(sent).toEqual([]);
    });

    it('gives up after the backstop until the next real move', () => {
        const { v } = view();
        pushToRightEdge(v);
        for (let t = 0; t <= StreamView.EDGE_TURN_MAX_MS; t += 100) {
            tick(100);
            move(v, right(), 0);
        }
        expect(v._edgeTurn).toBe(null);
        move(v, right(), 0);
        expect(v._edgeTurn).toBe(null);
        move(v, right() - 10, -10);
        move(v, right(), 10);
        expect(v._edgeTurn).not.toBe(null);
    });
});
