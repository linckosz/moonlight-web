/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect } from 'vitest';
import { escapeHtml } from '../js/util/escapeHtml.js';

describe('escapeHtml', () => {
    it('escapes the five HTML-sensitive characters', () => {
        expect(escapeHtml('&')).toBe('&amp;');
        expect(escapeHtml('<')).toBe('&lt;');
        expect(escapeHtml('>')).toBe('&gt;');
        expect(escapeHtml('"')).toBe('&quot;');
        expect(escapeHtml("'")).toBe('&#39;');
    });

    it('neutralizes an element-context XSS payload', () => {
        expect(escapeHtml('<img src=x onerror=alert(1)>')).toBe(
            '&lt;img src=x onerror=alert(1)&gt;',
        );
    });

    it('neutralizes an attribute-breakout payload (quotes escaped)', () => {
        // The old div.textContent trick left `"` intact, allowing breakout from
        // title="${esc(x)}" / src="${esc(x)}". Both quote kinds must be escaped.
        expect(escapeHtml('" onerror="alert(1)')).toBe('&quot; onerror=&quot;alert(1)');
        expect(escapeHtml("' onmouseover='alert(1)")).toBe('&#39; onmouseover=&#39;alert(1)');
    });

    it('escapes ampersands once (no double-encoding of the input)', () => {
        expect(escapeHtml('a & b')).toBe('a &amp; b');
        expect(escapeHtml('&amp;')).toBe('&amp;amp;');
    });

    it('passes plain text through unchanged', () => {
        expect(escapeHtml('Living Room PC')).toBe('Living Room PC');
    });

    it('accepts numbers and stringifies them', () => {
        expect(escapeHtml(42)).toBe('42');
        expect(escapeHtml(0)).toBe('0');
    });

    it('returns "" for non-string/number inputs', () => {
        expect(escapeHtml(null)).toBe('');
        expect(escapeHtml(undefined)).toBe('');
        expect(escapeHtml({})).toBe('');
        expect(escapeHtml(['x'])).toBe('');
        expect(escapeHtml(true)).toBe('');
    });
});
