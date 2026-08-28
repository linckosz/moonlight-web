/*
 * MoonlightWeb — bootstrap. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * The whole of what the introduction server ever runs in anyone's browser.
 *
 * It opens a connection to the machine named in the address, pulls the
 * application down through it, hands it to a service worker, and gets out of the
 * way. After that this file is not involved in anything: the application runs
 * from the user's own machine, over a connection this page checked the identity
 * of before creating any DTLS state.
 *
 * It is small on purpose, and it is published on purpose. It is the one piece of
 * the author's code a browser loads from a server, so the honest mitigation is
 * not "trust us" — it is that these bytes are few enough to read, identical for
 * everyone, and published with their digests so what is served can be compared
 * against what was released.
 *
 * What that does NOT cover, stated plainly: a compromised server can serve
 * different bytes to a chosen victim and the good ones to whoever is checking.
 * The comparison catches the untargeted case, which is nearly all of them. It is
 * not a guarantee and must never be described as one.
 */

import {
    Tunnel,
    hostIdFromLocation,
    hostKeyFromLocation,
    landingPathFromLocation,
    shareTokenFromLocation,
    rememberLastHost,
    SHELL_CACHE,
} from './tunnel.js';

const ui = {
    stage: document.getElementById('stage'),
    detail: document.getElementById('detail'),
    bar: document.getElementById('bar'),
    status: document.getElementById('status'),
    note: document.getElementById('note'),
};

function say(stage, detail) {
    // The heading is the failure voice and is hidden while this works; the line
    // under the bar carries the same words in the meantime.
    if (ui.stage) ui.stage.textContent = stage;
    if (ui.detail) ui.detail.textContent = detail || stage || '';
}

/* ── The meter ────────────────────────────────────────────────────────────
 *
 * What the bar shows is not what the work has done. It is
 *
 *     0.7 × (work actually done)  +  0.3 × (min(elapsed, 1.2s) / 1.2s)
 *
 * and it never goes backwards. The time term is there for one reason: a bar
 * that only tracks work sits frozen through every phase that has no percentage
 * to report — opening the connection, checking the machine's identity — and a
 * frozen bar reads as a hang. This way it always moves, and it still cannot
 * reach the end before the work does, because the work owns the larger share.
 *
 * Then the page stays for a moment longer than it strictly needs to: 300 ms at
 * a full bar so the completion is actually seen, and 1.6 s on screen in total.
 * That is a deliberate cost — a warm start could hand over sooner — paid so the
 * one moment where this page can say what it is doing is long enough to read.
 * Keep it honest: the seal closes when the connection is genuinely established
 * and the interface genuinely came down it, never on a timer alone.
 */
const WORK_WEIGHT = 0.7;
const TIME_WEIGHT = 0.3;
const TIME_SPAN_MS = 1200;
/* The bar eases into its new value rather than jumping to it, so it arrives this
   long after the number does. Waited out before the hold, or the moment at a
   full bar would be spent watching the bar still travelling. */
const BAR_EASE_MS = 260;
const HOLD_FULL_MS = 300;
const MIN_VISIBLE_MS = 1600;

const startedAt = performance.now();
let work = 0; // real progress, 0…1
let shown = 0; // what the bar displays, monotonic
let secured = false;
let ticking = true;

/** Milestone reached. Monotonic: a later, smaller number is ignored. */
function progress(fraction) {
    work = Math.max(work, Math.min(1, fraction));
}

function markSecured() {
    secured = true;
    document.body.dataset.state = 'secured';
    if (ui.status) ui.status.textContent = 'Secured';
}

/** Compute and paint one frame. Idempotent, so anything may call it. */
function render() {
    if (!ticking) return;
    const elapsed = performance.now() - startedAt;
    const target = WORK_WEIGHT * work + TIME_WEIGHT * Math.min(elapsed / TIME_SPAN_MS, 1);
    shown = Math.max(shown, target);
    if (ui.bar) ui.bar.style.width = `${(shown * 100).toFixed(1)}%`;
    if (shown >= 0.999 && !secured) markSecured();
}

/*
 * Two clocks, and the second one is not a nicety.
 *
 * requestAnimationFrame is the smooth one, and it does not run AT ALL in a
 * hidden tab. Since the handover waits for this bar to finish, a link opened in
 * a background tab — or a session restored at startup, where every tab but one
 * begins hidden — would sit here forever with the bar frozen at zero and never
 * start the application. The page would look like it had crashed the moment it
 * was finally looked at.
 *
 * So a timer runs the same frame as well. Browsers throttle it to about once a
 * second while hidden, which nobody sees, and it is enough to carry the
 * sequence to the end. Whoever changes the pacing here must keep something
 * ticking that a hidden tab still gets.
 */
