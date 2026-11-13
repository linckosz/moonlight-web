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
 * A TV on plain HTTP (issue #42): the routes it signs in by, and the ones the
 * administrator lets it in by.
 *
 * The TV's half answers only a TV listed in `lan_http_clients`, on the HTTP
 * port; to anyone else it does not exist (404), which is how the page tells a
 * listed TV from a browser that should go to HTTPS. The administrator's half
 * answers on HTTPS only, the administrator alone, with the admin key that
 * BackendClient.post adds.
 */

import { BackendClient } from './BackendClient.js';

/**
 * Whether this page is a TV listed for plain HTTP, and signed in.
 * @returns {Promise<{signed_in: boolean}|null>} null for any other browser
 */
export async function lanHttpStatus() {
    let resp;
    try {
        resp = await fetch('/api/lan-http/status', { cache: 'no-store' });
    } catch {
        return null;
    }
    if (!resp.ok) return null;
    try {
        const body = await resp.json();
        return { signed_in: body?.signed_in === true };
    } catch {
        return null;
    }
}

/**
 * The TV asks to be let in: its request, the code it shows, the claim it asks
 * again with, and where its QR code leads.
 * @returns {Promise<{request: string, code: string, claim: string, expires_in: number, approve_url: string}>}
 */
export async function lanHttpPair() {
    return BackendClient.post('/api/lan-http/pair', {});
}

/**
 * Where the TV's request stands: pending, refused, gone or approved (its
 * cookie then set by the answer). The claim travels in a header, never in
 * the address.
 * @param {string} id
 * @param {string} claim
 * @returns {Promise<'pending'|'refused'|'gone'|'approved'>}
 */
export async function lanHttpPoll(id, claim) {
    const path = `/api/lan-http/pair/${encodeURIComponent(id)}`;
    const resp = await fetch(path, { cache: 'no-store', headers: { 'X-MW-Claim': claim } });
    if (resp.status === 404) return 'gone';
    if (!resp.ok) {
        const err = /** @type {Error & {statusCode?: number}} */ (
            new Error(`Request failed (${resp.status})`)
        );
        err.statusCode = resp.status;
        throw err;
    }
    const body = await resp.json();
    const state = body?.state;
    return state === 'approved' || state === 'refused' || state === 'pending' ? state : 'gone';
}

/**
 * What the approval page shows of a TV's request: its address, the time left.
 * @param {string} id
 * @returns {Promise<{address: string, expires_in: number}>}
 */
export async function lanHttpRequest(id) {
    return BackendClient.get(`/api/lan-http/approve/${encodeURIComponent(id)}`);
}

/** The administrator lets the TV in, with the code read on its screen. */
export async function lanHttpApprove(id, code) {
    return BackendClient.post(`/api/lan-http/approve/${encodeURIComponent(id)}`, { code });
}

/** The administrator says no. */
export async function lanHttpRefuse(id) {
    return BackendClient.post(`/api/lan-http/refuse/${encodeURIComponent(id)}`, {});
}
