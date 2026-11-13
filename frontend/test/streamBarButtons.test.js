/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect, afterEach } from 'vitest';

import { placeGamepadButton, placeKeyboardButton } from '../js/ui/streamBarButtons.js';

/**
 * The stream bar on a touch screen, left to right: the mark, then the
 * microphone and Fullscreen in the middle, then the gamepad, the keyboard,
 * Share and Stop.
 */

/** The bar as the view first draws it: the mark, Fullscreen, Stop. */
function header() {
    const el = document.createElement('div');
    el.className = 'stream-header';
    el.innerHTML = `
        <div class="stream-brand"></div>
        <button id="btn-stream-fs"></button>
        <button id="btn-stream-quit"></button>`;
    document.body.appendChild(el);
    return el;
}

function button(id) {
    const b = document.createElement('button');
    b.id = id;
    return b;
}

/** The ids of the bar's children, left to right. */
const order = (el) => [...el.children].map((c) => c.id || c.className);

/** What ShareMenu does: its button before Stop. */
function share(el) {
    el.insertBefore(button('btn-stream-share'), el.querySelector('#btn-stream-quit'));
}

describe('the touch buttons on the stream bar', () => {
    afterEach(() => {
        document.body.innerHTML = '';
    });

    it('puts the keyboard right of the gamepad, before Share and Stop', () => {
        const el = header();
        // The view makes the keyboard button first, the gamepad button once the
        // stream shows, Share last.
        placeKeyboardButton(el, button('btn-stream-keyboard'));
        placeGamepadButton(el, button('btn-stream-pad'));
        share(el);
        expect(order(el)).toEqual([
            'stream-brand',
            'btn-stream-fs',
            'btn-stream-pad',
            'btn-stream-keyboard',
            'btn-stream-share',
            'btn-stream-quit',
        ]);
    });

    it('keeps that order whichever button comes first', () => {
        const el = header();
        placeGamepadButton(el, button('btn-stream-pad'));
        placeKeyboardButton(el, button('btn-stream-keyboard'));
        share(el);
        expect(order(el).slice(2)).toEqual([
            'btn-stream-pad',
            'btn-stream-keyboard',
            'btn-stream-share',
            'btn-stream-quit',
        ]);
    });

    it('puts the keyboard right of the on-screen controller button too', () => {
        for (const keyboardFirst of [true, false]) {
            const el = header();
            if (keyboardFirst) placeKeyboardButton(el, button('btn-stream-keyboard'));
            placeGamepadButton(el, button('btn-stream-touchpad'));
            if (!keyboardFirst) placeKeyboardButton(el, button('btn-stream-keyboard'));
            share(el);
            expect(order(el).slice(2)).toEqual([
                'btn-stream-touchpad',
                'btn-stream-keyboard',
                'btn-stream-share',
                'btn-stream-quit',
            ]);
            el.remove();
        }
    });

    it('never puts the keyboard before the mark', () => {
        const el = header();
        placeKeyboardButton(el, button('btn-stream-keyboard'));
        expect(order(el)[0]).toBe('stream-brand');
        expect(order(el).slice(-2)).toEqual(['btn-stream-keyboard', 'btn-stream-quit']);
    });

    it('puts the gamepad before Stop on a bar without a keyboard', () => {
        const el = header();
        placeGamepadButton(el, button('btn-stream-pad'));
        expect(order(el).slice(-2)).toEqual(['btn-stream-pad', 'btn-stream-quit']);
    });
});
