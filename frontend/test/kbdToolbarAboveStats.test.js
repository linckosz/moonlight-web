/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */

/**
 * The special-keys bar (Win, Esc, Tab…) in front of the stats card. The card
 * sat in front of everything in the stream (10000, for the CSS-fallback
 * fullscreen picture at 9999) and hid Win, Esc and Tab on an iPhone held
 * sideways (13/11/2026). It goes behind the bar while the bar shows.
 */
import { describe, it, expect, beforeAll, afterEach } from 'vitest';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const STREAM_CSS = readFileSync(
    join(dirname(fileURLToPath(import.meta.url)), '..', 'css', 'stream.css'),
    'utf8',
);

/** The stream view as StreamView builds it: the card, then the bar. */
function view({ barShown, cssFullscreen = false }) {
    const root = document.createElement('div');
    root.className = 'stream-overlay' + (cssFullscreen ? ' stream-css-fs' : '');
    root.innerHTML = `
        <div class="stream-header"></div>
        <div class="stream-stats-overlay"></div>
        <div class="stream-kbd-toolbar${barShown ? ' visible' : ''}"></div>`;
    document.body.appendChild(root);
    const z = (sel) => Number(getComputedStyle(root.querySelector(sel)).zIndex);
    return { card: z('.stream-stats-overlay'), bar: z('.stream-kbd-toolbar') };
}

describe('the special-keys bar and the stats card', () => {
    beforeAll(() => {
        const style = document.createElement('style');
        style.textContent = STREAM_CSS;
        document.head.appendChild(style);
    });

    afterEach(() => {
        for (const el of document.querySelectorAll('.stream-overlay')) el.remove();
    });

    it('puts the card behind the bar while the bar shows', () => {
        const { card, bar } = view({ barShown: true });
        expect(card).toBeLessThan(bar);
    });

    it('leaves the card in front of everything else the rest of the time', () => {
        expect(view({ barShown: false }).card).toBe(10000);
    });

    it('keeps the card in front of the fullscreen picture (9999)', () => {
        expect(view({ barShown: true, cssFullscreen: true }).card).toBe(10000);
    });
});
