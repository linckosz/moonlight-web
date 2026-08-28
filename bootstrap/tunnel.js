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

/** How long a request may wait for its first byte before it is given up on. */
const REQUEST_TIMEOUT_MS = 30000;

/** How long the whole connect sequence may take before it is called failed. */
const CONNECT_TIMEOUT_MS = 25000;
/** How many of the host's candidates to hold before its offer arrives. */
const MAX_EARLY_CANDIDATES = 64;

export function encodeFrame(kind, id, payload) {
    const body = payload ?? new Uint8Array(0);
    const out = new Uint8Array(5 + body.length);
    const view = new DataView(out.buffer);
    out[0] = kind;
    view.setUint32(1, id, false);
    out.set(body, 5);
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

        this._ws = null;
        this._pc = null;
        this._dc = null;
        this._cookies = new CookieJar(hostId);
        // Candidates the host sent before there was anywhere to put them. See
        // _onRemoteCandidate: nearly all of them arrive that early.
        this._earlyCandidates = [];

        this._nextId = 1;
        this._pending = new Map(); // request id → { resolve, reject, head, chunks }
        this._sockets = new Map(); // socket id → TunnelSocket

        /** Called with a short status word so a page can say what is happening. */
        this.onstatus = null;
        /** Called when the channel goes away for good. */
        this.onclosed = null;
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
            const deadline = setTimeout(
                () => reject(new Error('Your machine did not answer in time.')),
                CONNECT_TIMEOUT_MS,
            );
            const settle = (err) => {
                clearTimeout(deadline);
                if (err) reject(err);
                else resolve();
            };

            const wsUrl = new URL('/v1/peer', this.origin);
            wsUrl.protocol = wsUrl.protocol === 'http:' ? 'ws:' : 'wss:';
            wsUrl.searchParams.set('id', this.hostId);

            this._ws = new WebSocket(wsUrl);
            this._ws.onmessage = (ev) => this._onRelayFrame(ev.data, settle);
            this._ws.onerror = () => settle(new Error('Could not reach the introduction server.'));
            this._ws.onclose = (ev) => {
                // The code and reason are the only account of WHY the line went,
                // and this is the failure a user reports as "it just stops".
                console.warn(
                    `[MW] Signalling line closed: code=${ev.code} reason=${ev.reason || '(none)'}`,
                );
                if (!this.connected) {
                    settle(new Error('The introduction server hung up.'));
                    return;
                }
                // The line dropped under a working tunnel. The host treats that
                // as the browser leaving and takes its side down, so waiting for
                // the data channel to notice would only add a silent gap first.
                this._teardown('the introduction server hung up');
            };
            this._onChannelOpen = settle;
        });
    }

    _sendSignal(payload) {
        if (this._ws?.readyState === WebSocket.OPEN)
            this._ws.send(JSON.stringify({ t: 'msg', d: payload }));
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
            };
            settle(new Error(said[frame.code] || 'The introduction server refused.'));
            return;
        }
        if (frame.t === 'ready') {
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
            if (ev.candidate)
                this._sendSignal({
                    type: 'ice',
                    candidate: ev.candidate.candidate,
                    mid: ev.candidate.sdpMid,
                });
        };
        this._pc.ondatachannel = (ev) => this._adoptChannel(ev.channel);
        this._pc.onconnectionstatechange = () => {
            const state = this._pc.connectionState;
            if (state === 'failed' || state === 'closed') this._teardown('the connection ended');
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
        this._sendSignal({ type: 'sdp', sdp: answer.sdp, sig });
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
        };
        if (this._dc.readyState === 'open') this._dc.onopen();
    }

    _teardown(why) {
        for (const [, socket] of this._sockets) socket._closed(why);
        this._sockets.clear();
        for (const [, waiter] of this._pending) waiter.reject(new Error(why));
        this._pending.clear();
        if (this.onclosed) this.onclosed(why);
    }

    _sendFrame(kind, id, payload) {
        if (this._dc?.readyState !== 'open') return;
        this._dc.send(encodeFrame(kind, id, payload));
    }

    _onChannelMessage(bytes) {
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

    _buildResponse(waiter) {
        const head = waiter.head || { s: 502, h: {} };
        let total = 0;
        for (const c of waiter.chunks) total += c.length;
        const body = new Uint8Array(total);
        let offset = 0;
        for (const c of waiter.chunks) {
            body.set(c, offset);
            offset += c.length;
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
        if (!this.connected) throw new Error('the tunnel is not open');

        const url = new URL(typeof input === 'string' ? input : input.url, location.origin);
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
        const payload = encodeHead(
            {
                m: (init.method || 'GET').toUpperCase(),
                p: url.pathname + url.search,
                h: headers,
            },
            body,
        );

        return new Promise((resolve, reject) => {
            const timer = setTimeout(() => {
                this._pending.delete(id);
                reject(new Error('the host did not answer'));
            }, REQUEST_TIMEOUT_MS);
            this._pending.set(id, { resolve, reject, timer, head: null, chunks: [] });
            this._sendFrame(FRAME_REQUEST, id, payload);
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
 */
const LANDING_SHAPE = /^\/[A-Za-z0-9_-]{1,32}$/;

export function landingPathFromLocation() {
    for (const part of hashParts()) {
        if (part.startsWith('p=')) {
            const path = decodeURIComponent(part.slice(2));
            if (LANDING_SHAPE.test(path)) return path;
        }
    }
    return null;
}

/**
 * The invitation token this link carries, or null.
 *
 * An owner shares a slot of their session and the guest arrives here holding
 * this. It rides beside the landing path rather than inside it, and the
 * difference is the whole point: the token never appears in a URL path, at any
 * step, so no navigation can ever carry it to the introduction server. A path
 * would be answered from the cache by the service worker in the ordinary case —
 * but "the ordinary case" is not a property to rest a credential on, and the one
 * time that worker is missing is the one time the token would be in a request
 * line somebody else keeps.
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
