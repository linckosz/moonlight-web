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

/**
 * This worker's revision, which the entry page asks for before it hands over
 * where the interface check is enforced (boot.js, WORKER_REVISION). 2 is the
 * first to serve only the files that page checked.
 */
const SW_REVISION = 2;

/**
 * Whether this origin holds every machine to the official interface
 * (bootstrap/v1/official.js, interfacePolicy — repeated here, since a worker is a
 * classic script and cannot import the module; a test keeps the two equal).
 *
 * Where it does, this worker serves the checked files and nothing else: a file
 * the cache lacks is not asked of the machine, which could answer anything, and
 * what the machine answers on /api/ is never taken as code. Elsewhere it does
 * what it always did.
 */
const PRODUCTION_ENFORCES = true;
const ENFORCED = /^stream\.dev\./.test(self.location.hostname) || PRODUCTION_ENFORCES;

/**
 * A shell from before v0.3.1 never asks its host whether it is still current.
 *
 * It only learns of an update when the machine's link — /<id> — is opened,
 * because that is the one address that runs the bootstrap. Opened from a home
 * screen icon or from the bare root, it serves itself forever. The v0.3.1
 * application checks on every start; its VersionGuard carries this key, the
 * older one does not, and that is how the two are told apart.
 */
const SHELL_GUARD_FILE = '/js/util/VersionGuard.js';
const SHELL_GUARD_MARK = 'mw-shell-recheck';

/**
 * When this worker last sent a stale shell back through the link.
 *
 * Kept in a cache of its own, because a worker has no memory between wake-ups
 * and the bootstrap empties 'mw-shell' whenever it refills it. The note is what
 * stops a loop: a host still on the old version hands back the same old shell,
 * and the bootstrap then lands on the root again — which must now open it.
 */
const NOTES_CACHE = 'mw-sw-notes';
const STALE_NOTE = '/stale-shell-redirect';

/** One trip back through the link per stale shell per this long. */
const STALE_RETRY_MS = 60 * 60 * 1000;

/**
 * When the entry page last checked the cache, and how long a navigation may
 * still be answered from it after that.
 *
 * Every machine reached through this origin shares it, so any of their pages
 * can write the cache this worker serves. The entry page is the one thing here
 * no machine serves — it comes from the introduction server — and it checks the
 * cache against the official interface, or the machine's own hashes where no
 * list applies (boot.js, shellIsTrusted). So the cached interface opens only
 * right after one of those checks; every other navigation, a reload or a
 * home-screen shortcut, goes through the entry page first (security review R1,
 * 26/10/2026).
 *
 * Held in this worker's memory and nowhere else: a note in storage could be
 * written by any page. A worker stopped in between forgets it, and the
 * navigation simply goes through the entry page once more. Spent by the
 * navigation it lets through.
 */
const CHECKED_FOR_MS = 15000;
let checkedAt = 0;

self.addEventListener('message', (event) => {
    const data = event.data;
    if (!data) return;
    if (data.type === 'mw-sw-revision') {
        if (event.ports[0]) event.ports[0].postMessage({ revision: SW_REVISION });
        return;
    }
    // An entry page of before revision 2 says only 'mw-shell-checked', for a
    // check against the machine's own hashes: not the one enforced here.
    if (data.type === 'mw-shell-verified' || (data.type === 'mw-shell-checked' && !ENFORCED))
        checkedAt = Date.now();
});

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
 * The page a request is to be carried by: the one it came from.
 *
 * Each page's connection goes to the machine IT opened, and two tabs may be on
 * two machines. So where the check is enforced, a request whose page has gone —
 * a beacon sent as a tab closes — is not handed to another tab, whose machine
 * would receive it with that machine's own cookie; and one the browser named no
 * page for goes to the only page there is, or to none. Elsewhere any window
 * will do, as it always did.
 */
async function pageFor(clientId) {
    const own = clientId ? await self.clients.get(clientId) : null;
    if (own) return own;
    const windows = await self.clients.matchAll({ type: 'window' });
    if (!ENFORCED) return windows[0];
    return !clientId && windows.length === 1 ? windows[0] : null;
}

