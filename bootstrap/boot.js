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
    knownInstances,
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
    instances: document.getElementById('instance-menu'),
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
/* How long a handover reload stays on the record, so that another one counts as
   a loop rather than as a person refreshing the page.

   Time alone cannot tell those apart — a loop here costs a whole pass through
   this file, connection included, exactly like a deliberate refresh does. What
   separates them is that the application DELETES this note when it starts (see
   SecuringOverlay's HANDOVER_KEY, which is cleared the same way and for the same
   reason). So a note that is still here means the reload did not reach the
   application, and this window is only a backstop for a tab that never got
   there and was left sitting. */
const RELOAD_GUARD_MS = 60000;

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
    // Every path into here ends on a page the person is left looking at, and on
    // most of them the machine in the address is exactly the one that cannot
    // help them. This is the moment the caption becomes a way out.
    paintInstances(true);
}

/* ── The machine, and the way to another one ───────────────────────────────
 *
 * This page is the one that says "your machine did not answer", and until now
 * that was where it stopped. It is a bad place to stop: the person reading it
 * very likely has two or three machines, the other ones are switched on, and
 * nothing on screen offered a way to any of them. So the register the
 * application keeps — every machine this browser has actually reached, by name —
 * is read here and offered beside the product name.
 *
 * It has two modes, and which one is showing says something true about the page.
 *
 *   READING, while the connection is being made. A caption, nothing more: no
 *      arrow, no hover, nothing that invites a click. Leaving here is not on
 *      offer yet because arriving has not failed yet, and a control that says
 *      "go somewhere else" over a progress bar reads as "this is not working" a
 *      second and a half before anyone knows whether it is. The label is the
 *      machine the address names, and NOTHING AT ALL when this browser has
 *      never met it — an empty caption is the honest one, since at this point
 *      the identifier is a perfectly good address that simply has not answered
 *      yet. Saying "No server" over a working connection is a lie with a timer
 *      on it.
 *
 *   CHOOSING, once it has failed. Now leaving is the only useful thing left, so
 *      the caption becomes a menu — provided there is somewhere to go, which is
 *      exactly `others.length` and covers both shapes of the rule: a named
 *      machine needs a second one to be worth a menu, an unknown identifier
 *      needs only one. "No server" belongs here and only here: on a page that
 *      has already said it could not connect, it is the honest answer to a link
 *      mistyped, revoked, or belonging to somebody else.
 *
 * Read-only, always: nothing here writes to that register. Only the application
 * can, because only the application ever gets close enough to a machine to be
 * told what it calls itself.
 */

/** Neither name nor identifier is ours; both are drawn as text, never as HTML. */
function setText(el, value) {
    el.textContent = value;
    return el;
}

function makeIcon(paths) {
    const ns = 'http://www.w3.org/2000/svg';
    const svg = document.createElementNS(ns, 'svg');
    svg.setAttribute('viewBox', '0 0 24 24');
    svg.setAttribute('fill', 'none');
    svg.setAttribute('stroke', 'currentColor');
    svg.setAttribute('stroke-width', '2');
    svg.setAttribute('stroke-linecap', 'round');
    svg.setAttribute('stroke-linejoin', 'round');
    svg.setAttribute('aria-hidden', 'true');
    for (const d of paths) {
        const path = document.createElementNS(ns, 'path');
        path.setAttribute('d', d);
        svg.appendChild(path);
    }
    return svg;
}

const ICON_MONITOR = () =>
    makeIcon([
        'M4 3h16a2 2 0 0 1 2 2v10a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2Z',
        'M8 21h8',
        'M12 17v4',
    ]);
const ICON_CARET = () => makeIcon(['M6 9l6 6 6-6']);

let closeMenu = null;
/* The machine this page is for, so that a repaint after the outcome is known
   does not have to be handed it again from wherever the failure happened. */
let paintedHostId = null;

/**
 * @param {boolean} choosing  false while connecting (a caption), true once that
 *                            has failed (a menu, if there is anywhere to go).
 */
