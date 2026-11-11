/*
 * MoonlightWeb — bootstrap. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * The connection to the user's own machine.
 *
 * What it does
 * ------------
 * Opens one WebSocket to the introduction server, exchanges WebRTC signalling
 * over it under MW-BIND-v1, and ends up with a single data channel straight to
 * the host. From that point:
 *
 *   fetch(path, init)   an HTTP request and its answer, framed over the channel
 *   openSocket(path)    a WebSocket-shaped object for the streaming signalling
 *
 * The introduction server drops out of the path as soon as ICE completes.
 * Nothing else ever travels over it, and everything it did carry was signed with
 * a key it has never seen.
 *
 * Why fetch is not just fetch
 * ---------------------------
 * There is no HTTP route to the host — that is the whole point of the
 * architecture — so requests are framed and carried, and the answers are
 * assembled back into real Response objects. Cookies are kept HERE rather than
 * by the browser: a Set-Cookie on a Response a service worker constructs is
 * ignored, so this is the user agent for the far end, and it needs the jar.
 *
 * The frame format is pinned in backend/src/server/ControlTunnel.h. Both ends
 * are covered by tests that use the same vectors; a change to one without the
 * other fails them.
 */

import { loadIdentity, helloMessage, verifyOffer, signAnswer } from './pairing.js';

const FRAME_REQUEST = 0x01;
const FRAME_RESPONSE = 0x02;
const FRAME_BODY = 0x03;
const FRAME_END = 0x04;
const FRAME_WS_OPEN = 0x05;
const FRAME_WS_TEXT = 0x06;
const FRAME_WS_CLOSE = 0x07;
const FRAME_WS_OPENED = 0x08;

/**
 * Where the application's files are kept for the service worker to serve.
 *
 * One cache, not one per host, and stamped with whose interface is in it (see
 * boot.js). The service worker cannot read the fragment and so cannot know which
 * machine a page belongs to; keeping one cache and refilling it when the machine
 * changes is what makes that safe, at the cost of a re-download when someone
 * alternates between two machines in one browser.
 */
export const SHELL_CACHE = 'mw-shell';

/**
 * The order to fetch the application's files in: the largest first.
 *
 * Over a long round trip each answer is slow to get going, and a large one that
 * starts last trails alone at the end while the channel sits half idle (the
 * bench of 24/10, 115 ms away: 2 to 5 s per large file). Started first, the
 * large ones overlap everything else. `sizes` runs beside `files` in the host's
 * manifest since v0.4.1; an older host sends none, and its order is kept.
 */
export function shellFetchOrder(files, sizes) {
    if (!Array.isArray(sizes) || sizes.length !== files.length) return [...files];
    const size = (i) => (Number.isFinite(sizes[i]) ? sizes[i] : 0);
    return files
        .map((path, i) => i)
        .sort((a, b) => size(b) - size(a) || a - b)
        .map((i) => files[i]);
}

/** How long a refused file waits before its one more try. */
export const SHELL_STORE_RETRY_MS = 500;

/**
 * Put one of the application's files in the cache, trying once more after a
 * pause, and naming it when the browser refuses both times.
 *
 * A fresh Chrome profile once refused the whole interface with only "Failed to
 * execute 'put' on 'Cache': Entry already exists." (26/10, not reproduced since),
 * which looked like the storage failing for a moment rather than anything about
 * the files. One more try rides out such a moment. The path and the last storage
 * error's name are what make a report of the failure one to go on.
 *
 * The second try is handed a copy taken beforehand: the first put may have read
 * the body before it failed.
 */
export async function storeShellFile(cache, path, response) {
    const spare = response.clone();
    try {
        await cache.put(path, response);
        return;
    } catch {
        await new Promise((resolve) => setTimeout(resolve, SHELL_STORE_RETRY_MS));
    }
    try {
        await cache.put(path, spare);
    } catch (e) {
        const cause = e instanceof Error ? `${e.name}: ${e.message}` : String(e);
        throw new Error(`the browser could not store ${path} (${cause})`);
    }
}

/**
 * Requests answered here rather than sent to the machine.
 *
 * /api/auth/home-screen/instances carried the list of machines this browser
 * knows, for the menu of a home-screen shortcut. That list stays in the browser
 * (27/10/2026). The applications of 0.3.1 and 0.4.0 still send the request, and
 * every connection of every application version is a Tunnel of this file, so it
 * is answered here, as the host would answer it: a shortcut's menu fills as the
 * shortcut is used.
 */
const NEVER_SENT = new Set(['/api/auth/home-screen/instances']);

function sentNowhere() {
    return new Response(JSON.stringify({ status: 'ok' }), {
        status: 200,
        headers: { 'Content-Type': 'application/json' },
    });
}

/** How long a request may wait for its first byte before it is given up on. */
const REQUEST_TIMEOUT_MS = 30000;

/** How long the whole connect sequence may take before it is called failed. */
const CONNECT_TIMEOUT_MS = 25000;

/**
 * How long the machine has to say something on the channel once the signalling
 * line has gone from under a working tunnel. See _lineLost().
 */
const LINE_LOST_CHECK_MS = 5000;

/**
 * The protocol revision this page speaks INSIDE /v1/, sent as `?v=` when it
 * asks for a machine. Not the app version: it moves only when this page starts
 * understanding something the introduction server can send, and every such
 * change is additive — a breaking one is a /v2/ beside this file. The server
 * treats an absent value as 1, so the pages published before the field existed
 * are exactly revision-1 pages. Keep in step with hub.go and
 * RendezvousClient.cpp.
 */
export const PROTO = 1;
/** How many of the host's candidates to hold before its offer arrives. */
const MAX_EARLY_CANDIDATES = 64;

/** How long the failure report waits for the browser's own ICE counters. */
const DIAG_STATS_WAIT_MS = 500;

/** Where a count in the failure report stops: past it, the number says nothing more. */
const DIAG_COUNT_CAP = 999;

const DIAG_TYPES = new Set(['host', 'srflx', 'prflx', 'relay']);
const DIAG_PROTOCOLS = new Set(['udp', 'tcp']);