function raf() {
    if (!ticking) return;
    render();
    requestAnimationFrame(raf);
}
requestAnimationFrame(raf);
const heartbeat = setInterval(render, 250);

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

/** Wait for the bar to actually reach the end, then let it be seen there. */
async function settle() {
    progress(1);
    // Polled on a timer, for the reason above: awaiting a frame here would be
    // awaiting one that a hidden tab never delivers.
    while (!secured) await sleep(50);
    await sleep(BAR_EASE_MS + HOLD_FULL_MS);
    const remaining = MIN_VISIBLE_MS - (performance.now() - startedAt);
    if (remaining > 0) await sleep(remaining);
    clearInterval(heartbeat);
    ticking = false;
}

function fail(message, hint) {
    ticking = false;
    clearInterval(heartbeat);
    document.body.dataset.state = 'failed';
    say('Not connected', message);
    if (ui.note) ui.note.textContent = hint || '';
}

const STAGE_WORDS = {
    identity: 'Preparing this browser…',
    calling: 'Looking for your machine…',
    binding: 'Checking it is really yours…',
    connecting: 'Opening a direct connection…',
    ready: 'Connected.',
};

// Where each stage sits on the bar. Guesses, but ordered ones: the share left
// to the download is the part with a real denominator, and it is the largest.
const STAGE_PROGRESS = {
    identity: 0.06,
    calling: 0.12,
    binding: 0.22,
    connecting: 0.3,
    ready: 0.38,
};
const MANIFEST_AT = 0.45;
const SHELL_AT = 0.92;

/**
 * What the cache currently holds, if anything usable.
 *
 * Two things have to match, and both for the same reason — the cache is a copy
 * of ONE machine's interface at ONE version:
 *
 *   the host   two machines are reached through the same origin here, so an
 *              unstamped cache would serve the first machine's application for
 *              the second. Same product, possibly a different build.
 *   the version  an update on the host has to reach the browser.
 *
 * Stored in localStorage rather than in the cache itself because it must be
 * readable before deciding whether to open the cache at all.
 */
function cachedStamp() {
    try {
        return JSON.parse(localStorage.getItem('mw-shell-stamp') || 'null');
    } catch {
        return null;
    }
}

function writeStamp(hostId, version) {
    try {
        localStorage.setItem('mw-shell-stamp', JSON.stringify({ hostId, version }));
    } catch {
        /* the application still runs; only the next start pays for it again */
    }
}

/**
 * Pull the application down and put it where the service worker will find it.
 *
 * Skipped entirely when the cache already holds this machine's interface at this
 * version — which is the ordinary case, and the difference between an app that
 * opens and one that downloads itself every time.
 *
 * The file list comes from the host rather than from anything published here.
 * Guessing it by reading index.html would miss every module a script imports at
 * runtime, and hard-coding it here would make this page need a release every
 * time the application gained a file.
 */
async function fetchShell(tunnel, hostId) {
    say('Checking for updates…', '');

    const manifestResponse = await tunnel.fetch('/api/app/manifest');
    if (!manifestResponse.ok) throw new Error('your machine did not describe its interface');
    const manifest = await manifestResponse.json();

    progress(MANIFEST_AT);

    const files = Array.isArray(manifest.files) ? manifest.files : [];
    if (files.length === 0) throw new Error('your machine reported no interface files');

    const stamp = cachedStamp();
    if (stamp && stamp.hostId === hostId && stamp.version === manifest.version) {
        // Trust the stamp only as far as the cache backs it up: a browser may
        // evict a cache without telling anyone, and a stamp pointing at nothing
        // would hand the service worker an empty shelf.
        const cache = await caches.open(SHELL_CACHE);
        if (await cache.match('/index.html')) {
            progress(SHELL_AT);
            return manifest.version;
        }
    }

    say('Fetching the interface…', 'from your machine, not from here');

    // Start from nothing rather than merging: a file an update removed would
    // otherwise be served forever out of a cache nobody prunes, and a cache
    // half-belonging to another machine is worse still.
    await caches.delete(SHELL_CACHE);
    const fresh = await caches.open(SHELL_CACHE);

    let done = 0;
    // A handful at a time. One at a time is needlessly slow over a channel with
    // a round trip; all at once would put the whole application in the host's
    // send queue at the same moment.
    const queue = [...files];
    const worker = async () => {
        for (;;) {
            const path = queue.shift();
            if (!path) return;
            const response = await tunnel.fetch(path);
            if (response.ok) await fresh.put(path, response);
            done++;
            progress(MANIFEST_AT + (SHELL_AT - MANIFEST_AT) * (done / files.length));
            say('Fetching the interface…', `${done} of ${files.length} files`);
        }
    };
    await Promise.all([worker(), worker(), worker(), worker()]);

    // The navigation the service worker answers is this one, so it has to be
    // there under the name the worker looks for.
    const index = await fresh.match('/index.html');
    if (!index) throw new Error('your machine did not send its main page');
    await fresh.put('/', index.clone());

    writeStamp(hostId, manifest.version);
    return manifest.version;
}

