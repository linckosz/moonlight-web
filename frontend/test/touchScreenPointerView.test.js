/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect } from 'vitest';

import { StreamView } from '../js/ui/StreamView.js';
import { TouchScreenPointer } from '../js/stream/TouchScreenPointer.js';

/**
 * Issue #61, as the stream draws it: the pointer the page draws for the host
 * stays hidden in Touch Screen Mode until a mouse moves.
 */

const P = StreamView.prototype;

/** A phone's stream with the host's pointer handed over, at 100,100. */
function view(touchScreen) {
    const el = document.createElement('img');
    el.hidden = true;
    return {
        el,
        _clientCursorEl: el,
        _clientCursorPos: { x: 100, y: 100 },
        _clientCursorImg: null,
        _hostDrawsCursor: true,
        _hostCursorVisible: true,
        _hostCursorHotspot: [0, 0],
        _zoom: 1,
        _touchScreenPointer: new TouchScreenPointer(touchScreen),
        _clientCursorActive: () => true,
        _clientCursorCssScale: () => 1,
        _pictureWidth: () => 1280,
        _pictureHeight: () => 720,
        _mediaRect: () => ({ left: 0, top: 0, width: 1280, height: 720 }),
    };
}

describe('the pointer the page draws for the host', () => {
    it('stays hidden in Touch Screen Mode', () => {
        const v = view(true);
        P._placeClientCursor.call(v);
        expect(v.el.hidden).toBe(true);
    });

    it('shows once a mouse has moved, and hides again at a touch', () => {
        const v = view(true);
        v._touchScreenPointer.moved('mouse');
        P._placeClientCursor.call(v);
        expect(v.el.hidden).toBe(false);
        v._touchScreenPointer.touched();
        P._placeClientCursor.call(v);
        expect(v.el.hidden).toBe(true);
    });

    it('shows as before in the trackpad model', () => {
        const v = view(false);
        P._placeClientCursor.call(v);
        expect(v.el.hidden).toBe(false);
    });
});
