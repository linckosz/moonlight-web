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
 * This page's console, sent to the server's client log while the server is in
 * debug mode (--debug, or the box in Admin → Advanced) — so the browser side
 * of a problem ends up in the same "Download logs" archive as the host side,
 * without anyone opening the developer tools.
 *
 * The FIRST import of app.js: modules print while they load, and those lines
 * must be caught too. Until /api/health has answered, lines are kept in a
 * bounded buffer; when the server says debug is off, the console is given
 * back untouched and the buffer dropped.
 *
 * Sent every few seconds, when a stream ends, and as the page goes away (a
 * beacon), so a tab that crashes or is closed still leaves what it printed.
 * Only once the page can be heard: an owner's page once it has a session (a
 * refused request would count against it as a failed login), a guest's once
 * it holds its slot's cookie. The web workers' consoles are their own and are
 * not caught here.
 *
 * Also where pages learn the server's verbose mode (serverDiag.verbose).
 *
 * Through the rendezvous, every request rides the page's tunnel, which is not
 * up yet when this module loads: the first /api/health has no answer, and the
 * lines wait (bounded) for the one app.js asks once the tunnel is (14/10/2026:
 * a Mac over the Internet, debug on, and not one line nor a `[perf]` reached
 * the host). A page reached directly that gets no answer stops as before.
 */

const LEVELS = /** @type {const} */ (['log', 'info', 'warn', 'error', 'debug']);
/** Bounds of what waits to be sent: the oldest lines go first. */
const MAX_LINES = 3000;
const MAX_CHARS = 512 * 1024;
/** One line's cap; a dumped object can be huge. */
const MAX_LINE_CHARS = 8 * 1024;
const FLUSH_MS = 5000;
/** A beacon's body is capped by the browser (64 KB in Chrome). */
const BEACON_MAX_CHARS = 60 * 1024;

const IS_GUEST = location.pathname.startsWith('/p/');
const ENDPOINT = IS_GUEST ? '/api/share/player/log' : '/api/logs/client';

/** What the server said at the last /api/health. */
export const serverDiag = { debug: false, verbose: false };

/** @type {'probing' | 'on' | 'off'} */
let state = 'probing';
/** @type {{t: number, l: string, m: string}[]} */
let buffer = [];
let chars = 0;
let dropped = 0;
let seq = 0;
let authed = IS_GUEST; // a guest's cookie is checked by the route itself
let timer = 0;
let sending = false;
/** refreshClientLog calls made so far: only the latest one's answer counts. */
let probes = 0;
const pageId = Math.random().toString(36).slice(2, 10);

/** @type {Record<string, (...args: any[]) => void>} */
const original = {};

function stringify(arg) {
    if (typeof arg === 'string') return arg;
    if (arg instanceof Error) return arg.stack || String(arg);
    if (arg === undefined) return 'undefined';
    if (typeof arg === 'function') return `[function ${arg.name || 'anonymous'}]`;
    if (typeof Element !== 'undefined' && arg instanceof Element) {
        return `<${arg.tagName.toLowerCase()}${arg.id ? '#' + arg.id : ''}>`;
    }
    if (typeof Event !== 'undefined' && arg instanceof Event) return `[${arg.type} event]`;
    try {
        const seen = new WeakSet();
        return JSON.stringify(arg, (_k, v) => {
            if (typeof v === 'bigint') return String(v);
            if (v && typeof v === 'object') {
                if (seen.has(v)) return '[circular]';
                seen.add(v);
            }
            return v;
        });
    } catch {
        return String(arg);
    }
}

function capture(level, args) {
    let text = args.map(stringify).join(' ');
    if (text.length > MAX_LINE_CHARS) text = text.slice(0, MAX_LINE_CHARS) + ' […]';
    buffer.push({ t: Date.now(), l: level, m: text });
    chars += text.length;
    while (buffer.length > MAX_LINES || chars > MAX_CHARS) {
        const old = buffer.shift();
        if (!old) break;
        chars -= old.m.length;
        dropped++;
    }
}

function wrap() {
    for (const level of LEVELS) {
        if (original[level]) continue;
        const fn = console[level];
        original[level] = fn;
        console[level] = (...args) => {
            fn.apply(console, args);
            try {
                capture(level, args);
            } catch {
                // Never let the capture break the line it was catching.
            }
        };
    }
}

function unwrap() {
    for (const level of LEVELS) {
        if (!original[level]) continue;
        console[level] = original[level];
        delete original[level];
    }
}