/**
 * The kind of a candidate this page sent, as the failure report counts it:
 * `type/protocol/family`, for example `host/udp/v4`. Null when the browser does
 * not say enough to tell, and then it is not counted.
 *
 * The address is read here only to tell its family: an mDNS name (`….local`),
 * IPv6, or IPv4. It goes no further — the report carries this key and a count,
 * never an address or a port.
 */
export function candidateKind(candidate) {
    if (!candidate) return null;
    const type = String(candidate.type || '').toLowerCase();
    const protocol = String(candidate.protocol || '').toLowerCase();
    if (!DIAG_TYPES.has(type) || !DIAG_PROTOCOLS.has(protocol)) return null;
    // Older browsers leave `address` out; it is the fifth field of the line.
    const address = candidate.address || String(candidate.candidate || '').split(' ')[4] || '';
    if (!address) return null;
    const family = address.endsWith('.local') ? 'mdns' : address.includes(':') ? 'v6' : 'v4';
    return `${type}/${protocol}/${family}`;
}

/**
 * The browser's ICE counters, added up over every candidate pair: checks it
 * sent, answers it had to them, checks it received. Only these numbers are
 * read — never a pair's addresses, ports or URLs. Null when the browser does not
 * answer within DIAG_STATS_WAIT_MS: the report goes without them rather than
 * after the line has closed.
 */
export async function checkCounts(pc) {
    let timer;
    const stats = await Promise.race([
        Promise.resolve()
            .then(() => pc.getStats())
            .catch(() => null),
        new Promise((resolve) => {
            timer = setTimeout(() => resolve(null), DIAG_STATS_WAIT_MS);
        }),
    ]);
    clearTimeout(timer);
    if (!stats) return null;
    const sum = { sent: 0, answered: 0, received: 0 };
    const add = (key, value) => {
        if (Number.isFinite(value) && value > 0)
            sum[key] = Math.min(sum[key] + value, DIAG_COUNT_CAP);
    };
    stats.forEach((report) => {
        if (report.type !== 'candidate-pair') return;
        add('sent', report.requestsSent);
        add('answered', report.responsesReceived);
        add('received', report.requestsReceived);
    });
    return sum;
}

/**
 * The report a page sends its machine when the direct connection could not be
 * opened (issue #48: a car whose browser reached the introduction server and
 * then nothing at all reached its machine, with nothing on either side to say
 * where it stopped).
 *
 * It goes over the signalling line, to the machine and nowhere else: the
 * introduction server relays it as it relays the candidates, without reading or
 * keeping it. The machine writes one line in its own log. What it carries is
 * states and numbers — the ICE, connection and gathering states, how long since
 * the answer went out, how many candidates of each kind this page sent, how
 * many it had from the machine, and the ICE check counters. No address, no
 * port, no browser name.
 */
export function diagMessage({ why, ice, connection, gathering, ms, sent, received, checks }) {
    const cand = {};
    for (const [kind, n] of sent) cand[kind] = Math.min(n, DIAG_COUNT_CAP);
    const message = {
        type: 'diag',
        why,
        ice,
        connection,
        gathering,
        ms: Math.max(0, Math.round(ms)),
        cand,
        recv: Math.min(received, DIAG_COUNT_CAP),
    };
    if (checks) message.checks = checks;
    return message;
}

/**
 * The failure where the machine answered and the network did not carry it.
 *
 * Kept apart from the deadline's own wording because the two are different
 * accusations. "Your machine did not answer" sends someone home to restart a
 * machine that is running perfectly; once its signed offer has been verified,
 * the machine has answered, and everything after that is the network between
 * the two ends. Company networks and public Wi-Fi are where this happens: the
 * WebSocket above them is TCP 443 and goes through any proxy, while the
 * connection itself needs the two ends to reach each other directly, and there
 * is no relay to fall back on.
 */
function noRouteError() {
    const err = new Error('Your machine answered, but no direct connection could be opened.');
    err.answered = true;
    err.mayPass = true;
    err.hint =
        'This network is not letting the two ends reach each other. Company networks and ' +
        'public Wi-Fi often block what that takes. A phone hotspot is the quickest way to tell.';
    return err;
}

/**
 * A failure that may pass, but not within the minute, marked `mayPass`: the
 * network between the two ends (above), or a machine on its line that said
 * nothing in time. What fixes those is a router opening its ports again, a phone
 * leaving the office Wi-Fi, a machine done with whatever held it — nothing this
 * page can hurry. So a page may try again now and then, never every few
 * seconds: each try waits out the whole deadline, and the explanation on screen
 * is the one to read in the meantime (boot.js, connectWhenReachable).
 */
function silentError() {
    const err = new Error('Your machine did not answer in time.');
    err.mayPass = true;
    return err;
}

/**
 * A failure that can pass by itself within a minute, marked `transient` so a
 * page may keep calling under it. These are what a restart of the introduction
 * server looks like from here: the server not there, or back while the
 * machine's own line to it is not yet.
 */
function passingError(message) {
    const err = new Error(message);
    err.transient = true;
    return err;
}

export function encodeFrame(kind, id, payload) {
    const body = payload ?? new Uint8Array(0);
    const out = new Uint8Array(5 + body.length);
    const view = new DataView(out.buffer);
    out[0] = kind;
    view.setUint32(1, id, false);
    out.set(body, 5);
    return out;
}

/**
 * Compression of the interface's files (v0.4.2).
 *
 * A request carries `z: 'deflate'` in its head when this browser can inflate;
 * the host then may send the body deflated (zlib, RFC 1950) and marks its own
 * head the same way. The first visit downloads the whole application over the
 * tunnel, and its scripts shrink about four times. Both marks live in the
 * frames' heads, not in HTTP headers, so the Response built here is the file
 * as served, and every check on it (the hashes, the official list) sees the
 * same bytes as before. An older host ignores the field; a browser without
 * DecompressionStream never sends it. The format is pinned in
 * backend/src/server/TunnelFrame.h.
 */
export const DEFLATE = 'deflate';

export function canInflate() {
    return typeof DecompressionStream === 'function';
}