function paintInstances(choosing) {
    const mount = ui.instances;
    if (!mount) return;

    const instances = knownInstances();
    const current = paintedHostId ? instances.find((e) => e.id === paintedHostId) : null;
    const others = instances.filter((e) => e.id !== paintedHostId);

    if (closeMenu) closeMenu();
    closeMenu = null;
    mount.replaceChildren();

    if (!choosing || others.length === 0) {
        // An identifier this browser has never reached names nothing it can
        // show — and only a page that has given up may say so out loud.
        const label = current ? current.name : choosing ? 'No server' : '';
        if (!label) {
            mount.hidden = true;
            return;
        }
        mount.hidden = false;
        mount.className = 'instance-menu';
        const span = setText(document.createElement('span'), label);
        span.className = 'instance-name';
        span.title = label;
        mount.appendChild(span);
        return;
    }

    const label = current ? current.name : 'No server';
    mount.hidden = false;

    mount.className = 'instance-menu has-menu';

    const btn = document.createElement('button');
    btn.type = 'button';
    btn.className = 'instance-btn';
    btn.setAttribute('aria-haspopup', 'menu');
    btn.setAttribute('aria-expanded', 'false');
    btn.title = 'Switch to another machine';
    const name = setText(document.createElement('span'), label);
    name.className = 'instance-name';
    btn.appendChild(name);
    const caret = document.createElement('span');
    caret.className = 'instance-caret';
    caret.appendChild(ICON_CARET());
    btn.appendChild(caret);

    const panel = document.createElement('div');
    panel.className = 'instance-panel';
    panel.setAttribute('role', 'menu');
    panel.hidden = true;
    const title = setText(document.createElement('p'), 'Your machines');
    title.className = 'instance-panel-title';
    panel.appendChild(title);

    for (const entry of others) {
        const item = document.createElement('button');
        item.type = 'button';
        item.className = 'instance-item';
        item.setAttribute('role', 'menuitem');
        const icon = document.createElement('span');
        icon.className = 'instance-item-icon';
        icon.appendChild(ICON_MONITOR());
        item.appendChild(icon);
        const label2 = setText(document.createElement('span'), entry.name);
        label2.className = 'instance-item-name';
        item.appendChild(label2);
        // The identifier is what the address is built from, not the stored URL:
        // that URL was written by another page and this one is about to
        // navigate to it. Same origin, same shape, nothing to trust.
        item.addEventListener('click', () => {
            location.href = `/${entry.id}`;
        });
        panel.appendChild(item);
    }

    mount.appendChild(btn);
    mount.appendChild(panel);

    const onDocClick = (e) => {
        if (!mount.contains(e.target)) closeMenu();
    };
    const onKeyDown = (e) => {
        if (e.key === 'Escape') {
            closeMenu();
            btn.focus();
        }
    };

    closeMenu = () => {
        document.removeEventListener('click', onDocClick);
        document.removeEventListener('keydown', onKeyDown);
        panel.hidden = true;
        btn.setAttribute('aria-expanded', 'false');
        mount.classList.remove('is-open');
    };

    btn.addEventListener('click', (e) => {
        e.stopPropagation();
        if (!panel.hidden) {
            closeMenu();
            return;
        }
        panel.hidden = false;
        btn.setAttribute('aria-expanded', 'true');
        mount.classList.add('is-open');
        document.addEventListener('click', onDocClick);
        document.addEventListener('keydown', onKeyDown);
    });
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

    // The caption first — the name of the machine being called, when this
    // browser knows it. fail() is what turns it into a way out; nothing else
    // has to, because every ending that is not a failure is a handover.
    paintedHostId = hostId;
    paintInstances(false);

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
        // The failure names its own explanation when it has a better one than
        // "your machine is off" — a connection that was refused by the network
        // rather than never answered has nothing to do with the machine.
        fail(
            e.message,
            e.hint ||
                'If your machine is switched off or has no internet access, nothing here ' +
                    'can reach it. That is by design, not a fault.',
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
    const target =
        `${landing}#${hostId}` +
        (key ? `&k=${encodeURIComponent(key)}` : '') +
        (share ? `&t=${encodeURIComponent(share)}` : '');

    // Handing over to an address that differs from this one only by its
    // fragment — or not at all — is not a navigation. It is a jump within the
    // page: nothing loads, and this one would sit on a finished seal forever.
    //
    // That is the ordinary case after a hard refresh, and it is the whole
    // reason this branch exists. A hard refresh is precisely the reload that
    // bypasses the service worker, so it lands here rather than on the
    // application — and the address it lands on is the application's own, which
    // this page is trying to hand over to.
    //
    // Note what is compared, because comparing the whole address is the trap:
    // the application does not keep the identifier in the address bar. Its
    // router rewrites the URL to a bare "/" whenever it returns to the host list
    // (history.replaceState in app.js), fragment and all. So after any use of
    // the application the address is "/" while the target is "/#<id>", which
    // differ — and yet replace() still loads nothing, because a fragment is all
    // that separates them. Everything up to the fragment is what decides
    // whether a document is fetched.
    const targetUrl = new URL(target, location.href);
    const sameDocument =
        targetUrl.origin === location.origin &&
        targetUrl.pathname === location.pathname &&
        targetUrl.search === location.search;

    if (sameDocument) {
        // Guarded, because a reload that landed back here would do it all
        // again. The note is removed by the application the moment it starts,
        // so finding one still here means the last reload never reached it —
        // looping would only flash the seal forever. Say so and stop.
        let previous = 0;
        try {
            previous = Number(sessionStorage.getItem('mw-handover-reload') || 0);
        } catch {
            /* no session storage; only the guard is lost */
        }
        if (previous && Date.now() - previous < RELOAD_GUARD_MS) {
            fail(
                'The application is installed but did not start.',
                'Close this tab and open your machine’s link again.',
            );
            return;
        }
        try {
            sessionStorage.setItem('mw-handover-reload', String(Date.now()));
        } catch {
            /* nothing kept; the reload below still happens */
        }

        // Put the fragment in place before reloading, since the reload is what
        // actually loads and it has to load the right address. replaceState
        // rather than replace(): it rewrites the URL without a history entry
        // and without a fragment navigation on the way out.
        if (targetUrl.hash !== location.hash) {
            try {
                history.replaceState(history.state, '', target);
            } catch {
                /* the fragment stays as it was; the identifier is in session
                   storage either way, which is where the application looks
                   when the address carries none */
            }
        }

        location.reload();
        return;
    }

    location.replace(target);
}

main().catch((e) => fail(e.message, ''));
