/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect, afterEach, vi } from 'vitest';

import { StreamView } from '../js/ui/StreamView.js';
import {
    gamingHelperLevel,
    FIT_FULL,
    FIT_KEYS,
    FIT_NO_FS_COMBO,
    FIT_NONE,
} from '../js/ui/gamingHelperFit.js';

/**
 * HDRHELP: on an iPad the gaming-mode exit reminder, docked in the stream bar
 * right of the Fullscreen button, lay on the on-screen controller and keyboard
 * buttons, which could no longer be tapped (27/11/2026). The helpers now give
 * way, one by one, before anything of them reaches a button.
 */

const GAP = 12;

/** Where the group ends for a level, as the bar lays it out. */
function groupRight(m, level) {
    const fsW = level >= FIT_NO_FS_COMBO ? m.fsW - m.fsComboW : m.fsW;
    const remW = level >= FIT_NONE ? 0 : level >= FIT_KEYS ? m.remW - m.remTextW : m.remW;
    const right = remW > 0 ? remW + m.gap : 0;
    const shift = (right - m.soundW) / 2;
    return m.centre - shift + fsW / 2 + right;
}

/**
 * A bar `width` wide, whose buttons in the flow on the right take `buttonsW`
 * from its right edge, padding included, and the helpers' widths.
 */
function bar(width, helpers, buttonsW = BUTTONS_W) {
    return {
        centre: width / 2,
        limit: width - buttonsW,
        gap: GAP,
        ...helpers,
    };
}

// Measured in Chrome with the real stylesheets (27/11/2026): the on-screen
// controller, the keyboard, Share and Stop, and the helpers of an iPad (in
// symbols) and of Windows (whose keys keep their names).
const BUTTONS_W = 291;
const IPAD = { fsW: 214, fsComboW: 107, soundW: 90, remW: 145, remTextW: 33 };
const WINDOWS = { fsW: 289, fsComboW: 183, soundW: 90, remW: 213, remTextW: 33 };

describe('the gaming-mode helpers give way before reaching a button', () => {
    for (const [name, helpers] of [
        ['an iPad', IPAD],
        ['Windows', WINDOWS],
    ]) {
        it(`never lie on a button, at any width, on ${name}`, () => {
            for (let width = 280; width <= 2560; width += 2) {
                const m = bar(width, helpers);
                const level = gamingHelperLevel(m);
                // Either the group ends a gap short of the buttons, or every
                // helper that could go has gone.
                if (level < FIT_NONE)
                    expect(groupRight(m, level)).toBeLessThanOrEqual(m.limit - GAP);
            }
        });

        it(`show more, never less, as the bar widens, on ${name}`, () => {
            let last = FIT_NONE;
            for (let width = 280; width <= 2560; width += 2) {
                const level = gamingHelperLevel(bar(width, helpers));
                expect(level).toBeLessThanOrEqual(last);
                last = level;
            }
            expect(last).toBe(FIT_FULL);
        });
    }

    it('drop the words first, then the combo on Fullscreen, then the reminder', () => {
        // Where each step is just enough, on an iPad.
        const at = (level) => {
            for (let width = 2560; width >= 280; width--) {
                if (gamingHelperLevel(bar(width, IPAD)) > level) return width + 1;
            }
            return 280;
        };
        expect(at(FIT_FULL)).toBeGreaterThan(at(FIT_KEYS));
        expect(at(FIT_KEYS)).toBeGreaterThan(at(FIT_NO_FS_COMBO));
    });

    it('keep everything on an iPad held sideways, where the words did not fit', () => {
        // Safari on an iPad in landscape: about 1180px. The keys' names and
        // the whole sentence, as before, lay on the buttons there.
        const BEFORE = { fsW: 295, fsComboW: 189, soundW: 90, remW: 329, remTextW: 143 };
        expect(gamingHelperLevel(bar(1180, BEFORE))).toBeGreaterThan(FIT_FULL);
        expect(gamingHelperLevel(bar(1180, IPAD))).toBe(FIT_FULL);
    });
});

/** A rect of `width` at `left`. */
function rect(left, width, height = 30) {
    return () => ({
        left,
        right: left + width,
        top: 10,
        bottom: 10 + height,
        width,
        height,
        x: left,
        y: 10,
    });
}

describe('the stream bar applies the fit', () => {
    afterEach(() => {
        vi.restoreAllMocks();
        document.body.innerHTML = '';
    });

    /** A bar `width` wide, Fullscreen centred, Stop on the right. */
    function setup(width) {
        const header = document.createElement('div');
        header.className = 'stream-header';
        document.body.appendChild(header);
        header.getBoundingClientRect = rect(0, width, 54);
        const fs = document.createElement('button');
        fs.innerHTML = '<span class="fs-label">Fullscreen</span><span class="fs-combo"></span>';
        fs.getBoundingClientRect = rect(width / 2 - IPAD.fsW / 2, IPAD.fsW);
        /** @type {HTMLElement} */ (fs.querySelector('.fs-combo')).getBoundingClientRect = rect(
            0,
            IPAD.fsComboW - 10,
        );
        header.appendChild(fs);
        const stop = document.createElement('button');
        stop.getBoundingClientRect = rect(width - BUTTONS_W, BUTTONS_W - 24);
        header.appendChild(stop);
        const el = document.createElement('div');
        el.className = 'stream-gaming-overlay visible';
        el.innerHTML = '<span class="gm-keys"></span><span class="gm-text">Exit</span>';
        el.getBoundingClientRect = rect(0, IPAD.remW);
        /** @type {HTMLElement} */ (el.querySelector('.gm-text')).getBoundingClientRect = rect(
            0,
            IPAD.remTextW - 8,
        );
        const v = Object.create(StreamView.prototype);
        v._gamingMode = true;
        return { v, fs, el };
    }

    it('keeps everything where the bar is wide', () => {
        const { v, fs, el } = setup(2400);
        expect(
            v._fitGamingHelpers(fs, fs.getBoundingClientRect(), el, true, IPAD.soundW, GAP, []),
        ).toBe(FIT_FULL);
        expect(el.classList.contains('gm-short')).toBe(false);
        expect(fs.classList.contains('fs-no-combo')).toBe(false);
    });

    it('takes the words, then the combo, and says them in the tooltip', () => {
        vi.spyOn(navigator, 'platform', 'get').mockReturnValue('MacIntel');
        const { v, fs, el } = setup(700);
        const level = v._fitGamingHelpers(
            fs,
            fs.getBoundingClientRect(),
            el,
            true,
            IPAD.soundW,
            GAP,
            [],
        );
        expect(level).toBe(FIT_NONE);
        expect(el.classList.contains('gm-short')).toBe(true);
        expect(fs.classList.contains('fs-no-combo')).toBe(true);
        expect(fs.title).toContain('⌃⌥⌘X');
        expect(fs.title).toContain('⌃⌥⌘Z');
    });

    it('writes the combo in Apple symbols on a Mac or an iPad, and in key names elsewhere', () => {
        vi.spyOn(navigator, 'platform', 'get').mockReturnValue('MacIntel');
        expect(StreamView._gamingComboText('Z')).toBe('⌃⌥⌘Z');
        expect(StreamView._gamingComboHtml('Z', 'gm-plus')).toBe(
            '<kbd>⌃</kbd><kbd>⌥</kbd><kbd>⌘</kbd><kbd>Z</kbd>',
        );
        vi.spyOn(navigator, 'platform', 'get').mockReturnValue('Win32');
        expect(StreamView._gamingComboText('Z')).toBe('Shift+Ctrl+Alt+Z');
    });
});