export async function inflate(bytes) {
    const input = new ReadableStream({
        start(controller) {
            controller.enqueue(bytes);
            controller.close();
        },
    });
    const reader = input.pipeThrough(new DecompressionStream(DEFLATE)).getReader();
    const parts = [];
    let total = 0;
    for (;;) {
        const { done, value } = await reader.read();
        if (done) break;
        parts.push(value);
        total += value.length;
    }
    const out = new Uint8Array(total);
    let offset = 0;
    for (const p of parts) {
        out.set(p, offset);
        offset += p.length;
    }
    return out;
}

export function encodeHead(head, body) {
    const json = new TextEncoder().encode(JSON.stringify(head));
    const rest = body ?? new Uint8Array(0);
    const out = new Uint8Array(4 + json.length + rest.length);
    new DataView(out.buffer).setUint32(0, json.length, false);
    out.set(json, 4);
    out.set(rest, 4 + json.length);
    return out;
}

/**
 * The cookie jar for one host.
 *
 * It exists because the browser's own jar cannot be used: there is no HTTP
 * exchange for it to attach to, and a Set-Cookie on a Response a service worker
 * constructs is ignored. So this is the user agent for the far end, and like any
 * user agent it has to honour what the cookie asks for:
 *
 *   a cookie with an expiry     survives the browser closing  → localStorage
 *   a cookie without one        dies with the tab             → sessionStorage
 *
 * That mapping is the whole design, and it is what makes "keep me signed in"
 * mean the same thing here as on a direct connection. Getting it wrong is not a
 * small inconvenience: the access PIN regenerates after every successful use, so
 * a browser that forgets a session it was told to keep leaves its owner unable
 * to get back in without walking to the machine.
 *
 * What is genuinely lost, and cannot be recovered here: on a direct connection
 * that cookie is HttpOnly, out of reach of any script on the page. A jar the
 * page maintains is a jar the page can read. The exposure that adds is to code
 * running on this origin — which, if it is hostile, is already the user agent
 * and already sees the credential in flight. Storing it at rest lets such code
 * take the token without waiting for a login, and that is the real cost of the
 * trade; it is worth it against locking users out of their own machines, and it
 * is why the PAIRING key next door is generated non-extractable instead.
 */
class CookieJar {
    constructor(hostId) {
        this.key = `mw-jar:${hostId}`;
        /** name → { value, expires } — expires null for a session cookie. */
        this.jar = new Map();
        this._load(sessionStorage);
        this._load(localStorage);
    }

    _load(store) {
        try {
            const saved = store.getItem(this.key);
            if (!saved) return;
            for (const [name, rec] of JSON.parse(saved)) {
                if (rec.expires && rec.expires <= Date.now()) continue;
                this.jar.set(name, rec);
            }
        } catch {
            /* no storage, or a shape we no longer write: skip it */
        }
    }

    _save() {
        const session = [];
        const persistent = [];
        for (const [name, rec] of this.jar) (rec.expires ? persistent : session).push([name, rec]);
        try {
            sessionStorage.setItem(this.key, JSON.stringify(session));
            // Written even when empty, so signing out actually clears what was
            // kept rather than leaving a stale token behind.
            localStorage.setItem(this.key, JSON.stringify(persistent));
        } catch {
            /* the jar still works for this page; only the next start pays */
        }
    }

    header() {
        const live = [...this.jar].filter(([, r]) => !r.expires || r.expires > Date.now());
        if (live.length === 0) return null;
        return live.map(([k, r]) => `${k}=${r.value}`).join('; ');
    }

    /**
     * Take what a Set-Cookie says, and no more than that.
     *
     * Max-Age and Expires are read because they decide how long the credential
     * lives and therefore where it is kept. Path and Domain are ignored on
     * purpose: this jar serves exactly one host over one channel, so there is
     * nothing to scope them against, and pretending to honour them would be a
     * fiction that hides bugs.
     */
    absorb(setCookie) {
        for (const line of Array.isArray(setCookie) ? setCookie : [setCookie]) {
            if (!line) continue;
            const [pair, ...attrs] = line.split(';');
            const eq = pair.indexOf('=');
            if (eq <= 0) continue;
            const name = pair.slice(0, eq).trim();
            const value = pair.slice(eq + 1).trim();

            let expires = null;
            let deleted = false;
            for (const attr of attrs) {
                const t = attr.trim().toLowerCase();
                if (t.startsWith('max-age=')) {
                    const seconds = parseInt(t.slice(8), 10);
                    if (Number.isNaN(seconds)) continue;
                    if (seconds <= 0) deleted = true;
                    else expires = Date.now() + seconds * 1000;
                } else if (t.startsWith('expires=')) {
                    const when = new Date(attr.trim().slice(8)).getTime();
                    if (Number.isNaN(when)) continue;
                    if (when <= Date.now()) deleted = true;
                    else expires = when;
                }
            }

            if (deleted || value === '') this.jar.delete(name);
            else this.jar.set(name, { value, expires });
        }
        this._save();
    }
}

/** A WebSocket-shaped view of one signalling socket carried over the channel. */
class TunnelSocket {
    constructor(tunnel, id, path) {
        this.readyState = 0; // CONNECTING
        this.onopen = null;
        this.onmessage = null;
        this.onclose = null;
        this.onerror = null;
        this.url = path;
        this._tunnel = tunnel;
        this._id = id;
    }

    send(data) {
        if (this.readyState !== 1) return;
        this._tunnel._sendFrame(FRAME_WS_TEXT, this._id, new TextEncoder().encode(String(data)));
    }

    close() {
        if (this.readyState === 3) return;
        this.readyState = 2; // CLOSING
        this._tunnel._sendFrame(FRAME_WS_CLOSE, this._id, new Uint8Array(0));
    }

    _opened() {
        this.readyState = 1;
        if (this.onopen) this.onopen(new Event('open'));
    }

    _received(text) {
        if (this.onmessage) this.onmessage({ data: text });
    }

    _closed(reason) {
        if (this.readyState === 3) return;
        this.readyState = 3;
        this._tunnel._sockets.delete(this._id);
        if (this.onclose) this.onclose({ code: 1000, reason: reason || '' });
    }
}

