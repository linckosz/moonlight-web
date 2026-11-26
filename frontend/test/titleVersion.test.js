/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect, beforeEach } from 'vitest';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
import { titleVersionHtml } from '../js/ui/titleVersion.js';

// The version badge beside the titles of Streaming Settings and Server
// Settings: the header's own text, only once the server has said it, and shown
// on a phone only — where the header hides its badge.
describe('titleVersionHtml', () => {
    beforeEach(() => {
        document.body.innerHTML =
            '<header class="app-header"><span class="version" hidden></span></header>';
    });

    it('is empty while the version is unknown', () => {
        expect(titleVersionHtml()).toBe('');
        document.body.innerHTML = '';
        expect(titleVersionHtml()).toBe('');
    });

    it("carries the header's version once it is known", () => {
        const header = document.querySelector('.app-header .version');
        header.textContent = 'v0.4.4';
        header.hidden = false;
        const holder = document.createElement('h2');
        holder.innerHTML = 'Streaming Settings' + titleVersionHtml();
        const badge = holder.querySelector('.version.title-version');
        expect(badge).not.toBeNull();
        expect(badge.textContent).toBe('v0.4.4');
    });

    it('never writes markup it was given', () => {
        const header = document.querySelector('.app-header .version');
        header.textContent = 'v<img src=x onerror=1>';
        header.hidden = false;
        expect(titleVersionHtml()).not.toContain('<img');
    });

    it('is shown on a phone only', () => {
        const css = readFileSync(
            join(dirname(fileURLToPath(import.meta.url)), '..', 'css', 'views', 'settings.css'),
            'utf8',
        );
        expect(css).toMatch(/\.title-version\s*\{[^}]*display:\s*none/);
        expect(css).toMatch(
            /@media \(max-width: 600px\)\s*\{\s*\.version\.title-version\s*\{\s*display:\s*inline-block/,
        );
    });
});