/** The next batch, taken out of the buffer, or null when there is nothing. */
function takeBatch(maxChars = Infinity) {
    if (buffer.length === 0 && dropped === 0) return null;
    // From the end when capped: the last lines are the ones that explain a
    // page going away.
    let start = buffer.length;
    let size = 0;
    while (start > 0 && size + buffer[start - 1].m.length <= maxChars) {
        start--;
        size += buffer[start].m.length;
    }
    const skip = start; // the older lines that did not fit
    const lines = buffer.slice(start);
    const batch = {
        id: pageId,
        page: location.pathname,
        ua: navigator.userAgent,
        seq: seq++,
        dropped: dropped + skip,
        lines,
    };
    buffer = [];
    chars = 0;
    dropped = 0;
    return batch;
}

/** Put a batch back at the front after a refused send. */
function giveBack(batch) {
    seq = Math.max(0, seq - 1);
    dropped += batch.dropped;
    buffer = batch.lines.concat(buffer);
    chars = buffer.reduce((n, l) => n + l.m.length, 0);
}

async function checkAuth() {
    if (authed) return true;
    try {
        const r = await fetch('/api/auth/status', { cache: 'no-store' });
        const s = r.ok ? await r.json() : {};
        authed = s.authenticated === true || s.is_localhost === true;
    } catch {
        authed = false;
    }
    return authed;
}

/**
 * Send what waits now. With `beacon`, as the page goes away: one beacon,
 * capped, the newest lines kept.
 * @param {{beacon?: boolean}} [opts]
 */
export async function flushClientLog({ beacon = false } = {}) {
    if (state !== 'on' || !authed) return;
    if (beacon) {
        const batch = takeBatch(BEACON_MAX_CHARS);
        if (!batch || !navigator.sendBeacon) return;
        navigator.sendBeacon(
            ENDPOINT,
            new Blob([JSON.stringify(batch)], { type: 'application/json' }),
        );
        return;
    }
    if (sending) return;
    const batch = takeBatch();
    if (!batch) return;
    sending = true;
    try {
        const r = await fetch(ENDPOINT, {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify(batch),
        });
        if (r.status === 404) {
            // Debug mode was switched off meanwhile.
            stop();
        } else if (!r.ok) {
            // A guest before it holds its slot, a session gone: keep the lines
            // for the next try, and ask again whether we can be heard.
            giveBack(batch);
            if (!IS_GUEST) authed = false;
        }
    } catch {
        giveBack(batch);
    } finally {
        sending = false;
    }
}

/** This document was served by the bootstrap's worker, i.e. down a tunnel
 *  (net/tunnelBridge.js pageCameThroughTunnel, which this first import cannot
 *  load without the modules it would then miss the lines of). */
function cameThroughTunnel() {
    return !!(navigator.serviceWorker && navigator.serviceWorker.controller);
}

function stop() {
    state = 'off';
    unwrap();
    buffer = [];
    chars = 0;
    dropped = 0;
    if (timer) clearInterval(timer);
    timer = 0;
}

async function tick() {
    if (state !== 'on') return;
    if (!authed && !(await checkAuth())) return;
    await flushClientLog();
}

/**
 * Ask the server for its modes and start or stop accordingly. Called once as
 * the page loads, once the rendezvous tunnel is up (app.js), and again when
 * the admin page switches debug mode.
 */
export async function refreshClientLog() {
    const probe = ++probes;
    let health = null;
    try {
        const r = await fetch('/api/health', { cache: 'no-store' });
        if (r.ok) health = await r.json();
    } catch {
        // No answer: the next call decides.
    }
    // A later call was made meanwhile: its answer is the newer one.
    if (probe !== probes) return;
    // No answer on a page the tunnel serves: the tunnel is still being made.
    // Keep listening; app.js asks again once it is up.
    if (!health && state === 'probing' && cameThroughTunnel()) return;
    // No answer is not "debug off": a mirror the server switched on stays on.
    // The first probe through a tunnel can be held by the service worker
    // until it gives up (a 504, tens of seconds on), long after the call made
    // once the tunnel was up has switched the mirror on.
    if (!health && state === 'on') return;
    serverDiag.debug = !!(health && health.debug === true);
    serverDiag.verbose = !!(health && health.verbose === true);
    if (!serverDiag.debug) {
        stop();
        return;
    }
    if (state !== 'on') {
        state = 'on';
        wrap();
        timer = setInterval(() => void tick(), FLUSH_MS);
        // What this browser can do, for the same archive (`[Caps]`, once per
        // page). Loaded only here: a page out of debug mode never fetches it.
        import('./BrowserCaps.js').then((m) => m.logBrowserCaps()).catch(() => {});
    }
    void tick();
}

wrap();
addEventListener('pagehide', () => void flushClientLog({ beacon: true }));
void refreshClientLog();
