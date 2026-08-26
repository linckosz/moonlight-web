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

import { Tunnel, hostIdFromLocation, rememberLastHost, SHELL_CACHE } from './tunnel.js';

const ui = {
    stage: document.getElementById('stage'),
    detail: document.getElementById('detail'),
    bar: document.getElementById('bar'),
    note: document.getElementById('note'),
};

function say(stage, detail) {
    if (ui.stage) ui.stage.textContent = stage;
    if (ui.detail) ui.detail.textContent = detail || '';
}

/** null = no measurable progress yet; the bar sweeps instead of sitting at 0. */
function progress(fraction) {
    if (!ui.bar) return;
    if (fraction === null) {
        ui.bar.setAttribute('data-indeterminate', '');
        ui.bar.style.width = '';
        return;
    }
    ui.bar.removeAttribute('data-indeterminate');
    ui.bar.style.width = `${Math.round(fraction * 100)}%`;
}

function fail(message, hint) {
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
    progress(null);

    const manifestResponse = await tunnel.fetch('/api/app/manifest');
    if (!manifestResponse.ok) throw new Error('your machine did not describe its interface');
    const manifest = await manifestResponse.json();

    const files = Array.isArray(manifest.files) ? manifest.files : [];
    if (files.length === 0) throw new Error('your machine reported no interface files');

    const stamp = cachedStamp();
    if (stamp && stamp.hostId === hostId && stamp.version === manifest.version) {
        // Trust the stamp only as far as the cache backs it up: a browser may
        // evict a cache without telling anyone, and a stamp pointing at nothing
        // would hand the service worker an empty shelf.
        const cache = await caches.open(SHELL_CACHE);
        if (await cache.match('/index.html')) return manifest.version;
    }

    say('Fetching the interface…', 'from your machine, not from here');
    progress(0);

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
            progress(done / files.length);
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
    tunnel.onstatus = (stage) => say(STAGE_WORDS[stage] || stage, '');

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
    progress(1);
    try {
        await navigator.serviceWorker.register('/sw.js', { scope: '/' });
        await navigator.serviceWorker.ready;
    } catch (e) {
        fail(`This browser refused to install the application: ${e.message}.`, '');
        return;
    }

    // Remembered so that stream.{domain} on its own — no identifier — opens the
    // machine this browser last used, rather than an application with nothing
    // behind it.
    rememberLastHost(hostId);

    // The identifier moves from the path to the fragment. The application uses
    // ordinary absolute paths for its own routes and its own assets, and a
    // prefix in the path would break every one of them; a fragment is invisible
    // to all of it and still survives a reload.
    location.replace(`/#${hostId}`);
}

main().catch((e) => fail(e.message, ''));