export class Tunnel {
    /**
     * @param {string} hostId  the 26-character identifier in the address bar
     * @param {string} origin  where the introduction server answers
     */
    constructor(hostId, origin = location.origin) {
        this.hostId = hostId;
        this.origin = origin;
        this.identity = null;
        this.firstContact = false;
        /**
         * The revision the machine on the line speaks, from the ready frame;
         * 1 when the server predates the field. Nothing reads it yet — it is
         * what lets an addition be used only with a machine that understands
         * it, the day there is one.
         */
        this.hostProto = 1;

        this._ws = null;
        this._pc = null;
        this._dc = null;
        this._cookies = new CookieJar(hostId);
        // Candidates the host sent before there was anywhere to put them. See
        // _onRemoteCandidate: nearly all of them arrive that early.
        this._earlyCandidates = [];
        // Kinds of local address this browser found (host, srflx). Reported if
        // the connection fails, where it is the difference between "this
        // network blocks everything" and "it found its public address and the
        // checks still went nowhere".
        this._localTypes = new Set();
        // For the report a failed connection sends its machine (diagMessage):
        // candidates by kind, counted only once actually sent, so the report
        // never claims more than the machine was given; the machine's own
        // candidates, counted as they come; and whether this page's signed
        // answer went out — before it has, the machine would not listen, and
        // nothing is sent.
        this._sentKinds = new Map();
        this._remoteCount = 0;
        this._answeredAt = null;
        this._diagSent = false;

        this._nextId = 1;
        this._pending = new Map(); // request id → { resolve, reject, head, chunks }
        this._sockets = new Map(); // socket id → TunnelSocket
        // Frames received from the host, counted so that _lineLost() can tell a
        // machine that is still there from a channel that only looks open.
        this._framesIn = 0;

        /** Called with a short status word so a page can say what is happening. */
        this.onstatus = null;
        /** Called when the channel goes away for good. */
        this.onclosed = null;
        /**
         * True once connect() has given up at its deadline. Nothing stops the
         * attempt then: on a slow network the channel can still open a few
         * seconds later, and `onlate` is called if it does. Whoever gave up on
         * it either takes it at that moment or closes it.
         */
        this.overdue = false;
        this.onlate = null;
    }

    get connected() {
        return this._dc?.readyState === 'open';
    }

    _status(what) {
        if (this.onstatus) this.onstatus(what);
    }

    /**
     * Bring the channel up. Rejects with a message meant to be shown to a
     * person, because every failure here is one they may be able to act on.
     */
    async connect() {
        this._status('identity');
        this.identity = await loadIdentity(this.hostId);
        if (!this.identity) {
            throw new Error(
                'This browser cannot store a key for your machine — private browsing, or ' +
                    'storage turned off for this site.',
            );
        }

        this._status('calling');
        await new Promise((resolve, reject) => {
            const deadline = setTimeout(() => {
                this.overdue = true;
                reject(this._deadlineError());
                // After the verdict, not before it: a page that gives up here
                // keeps the attempt going (see `overdue`), so the line is still
                // there to carry the report.
                this._reportNoRoute('deadline');
            }, CONNECT_TIMEOUT_MS);
            const settle = (err) => {
                clearTimeout(deadline);
                if (err) reject(err);
                else if (this.overdue) this.onlate?.();
                else resolve();
            };

            const wsUrl = new URL('/v1/peer', this.origin);
            wsUrl.protocol = wsUrl.protocol === 'http:' ? 'ws:' : 'wss:';
            wsUrl.searchParams.set('id', this.hostId);
            wsUrl.searchParams.set('v', String(PROTO));

            this._ws = new WebSocket(wsUrl);
            let opened = false;
            this._ws.onopen = () => {
                opened = true;
            };
            this._ws.onmessage = (ev) => this._onRelayFrame(ev.data, settle);
            this._ws.onerror = () =>
                settle(passingError('Could not reach the introduction server.'));
            this._ws.onclose = (ev) => {
                // The code and reason are the only account of WHY the line went,
                // and this is the failure a user reports as "it just stops".
                // A line that never opened has no such account to give — 1006,
                // nothing more — and its failure is the rejection below; a page
                // that keeps calling a server that is down (lineKeeper behind a
                // stream, the entry page's retries) would log it on every try.
                if (opened) {
                    console.warn(
                        `[MW] Signalling line closed: code=${ev.code} reason=${ev.reason || '(none)'}`,
                    );
                }
                if (!this.connected) {
                    settle(passingError('The introduction server hung up.'));
                    return;
                }
                this._lineLost();
            };
            this._onChannelOpen = settle;
        });
    }

    /**
     * The signalling line went away under a working tunnel — and the tunnel
     * stays.
     *
     * It used to go down with the line, on the reasoning that the host takes a
     * closed line for the browser leaving. That is true of one thing only: the
     * introduction server telling the host this browser's session is over. The
     * host does nothing to its tunnels when its OWN line drops. So when the
     * server itself goes — a restart, a deploy, a reboot — both lines fall
     * together, nobody tells the host anything, and the channel is still a
     * perfectly good direct connection between the two machines. Tearing it
     * down was the only thing that ended every session on the server's way out,
     * the ones on the same LAN as their machine included, and every stream with
     * them.
     *
     * Whether the host is still there is asked of the host, over the channel:
     * one request, and any frame back within LINE_LOST_CHECK_MS settles it. Any
     * frame, not the answer to that request in particular — the answer can be
     * queued behind a large body the channel is still carrying. Silence means
     * the line went because the network did, and the tunnel goes down as it
     * always did. A host that did hear "the browser left" closes the channel
     * itself, which ends it by the ordinary route below.
     *
     * The line is not dialled again. A new one would be a new session, with a
     * new offer from the host for a connection this page already has.
     */
    _lineLost() {
        console.warn('[MW] Keeping the tunnel without its signalling line — asking the machine');
        const heard = this._framesIn;
        this.fetch('/api/health').catch(() => {
            /* only an occasion for the host to speak; the count is the verdict */
        });
        setTimeout(() => {
            // Closed meanwhile: the channel's own close has said so already.
            if (!this.connected) return;
            if (this._framesIn > heard) {
                console.info('[MW] The machine still answers — the tunnel carries on');
                return;
            }
            this._teardown('the introduction server hung up and the machine went silent');
        }, LINE_LOST_CHECK_MS);
    }

