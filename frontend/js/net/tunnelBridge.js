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
 * The application's end of the rendezvous connection.
 *
 * When MoonlightWeb is reached at its own address — a LAN IP, localhost, a mesh
 * VPN name — none of this runs and nothing in the application knows it exists.
 * When it is reached through the introduction server, there is no HTTP route to
 * the host at all, and this is what stands in for one.
 *
 * Two things happen here:
 *
 *  1. The page opens its own connection to the host. It has to be the page: a
 *     service worker cannot hold a WebRTC connection, so the worker asks this
 *     module to make each request and hands back what comes out. Every
 *     `fetch('/api/…')` in the application keeps working untouched, which is the
 *     whole reason for going through a worker rather than rewriting the client.
 *
 *  2. WebSockets are answered separately, because a service worker cannot
 *     intercept them at all. The streaming signalling asks for one through
 *     openSignalingSocket() below and gets an object of the same shape carried
 *     over the same channel. The video and the input never come this way — they
 *     get their own peer connection and go straight between the two machines.
 *
 * The transport itself is loaded from the bootstrap's own origin rather than
 * copied here. There is exactly one copy of that code, it is the copy the
 * published digests cover, and a watchdog comparing the live host against the
 * reference copy is therefore checking the code that actually runs.
 */

let tunnel = null;
let hostId = null;

/** The identifier this tab is bound to, or null when there is no tunnel. */
export function tunnelHostId() {
    return hostId;
}

/** Whether this page is reached through the introduction server. */
export function isTunnelMode() {
    return tunnel !== null;
}

/**
 * A WebSocket, or something shaped like one.
 *
 * On a direct connection this is `new WebSocket(url)` and nothing more. Through
 * the tunnel it is a socket carried on the data channel — same events, same
 * send(), same close() — so the calling code does not branch.
 */
export function openSignalingSocket(url) {
    if (!tunnel) return new WebSocket(url);
    // Only the path matters to the host; the scheme and authority describe an
    // address that does not answer on this path.
    const parsed = new URL(url, location.origin);
    return tunnel.openSocket(parsed.pathname + parsed.search);
}

/**
 * Answer the service worker's requests from the live connection.
 *
 * The worker sends the pieces of a request and a port to reply on. Anything that
 * goes wrong is reported as an error rather than left to time out: a worker
 * waiting on a port that will never be posted to is a page that hangs with no
 * explanation.
 */
function serveWorkerRequests() {
    navigator.serviceWorker.addEventListener('message', async (event) => {
        const msg = event.data;
        if (!msg || msg.type !== 'mw-tunnel-request') return;

        const port = event.ports[0];
        if (!port) return;

        try {
            const response = await tunnel.fetch(msg.url, {
                method: msg.method,
                headers: msg.headers,
                body: msg.body ? new Uint8Array(msg.body) : undefined,
            });
            const body = await response.arrayBuffer();
            const headers = [];
            response.headers.forEach((value, name) => headers.push([name, value]));
            port.postMessage({ status: response.status, headers, body }, [body]);
        } catch (e) {
            port.postMessage({ error: e.message || 'the connection failed' });
        }
    });
}

/**
 * Bring the connection up, if this page is reached that way.
 *
 * Resolves either way — a direct connection is not a failure — so the caller can
 * await it unconditionally before starting the application. Rejects only when
 * the page IS on the rendezvous and the connection could not be made, because
 * there is nothing to start in that case.
 */
export async function startTunnel() {
    if (!('serviceWorker' in navigator)) return false;

    // The bootstrap's transport, from the bootstrap's own origin. On a direct
    // connection this file does not exist and the import fails — which is the
    // correct answer to "are we on the rendezvous", arrived at without having to
    // guess from the shape of the URL.
    // Addressed through location rather than as a bare specifier so nothing
    // tries to resolve it at build time: it is not part of this bundle, it is a
    // file the other origin serves, and on a direct connection it is not there
    // at all.
    let mod;
    try {
        mod = await import(new URL('/tunnel.js', location.origin).href);
    } catch {
        return false;
    }

    hostId = mod.hostIdFromLocation();
    if (!hostId) {
        // We are on the rendezvous — the transport loaded — but nothing names a
        // machine: a fresh browser opening the bare address, or one whose
        // remembered machine was cleared. The service worker would happily serve
        // the cached interface here, and it would come up with no hosts, no
        // apps and no explanation, which is the worst of the three outcomes.
        //
        // Hand back to the bootstrap instead. `?mw=pick` is what tells the
        // worker to let this navigation reach the network rather than answering
        // it from the cache.
        location.replace('/?mw=pick');
        // Never resolves: the navigation is already under way, and letting the
        // caller continue would race the application against it.
        await new Promise(() => {});
    }

    tunnel = new mod.Tunnel(hostId);
    tunnel.onclosed = () => {
        // Go back through the bootstrap. It is the one path that can re-establish
        // everything — connection, cached interface, worker — and it says what is
        // happening while it does, rather than leaving a dead page on screen.
        location.replace(`/${hostId}`);
    };

    try {
        await tunnel.connect();
    } catch (e) {
        tunnel = null;
        throw e;
    }

    serveWorkerRequests();
    return true;
}
