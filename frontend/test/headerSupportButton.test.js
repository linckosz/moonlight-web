/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */

/**
 * The header's Buy me a coffee button is its cup alone on a phone, held
 * upright or sideways. Sideways, the phone is wider than the 600px query, so
 * the button showed its whole yellow wordmark beside Admin and Settings
 * (iPhone, Chrome, 12/11/2026). Admin and Settings keep their names there.
 */
import { describe, it, expect } from 'vitest';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const root = join(dirname(fileURLToPath(import.meta.url)), '..');
const layout = readFileSync(join(root, 'css', 'layout.css'), 'utf8')
    // Comments name the selectors too; only the rules count.
    .replace(/\/\*[\s\S]*?\*\//g, '');

/** Each top-level @media block: its condition and the rules inside it. */
function mediaBlocks(src) {
    const blocks = [];
    for (const m of src.matchAll(/@media([^{]+)\{/g)) {
        let depth = 1;
        let i = m.index + m[0].length;
        for (; i < src.length && depth > 0; i++) {
            if (src[i] === '{') depth++;
            else if (src[i] === '}') depth--;
        }
        blocks.push({ cond: m[1].trim(), body: src.slice(m.index + m[0].length, i - 1) });
    }
    return blocks;
}

/** The declarations of the rule for `selector` in `body`, or null. */
function rule(body, selector) {
    for (const m of body.matchAll(/([^{}]+)\{([^{}]*)\}/g)) {
        if (m[1].split(',').some((s) => s.trim() === selector)) return m[2];
    }
    return null;
}

const PHONE_SIDEWAYS = '(max-height: 520px) and (orientation: landscape)';

describe("the header's Buy me a coffee button on a phone", () => {
    const blocks = mediaBlocks(layout);
    const cup = blocks.filter((b) => rule(b.body, '.header-btn-bmc .header-btn-bmc-full') !== null);

    it('is its cup alone, held upright or sideways', () => {
        expect(cup).toHaveLength(1);
        const { cond, body } = cup[0];
        expect(cond).toContain('(max-width: 600px)');
        expect(cond).toContain(PHONE_SIDEWAYS);
        expect(rule(body, '.header-btn-bmc .header-btn-bmc-full')).toMatch(/display:\s*none/);
        expect(rule(body, '.header-btn-bmc .header-btn-bmc-logo')).toMatch(
            /display:\s*inline-flex/,
        );
        expect(rule(body, '.header-actions .header-btn-bmc')).toMatch(/width:\s*38px/);
    });

    it('leaves Admin and Settings their names when the phone is held sideways', () => {
        for (const b of blocks.filter((x) => x.cond.includes(PHONE_SIDEWAYS))) {
            expect(rule(b.body, '.header-btn-label'), b.cond).toBeNull();
        }
    });

    it('keeps an accessible name and a tooltip once its wordmark is gone', () => {
        const html = readFileSync(join(root, 'index.html'), 'utf8');
        const link = html.match(/<a[^>]*id="btn-bmc"[^>]*>/)?.[0] ?? '';
        expect(link).toMatch(/aria-label="[^"]+"/);
        expect(link).toMatch(/title="[^"]+"/);
        expect(link).toContain('aria-label:support.action');
    });
});
