/*
 * MoonlightWeb — ICE server fallback list.
 * Copyright (C) 2026 Bruno Martin. GPLv3 — see repository LICENSE.
 */

/**
 * The list a PeerConnection starts with when the host has not said otherwise.
 *
 * It is a fallback and nothing else: every current host sends an `ice-config`
 * message naming the one server it wants used — or an empty list, which is how
 * a LAN session says "none" — and that message replaces this. What is left here
 * is the case where no such message ever arrives, which means a host older than
 * this file.
 *
 * A STUN binding request tells whoever answers the public address of whoever
 * asked, so this list is the set of third parties that learn a viewer's IP.
 * Ours goes first for the same reason the backend's does: it belongs to the
 * operator the consent text already names. The public servers stay behind it
 * because a viewer who cannot discover their own address cannot connect at all,
 * and a failed connection is a worse answer than a widely-used third party.
 *
 * Ours is derived from where the page came from rather than hard-coded: an
 * instance running under its own MW_DOMAIN should reach its own operator, not
 * this project's. Served from `stream.<domain>` (the introduction server, which
 * answers STUN on 3478 as well) that host is used directly; anywhere else —
 * loopback, a LAN address — the project default stands, since a LAN page has no
 * way to know which introduction server its host talks to and, being on the LAN,
 * will be told to use no STUN at all.
 *
 * @returns {RTCIceServer[]}
 */
export function defaultIceServers() {
    const servers = [];
    const ours = ourStunHost();
    if (ours) servers.push({ urls: `stun:${ours}:3478` });
    servers.push(
        { urls: 'stun:stun.l.google.com:19302' },
        { urls: 'stun:stun.cloudflare.com:3478' },
        { urls: 'stun:stun.nextcloud.com:443' },
        { urls: 'stun:relay.metered.ca:80' },
    );
    return servers;
}

/**
 * Hostname of the introduction server this page was served from, or the project
 * default. Empty when there is no usable location at all.
 *
 * @returns {string}
 */
export function ourStunHost() {
    try {
        const host = globalThis.location?.hostname || '';
        if (host.startsWith('stream.')) return host;
    } catch (_e) {
        /* no location (worker, test harness) — fall through to the default */
    }
    return 'stream.moonlightweb.top';
}
