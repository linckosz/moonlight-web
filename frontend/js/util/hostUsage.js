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
 * MoonlightWeb — which machines this browser actually streams from.
 *
 * The host list is discovery order, which is the network's opinion, not the
 * user's: the machine played on every evening can sit under three boxes seen
 * once. This keeps a tiny per-browser ledger so the list can lead with the
 * machine most likely to be wanted.
 *
 * Not a plain counter. A counter never forgets, so a machine retired months ago
 * would hold the top spot forever. Each use adds 1 to a score that halves every
 * HALF_LIFE_MS, which ranks "used a lot, lately" above both "used a lot, once,
 * long ago" and "used yesterday, once" — and needs one number per host rather
 * than a history.
 *
 * Per-browser by design, like the app-list memory next door: this is a habit,
 * and habits belong to the person sitting in front of the browser, not to the
 * machine everybody shares.
 */

const USAGE_KEY = 'mw-host-usage';

// A fortnight: a machine dropped today still leads for about two weeks, then
// yields to whatever replaced it. Long enough to survive a holiday, short
// enough that changing machines is felt within days.
const HALF_LIFE_MS = 14 * 24 * 60 * 60 * 1000;

// Below this a score is noise — one launch a very long time ago — and the entry
// is dropped rather than carried forever.
const MIN_SCORE = 0.02;

// Hosts kept at once. Same reasoning as the app-list memory: a browser that has
// scanned a few LANs must not grow this without bound.
const MAX_HOSTS = 24;

function readStore() {
    try {
        const raw = localStorage.getItem(USAGE_KEY);
        const map = raw ? JSON.parse(raw) : null;
        return map && typeof map === 'object' ? map : {};
    } catch (e) {
        console.warn('[MW] Could not read the host-usage memory:', e);
        return {};
    }
}

function writeStore(map) {
    try {
        localStorage.setItem(USAGE_KEY, JSON.stringify(map));
    } catch (e) {
        console.warn('[MW] Could not persist the host-usage memory:', e);
    }
}

/** An entry's score brought forward to `now`, or 0 if there is nothing usable. */
function decayed(entry, now) {
    if (!entry || !(entry.score > 0) || !(entry.at > 0)) return 0;
    const age = now - entry.at;
    if (age <= 0) return entry.score; // clock stepped backwards; don't inflate it
    return entry.score * Math.pow(0.5, age / HALF_LIFE_MS);
}

/**
 * Record that this browser just streamed from a host.
 *
 * Called on launch rather than on pairing or on a poll answering: pairing
 * happens once and says nothing about habit, and a host answering a poll is the
 * network's doing, not the user's.
 */
export function noteHostUse(uuid) {
    if (!uuid) return;
    const now = Date.now();
    const map = readStore();
    map[uuid] = { score: decayed(map[uuid], now) + 1, at: now };

    // Prune what has faded to nothing, then cap by score so the survivors are
    // the ones the ranking would have used anyway.
    for (const key of Object.keys(map)) {
        if (key !== uuid && decayed(map[key], now) < MIN_SCORE) delete map[key];
    }
    const uuids = Object.keys(map);
    if (uuids.length > MAX_HOSTS) {
        uuids
            .sort((a, b) => decayed(map[b], now) - decayed(map[a], now))
            .slice(MAX_HOSTS)
            .forEach((stale) => delete map[stale]);
    }
    writeStore(map);
}

/** Drop what we remember about a host (it was removed). */
export function forgetHostUse(uuid) {
    if (!uuid) return;
    const map = readStore();
    if (!(uuid in map)) return;
    delete map[uuid];
    writeStore(map);
}

/**
 * Usage scores for the hosts on screen, keyed by uuid — read once per render so
 * a list of a dozen hosts costs one localStorage read, and so every card in one
 * paint is ranked against the same instant.
 * @returns {(uuid: string) => number}
 */
export function hostUsageRanker() {
    const now = Date.now();
    const map = readStore();
    return (uuid) => decayed(map[uuid], now);
}