    /**
     * What to say when the connect deadline expires.
     *
     * Past the offer there is a peer connection, and there is one only because
     * the host's offer arrived and its signature checked out — so the machine
     * answered, whatever happened afterwards. Before that, nothing has been
     * heard from it and the plain reading is the right one.
     */
    _deadlineError() {
        return this._pc ? noRouteError() : silentError();
    }

    /** Returns whether the payload went out on the line. */
    _sendSignal(payload) {
        if (this._ws?.readyState !== WebSocket.OPEN) return false;
        this._ws.send(JSON.stringify({ t: 'msg', d: payload }));
        return true;
    }

    /**
     * Tell the machine where the direct connection stopped (diagMessage), once
     * per connection, and only when this page's signed answer has gone out:
     * before that, the offer may not have been verified and the machine would
     * not listen. `why` is 'failed' (the browser gave up on ICE) or 'deadline'
     * (connect()'s 25 s ran out first).
     */
    async _reportNoRoute(why) {
        if (this._diagSent || this._answeredAt === null || !this._pc) return;
        this._diagSent = true;
        const pc = this._pc;
        const checks = await checkCounts(pc);
        // Opened while the counters were read: there is nothing to report.
        if (this.connected) return;
        this._sendSignal(
            diagMessage({
                why,
                ice: pc.iceConnectionState,
                connection: pc.connectionState,
                gathering: pc.iceGatheringState,
                ms: performance.now() - this._answeredAt,
                sent: this._sentKinds,
                received: this._remoteCount,
                checks,
            }),
        );
    }

    async _onRelayFrame(raw, settle) {
        let frame;
        try {
            frame = JSON.parse(raw);
        } catch {
            return;
        }

        if (frame.t === 'error') {
            const said = {
                offline: 'That machine is not online right now.',
                busy: 'That machine already has as many connections as it accepts.',
                'host-gone': 'That machine went offline.',
                // This page is one the server no longer serves. The bytes
                // came from the network with no-cache, so a reload really
                // does fetch the current ones — unlike most "reload" advice.
                unsupported_version: 'This page is out of date — reload it.',
            };
            // A machine not online may only be reconnecting its line; one at
            // its connection limit, or a page out of date, is a plain answer.
            const passing = frame.code === 'offline' || frame.code === 'host-gone';
            const message = said[frame.code] || 'The introduction server refused.';
            settle(passing ? passingError(message) : new Error(message));
            return;
        }
        if (frame.t === 'ready') {
            this.hostProto = Number.isInteger(frame.v) && frame.v > 0 ? frame.v : 1;
            this._status('binding');
            this._sendSignal(helloMessage(this.identity));
            return;
        }
        if (frame.t !== 'msg' || !frame.d) return;

        const msg = frame.d;
        if (msg.type === 'sdp') await this._onOffer(msg, settle);
        else if (msg.type === 'ice') await this._onRemoteCandidate(msg);
    }

    async _onOffer(msg, settle) {
        if (this._pc) return; // one offer per connection

        // Verified BEFORE anything reaches a PeerConnection. When this fails, no
        // DTLS state has been created — that ordering is the guarantee, not a
        // detail of the implementation.
        const verdict = await verifyOffer(this.identity, msg);
        if (!verdict.ok) {
            settle(new Error(`Refused: ${verdict.reason}.`));
            return;
        }
        this.firstContact = verdict.firstContact;

        // The introduction server answers STUN on 3478, and it is the one host
        // this page is already talking to — so discovering our own address adds
        // no third party to the ones already involved. Google's public server
        // was here, on the page that refuses Google's web fonts three files
        // over precisely so that nobody outside learns who is connecting to
        // whom; the fonts were the smaller half of that leak.
        //
        // No fallback list, deliberately. If this server is unreachable the
        // WebSocket above is down too and there is no session to rescue.
        this._pc = new RTCPeerConnection({
            iceServers: [{ urls: `stun:${location.hostname}:3478` }],
        });
        this._pc.onicecandidate = (ev) => {
            if (ev.candidate) {
                // Kept for the failure log below: which kinds of address this
                // browser managed to find is most of the diagnosis, and it is
                // otherwise only visible in chrome://webrtc-internals.
                if (ev.candidate.type) this._localTypes.add(ev.candidate.type);
                const sent = this._sendSignal({
                    type: 'ice',
                    candidate: ev.candidate.candidate,
                    mid: ev.candidate.sdpMid,
                });
                const kind = sent ? candidateKind(ev.candidate) : null;
                if (kind) this._sentKinds.set(kind, (this._sentKinds.get(kind) || 0) + 1);
            }
        };
        this._pc.ondatachannel = (ev) => this._adoptChannel(ev.channel);
        const pc = this._pc;
        pc.onconnectionstatechange = async () => {
            const state = pc.connectionState;
            if (state !== 'failed' && state !== 'closed') return;
            // A verdict, not a symptom: before the channel has ever opened this
            // IS the outcome of the attempt, and sitting on it until the
            // deadline only spends another twenty seconds to end up saying
            // something less true.
            const waiting = !this.connected ? this._onChannelOpen : null;
            if (waiting) {
                console.warn(
                    `[MW] No route to the host: ice=${pc.iceConnectionState} ` +
                        `local candidates=${[...this._localTypes].join(',') || 'none'}`,
                );
                // Sent before the verdict: the page that hears it may close the
                // signalling line at once.
                if (state === 'failed') await this._reportNoRoute('failed');
                waiting(noRouteError());
            }
            this._teardown('the connection ended');
        };

        await this._pc.setRemoteDescription({ type: 'offer', sdp: msg.sdp });

        // Now there is somewhere to put them. Emptied before the answer is even
        // made, so the checks start on the earliest paths rather than on
        // whichever one happened to arrive late.
        await this._flushEarlyCandidates();

        const answer = await this._pc.createAnswer();

        const sig = await signAnswer(this.identity, msg.nonce, verdict.fingerprintHost, answer.sdp);
        if (!sig) {
            settle(new Error('Refused: this browser could not sign its own answer.'));
            return;
        }
        await this._pc.setLocalDescription(answer);
        if (this._sendSignal({ type: 'sdp', sdp: answer.sdp, sig }))
            this._answeredAt = performance.now();
        this._status('connecting');
    }

