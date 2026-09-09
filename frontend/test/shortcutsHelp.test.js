/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect } from 'vitest';
import {
    comboModifiers,
    keyboardShortcutRows,
    gestureRows,
    shortcutsGridHtml,
    shortcutsTitle,
} from '../js/util/shortcutsHelp.js';

/**
 * The controls cheat-sheet has two homes — the slide that flashes over the
 * stream for five seconds, and the last section of the settings page, which is
 * where you look once it is gone. They must never drift apart, which is why
 * both read these rows rather than each building their own.
 *
 * No dictionary is loaded here, so `t()` returns the key itself: the keys are
 * what these tests pin, and the i18n check (`npm run i18n`) is what proves each
 * of them has a translation.
 */

describe('the modifiers, by client platform', () => {
    it('takes Shift on Windows and Linux, Ctrl on a Mac', () => {
        expect(comboModifiers(false)).toEqual(['Shift', 'Ctrl', 'Alt']);
        expect(comboModifiers(true)).toEqual(['Ctrl', 'Option', 'Cmd']);
    });
});

describe('the keyboard rows', () => {
    it('puts the desktop arrows on the same three modifiers as the other combos', () => {
        const rows = keyboardShortcutRows(true);
        const quit = rows.find((r) => r[0] === 'stream.scQuit');
        const prev = rows.find((r) => r[0] === 'stream.scDesktopPrev');
        const next = rows.find((r) => r[0] === 'stream.scDesktopNext');

        expect(quit).toEqual(['stream.scQuit', 'Ctrl', 'Option', 'Cmd', 'Q']);
        expect(prev).toEqual(['stream.scDesktopPrev', 'Ctrl', 'Option', 'Cmd', '←']);
        expect(next).toEqual(['stream.scDesktopNext', 'Ctrl', 'Option', 'Cmd', '→']);
    });

    it('says Ctrl+Alt+Shift on a non-Mac client', () => {
        const rows = keyboardShortcutRows(false);
        expect(rows.find((r) => r[0] === 'stream.scDesktopNext')).toEqual([
            'stream.scDesktopNext',
            'Shift',
            'Ctrl',
            'Alt',
            '→',
        ]);
    });
});

describe('the gesture rows', () => {
    it('carries the 3-finger desktop swipe in both touch models', () => {
        for (const touchScreen of [false, true]) {
            const rows = gestureRows(touchScreen);
            expect(rows.map((r) => r[0])).toContain('stream.tcDesktop');
            // The 3-finger tap keeps its own row: the swipe did not replace it.
            expect(rows.map((r) => r[0])).toContain('stream.tcKeyboard');
        }
    });

    it('changes what one finger does in touch-screen mode', () => {
        const trackpad = gestureRows(false).find((r) => r[0] === 'stream.tcMoveCursor');
        const screen = gestureRows(true).find((r) => r[0] === 'stream.tcMoveCursor');
        expect(trackpad[1]).not.toBe(screen[1]);
    });
});

describe('the rendered grid', () => {
    it('shows keys on a device without touch', () => {
        const html = shortcutsGridHtml({ touch: false, isMac: false });
        expect(html).toContain('<kbd>←</kbd>');
        expect(html).toContain('<kbd>Shift</kbd>');
        expect(html).not.toContain('kbd class="gesture"');
    });

    it('shows gesture chips on a device with touch, and no key combos', () => {
        const html = shortcutsGridHtml({ touch: true, touchScreen: false });
        expect(html).toContain('kbd class="gesture"');
        expect(html).not.toContain('<kbd>Shift</kbd>');
        expect(html).toContain('stream.tcDesktopVal');
    });

    it('titles itself for the device it is describing', () => {
        expect(shortcutsTitle({ touch: false })).toBe('stream.shortcutsTitle');
        expect(shortcutsTitle({ touch: true, touchScreen: false })).toBe('stream.touchTitle');
        expect(shortcutsTitle({ touch: true, touchScreen: true })).toBe('stream.touchScreenTitle');
    });
});