async function main() {
    const hostId = hostIdFromLocation();
    if (!hostId) {
        fail(
            'This address does not name a machine.',
            'A MoonlightWeb link ends in a 26-character identifier. Your machine shows ' +
                'its own on its settings page and in its tray menu.',
        );
        return;
    }

    if (!('serviceWorker' in navigator)) {
        fail(
            'This browser cannot run the application this way.',
            'Service workers are unavailable — often because the page is in a private ' +
                'window, or because they are disabled in the browser’s settings.',
        );
        return;
    }

    const tunnel = new Tunnel(hostId);
    tunnel.onstatus = (stage) => {
        say(STAGE_WORDS[stage] || stage, '');
        if (STAGE_PROGRESS[stage]) progress(STAGE_PROGRESS[stage]);
    };

    try {
        await tunnel.connect();
    } catch (e) {
        fail(
            e.message,
            'If your machine is switched off or has no internet access, nothing here ' +
                'can reach it — that is by design, not a fault.',
        );
        return;
    }

    if (tunnel.firstContact) {
        // Said once, and said honestly: this is the connection the pinning
        // cannot protect, so the only defence available is the user's own eyes.
        console.info(
            '[MW] First connection to this machine — its identity key has been remembered. ' +
                'You can compare it with the fingerprint shown on the machine itself.',
        );
    }

    try {
        await fetchShell(tunnel, hostId);
    } catch (e) {
        fail(`The interface did not come through: ${e.message}.`, '');
        return;
    }

    say('Starting…', '');
    try {
        await navigator.serviceWorker.register('/sw.js', { scope: '/' });
        await navigator.serviceWorker.ready;
    } catch (e) {
        fail(`This browser refused to install the application: ${e.message}.`, '');
        return;
    }

    // Everything real is done. The rest is the bar catching up, its moment at
    // the end, and the floor on how briefly this page may exist.
    await settle();

    // Remembered so that stream.{domain} on its own — no identifier — opens the
    // machine this browser last used, rather than an application with nothing
    // behind it.
    rememberLastHost(hostId);

    // The application draws this same seal on every start, because every later
    // start makes the same connection with nothing to download. Leave it a note
    // saying the seal has just been shown, or the first visit to a machine
    // would show it twice in a row. The application reads this once and deletes
    // it, so only this handover is covered — a refresh a second later shows the
    // seal, which is the point of it being there at all.
    try {
        sessionStorage.setItem('mw-seal', String(Date.now()));
    } catch {
        /* worst case the seal is shown twice; nothing breaks */
    }

    // The identifier moves from the path to the fragment. The application uses
    // ordinary absolute paths for its own routes and its own assets, and a
    // prefix in the path would break every one of them; a fragment is invisible
    // to all of it and still survives a reload.
    //
    // A host key rides along when the link carried one — it arrived in the
    // fragment and it stays there, because this page hands over by navigating
    // and a query would put it in the introduction server's next request line.
    // The application spends it and strips it; nothing here reads it.
    //
    // An invitation token rides along the same way and for the same reason, with
    // one difference: it is not spent. It is what the guest's page IS, so it
    // stays in the fragment and survives every reload of that page.
    //
    // The path, freed by that same move, is where the link says which page it
    // meant. The machine's own tray asks for /admin outright; an invitation does
    // not have to ask, because a token is only ever for the guest page and
    // landingPathFromLocation() reads that off it. Everything else asks for
    // nothing and lands at the root, which is what every link did before this.
    const key = hostKeyFromLocation();
    const share = shareTokenFromLocation();
    const landing = landingPathFromLocation() || '/';
    location.replace(
        `${landing}#${hostId}` +
            (key ? `&k=${encodeURIComponent(key)}` : '') +
            (share ? `&t=${encodeURIComponent(share)}` : ''),
    );
}

main().catch((e) => fail(e.message, ''));
