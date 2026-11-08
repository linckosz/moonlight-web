/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */

/**
 * During a stream, toasts sit on the bottom of the screen, or right above the
 * touch keyboard's special-keys bar — not where the Discord button would be.
 *
 * The button is hidden while streaming but stays in the page, so a rule keyed
 * on `body:has(.discord-fab)` still matched there: every toast went ~100px up
 * over the picture, the issue #44 input-method warning well above the bar
 * (iPhone, 08/11/2026).
 */
import { describe, it, expect } from 'vitest';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const css = (name) =>
    readFileSync(join(dirname(fileURLToPath(import.meta.url)), '..', 'css', name), 'utf8')
        // Comments name the selectors too; only the rules count.
        .replace(/\/\*[\s\S]*?\*\//g, '');

/** The selectors of every rule whose selector list names `part`. */
function selectorsNaming(src, part) {
    return [...src.matchAll(/([^{}]+)\{/g)]
        .flatMap((m) => m[1].split(','))
        .map((s) => s.trim())
        .filter((s) => s.includes(part));
}

describe('toasts during a stream', () => {
    it('the Discord button is only hidden while streaming, so it is still in the page', () => {
        // The premise: if the button left the page instead, the rule below
        // would no longer need its exception.
        expect(selectorsNaming(css('stream.css'), '.discord-fab')).toContain(
            'body.streaming-active .discord-fab',
        );
    });

    it('no toast clearance for the Discord button applies while streaming (issue #44)', () => {
        const lifts = selectorsNaming(css('components.css'), '#toast-container').filter((s) =>
            s.includes('discord-fab'),
        );
        expect(lifts.length).toBeGreaterThan(0);
        for (const s of lifts) expect(s, s).toContain(':not(.streaming-active)');
    });
});
