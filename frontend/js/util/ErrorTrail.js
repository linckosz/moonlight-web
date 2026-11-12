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
 * This page's errors, for the error reports (backend/src/server/ErrorReports.h).
 *
 * Only what reaches console.error — the uncaught errors and rejections land
 * there too (app.js) — and nothing else: no other level, no whole console. Kept
 * here, bounded, and handed to the page's own host when a stream ends; the host
 * scrubs them with the "Download logs" scrubber and decides whether anything
 * leaves. This page never talks to the project's server.
 *
 * Nothing heavy on the page's thread: a line is turned into text without walking
 * what was logged (never a JSON.stringify, never a loop over an object's keys), only when an error is
 * logged, and the send waits until the page is idle after the stream.
 *
 * Imported first by ClientLog.js, itself app.js's first import, so its wrapper
 * sits under the debug mirror's and survives the mirror being taken off.
 *
 * A guest's page sends only if the box at the bottom of its page is ticked
 * (PlayerJoinView.js registers that answer with setErrorTrailAllowed).
 */

const MAX_ENTRIES = 40;
const MAX_CHARS = 32 * 1024;
const MAX_LINE = 1024;
/** Lines whose first characters match are one line, counted. */
const KEY_CHARS = 160;
const MAX_FRAMES = 6;
/** What an error-like object says about itself; nothing else of it is read. */
const KNOWN_FIELDS = ['name', 'message', 'code', 'type', 'reason', 'status', 'statusText'];
const MAX_VALUE = 200;
/** A beacon's body is capped by the browser (64 KB in Chrome). */
const BEACON_MAX_CHARS = 40 * 1024;

const IS_GUEST = location.pathname.startsWith('/p/');
const ENDPOINT = IS_GUEST ? '/api/share/player/errors' : '/api/errors/client';

/** @type {{t: number, n: number, m: string}[]} */
let entries = [];
let chars = 0;
let dropped = 0;
let scheduled = false;
/** An owner's page sends; a guest's waits for the answer of its box. */
let allowed = IS_GUEST ? () => false : () => true;

const URL_RE = /\b(?:https?|wss?|blob:https?):\/\/[^\s)'"]+/gi;
/** A path's query string or fragment, up to a ":line" or the end of the word. */
const RELATIVE_QUERY_RE = /(\/[^\s?#)'"]*)[?#][^\s:)'"]*/g;
/** The token in a guest's link. */
const GUEST_LINK_RE = /\/p\/[^\s/?#)'"]+/g;

/**
 * "https://host:8443/js/a.js:12:3" → "/js/a.js:12:3": where in the code, not
 * where it was served. Nothing before /js/ survives — not the address, and not
 * the instance's identifier in a stream.{domain}/{id}/ path, nor a query
 * string; a URL that is not one of the page's scripts is only "<url>". A
 * relative path loses its query string too, and a guest's link its token
 * ("/p/{token}/…" → "/p/…/…").
 */
function pathsOnly(s) {
    return s
        .replace(URL_RE, (url) => {
            const plain = url.replace(/[?#][^:\s]*/, '');
            const js = plain.indexOf('/js/');
            // Any other URL may name a host or a session (/api/hosts/{uuid}): gone.
            return js >= 0 ? plain.slice(js) : '<url>';
        })
        .replace(RELATIVE_QUERY_RE, '$1')
        .replace(GUEST_LINK_RE, '/p/…');
}

function cut(s, max) {
    return s.length > max ? s.slice(0, max) + ' […]' : s;
}

function errorText(err) {
    const head = `${err.name || 'Error'}: ${err.message || ''}`;
    const stack = typeof err.stack === 'string' ? err.stack.split('\n') : [];
    // V8 starts the stack with the head again; Firefox and Safari do not.
    const frames = stack.filter((l) => l.trim() && !l.startsWith(head)).slice(0, MAX_FRAMES);
    return [head, ...frames.map((l) => '  ' + l.trim())].join(' | ');
}

/** A short picture of anything logged, in constant time: text, an error, or an object's kind. */
function brief(arg) {
    if (typeof arg === 'string') return cut(arg, MAX_LINE);
    if (arg instanceof Error) return errorText(arg);
    if (arg === null || arg === undefined) return String(arg);
    if (typeof arg !== 'object') {
        return typeof arg === 'function' ? `[function ${arg.name || 'anonymous'}]` : String(arg);
    }
    if (typeof Element !== 'undefined' && arg instanceof Element) {
        return `<${arg.tagName.toLowerCase()}${arg.id ? '#' + arg.id : ''}>`;
    }
    if (typeof Event !== 'undefined' && arg instanceof Event) {
        // An ErrorEvent carries the error itself: that is the useful part.
        const inner = /** @type {any} */ (arg).error;
        return inner instanceof Error ? errorText(inner) : `[${arg.type} event]`;
    }
    // Never a walk over the object: enumerating its keys costs as much as it
    // has (a for…in over 200,000 of them took 600 ms), and what it holds is not
    // the minimum. Its kind, and the few fields an error-like object carries.
    let kind = 'object';
    try {
        if (Array.isArray(arg)) return `[array of ${arg.length}]`;
        kind = (arg.constructor && arg.constructor.name) || 'object';
        const parts = [];
        for (const key of KNOWN_FIELDS) {
            const v = arg[key];
            if (v === undefined || v === null || typeof v === 'object' || typeof v === 'function') {
                continue;
            }
            parts.push(`${key}: ${cut(String(v), MAX_VALUE)}`);
        }
        return parts.length ? `[${kind} ${parts.join(', ')}]` : `[${kind}]`;
    } catch {
        return `[${kind}]`;
    }
}

function record(args) {
    let text = '';
    for (const a of args) {
        text += (text ? ' ' : '') + brief(a);
        if (text.length > MAX_LINE) break;
    }
    text = cut(pathsOnly(text).trim(), MAX_LINE);
    if (!text) return;
    const now = Date.now();
    const key = text.slice(0, KEY_CHARS);
    for (const e of entries) {
        if (e.m.slice(0, KEY_CHARS) === key) {
            e.n++;
            e.t = now;
            return;
        }
    }
    entries.push({ t: now, n: 1, m: text });
    chars += text.length;
    while (entries.length > MAX_ENTRIES || chars > MAX_CHARS) {
        const old = entries.shift();
        if (!old) break;
        chars -= old.m.length;
        dropped++;
    }
}

const originalError = console.error;
console.error = function (...args) {
    originalError.apply(console, args);
    try {
        record(args);
    } catch {
        // Never let the trail break the line it was keeping.
    }
};

/** For a guest's page: whether its box says yes, asked at each send. */
export function setErrorTrailAllowed(fn) {
    allowed = typeof fn === 'function' ? fn : () => false;
}

/** Whether errors wait to be handed over. */
export function hasErrorTrail() {
    return entries.length > 0 || dropped > 0;
}

function take(maxChars = Infinity) {
    let start = entries.length;
    let size = 0;
    while (start > 0 && size + entries[start - 1].m.length <= maxChars) {
        start--;
        size += entries[start].m.length;
    }
    const batch = {
        sent: Date.now(),
        dropped: dropped + start,
        lines: entries.slice(start).map((e) => ({ t: e.t, n: e.n, m: e.m })),
    };
    entries = [];
    chars = 0;
    dropped = 0;
    return batch;
}

function allowedNow() {
    try {
        return allowed() === true;
    } catch {
        return false;
    }
}

/**
 * Hand what waits to the host, now. With `beacon`, as the page goes away.
 * @param {{beacon?: boolean}} [opts]
 */
export function sendErrorTrail({ beacon = false } = {}) {
    if (entries.length === 0 || !allowedNow()) return;
    if (beacon) {
        if (!navigator.sendBeacon) return;
        const batch = take(BEACON_MAX_CHARS);
        navigator.sendBeacon(
            ENDPOINT,
            new Blob([JSON.stringify(batch)], { type: 'application/json' }),
        );
        return;
    }
    const batch = take();
    // One try: the host reports what it has; a line lost here is not worth a retry.
    fetch(ENDPOINT, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(batch),
    }).catch(() => {});
}

/** A stream ended or failed: hand the errors over once the page is idle. */
export function scheduleErrorTrail() {
    if (scheduled || entries.length === 0) return;
    scheduled = true;
    const run = () => {
        scheduled = false;
        sendErrorTrail();
    };
    if (typeof requestIdleCallback === 'function') requestIdleCallback(run, { timeout: 5000 });
    else setTimeout(run, 1000);
}

addEventListener('pagehide', () => sendErrorTrail({ beacon: true }));

/** Tests only. */
export const _forTests = {
    reset() {
        entries = [];
        chars = 0;
        dropped = 0;
        scheduled = false;
        allowed = IS_GUEST ? () => false : () => true;
    },
    entries: () => entries,
    dropped: () => dropped,
    brief,
};
