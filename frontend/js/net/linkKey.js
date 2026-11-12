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
 * The key a link of this machine carries, on a page the host serves itself.
 *
 * The tray icon, a launch and the installer's last page open the browser on
 * `https://localhost:<port>/<page>#k=<key>`. The key says "this browser was
 * opened by this machine's owner, here"; the page spends it once, at
 * POST /api/auth/host-key, for a session of the host machine.
 *
 * It is taken off the address as this module loads, and this module is the
 * first the application imports: before any request leaves, before a socket
 * opens, before any other module runs and could read the address into a log.
 * It then lives in a variable here until it is asked for, once.
 *
 * A page that came through the rendezvous is not this module's: there the
 * bootstrap's own transport reads the fragment (net/tunnelBridge.js), and the
 * address keeps what names the machine.
 *
 * No import, on purpose: nothing may run before it.
 */

let taken = null;

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

/** Takes the key off this page's address. Runs once, as the module loads. */
function takeFromAddress() {
    try {
        // Through the rendezvous: not ours (see above).
        if (navigator.serviceWorker && navigator.serviceWorker.controller) return;
        const current = window.location.hash;
        if (!current || current.indexOf('k=') < 0) return;
        const { key, hash } = splitLinkKey(current);
        if (hash === current) return; // no key in there after all
        // Off the address first, kept second: if the address cannot be
        // rewritten, the key is not used at all rather than left in the bar.
        history.replaceState(
            history.state,
            '',
            window.location.pathname + window.location.search + hash,
        );
        taken = key;
    } catch {
        taken = null;
    }
}

takeFromAddress();

/**
 * The key this page was opened with, once: whoever asks first gets it, and a
 * second caller gets null rather than a chance to send it again.
 *
 * @returns {string|null}
 */
export function takeDirectLinkKey() {
    const key = taken;
    taken = null;
    return key;
}
