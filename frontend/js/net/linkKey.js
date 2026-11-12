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
 * The key a link of this machine carries.
 *
 * The tray icon, a launch and the installer's last page open the browser on a
 * link that ends in `#k=<key>`: this machine's own address
 * (`https://localhost:<port>/<page>#k=<key>`), or its rendezvous link, which
 * the bootstrap hands over to this page with the key still in the fragment.
 * The key says "this browser was opened by this machine's owner, here"; the
 * page spends it once, at POST /api/auth/host-key, for a session of the host
 * machine.
 *
 * It is taken off the address as this module loads, and this module is the
 * first the application imports: before any request leaves, before a socket
 * opens, before any other module runs and could read the address into a log.
 * It then lives in a variable here, and nowhere that outlives the page: no
 * storage, no cookie.
 *
 * Through the rendezvous a page may have to start over before the host has
 * answered — the line dropped, the cached interface is not the host's — and
 * starting over is a navigation through the bootstrap, which reads the address
 * it is given. So until the host has answered, the key goes along: into the
 * address the page leaves for (carriedLinkKey), or back into this page's own
 * just before a reload it does itself (putLinkKeyBack). Once the host has
 * answered (forgetLinkKey), it is gone. A page the host serves itself has no
 * bootstrap to go back through, and keeps nothing to carry.
 *
 * No import, on purpose: nothing may run before it.
 */

/** The key, until it is asked for. */
let taken = null;
/** Through the rendezvous: the same key, until the host has answered. */
let carried = null;
/** Whether the page came through the rendezvous. */
let throughRendezvous = false;

/**
 * Splits a fragment into the key it carries and the fragment without it.
 * Pure. `#k=K` gives `{ key: 'K', hash: '' }`; `#a=1&k=K&b=2` keeps `#a=1&b=2`;
 * a fragment without a key comes back as it was, with a null key.
 *
 * @param {string} hash a `location.hash`, with or without its `#`
 * @returns {{ key: string|null, hash: string }}
 */
export function splitLinkKey(hash) {
    const raw = String(hash || '').replace(/^#/, '');
    if (!raw) return { key: null, hash: '' };
    let key = null;
    const kept = [];
    for (const part of raw.split('&')) {
        if (key === null && part.startsWith('k=') && part.length > 2) {
            try {
                key = decodeURIComponent(part.slice(2));
            } catch {
                // Not something a link of this machine carries: no key, and
                // the part is dropped all the same, never left in the address.
                key = null;
            }
            continue;
        }
        kept.push(part);
    }
    return { key: key || null, hash: kept.length ? '#' + kept.join('&') : '' };
}

/** This page's address with `hash` for a fragment. */
function addressWith(hash) {
    return window.location.pathname + window.location.search + hash;
}

/** Takes the key off this page's address. Runs once, as the module loads. */
function takeFromAddress() {
    try {
        const current = window.location.hash;
        if (!current || current.indexOf('k=') < 0) return;
        const { key, hash } = splitLinkKey(current);
        if (hash === current) return; // no key in there after all
        // A controlling service worker is the bootstrap's, the only one there
        // is: the page came through the rendezvous.
        const viaWorker = !!(navigator.serviceWorker && navigator.serviceWorker.controller);
        // Off the address first, kept second: if the address cannot be
        // rewritten, the key is not used at all rather than left in the bar.
        history.replaceState(history.state, '', addressWith(hash));
        taken = key;
        throughRendezvous = viaWorker;
        carried = viaWorker ? key : null;
    } catch {
        taken = null;
        carried = null;
    }
}

takeFromAddress();

/**
 * The key this page was opened with, once: whoever asks first gets it, and a
 * second caller gets null rather than a chance to send it again. `direct` says
 * the page is one the host serves itself, not one that came through the
 * rendezvous.
 *
 * @returns {{ key: string, direct: boolean }|null}
 */
export function takeLinkKey() {
    const key = taken;
    taken = null;
    return key ? { key, direct: !throughRendezvous } : null;
}

/**
 * Through the rendezvous: the key a page that starts over takes along, for the
 * address it leaves for. Null once the host has answered, and on a page the
 * host serves itself.
 *
 * @returns {string|null}
 */
export function carriedLinkKey() {
    return carried;
}

/**
 * Just before a reload this page does itself: the key goes back into the
 * address, for the page that follows to find. Does nothing once the host has
 * answered, and on a page the host serves itself.
 */
export function putLinkKeyBack() {
    if (!carried) return;
    try {
        // At the end: the identifier, when the fragment names it, stays first.
        const rest = splitLinkKey(window.location.hash).hash.replace(/^#/, '');
        const hash = '#' + (rest ? rest + '&' : '') + 'k=' + encodeURIComponent(carried);
        history.replaceState(history.state, '', addressWith(hash));
    } catch {
        // The address stays as it is: the page that follows asks for the PIN.
    }
}

/** The host has answered, yes or no: the key has had its use. */
export function forgetLinkKey() {
    taken = null;
    carried = null;
}
