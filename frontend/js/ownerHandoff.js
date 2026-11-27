/*
 * MoonlightWeb — browser-based Sunshine/GameStream client.
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
 * The page a link of this machine opens first (owner-handoff.html,
 * server/OwnerHandoff.h). The browser was started on it with a nonce in its
 * fragment: this page trades the nonce, once, on the loopback connection it
 * was served on, for the owner's link, and opens that link in its own place,
 * so that it is not left behind in the history. A refusal says to open the
 * page again; nothing is tried twice.
 */
import { init, t } from './i18n/i18n.js';

const TRADE_PATH = '/api/auth/owner-handoff';
const NONCE = /^#n=([A-Za-z0-9_-]{22})$/;
const PAGE_PATHS = new Set(['/owner-handoff', '/owner-handoff.html']);

/** The nonce of a fragment `#n=<nonce>`, or null. */
export function nonceOf(hash) {
    const match = NONCE.exec(hash || '');
    return match ? match[1] : null;
}

/**
 * The link the server answered, when this page may open it: an https: link,
 * or one on this page's own origin. Never a javascript: link, never another
 * host over plain HTTP.
 */
export function openableTarget(url, pageOrigin) {
    if (typeof url !== 'string' || !url) return null;
    let target;
    try {
        target = new URL(url);
    } catch {
        return null;
    }
    if (target.protocol === 'https:') return target.href;
    if (target.origin !== 'null' && target.origin === pageOrigin) return target.href;
    return null;
}

/** The sentence that says to open the page again, in the page's language when it loads. */
async function showReopen(doc) {
    const note = doc.getElementById('handoff-reopen');
    if (!note) return;
    try {
        await init();
        const text = t('handoff.reopen');
        if (text && text !== 'handoff.reopen') note.textContent = text;
    } catch {
        // The English already in the page stays.
    }
    note.hidden = false;
}

/**
 * Trades the nonce in `win`'s address for the owner's link and goes there.
 * True when it went; false when it says to open the page again.
 */
export async function handOff(win = window) {
    const nonce = nonceOf(win.location.hash);
    // Out of the address bar and the history at once: it is spent either way.
    win.history.replaceState(null, '', win.location.pathname);
    if (nonce) {
        try {
            const resp = await win.fetch(TRADE_PATH, {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ n: nonce }),
                cache: 'no-store',
                credentials: 'omit',
                referrerPolicy: 'no-referrer',
            });
            if (resp.ok) {
                const answer = await resp.json();
                const target = openableTarget(answer && answer.url, win.location.origin);
                if (target) {
                    win.location.replace(target);
                    return true;
                }
            }
        } catch {
            // Said below.
        }
    }
    await showReopen(win.document);
    return false;
}

if (typeof window !== 'undefined' && PAGE_PATHS.has(window.location.pathname)) handOff();
