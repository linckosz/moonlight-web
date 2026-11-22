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
 * A link straight to one app made into a home-screen icon (issue #63).
 *
 * A phone makes its icon from the manifest of the page as the page loaded, and
 * never from one the page points at afterwards (seen on an iPhone, Safari 26.5:
 * the icon opened "/" and named the edition). So the link button of a card, on
 * a phone, notes the alias in this tab (appLink.js markAppIcon) and reloads the
 * page already being that link:
 *
 *   straight to the host   on "/?icon=<alias>", whose shell names the manifest
 *                          that opens the link (server/HomeScreenShell.h);
 *   through the            on its own address, once the service worker's cache
 *   introduction server    holds that manifest: nothing that goes out carries
 *                          the alias, only the fragment changes.
 *
 * As this module loads it decides, once, whether this page is that icon's. The
 * query leaves the address before anything else is asked of the network, and a
 * page that is not an icon's makes sure no manifest the cache holds still opens
 * a link. Whether the page came through the tunnel is the service worker's word
 * (tunnelBridge.js pageCameThroughTunnel), never the address's.
 *
 * It never navigates: a page that cannot be the icon fails closed. Reloaded
 * without the query, straight to the host, its shell names the ordinary
 * manifest — and a page that came through the tunnel may have lost its worker
 * on the way (a hard reload, an update), with nothing to tell it from one that
 * never had one. So such a reload simply ends the icon, launching nothing; the
 * link button is there to start again.
 */

import {
    appIconMark,
    clearAppIconMark,
    isAppLinkAlias,
    isGuestPage,
    splitAppLink,
} from './appLink.js';
import { pageCameThroughTunnel } from './tunnelBridge.js';

/** The bootstrap's cache of the application's files (bootstrap/sw.js). */
const SHELL_CACHE = 'mw-shell';
const MANIFEST = '/manifest.webmanifest';

/** `{ alias, title }` of the icon this page is being made into, or null. */
let current = null;

function decide() {
    let asked = null;
    try {
        const url = new URL(window.location.href);
        if (url.searchParams.has('icon')) asked = url.searchParams.get('icon');
        const mark = appIconMark();
        const tunnel = pageCameThroughTunnel();
        const linked = splitAppLink(window.location.hash);
        // The alias this tab's mark left in the address for this page (appLink.js
        // takeFromAddress): this page's to take, or to drop.
        const ownLink = !!mark && !isGuestPage() && linked.alias === mark.alias;
        if (mark && !isGuestPage()) {
            if (asked !== null) {
                // The shell this query asks for is the host's own answer; through
                // the tunnel it is never asked for, and never believed.
                if (!tunnel && asked === mark.alias) current = mark;
            } else if (tunnel && ownLink) {
                // Through the tunnel the page becomes the icon by reloading on
                // its own address, the worker's cache holding the link's manifest.
                current = mark;
            }
        }
        if (asked !== null || url.searchParams.has('n') || url.searchParams.has('s')) {
            // First, before any request: the alias leaves the query, and the
            // icon's names with it — every one of them, given once or more.
            url.searchParams.delete('icon');
            url.searchParams.delete('n');
            url.searchParams.delete('s');
            const hash = current ? `#go=${current.alias}` : ownLink ? linked.hash : url.hash;
            history.replaceState(history.state, '', url.pathname + url.search + hash);
        } else if (!current && ownLink) {
            // Not the icon after all: the alias leaves the address too, so that
            // no later load takes it for a link to follow.
            history.replaceState(
                history.state,
                '',
                window.location.pathname + window.location.search + linked.hash,
            );
        }
        if (!current) {
            if (mark) clearAppIconMark();
            forgetCachedAppLink();
        }
    } catch {
        current = null;
    }
}

/**
 * The icon this page is being made into, `{ alias, title }`, or null. Decided
 * once, as the module loaded; null again after endAppIcon().
 */
export function appIconMode() {
    return current;
}

/** This page is no icon's any more: forgotten here and in this tab. */
export function endAppIcon() {
    current = null;
    clearAppIconMark();
}

/**
 * What the address bar shows while the icon is made: the link itself, so that a
 * phone that bookmarks the address rather than the manifest keeps it too. The
 * path stays the application's own: only the fragment is the link's.
 */
export function appIconAddress(alias) {
    return isAppLinkAlias(alias) ? `/#go=${alias}` : '/';
}

/**
 * The machine's manifest made into the one that opens the link to @p alias,
 * here rather than asked of the host, so the alias is never in an address
 * that goes out: `go=` first in the fragment of where it opens, the application
 * named apart in its `id` (one icon per app), and no names but @p name, the
 * icon's, and @p shortName, the one under it (the app alone), each when it
 * holds as one (appIconName); without a short name, the name cut short. The
 * same shape and rules as the host's own (server/HomeScreenShell.cpp
 * manifest). Anything but an alias: unchanged.
 */
export function manifestWithAppLink(manifest, alias, name = '', shortName = '') {
    if (!isAppLinkAlias(alias) || !manifest || typeof manifest !== 'object') return manifest;
    const out = { ...manifest };
    const start = String(out.start_url || '/');
    const cut = start.indexOf('#');
    const path = cut < 0 ? start : start.slice(0, cut);
    const rest = (cut < 0 ? '' : start.slice(cut + 1))
        .split('&')
        .filter((part) => part && !part.startsWith('go='));
    out.start_url = `${path}#${[`go=${alias}`, ...rest].join('&')}`;
    out.id = `${String(out.id || path).split('?')[0]}?go=${alias}`;
    delete out.name;
    delete out.short_name;
    const shown = appIconName(name);
    // A short name refused counts as none; it never stands for the name.
    const under = appIconName(shortName) || appIconShortName(shown);
    if (shown) out.name = shown;
    if (under) out.short_name = under;
    return out;
}

/** The longest name an icon is given, and its short name, in code points
 *  (server/HomeScreenShell.h kAppIconNameMax, kAppIconShortNameMax). */
const NAME_MAX = 64;
const SHORT_NAME_MAX = 12;

/** What a name may not hold, refused whole: the host's own list
 *  (HomeScreenShell.cpp refusedInName). */
function refusedInName(c) {
    return (
        c < 0x20 ||
        (c >= 0x7f && c <= 0x9f) ||
        (c >= 0x200b && c <= 0x200f) ||
        (c >= 0x2028 && c <= 0x202e) ||
        (c >= 0x2066 && c <= 0x2069) ||
        c === 0xfeff ||
        c === 0xfffd ||
        (c >= 0xd800 && c <= 0xdfff) // a surrogate left alone
    );
}

/**
 * @p name when it holds as the name of an icon, as the host decides it
 * (HomeScreenShell.h appIconName): not blank, at most 64 code points, nothing
 * refusedInName. '' otherwise: refused whole, never cleaned.
 */
export function appIconName(name) {
    if (typeof name !== 'string' || !name.trim()) return '';
    const points = Array.from(name, (ch) => ch.codePointAt(0));
    if (points.length > NAME_MAX || points.some(refusedInName)) return '';
    return name;
}

/** appIconName(@p name) cut to 12 code points, never between a letter and its
 *  marks (HomeScreenShell.h appIconShortName). */
export function appIconShortName(name) {
    const whole = appIconName(name);
    const chars = Array.from(whole);
    if (chars.length <= SHORT_NAME_MAX) return whole;
    let cut = SHORT_NAME_MAX;
    while (cut > 0 && /\p{M}/u.test(chars[cut])) cut -= 1;
    return chars.slice(0, cut).join('').trim();
}

/**
 * Takes the link out of the manifest the service worker's cache holds, when it
 * carries one: an icon made later, from any page of this browser, must not open
 * an app nobody asked for any more. Read and written here, never asked of the
 * network. Left alone if the cache changed meanwhile (a fresh manifest written
 * by the page). Resolves true when one was taken out.
 */
export async function forgetCachedAppLink() {
    try {
        if (typeof caches === 'undefined' || !(await caches.has(SHELL_CACHE))) return false;
        const cache = await caches.open(SHELL_CACHE);
        const hit = await cache.match(MANIFEST);
        if (!hit) return false;
        const text = await hit.text();
        const manifest = JSON.parse(text);
        const start = String(manifest.start_url || '');
        const id = String(manifest.id || '');
        if (start.indexOf('go=') < 0 && id.indexOf('go=') < 0) return false;

        const cut = start.indexOf('#');
        const path = cut < 0 ? start : start.slice(0, cut);
        const kept = (cut < 0 ? '' : start.slice(cut + 1))
            .split('&')
            .filter((part) => part && !part.startsWith('go='));
        manifest.start_url = kept.length ? `${path}#${kept.join('&')}` : path;
        if (manifest.id !== undefined) manifest.id = id.split('?')[0];
        // And the app's name goes with its link: the machine's manifest names
        // nothing until the page writes it again.
        delete manifest.name;
        delete manifest.short_name;

        const now = await cache.match(MANIFEST);
        if (!now || (await now.text()) !== text) return false;
        await cache.put(
            MANIFEST,
            new Response(JSON.stringify(manifest), {
                headers: { 'Content-Type': 'application/manifest+json' },
            }),
        );
        return true;
    } catch {
        return false;
    }
}

decide();
