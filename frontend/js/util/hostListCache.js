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
 * MoonlightWeb — the host list a machine last answered with, per machine.
 *
 * The page asks its machine for the host list as soon as it opens, and the
 * machine answers in a few tens of milliseconds. Through the rendezvous, that
 * question and its answer ride the tunnel the page has only just opened, and
 * on a phone the wait between the seal lifting and the first card was seconds:
 * an empty page under the header (01/11/2026, an iPhone on the DualRTX
 * production — 5 to 6 s, while the same machine answered its own loopback in
 * 20 to 55 ms). The remembered app lists (appCache.js) could not help, because
 * they are painted inside a host's card, and the card waited for that answer.
 *
 * So the last list is kept here and drawn the moment the page is up; the real
 * answer still comes, replaces it and drops any host it no longer has. Kept per
 * machine — the rendezvous serves every machine from one origin, and a list
 * belongs to the machine that sent it.
 */

const HOSTS_KEY = 'mw-host-list';

// A list untouched for a month is likelier to mislead than to help.
const MAX_AGE_MS = 30 * 24 * 60 * 60 * 1000;

// Machines kept at once, so a browser that has visited many never grows this
// without bound.
const MAX_MACHINES = 8;

function readStore() {
    try {
        const raw = localStorage.getItem(HOSTS_KEY);
        const map = raw ? JSON.parse(raw) : null;
        return map && typeof map === 'object' ? map : {};
    } catch (e) {
        console.warn('[MW] Could not read the host-list memory:', e);
        return {};
    }
}

function writeStore(map) {
    try {
        localStorage.setItem(HOSTS_KEY, JSON.stringify(map));
    } catch (e) {
        console.warn('[MW] Could not persist the host-list memory:', e);
    }
}

/**
 * The host list this machine last answered with, in the shape the API returns,
 * or null when there is nothing worth drawing.
 * @param {string} machine which machine's page this is (instances.js id)
 * @returns {object[]|null}
 */
export function loadCachedHosts(machine) {
    if (!machine) return null;
    const entry = readStore()[machine];
    if (!entry || !Array.isArray(entry.hosts) || !(entry.ts > 0)) return null;
    if (Date.now() - entry.ts > MAX_AGE_MS) return null;
    const hosts = entry.hosts.filter((h) => h && typeof h.uuid === 'string' && h.uuid);
    return hosts.length ? hosts : null;
}

/**
 * Remember the host list this machine just answered with.
 * @param {string} machine
 * @param {object[]} hosts
 */
export function saveCachedHosts(machine, hosts) {
    if (!machine || !Array.isArray(hosts)) return;
    const map = readStore();
    // An empty answer is never drawn from memory: the empty page has its own
    // invitation, which waits for a real answer before saying "nothing here".
    if (hosts.length === 0) {
        if (!(machine in map)) return;
        delete map[machine];
    } else {
        map[machine] = { ts: Date.now(), hosts };
        const machines = Object.keys(map);
        if (machines.length > MAX_MACHINES) {
            machines
                .sort((a, b) => (map[b].ts || 0) - (map[a].ts || 0))
                .slice(MAX_MACHINES)
                .forEach((stale) => delete map[stale]);
        }
    }
    writeStore(map);
}
