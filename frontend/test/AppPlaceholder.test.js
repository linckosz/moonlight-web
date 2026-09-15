/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect } from 'vitest';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

import { SPRITES, spriteIndexFor, appPlaceholderHtml } from '../js/ui/AppPlaceholder.js';

const LOCALES = join(dirname(fileURLToPath(import.meta.url)), '..', 'locales');
const locale = (lang) => JSON.parse(readFileSync(join(LOCALES, `${lang}.json`), 'utf8'));
const lookup = (dict, key) => key.split('.').reduce((o, p) => (o ? o[p] : undefined), dict);

/**
 * The cast of sprites that stand in for a missing cover. They are hand-drawn
 * grids, so what can silently break is the drawing itself (a row one character
 * short shifts every pixel after it) and the id → sprite pick.
 */
describe('AppPlaceholder', () => {
    it('draws every sprite on a rectangular grid that fits the shared canvas', () => {
        for (const s of SPRITES) {
            const widths = new Set(s.rows.map((r) => r.length));
            expect(widths.size, s.name).toBe(1);
            expect(s.rows[0].length, s.name).toBeLessThanOrEqual(15);
            expect(s.rows.length, s.name).toBeLessThanOrEqual(13);
            expect(s.rows.join(''), s.name).toMatch(/^[.cymw]+$/);
        }
    });

    it('has a line for every sprite in every language', () => {
        for (const lang of ['en', 'fr', 'zh']) {
            const dict = locale(lang);
            for (const s of SPRITES)
                expect(lookup(dict, s.label), `${lang} ${s.label}`).toBeTruthy();
        }
    });

    it('keeps the arcade ship in the cast', () => {
        expect(SPRITES.map((s) => s.name)).toContain('ship');
    });

    it('picks the same sprite for the same app, every time', () => {
        expect(spriteIndexFor(881448767)).toBe(spriteIndexFor('881448767'));
        expect(appPlaceholderHtml(42, (k) => k)).toBe(appPlaceholderHtml(42, (k) => k));
    });

    it('gives consecutive ids — a host’s displays — different sprites', () => {
        for (let id = 0; id < 12; id++) expect(spriteIndexFor(id)).not.toBe(spriteIndexFor(id + 1));
    });

    it('stays in range for ids that are negative, huge or not numbers at all', () => {
        for (const id of [-1, -7, 2 ** 31 - 1, NaN, undefined, 'abc', 3.7]) {
            const i = spriteIndexFor(id);
            expect(i).toBeGreaterThanOrEqual(0);
            expect(i).toBeLessThan(SPRITES.length);
        }
    });

    it('escapes the line it is given', () => {
        expect(appPlaceholderHtml(0, () => '<b>')).toContain('&lt;b&gt;');
    });
});