    /**
     * A candidate from the host.
     *
     * Held rather than dropped when it arrives before the offer has been
     * verified and applied, because that is what happens to nearly all of them.
     * The host builds its peer connection the moment the session opens and its
     * local addresses are ready within milliseconds, while this end has a
     * signature to check, an answer to make and an answer to sign first. Every
     * one of those candidates used to be thrown away in the gap.
     *
     * What survived was the single reflexive candidate, which arrives a second
     * or two later because it costs a round trip to a STUN server. So a
     * connection that had a working path over the local network — or over IPv6 —
     * was left betting everything on the one address that has to come back in
     * through a NAT. On a router without hairpin, or from a guest's network with
     * a NAT of its own, there was nothing left to try and the page sat at
     * "Opening a direct connection…" until it timed out.
     *
     * The queue is bounded because it is filled from the relay before the far
     * end has been authenticated: whoever is on that session can push into it.
     * The bound is far above what a host emits (ten or so) and far below
     * anything that costs memory.
     */
    async _onRemoteCandidate(msg) {
        if (this._remoteCount < DIAG_COUNT_CAP) this._remoteCount++;
        if (!this._pc?.remoteDescription) {
            if (this._earlyCandidates.length < MAX_EARLY_CANDIDATES)
                this._earlyCandidates.push(msg);
            return;
        }
        await this._addCandidate(msg);
    }

    /** Hand over everything held, in the order the host sent it. */
    async _flushEarlyCandidates() {
        const held = this._earlyCandidates;
        this._earlyCandidates = [];
        for (const candidate of held) await this._addCandidate(candidate);
    }

    async _addCandidate(msg) {
        try {
            await this._pc.addIceCandidate({
                candidate: msg.candidate,
                sdpMid: msg.mid,
            });
        } catch {
            /* one path among several */
        }
    }

    _adoptChannel(channel) {
        this._dc = channel;
        this._dc.binaryType = 'arraybuffer';
        this._dc.onmessage = (ev) => this._onChannelMessage(new Uint8Array(ev.data));
        this._dc.onclose = () => this._teardown('the channel closed');
        this._dc.onopen = () => {
            this._status('ready');
            // The relay socket STAYS OPEN for the life of the tunnel.
            //
            // It is tempting to close it here — the media path is up, the
            // introduction server has done its job, and one fewer socket held on
            // someone else's machine reads as a straightforward win. It is not:
            // to the host, the rendezvous session and this peer connection are
            // one lifetime, so hanging up the socket takes the tunnel down with
            // it. Closing it also throws away candidates that arrive after the
            // first working pair, which are often the better ones.
            //
            // Nothing further crosses it. It carries signalling, it is idle
            // between renegotiations, and the server already knows this browser
            // is connected — it is the party that introduced them.
            if (this._onChannelOpen) this._onChannelOpen(null);
            // Dropped once used: it is what tells a later failure that there is
            // still a connect() waiting on a verdict, and a tunnel that dies an
            // hour from now must not be reported as one that never came up.
            this._onChannelOpen = null;
        };
        if (this._dc.readyState === 'open') this._dc.onopen();
    }

    /**
     * Hang up on purpose — a page that only had a question for this machine
     * and has its answer. Requests still in flight are failed, and onclosed is
     * NOT called: it reports a line that went away, and this one was put down.
     */
    close() {
        this.onclosed = null;
        this.onlate = null;
        this._teardown('closed');
        for (const part of [this._dc, this._pc, this._ws]) {
            try {
                part?.close();
            } catch {
                /* already gone */
            }
        }
        this._dc = null;
        this._pc = null;
        this._ws = null;
    }

    _teardown(why) {
        for (const [, socket] of this._sockets) socket._closed(why);
        this._sockets.clear();
        for (const [, waiter] of this._pending) waiter.reject(new Error(why));
        this._pending.clear();
        if (this.onclosed) this.onclosed(why);
    }

    /// Returns false when the frame could not be handed to the channel, so a
    /// caller that is waiting for an answer can give up now rather than sit out
    /// the request timeout for a message that was never sent.
    _sendFrame(kind, id, payload) {
        if (this._dc?.readyState !== 'open') return false;
        try {
            this._dc.send(encodeFrame(kind, id, payload));
            return true;
        } catch (e) {
            console.warn('[tunnel] send failed:', e && e.message);
            return false;
        }
    }

    _onChannelMessage(bytes) {
        this._framesIn++;
        if (bytes.length < 5) return;
        const kind = bytes[0];
        const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
        const id = view.getUint32(1, false);
        const payload = bytes.subarray(5);

        switch (kind) {
            case FRAME_RESPONSE: {
                const waiter = this._pending.get(id);
                if (!waiter) return;
                const headLen = new DataView(
                    payload.buffer,
                    payload.byteOffset,
                    payload.byteLength,
                ).getUint32(0, false);
                waiter.head = JSON.parse(
                    new TextDecoder().decode(payload.subarray(4, 4 + headLen)),
                );
                clearTimeout(waiter.timer);
                break;
            }
            case FRAME_BODY: {
                const waiter = this._pending.get(id);
                if (waiter) waiter.chunks.push(payload.slice());
                break;
            }
            case FRAME_END: {
                const waiter = this._pending.get(id);
                if (!waiter) return;
                this._pending.delete(id);
                waiter.resolve(this._buildResponse(waiter));
                break;
            }
            case FRAME_WS_OPENED:
                this._sockets.get(id)?._opened();
                break;
            case FRAME_WS_TEXT:
                this._sockets.get(id)?._received(new TextDecoder().decode(payload));
                break;
            case FRAME_WS_CLOSE:
                this._sockets.get(id)?._closed(new TextDecoder().decode(payload));
                break;
            default:
                break;
        }
    }

