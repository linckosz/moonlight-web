/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';
import {
    t,
    init,
    getLanguage,
    setLanguage,
    applyDOM,
    AVAILABLE_LANGUAGES,
} from '../js/i18n/i18n.js';

const EN = { hello: 'Hello {{name}}', only: { en: 'English only' }, attr: 'Attr text' };
const FR = { hello: 'Bonjour {{name}}' };

// Resolve locale fetches by URL so init() can load both catalogs.
function stubLocaleFetch(map = { en: EN, fr: FR }) {
    vi.stubGlobal(
        'fetch',
        vi.fn(async (url) => {
            const code = url.includes('/fr.json') ? 'fr' : 'en';
            const body = map[code];
            if (!body) return { ok: false, status: 404, json: async () => ({}) };
            return { ok: true, status: 200, json: async () => body };
        }),
    );
}

describe('i18n', () => {
    beforeEach(() => {
        localStorage.clear();
        vi.restoreAllMocks();
    });
    afterEach(() => {
        document.body.innerHTML = '';
    });

    it('exposes the available languages', () => {
        expect(AVAILABLE_LANGUAGES.map((l) => l.code)).toContain('en');
        expect(AVAILABLE_LANGUAGES.map((l) => l.code)).toContain('fr');
    });

    it('returns the raw key when nothing is loaded', () => {
        expect(t('missing.key')).toBe('missing.key');
    });

    it('loads the stored language and interpolates variables', async () => {
        localStorage.setItem('mw-lang', 'fr');
        stubLocaleFetch();
        const lang = await init();
        expect(lang).toBe('fr');
        expect(getLanguage()).toBe('fr');
        expect(t('hello', { name: 'Bruno' })).toBe('Bonjour Bruno');
    });

    it('falls back to English then to the key for missing entries', async () => {
        localStorage.setItem('mw-lang', 'fr');
        stubLocaleFetch();
        await init();
        expect(t('only.en')).toBe('English only'); // only in the EN fallback
        expect(t('does.not.exist')).toBe('does.not.exist');
        // leaves an unknown {{var}} untouched
        expect(t('hello', {})).toBe('Bonjour {{name}}');
    });

    it('translates static markup via applyDOM', async () => {
        localStorage.setItem('mw-lang', 'en');
        stubLocaleFetch();
        await init();
        document.body.innerHTML =
            '<span data-i18n="attr"></span><input data-i18n-attr="placeholder:hello">';
        applyDOM(document);
        expect(document.querySelector('span').textContent).toBe('Attr text');
        expect(document.querySelector('input').getAttribute('placeholder')).toMatch(/Hello/);
    });

    it('setLanguage is a no-op when unchanged', async () => {
        localStorage.setItem('mw-lang', 'en');
        stubLocaleFetch();
        await init();
        const setItem = vi.spyOn(Storage.prototype, 'setItem');
        setLanguage('en'); // same language → returns before persisting/reloading
        expect(setItem).not.toHaveBeenCalled();
    });

    it('setLanguage persists a new choice', async () => {
        localStorage.setItem('mw-lang', 'en');
        stubLocaleFetch();
        await init();
        try {
            setLanguage('fr'); // persists then reloads (reload is a jsdom no-op/throw)
        } catch {
            /* jsdom navigation is not implemented — ignore */
        }
        expect(localStorage.getItem('mw-lang')).toBe('fr');
    });

    it('degrades gracefully when a locale fails to load', async () => {
        localStorage.setItem('mw-lang', 'fr');
        stubLocaleFetch({ en: EN }); // fr missing → 404 → {}
        const lang = await init();
        expect(lang).toBe('fr');
        // fr catalog empty → falls back to EN
        expect(t('only.en')).toBe('English only');
    });
});
