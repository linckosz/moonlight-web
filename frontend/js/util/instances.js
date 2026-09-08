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
 * The machines this browser has been to.
 *
 * One person runs MoonlightWeb on several PCs, and until now the browser had no
 * way to say which one it was looking at, let alone reach the next one without a
 * bookmark. This is the little register behind the name in the header: every
 * install that has answered here, so the header can offer them as a menu.
 *
 * ── Why one list works at all ─────────────────────────────────────────────────
 *
 * Because every machine is now reached through the SAME origin. Since the
 * rendezvous became the only way in — the LAN included, see
 * docs/design/rendezvous-*.md — a browser talking to four PCs is a browser on
 * one origin with four host identifiers, and localStorage is shared across all
 * of them. That is what makes a cross-machine list possible without a server
 * holding it, and it is worth stating because the obvious reading ("of course a
 * page can list other pages") is wrong on any other layout: reached directly at
 * https://192.168.1.40:48443, an install shares storage with nothing, sees only
 * itself, and the header correctly renders a plain label with no menu.
 *
 * ── What is NOT here ──────────────────────────────────────────────────────────
 *
 * No secret of any kind. An entry is a name, an identifier already sitting in
 * the address bar, and the last time it answered. The pairing keys live in
 * IndexedDB under the bootstrap's own module and never come near this file; an
 * entry here grants nothing, and a machine whose pairing was cleared simply
 * pairs again on the next visit.
 *
 * An entry is written only after a machine has actually answered, so a mistyped
 * link never becomes a permanent row in someone's menu.
 */

const STORE_KEY = 'mw-instances';

/**
 * How many machines the menu will remember.
 *
 * A cap rather than an unbounded list, because the failure mode of no cap is not
 * a long menu — it is a menu of dead entries: every identifier ever pasted into
 * this browser, forever. Sixteen is far past what anyone runs and small enough
 * that the oldest falling off is always the right answer.
 */
const MAX_ENTRIES = 16;

/** Entries untouched for this long stop being offered. */
const STALE_MS = 180 * 24 * 60 * 60 * 1000; // 180 days

/**
 * @typedef {object} Instance
 * @property {string} id    rendezvous identifier, or the origin for a direct visit
 * @property {string} name  what that install calls itself
 * @property {string} url   where to send the browser to reach it
 * @property {number} seen  epoch ms of the last successful visit
 */

function read() {
    try {
        const raw = JSON.parse(localStorage.getItem(STORE_KEY) || '[]');
        if (!Array.isArray(raw)) return [];
        const now = Date.now();
        return raw.filter(
            (e) =>
                e &&
                typeof e.id === 'string' &&
                e.id &&
                typeof e.name === 'string' &&
                typeof e.url === 'string' &&
                e.url &&
                typeof e.seen === 'number' &&
                now - e.seen < STALE_MS,
        );
    } catch {
        // Private mode, a quota refusal, or a value someone else wrote. The
        // header falls back to a plain name, which is exactly right.
        return [];
    }
}

function write(entries) {
    try {
        localStorage.setItem(STORE_KEY, JSON.stringify(entries));
    } catch {
        /* the menu is a convenience; nothing depends on it being kept */
    }
}

/**
 * This page's own identity, as the register keys it.
 *
 * The rendezvous identifier when there is one, and the origin otherwise. Two
 * different answers to the same question — "which install is this?" — and the
 * second is not a fallback so much as the truthful answer on a direct visit,
 * where the address IS the machine.
 *
 * @param {string|null} hostId `tunnelHostId()`, or null on a direct connection
 * @returns {{id: string, url: string}}
 */
export function currentInstanceRef(hostId) {
    if (hostId) {
        // The bootstrap's own entry point. Not the current address: the
        // application rewrites that to a bare "/" as soon as it reaches the host
        // list, and a link back to the identifier has to survive that.
        return { id: hostId, url: `${location.origin}/${hostId}` };
    }
    return { id: location.origin, url: `${location.origin}/` };
}

/**
 * Record that this machine answered, under the name it gave.
 *
 * Called on every start rather than only on the first, so a machine renamed in
 * its admin page is renamed in every browser's menu the next time that browser
 * visits it — which is the only moment the new name can be learned, since
 * nothing here can reach a machine it is not talking to.
 *
 * @param {{id: string, url: string, name: string}} instance
 * @returns {Instance[]} the register as it now stands
 */
export function rememberInstance({ id, url, name }) {
    if (!id || !url || !name) return read();

    const entries = read().filter((e) => e.id !== id);
    entries.push({ id, url, name, seen: Date.now() });

    // Oldest first out. `seen` is the sort key rather than insertion order
    // because insertion order is a property of this array, and this array is
    // rewritten by every tab that starts.
    entries.sort((a, b) => b.seen - a.seen);
    const kept = entries.slice(0, MAX_ENTRIES);
    write(kept);
    return kept;
}

/**
 * Every machine this browser knows, the current one included.
 *
 * Ordered by name rather than by recency, and deliberately: a menu whose rows
 * move between two openings is a menu that has to be read every time. The names
 * are stable, so the positions are.
 *
 * @returns {Instance[]}
 */
export function listInstances() {
    return read().sort((a, b) => a.name.localeCompare(b.name, undefined, { sensitivity: 'base' }));
}

/** Drop one machine from the register. */
export function forgetInstance(id) {
    write(read().filter((e) => e.id !== id));
}
