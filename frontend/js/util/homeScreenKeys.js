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
 * The keys a home-screen shortcut opens with, for every machine at once.
 *
 * A shortcut starts in a storage of its own, and the one thing it is given is
 * the address in its manifest. That address carries `#k=` with the key of the
 * machine the page was on. One person has several machines, though, and a
 * shortcut that signs in on one and asks the PIN on each of the others is a
 * shortcut half made. So the page, while it is signed in and can reach them,
 * asks each of the other machines for a key of its own and writes all of them
 * into that same `#k=` value:
 *
 *     hs-AAAA,0123456789abcdefghjkmnpqrs:hs-BBBB,zyxw…:hs-CCCC
 *
 * The machine's own key first, then `<identifier>:<key>` for each of the
 * others. One value rather than one fragment part per machine, because the
 * bootstrap hands the fragment over by rebuilding it from the parts it knows
 * (boot.js) — `k=` travels as it is, and so does whatever is inside it, with a
 * bootstrap of any age.
 *
 * ',' and ':' are outside both alphabets (a key is `hs-` and base64url, an
 * identifier is Crockford lower-case), so the value splits without escaping.
 *
 * Every launch of the shortcut starts from that same address, so every launch
 * carries every key again, spent or not. A spent key sent back is a refusal at
 * the far end, and a refusal counts against the abuse guard like a wrong PIN.
 * The keys that have had their answer are remembered here, by their
 * fingerprint, and never sent twice.
 */

/** The identifier's shape (bootstrap/v1/tunnel.js ID_SHAPE). */
const ID_SHAPE = /^[0-9a-z]{26}$/;
/** A home-screen key: the prefix and base64url (AuthManager::homeScreenKey). */
const KEY_SHAPE = /^hs-[A-Za-z0-9_-]{16,}$/;

/**
 * @typedef {{ id: string, key: string }} SideKey  a key for one other machine
 */

/**
 * One `#k=` value from the machine's own key and the other machines'. A machine
 * with no key of its own (a page that was not remembered) leaves the first
 * slot empty; anything not shaped like a key is left out.
 *
 * @param {string} own
 * @param {SideKey[]} others
 * @returns {string}
 */
export function packHandoffKeys(own, others) {
    const parts = [KEY_SHAPE.test(own) ? own : ''];
    for (const o of others || []) {
        if (o && ID_SHAPE.test(o.id) && KEY_SHAPE.test(o.key)) parts.push(`${o.id}:${o.key}`);
    }
    return parts.length === 1 ? parts[0] : parts.join(',');
}

/**
 * The reverse. A value that is not a home-screen key at all — the host key the
 * machine's own tray puts in the same `#k=` — comes back as `own`, untouched.
 *
 * @param {string|null|undefined} value
 * @returns {{ own: string, others: SideKey[] }}
 */
export function unpackHandoffKeys(value) {
    const raw = typeof value === 'string' ? value : '';
    const [own = '', ...rest] = raw.split(',');
    const others = [];
    for (const part of rest) {
        const colon = part.indexOf(':');
        if (colon < 0) continue;
        const id = part.slice(0, colon);
        const key = part.slice(colon + 1);
        if (ID_SHAPE.test(id) && KEY_SHAPE.test(key)) others.push({ id, key });
    }
    return { own, others };
}

/** Whether a `#k=` value is a home-screen shortcut's, rather than a host key. */
export function isHomeScreenHandoff({ own, others }) {
    return own.startsWith('hs-') || (!own && others.length > 0);
}

// ── Keys that have had their answer ──────────────────────────────────────────
//
// What is remembered of a key is its fingerprint, never the key: enough to
// know it again at the next launch, and nothing anyone could send.

const SPENT_STORE = 'mw-hs-spent';
/** Far more than one shortcut ever carries; the oldest fall off. */
const SPENT_MAX = 64;
/** A fingerprint: the SHA-256 of the key's text, in hex. */
const PRINT_SHAPE = /^[0-9a-f]{64}$/;

/** The fingerprint of @p key, or null in a browser that cannot make one. */
async function fingerprint(key) {
    try {
        const digest = await globalThis.crypto.subtle.digest(
            'SHA-256',
            new TextEncoder().encode(key),
        );
        return [...new Uint8Array(digest)].map((b) => b.toString(16).padStart(2, '0')).join('');
    } catch {
        return null;
    }
}

function readSpent(storage) {
    try {
        const list = JSON.parse(storage.getItem(SPENT_STORE) || '[]');
        return Array.isArray(list) ? list.filter((k) => typeof k === 'string') : [];
    } catch {
        return [];
    }
}

function writeSpent(storage, list) {
    try {
        storage.setItem(SPENT_STORE, JSON.stringify(list.slice(-SPENT_MAX)));
    } catch {
        /* no storage: the key is refused once more next launch, nothing worse */
    }
}

/**
 * The fingerprints on record. A version before this one kept the keys
 * themselves: each is turned into its fingerprint here, and the record written
 * back at once, so that none stays in clear past the first launch.
 */
async function spentPrints(storage) {
    const list = readSpent(storage);
    if (list.every((entry) => PRINT_SHAPE.test(entry))) return list;
    const prints = [];
    for (const entry of list) {
        const print = PRINT_SHAPE.test(entry) ? entry : await fingerprint(entry);
        // Without a fingerprint the key is dropped rather than kept as it is.
        if (print) prints.push(print);
    }
    writeSpent(storage, prints);
    return prints;
}

/** True once @p key has been answered — accepted or refused — by its machine. */
export async function isHandoffKeySpent(key, storage = globalThis.localStorage) {
    if (!storage || !key) return false;
    const prints = await spentPrints(storage);
    const print = await fingerprint(key);
    return !!print && prints.includes(print);
}

/**
 * Record that @p key has had its answer, so no launch sends it again. In a
 * browser that cannot make a fingerprint nothing is recorded: the key is
 * refused once more at the next launch, nothing worse.
 */
export async function markHandoffKeySpent(key, storage = globalThis.localStorage) {
    if (!storage || !key) return;
    const prints = await spentPrints(storage);
    const print = await fingerprint(key);
    if (!print) return;
    writeSpent(storage, [...prints.filter((p) => p !== print), print]);
}