    async _buildResponse(waiter) {
        const head = waiter.head || { s: 502, h: {} };
        let total = 0;
        for (const c of waiter.chunks) total += c.length;
        let body = new Uint8Array(total);
        let offset = 0;
        for (const c of waiter.chunks) {
            body.set(c, offset);
            offset += c.length;
        }
        if (head.z !== undefined) {
            // Only ever sent when this page asked, and only the one encoding.
            if (head.z !== DEFLATE) throw new Error(`your machine sent a body in ${head.z}`);
            try {
                body = await inflate(body);
            } catch (e) {
                throw new Error(`a file from your machine did not inflate (${e && e.message})`);
            }
        }

        const headers = new Headers();
        for (const [name, value] of Object.entries(head.h || {})) {
            // Set-Cookie is taken by the jar and dropped here. The browser
            // ignores it on a constructed Response anyway, so leaving it on
            // would only look like it was doing something.
            if (name.toLowerCase() === 'set-cookie') {
                this._cookies.absorb(value);
                continue;
            }
            try {
                headers.set(name, value);
            } catch {
                /* a header the Headers guard refuses is not ours to force */
            }
        }

        // 204 and 304 must be built with a null body or the constructor throws.
        const bodyless = head.s === 204 || head.s === 304 || head.s < 200;
        return new Response(bodyless ? null : body, { status: head.s, headers });
    }

    /** One HTTP request over the channel. Same shape as window.fetch. */
    async fetch(input, init = {}) {
        const url = new URL(typeof input === 'string' ? input : input.url, location.origin);
        // Answered here and never sent: see NEVER_SENT.
        if (NEVER_SENT.has(url.pathname)) return sentNowhere();

        if (!this.connected) throw new Error('the tunnel is not open');

        const headers = {};
        new Headers(
            init.headers || (typeof input === 'object' ? input.headers : undefined),
        ).forEach((value, name) => {
            headers[name] = value;
        });

        const cookie = this._cookies.header();
        if (cookie) headers.cookie = cookie;
        // The host reads these to decide cross-site and rebinding questions, so
        // they say where this page really is rather than where it is going.
        headers.host = location.host;
        if (!headers.origin) headers.origin = location.origin;

        let body = init.body;
        if (typeof body === 'string') body = new TextEncoder().encode(body);
        else if (body instanceof ArrayBuffer) body = new Uint8Array(body);
        else if (body && !(body instanceof Uint8Array))
            body = new TextEncoder().encode(String(body));

        const id = this._nextId++;
        const head = {
            m: (init.method || 'GET').toUpperCase(),
            p: url.pathname + url.search,
            h: headers,
        };
        // The host decides which answers to deflate (its interface files only).
        if (canInflate()) head.z = DEFLATE;
        const payload = encodeHead(head, body);

        return new Promise((resolve, reject) => {
            const timer = setTimeout(() => {
                this._pending.delete(id);
                reject(new Error('the host did not answer'));
            }, REQUEST_TIMEOUT_MS);
            this._pending.set(id, { resolve, reject, timer, head: null, chunks: [] });
            // A channel that is not open swallows the frame. Without this the
            // request would wait out the full timeout for an answer to a
            // question nobody was asked — which is what the page's first render
            // did whenever it landed while a tunnel session was being replaced:
            // every app list came back "timed out" thirty seconds later.
            if (!this._sendFrame(FRAME_REQUEST, id, payload)) {
                clearTimeout(timer);
                this._pending.delete(id);
                reject(new Error('the connection to your machine is not open'));
            }
        });
    }

    /**
     * A WebSocket-shaped object for the streaming signalling.
     *
     * Only the signalling travels this way. The video and the input do not: they
     * get their own peer connection, negotiated over this one, and go directly
     * between the two machines. Pushing frames through here instead would put
     * every one of them on the same SCTP stream that is answering API calls.
     */
    openSocket(path) {
        const id = this._nextId++;
        const socket = new TunnelSocket(this, id, path);
        this._sockets.set(id, socket);

        const cookie = this._cookies.header();
        const headers = { host: location.host, origin: location.origin };
        if (cookie) headers.cookie = cookie;

        this._sendFrame(FRAME_WS_OPEN, id, encodeHead({ p: path, h: headers }, null));
        return socket;
    }
}

const ID_SHAPE = /^[0-9a-z]{26}$/;

/**
 * Remember this machine as the one this browser last used.
 *
 * Written only once a connection actually succeeded, so a mistyped identifier
 * never becomes the default. Kept in localStorage on purpose: it is what makes
 * the bare address work, and it is not a credential — it is the same identifier
 * already sitting in the address bar, in the history, and in any bookmark.
 */
export function rememberLastHost(hostId) {
    try {
        localStorage.setItem('mw-last-host', hostId);
    } catch {
        /* the address bar still carries it */
    }
}

/**
 * The machines this browser has been to, newest first.
 *
 * READ ONLY, but for one removal (forgetInstance): the register is written by the
 * application (frontend/js/util/instances.js), which is the only code that ever
 * learns what a machine calls itself — it asks the machine. This page never
 * reaches one far enough to be told a name, so it can offer the list and must
 * never add to it or edit it.
 *
 * It exists here for the case this page is FOR: the machine did not answer. The
 * entry page is then the last thing on screen, and without the list it is a dead
 * end — the owner's other machines are perfectly reachable and nothing on the
 * page says so. An entry is a name, an identifier already in the address bar,
 * and a date; there is no credential in any of it.
 *
 * Anything malformed is dropped rather than repaired: this is shared storage,
 * a future version may write fields this one does not know, and a menu that
 * silently skips a row it cannot read is better than one that renders "undefined".
 */
export function knownInstances() {
    try {
        const raw = JSON.parse(localStorage.getItem('mw-instances') || '[]');
        if (!Array.isArray(raw)) return [];
        return (
            raw
                .filter(
                    (e) =>
                        e &&
                        typeof e.id === 'string' &&
                        ID_SHAPE.test(e.id) &&
                        typeof e.name === 'string' &&
                        e.name &&
                        typeof e.url === 'string' &&
                        e.url,
                )
                // By name, like the application's own copy of this menu. The two are
                // the same list seen from two pages, and a row that moves between
                // them is a row that has to be read twice.
                .sort((a, b) => a.name.localeCompare(b.name, undefined, { sensitivity: 'base' }))
        );
    } catch {
        // No storage, or a value this build cannot parse. The page keeps working
        // and simply has nothing to offer, which is what it did before.
        return [];
    }
}

