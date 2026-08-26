/*
 * MoonlightWeb — bootstrap. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * The service worker: what makes the application look like an ordinary website
 * when there is no website.
 *
 * The page is served from an address that belongs to the introduction server,
 * but the application it runs comes from the user's own machine and reaches the
 * browser over a data channel. Something has to stand between the two, because
 * the application's own code does the ordinary thing — <script src>, CSS url(),
 * fetch('/api/…') — and none of that knows about data channels.
 *
 * Two jobs, and the split matters:
 *
 *   static files   answered from the cache the bootstrap filled over the
 *                  channel. They are the same bytes for every visitor and they
 *                  never change between updates, so a copy is exactly right.
 *
 *   everything     handed to the page, which owns the live connection. A worker
 *   else           cannot hold one: WebRTC is not available in this context, and
 *                  that is not an oversight to work around — it is why the
 *                  connection lives in the page and only its RESULTS come here.
 *
 * A worker has no memory between wake-ups, so nothing is kept in a variable
 * here that matters. The client is looked up on every request.
 */

const SHELL_CACHE = 'mw-shell';

/** How long a request may wait for the page to answer before it is given up. */
const CLIENT_TIMEOUT_MS = 30000;

self.addEventListener('install', () => {
    // Take over straight away. The bootstrap registers this worker and then
    // navigates, and waiting a lifecycle would mean that navigation is the one
    // request nobody serves.
    self.skipWaiting();
});

self.addEventListener('activate', (event) => {
    event.waitUntil(self.clients.claim());
});

/**
 * Ask the page that owns the tunnel to make this request for us.
 *
 * The client is resolved by id first — that is the page the request came from —
 * and only then by "any window", which covers a request from a worker or an
 * <img> whose client id the browser did not attach.
 */
async function askThePage(request, clientId) {
    let client = clientId ? await self.clients.get(clientId) : null;
    if (!client) {
        const windows = await self.clients.matchAll({ type: 'window' });
        client = windows[0];
    }
    if (!client) return new Response('no page holds the connection', { status: 503 });

    const body =
        request.method === 'GET' || request.method === 'HEAD'
            ? null
            : await request.clone().arrayBuffer();

    const headers = [];
    request.headers.forEach((value, name) => headers.push([name, value]));

    return new Promise((resolve) => {
        const channel = new MessageChannel();
        const timer = setTimeout(
            () => resolve(new Response('the connection did not answer', { status: 504 })),
            CLIENT_TIMEOUT_MS,
        );

        channel.port1.onmessage = (event) => {
            clearTimeout(timer);
            const answer = event.data;
            if (!answer || answer.error) {
                resolve(
                    new Response(answer?.error || 'the connection failed', {
                        status: 502,
                    }),
                );
                return;
            }
            resolve(
                new Response(answer.status === 204 || answer.status === 304 ? null : answer.body, {
                    status: answer.status,
                    headers: answer.headers,
                }),
            );
        };

        client.postMessage(
            {
                type: 'mw-tunnel-request',
                method: request.method,
                url: request.url,
                headers,
                body,
            },
            [channel.port2],
        );
    });
}

/**
 * Fall back to the bootstrap when the cache holds nothing.
 *
 * Fetched from the network rather than written out here, so there is one entry
 * page rather than a second, uglier one that drifts. `?mw=pick` keeps this
 * request out of the branch above and stops it looping back into the cache.
 */
async function bootstrapPage() {
    try {
        return await fetch('/?mw=pick', { cache: 'no-store' });
    } catch {
        return new Response('Open your machine’s link again — it ends in 26 characters.', {
            status: 200,
            headers: { 'Content-Type': 'text/plain; charset=utf-8' },
        });
    }
}

self.addEventListener('fetch', (event) => {
    const request = event.request;
    const url = new URL(request.url);

    // Anything not on this origin is none of our business.
    if (url.origin !== self.location.origin) return;

    // The bootstrap's own files stay on the network. They are the pinned,
    // published set that a watchdog compares against a reference copy, and
    // serving them from a cache filled over the tunnel would quietly move them
    // out from under that check.
    //
    // `?mw=pick` joins them: it is how a page that found no machine to talk to
    // asks for the entry page back. Answering that from the cache would return
    // the very application it just gave up on.
    if (
        url.pathname === '/sw.js' ||
        url.pathname === '/boot.js' ||
        url.pathname === '/tunnel.js' ||
        url.pathname === '/pairing.js' ||
        url.pathname === '/frame-guard.js' ||
        url.searchParams.get('mw') === 'pick' ||
        /^\/[0-9a-z]{26}\/?$/.test(url.pathname)
    )
        return;

    event.respondWith(
        (async () => {
            const cache = await caches.open(SHELL_CACHE);

            if (request.mode === 'navigate') {
                const shell = await cache.match('/index.html');
                return shell || (await bootstrapPage());
            }

            // /version.json is how the application notices the host was updated,
            // so it must never come from the copy it is checking.
            if (url.pathname !== '/version.json') {
                const hit = await cache.match(url.pathname);
                if (hit) return hit;
            }

            return askThePage(request, event.clientId);
        })(),
    );
});
