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

/** How long a request may wait for its first byte before it is given up on. */
const REQUEST_TIMEOUT_MS = 30000;

/** How long the whole connect sequence may take before it is called failed. */
const CONNECT_TIMEOUT_MS = 25000;

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
 * constructs is ignored. So this is the user agent for the far end.
 *
 * It is kept in sessionStorage, and the choice of sessionStorage over
 * localStorage is deliberate rather than incidental. On a direct connection the
 * session cookie is HttpOnly, which puts it out of reach of any script on the
 * page. Nothing here can reproduce that — a jar the page maintains is a jar the
 * page can read — so the question is only how long the credential sits at rest.
 * sessionStorage answers "until this tab closes", which is what it takes for a
 * reload to keep you signed in, and no longer.
 *
 * The cost, stated rather than hidden: "stay signed in on this device" does not
 * survive closing the tab for someone arriving through the rendezvous. Making it
 * survive would mean parking a long-lived credential in localStorage, readable
 * by whatever runs on this origin — the same "steal once, use indefinitely"
 * exposure the pairing key is generated non-extractable to avoid.
 */
class CookieJar {
    constructor(hostId) {
        this.key = `mw-jar:${hostId}`;
        this.jar = new Map();
        try {
            const saved = sessionStorage.getItem(this.key);
            if (saved) this.jar = new Map(JSON.parse(saved));
        } catch {
            /* no storage, or a shape we no longer write: start empty */
        }
    }

    _save() {
        try {
            sessionStorage.setItem(this.key, JSON.stringify([...this.jar]));
        } catch {
            /* the jar still works for this page; only the reload loses it */
        }
    }

    header() {
        if (this.jar.size === 0) return null;
        return [...this.jar].map(([k, v]) => `${k}=${v}`).join('; ');
    }

    /**
     * Take what a Set-Cookie says, and no more than that.
     *
     * Attributes are read only far enough to know whether the cookie is being
     * deleted. Path and Domain are ignored on purpose: this jar serves exactly
     * one host over one channel, so there is nothing to scope them against, and
     * pretending to honour them would be a fiction that hides bugs.
     */
    absorb(setCookie) {
        for (const line of Array.isArray(setCookie) ? setCookie : [setCookie]) {
            if (!line) continue;
            const [pair, ...attrs] = line.split(';');
            const eq = pair.indexOf('=');
            if (eq <= 0) continue;
            const name = pair.slice(0, eq).trim();
            const value = pair.slice(eq + 1).trim();

            const expired = attrs.some((a) => {
                const t = a.trim().toLowerCase();
                if (t.startsWith('max-age=')) return parseInt(t.slice(8), 10) <= 0;
                if (t.startsWith('expires=')) return new Date(t.slice(8)).getTime() <= Date.now();
                return false;
            });
            if (expired || value === '') this.jar.delete(name);
            else this.jar.set(name, value);
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

        this._pc = new RTCPeerConnection({
            iceServers: [{ urls: 'stun:stun.l.google.com:19302' }],
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

    async _onRemoteCandidate(msg) {
        if (!this._pc?.remoteDescription) return;
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

/**
 * The identifier this page is bound to, or null.
 *
 * Three places, in order, and each exists for a reason. The path is the
 * canonical address people bookmark and share. The fragment is what the
 * bootstrap moves it to before handing over, so the application keeps a URL it
 * can reload. Session storage is the backstop for the moment the application's
 * own routing rewrites the URL and drops the fragment — it is per tab, so two
 * tabs can hold two different machines at once.
 */
export function hostIdFromLocation() {
    const fromPath = location.pathname.replace(/^\/+|\/+$/g, '');
    if (/^[0-9a-z]{26}$/.test(fromPath)) return fromPath;

    const fromHash = location.hash.replace(/^#/, '');
    if (/^[0-9a-z]{26}$/.test(fromHash)) {
        try {
            sessionStorage.setItem('mw-host-id', fromHash);
        } catch {
            /* storage refused; the fragment alone will have to do */
        }
        return fromHash;
    }

    try {
        const stored = sessionStorage.getItem('mw-host-id');
        if (stored && /^[0-9a-z]{26}$/.test(stored)) return stored;
    } catch {
        /* no session storage */
    }
    return null;
}