/**
 * Drop one machine from the register, at the person's request (boot.js, the
 * "Not connected" page). The single exception to the read-only rule above, and
 * a narrow one: it removes a row, never writes one, and leaves every other row
 * exactly as the application stored it — fields this build does not know
 * included. The last-used marker goes with it, or the bare address would keep
 * opening the machine that was just let go.
 */
export function forgetInstance(id) {
    try {
        const raw = JSON.parse(localStorage.getItem('mw-instances') || '[]');
        if (Array.isArray(raw)) {
            localStorage.setItem(
                'mw-instances',
                JSON.stringify(raw.filter((e) => !(e && e.id === id))),
            );
        }
        if (localStorage.getItem('mw-last-host') === id) localStorage.removeItem('mw-last-host');
    } catch {
        /* no storage: there was no list to remove it from */
    }
}

/**
 * The identifier this page is bound to, or null.
 *
 * Four places, most specific first, and each earns its place:
 *
 *   path       the canonical address people bookmark and share
 *   fragment   where the bootstrap moves it before handing over, so the
 *              application keeps a URL that survives a reload
 *   session    per tab — the backstop for when the application's own routing
 *              rewrites the URL and drops the fragment, and what lets two tabs
 *              hold two different machines at once
 *   last used  per browser — so stream.{domain} with nothing after it opens the
 *              machine you were on, instead of an application with nothing
 *              behind it
 *
 * The order is what keeps the last two from being a trap: an explicit
 * identifier in the URL always wins, so following a link to a different machine
 * goes to that machine. Nothing is shared between them either way — the pairing
 * key, the cookie jar and the cached interface are each named after the host
 * they belong to.
 */
/**
 * The fragment, split on '&'. The identifier is the one bare segment; anything
 * else is `name=value`.
 *
 * The fragment is used rather than the query for one reason, and it is the
 * reason the host key can be carried at all: a browser never sends the fragment
 * to the server it fetched the page from. The introduction server therefore
 * sees which machine is being asked for — it has to, that is its job — and
 * nothing else, whatever else the address carries.
 */
function hashParts() {
    return location.hash.replace(/^#/, '').split('&').filter(Boolean);
}

/**
 * The host key this link carries, or null.
 *
 * Put there by the machine's own tray so its owner can reach an admin page under
 * a certificate the browser already trusts. Single-use: the host burns it on
 * redemption, so a link that reaches anyone else is spent by then, and one that
 * stays in someone's history is spent too.
 */
export function hostKeyFromLocation() {
    for (const part of hashParts()) {
        if (part.startsWith('k=')) {
            const key = decodeURIComponent(part.slice(2));
            if (key) return key;
        }
    }
    return null;
}

/**
 * Where inside the application this link asks to land, or null.
 *
 * At handover the identifier moves out of the path, which frees the path for the
 * application's own routes — but the address that started all this had to spend
 * its path on the identifier, so it has no other way to say "the settings page"
 * rather than "the front door". This is that way. The machine's own tray uses it;
 * an ordinary link carries nothing and lands at the root, as before.
 *
 * One segment, letters, digits, '-' and '_'. That is not fussiness: the value is
 * handed to location.replace(), and "//somewhere.else" is a perfectly valid URL
 * that leaves this origin altogether.
 *
 * An invitation needs no such part: it says where it goes by existing. The
 * guest page is the only page a token is ever for, so a link that carried both
 * spelled the same fact twice and made the thing a person pastes into a message
 * longer for it. An explicit request still wins, so an older link that does
 * carry one keeps working.
 */
const LANDING_SHAPE = /^\/[A-Za-z0-9_-]{1,32}$/;

export function landingPathFromLocation() {
    for (const part of hashParts()) {
        if (part.startsWith('p=')) {
            const path = decodeURIComponent(part.slice(2));
            if (LANDING_SHAPE.test(path)) return path;
        }
    }
    if (shareTokenFromLocation()) return '/p';
    // A start of the application itself — a reload of /admin, a shortcut —
    // that the service worker sent here so the cache is checked first (R1):
    // the page it was on.
    const here = location.pathname;
    if (here !== '/' && LANDING_SHAPE.test(here) && !ID_SHAPE.test(here.slice(1))) return here;
    return null;
}

/**
 * The invitation token this link carries, or null.
 *
 * An owner shares a slot of their session and the guest arrives here holding
 * this. It is the whole of what the link carries — the page it opens follows
 * from it — and it never appears in a URL path, at any step, so no navigation
 * can ever carry it to the introduction server. A path would be answered from
 * the cache by the service worker in the ordinary case — but "the ordinary
 * case" is not a property to rest a credential on, and the one time that worker
 * is missing is the one time the token would be in a request line somebody else
 * keeps.
 *
 * Read, not consumed — the opposite of the host key above. That key is spent on
 * redemption and must never be replayed; an invitation is what this page IS, and
 * it has to survive every reload of it.
 *
 * The shape is checked because the value is put back into what is handed to
 * location.replace() at handover. It is base64url at the far end, so anything
 * outside that alphabet is not a token this machine ever minted.
 */
const SHARE_TOKEN_SHAPE = /^[A-Za-z0-9_-]{16,128}$/;

export function shareTokenFromLocation() {
    for (const part of hashParts()) {
        if (part.startsWith('t=')) {
            const token = decodeURIComponent(part.slice(2));
            if (SHARE_TOKEN_SHAPE.test(token)) return token;
        }
    }
    return null;
}

export function hostIdFromLocation() {
    const fromPath = location.pathname.replace(/^\/+|\/+$/g, '');
    if (ID_SHAPE.test(fromPath)) return fromPath;

    const fromHash = hashParts()[0] || '';
    if (ID_SHAPE.test(fromHash)) {
        try {
            sessionStorage.setItem('mw-host-id', fromHash);
        } catch {
            /* storage refused; the fragment alone will have to do */
        }
        return fromHash;
    }

    try {
        const stored = sessionStorage.getItem('mw-host-id');
        if (stored && ID_SHAPE.test(stored)) return stored;
    } catch {
        /* no session storage */
    }

    try {
        const last = localStorage.getItem('mw-last-host');
        if (last && ID_SHAPE.test(last)) return last;
    } catch {
        /* no local storage */
    }
    return null;
}
