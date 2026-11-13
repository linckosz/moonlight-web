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
 * The link straight to one app this page was opened with (issue #63).
 *
 * A home-screen icon that opens one computer's Desktop in one tap ends in
 * `#go=<alias>`: an alias the host minted for one app of one of its hosts
 * (server/AppLinks.h). It is no key — following it still takes this server's
 * session, and it launches only what a tap on that app's card launches, through
 * that very card — and it rides in the fragment so that no server along the
 * way, the introduction server least of all, learns which app anyone opens.
 *
 * It leaves the address as this module loads, so a reload or a Stop never
 * opens the app again, and waits in this tab's sessionStorage until it has been
 * served: a page that has to ask for the PIN first, or start over through the
 * rendezvous, still opens the app once it is in. Only an alias of the exact
 * shape the host mints is kept; anything else is dropped where it was read. A
 * guest's page has nothing to do with it, and keeps nothing.
 *
 * Nothing it holds is old: an alias not served within a few minutes is
 * forgotten rather than acted on long after the tap that asked for it.
 */

/** The shape the host mints: ten letters and digits (AppLinks.h validAlias). */
const ALIAS_SHAPE = /^[A-Za-z0-9]{10}$/;

/** Where the alias waits until it is served. */
const STORE_KEY = 'mw-app-link';

/** How long an alias waits for the page to be ready to serve it. */
const PATIENCE_MS = 10 * 60 * 1000;

/** Whether @p alias has an alias's shape. */
export function isAppLinkAlias(alias) {
    return typeof alias === 'string' && ALIAS_SHAPE.test(alias);
}

/**
 * Splits a fragment into the alias it carries and the fragment without it.
 * Pure. Every `go=` part leaves the fragment, a well-formed one or not; the
 * alias is the first well-formed one, read as it is written (never decoded).
 *
 * @param {string} hash a `location.hash`, with or without its `#`
 * @returns {{ alias: string|null, hash: string }}
 */
export function splitAppLink(hash) {
    const raw = String(hash || '').replace(/^#/, '');
    if (!raw) return { alias: null, hash: '' };
    let alias = null;
    const kept = [];
    for (const part of raw.split('&')) {
        if (part.startsWith('go=')) {
            const value = part.slice(3);
            if (alias === null && isAppLinkAlias(value)) alias = value;
            continue;
        }
        kept.push(part);
    }
    return { alias, hash: kept.length ? '#' + kept.join('&') : '' };
}

/** Whether this page is a guest's: an invitation in its fragment, or /p. */
function isGuestPage() {
    if (window.location.pathname === '/p') return true;
    const raw = String(window.location.hash || '').replace(/^#/, '');
    return raw.split('&').some((part) => part.startsWith('t='));
}

function store(alias) {
    try {
        sessionStorage.setItem(STORE_KEY, JSON.stringify({ alias, at: Date.now() }));
    } catch {
        // No storage: the alias is served on this page or not at all.
    }
}

/** The alias read from the address, for a page whose storage refused it. */
let inMemory = null;

/** Takes the alias off this page's address. Runs once, as the module loads. */
function takeFromAddress() {
    try {
        const current = window.location.hash;
        if (!current || current.indexOf('go=') < 0) return;
        const { alias, hash } = splitAppLink(current);
        if (hash === current) return;
        const guest = isGuestPage();
        history.replaceState(
            history.state,
            '',
            window.location.pathname + window.location.search + hash,
        );
        if (!alias || guest) return;
        inMemory = { alias, at: Date.now() };
        store(alias);
    } catch {
        inMemory = null;
    }
}

takeFromAddress();

/**
 * The alias waiting to be served, or null. Read, not taken: it stays until
 * forgetAppLink() says it was served, so a page that reloads before that still
 * finds it.
 *
 * @returns {string|null}
 */
export function pendingAppLink() {
    let entry = null;
    try {
        const raw = sessionStorage.getItem(STORE_KEY);
        if (raw) entry = JSON.parse(raw);
    } catch {
        entry = null;
    }
    if (!entry) entry = inMemory;
    if (!entry || !isAppLinkAlias(entry.alias)) return null;
    const age = Date.now() - Number(entry.at);
    if (!(age >= 0 && age <= PATIENCE_MS)) {
        forgetAppLink();
        return null;
    }
    return entry.alias;
}

/** Served — opened, asked about, or found to lead nowhere: it is done. */
export function forgetAppLink() {
    inMemory = null;
    try {
        sessionStorage.removeItem(STORE_KEY);
    } catch {
        // Nothing kept.
    }
}

/**
 * The address of a link straight to one app, as a person keeps it: through the
 * rendezvous, the machine's own link (`<origin>/<id>#go=<alias>`), which the
 * entry page opens wherever the browser is; otherwise this origin's own root.
 *
 * @param {string} alias
 * @param {{ origin: string, tunnelId?: string|null }} where
 * @returns {string}
 */
export function appLinkAddress(alias, { origin, tunnelId = null }) {
    if (!isAppLinkAlias(alias)) return '';
    const base = String(origin || '').replace(/\/+$/, '');
    return tunnelId ? `${base}/${tunnelId}#go=${alias}` : `${base}/#go=${alias}`;
}
