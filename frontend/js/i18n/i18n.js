/*
 * Moonlight-Web — browser-based Sunshine/GameStream client.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * Moonlight-Web — lightweight i18n runtime (no build step / no bundler).
 *
 * - Loads locale JSON from /locales/<lang>.json (English always loaded as fallback).
 * - t(key, vars) — nested-key lookup with {{var}} interpolation; falls back to
 *   English, then to the raw key so a missing translation never breaks the UI.
 * - applyDOM(root) — translates static markup: [data-i18n] text content and
 *   [data-i18n-attr="attr:key, attr2:key2"] attributes (title, placeholder…).
 * - setLanguage(lang) — persists to localStorage and reloads the app.
 *
 * The JSON format is Tolgee-compatible (nested namespaces) so the catalog can be
 * edited in a self-hosted Tolgee instance and exported back to /locales/*.json.
 */

const STORAGE_KEY = 'mw-lang';
const FALLBACK_LANG = 'en';

// Languages exposed in the UI selector. Add an entry + a locales/<code>.json
// file to ship a new language — nothing else to wire.
export const AVAILABLE_LANGUAGES = [
    { code: 'en', label: 'English' },
    { code: 'fr', label: 'Français' },
];

const state = {
    lang: FALLBACK_LANG,
    dict: {},        // active-language catalog
    fallback: {},    // English catalog (always loaded)
};

/** Pick the initial language: stored choice → browser language → English. */
function detectLanguage() {
    const stored = localStorage.getItem(STORAGE_KEY);
    if (stored && AVAILABLE_LANGUAGES.some(l => l.code === stored)) return stored;
    const nav = (navigator.language || '').slice(0, 2).toLowerCase();
    if (AVAILABLE_LANGUAGES.some(l => l.code === nav)) return nav;
    return FALLBACK_LANG;
}

async function fetchLocale(code) {
    try {
        const res = await fetch(`locales/${code}.json`, { cache: 'no-cache' });
        if (!res.ok) throw new Error(`HTTP ${res.status}`);
        return await res.json();
    } catch (err) {
        console.warn(`[i18n] Failed to load locale "${code}":`, err);
        return {};
    }
}

/** Resolve a dotted key ("apps.backToHosts") against a nested catalog. */
function resolve(catalog, key) {
    let node = catalog;
    for (const part of key.split('.')) {
        if (node && typeof node === 'object' && part in node) {
            node = node[part];
        } else {
            return undefined;
        }
    }
    return typeof node === 'string' ? node : undefined;
}

function interpolate(str, vars) {
    if (!vars) return str;
    return str.replace(/\{\{\s*(\w+)\s*\}\}/g, (m, name) =>
        name in vars ? String(vars[name]) : m);
}

/**
 * Translate a key. Missing keys fall back to English then to the key itself,
 * so the UI degrades gracefully instead of breaking.
 */
export function t(key, vars) {
    let str = resolve(state.dict, key);
    if (str === undefined) str = resolve(state.fallback, key);
    if (str === undefined) return key;
    return interpolate(str, vars);
}

export function getLanguage() { return state.lang; }

/** Persist a new language and reload so every rendered view picks it up. */
export function setLanguage(code) {
    if (code === state.lang) return;
    localStorage.setItem(STORAGE_KEY, code);
    window.location.reload();
}

/** Translate static markup inside `root` (defaults to document). */
export function applyDOM(root = document) {
    root.querySelectorAll('[data-i18n]').forEach(el => {
        el.textContent = t(el.getAttribute('data-i18n'));
    });
    // [data-i18n-attr="title:foo.bar, placeholder:foo.baz"]
    root.querySelectorAll('[data-i18n-attr]').forEach(el => {
        el.getAttribute('data-i18n-attr').split(',').forEach(pair => {
            const [attr, key] = pair.split(':').map(s => s.trim());
            if (attr && key) el.setAttribute(attr, t(key));
        });
    });
}

/** Load catalogs for the detected language. Call once before app init. */
export async function init() {
    state.lang = detectLanguage();
    document.documentElement.lang = state.lang;
    state.fallback = await fetchLocale(FALLBACK_LANG);
    state.dict = state.lang === FALLBACK_LANG ? state.fallback : await fetchLocale(state.lang);
    return state.lang;
}