/** Ask the page that owns the tunnel to make this request for us. */
async function askThePage(request, clientId) {
    const client = await pageFor(clientId);
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
 * What the host answers on these is the machine's own word, and only ever data:
 * /api/… and the version it runs. Asked of it each time, never from the cache.
 */
function fromTheMachine(pathname) {
    return pathname.startsWith('/api/') || pathname === '/version.json';
}

/**
 * The kinds of request whose answer the browser runs, or lays out, as code. A
 * checked page never makes one for an answer of the machine's own, so where the
 * check is enforced such a request is refused rather than carried.
 */
const RUNS_AS_CODE = new Set([
    'audioworklet',
    'document',
    'embed',
    'frame',
    'iframe',
    'json',
    'object',
    'paintworklet',
    'script',
    'serviceworker',
    'sharedworker',
    'style',
    'worker',
    'xslt',
]);

/** The types the host serves the application's files under (StaticFileHandler). */
const CONTENT_TYPES = {
    html: 'text/html; charset=utf-8',
    css: 'text/css; charset=utf-8',
    js: 'application/javascript; charset=utf-8',
    json: 'application/json',
    webmanifest: 'application/manifest+json',
    png: 'image/png',
    jpg: 'image/jpeg',
    jpeg: 'image/jpeg',
    gif: 'image/gif',
    webp: 'image/webp',
    svg: 'image/svg+xml',
    ico: 'image/x-icon',
    woff: 'font/woff',
    woff2: 'font/woff2',
    ttf: 'font/ttf',
    map: 'application/json',
};

/**
 * The headers a checked file is served with, where the check is enforced.
 *
 * Written here rather than read from the cache, which holds them as the machine
 * sent them. The policy is what keeps a checked page from running anything that
 * was not checked, so a machine that chose it could lift it. It is the host's
 * own (HttpServer::securityHeaders), unchanged from v0.3.0 to v0.4.1.
 */
function shellHeaders(extension) {
    return {
        'Content-Type': CONTENT_TYPES[extension] || 'application/octet-stream',
        'X-Content-Type-Options': 'nosniff',
        'X-Frame-Options': 'DENY',
        'Referrer-Policy': 'strict-origin-when-cross-origin',
        'Content-Security-Policy':
            "default-src 'self'; script-src 'self' 'wasm-unsafe-eval'; " +
            "style-src 'self' 'unsafe-inline'; font-src 'self'; " +
            `img-src 'self' data: blob:; connect-src 'self' wss://${self.location.host}; ` +
            "worker-src 'self' blob:; frame-ancestors 'none'; base-uri 'self'; " +
            "form-action 'self'; object-src 'none'",
        'Cache-Control': 'no-cache',
    };
}

function extensionOf(pathname) {
    const name = pathname.slice(pathname.lastIndexOf('/') + 1);
    const dot = name.lastIndexOf('.');
    return dot < 0 ? '' : name.slice(dot + 1).toLowerCase();
}

/** A cached file as this worker serves it where the check is enforced. */
function checkedFile(hit, extension) {
    return new Response(hit.body, { status: 200, headers: shellHeaders(extension) });
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

/** Whether the cached application checks its own version when it starts. */
async function shellChecksItself(cache) {
    const guard = await cache.match(SHELL_GUARD_FILE);
    if (!guard) return false;
    return (await guard.text()).includes(SHELL_GUARD_MARK);
}

/**
 * Whether a stale shell should be sent back through the link now.
 *
 * Writes the note as it answers yes, so the navigation the bootstrap ends with
 * finds it and opens the shell instead of turning round again.
 */
async function timeForStaleRedirect() {
    const notes = await caches.open(NOTES_CACHE);
    const note = await notes.match(STALE_NOTE);
    const last = note ? Number(await note.text()) : 0;
    if (last && Date.now() - last < STALE_RETRY_MS) return false;
    await notes.put(STALE_NOTE, new Response(String(Date.now())));
    return true;
}

/**
 * A page that goes to the machine's link, which the bootstrap answers.
 *
 * Only the page can read which machine that is: the old bootstrap left the
 * identifier in localStorage, which a worker cannot see. With none there, the
 * reload finds the note written and opens the shell as before.
 */
function backThroughTheLink() {
    const html =
        '<!doctype html><meta charset="utf-8"><script>' +
        'var id=null;' +
        "try{id=(JSON.parse(localStorage.getItem('mw-shell-stamp')||'null')||{}).hostId}catch(e){}" +
        "if(typeof id==='string'&&/^[0-9a-z]{26}$/.test(id))location.replace('/'+id);" +
        'else location.reload();' +
        '</script>';
    return new Response(html, {
        status: 200,
        headers: { 'Content-Type': 'text/html; charset=utf-8', 'Cache-Control': 'no-store' },
    });
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
    // They live under /v1/ (the protocol shape they speak — a /v2/ would sit
    // beside them, hence the pattern rather than a list), and the root names
    // stay for the application: it imports /tunnel.js by that name, and the
    // server answers it with the v1 file.
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
        /^\/v\d+\//.test(url.pathname) ||
        url.searchParams.get('mw') === 'pick' ||
        /^\/[0-9a-z]{26}\/?$/.test(url.pathname)
    )
        return;

    event.respondWith(
        (async () => {
            const cache = await caches.open(SHELL_CACHE);

            if (request.mode === 'navigate') {
                const shell = await cache.match('/index.html');
                if (!shell) return await bootstrapPage();
                if (Date.now() - checkedAt > CHECKED_FOR_MS) return await bootstrapPage();
                checkedAt = 0;
                if (!(await shellChecksItself(cache)) && (await timeForStaleRedirect())) {
                    return backThroughTheLink();
                }
                return ENFORCED ? checkedFile(shell, 'html') : shell;
            }

            if (!ENFORCED) {
                // /version.json is how the application notices the host was
                // updated, so it must never come from the copy it is checking.
                if (url.pathname !== '/version.json') {
                    const hit = await cache.match(url.pathname);
                    if (hit) return hit;
                }
                return askThePage(request, event.clientId);
            }

            if (fromTheMachine(url.pathname)) {
                if (RUNS_AS_CODE.has(request.destination))
                    return new Response('not code', { status: 403 });
                return askThePage(request, event.clientId);
            }
            const hit = await cache.match(url.pathname);
            // The web manifest is the one entry no list checks (official.js,
            // WEB_MANIFEST): served as a manifest, never as anything that runs.
            if (
                !hit ||
                (url.pathname === '/manifest.webmanifest' && RUNS_AS_CODE.has(request.destination))
            )
                return new Response('not part of the interface', { status: 404 });
            return checkedFile(hit, extensionOf(url.pathname));
        })(),
    );
});
