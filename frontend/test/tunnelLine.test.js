/*
 * MoonlightWeb — frontend TNR. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * The signalling line going away under a working tunnel.
 *
 * Restarting the introduction server — a deploy, a reboot — drops every line
 * to it at once, the host's and the browser's. The host does nothing to its
 * tunnels when its own line drops; it was the page that took the tunnel down
 * with the line, and with it every session on the server's way out, streams on
 * the same LAN as their machine included. These pin the replacement: the
 * tunnel stays as long as the machine still answers on it, and goes down as
 * before when it does not.
 */
import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';

function memoryStorage() {
    const map = new Map();
    return {
        getItem: (k) => (map.has(k) ? map.get(k) : null),
        setItem: (k, v) => map.set(k, String(v)),
        removeItem: (k) => map.delete(k),
    };
}

// The pairing layer needs WebCrypto and IndexedDB, which is not what is under
// test: an identity that exists is all connect() needs to reach the socket.
vi.mock('../../bootstrap/v1/pairing.js', () => ({
    loadIdentity: async () => ({ keyId: 'k', publicKeyBase64: 'p' }),
    helloMessage: () => ({ type: 'hello' }),
    verifyOffer: async () => true,
    signAnswer: async () => ({}),
}));

const HOST = 'a'.repeat(26);
const FRAME_REQUEST = 0x01;
const FRAME_RESPONSE = 0x02;

class FakeWebSocket {
    static last = null;
    static OPEN = 1;
    constructor(url) {
        this.url = String(url);
        this.readyState = FakeWebSocket.OPEN;
        FakeWebSocket.last = this;
    }
    send() {}
    close() {}
    /** The handshake completing. */
    open() {
        this.onopen?.();
    }
    /** The server, or whatever sits in front of it, going away. */
    drop(code = 1001) {
        this.readyState = 3;
        this.onclose({ code, reason: '' });
    }
}

/** The data channel to the host: records what is sent, and can close. */
class FakeChannel {
    constructor() {
        this.readyState = 'open';
        this.sent = [];
    }
    send(bytes) {
        this.sent.push(bytes);
    }
    /** What the far end closing it looks like from here. */
    closeFromFarEnd() {
        this.readyState = 'closed';
        this.onclose();
    }
}

let tunnelModule;

beforeEach(async () => {
    vi.stubGlobal('sessionStorage', memoryStorage());
    vi.stubGlobal('localStorage', memoryStorage());
    vi.stubGlobal('WebSocket', FakeWebSocket);
    vi.resetModules();
    tunnelModule = await import('../../bootstrap/v1/tunnel.js');
});

afterEach(() => {
    vi.useRealTimers();
    vi.unstubAllGlobals();
});

/** A tunnel through connect(), up to an open channel, with its line and channel. */
async function openTunnel() {
    const t = new tunnelModule.Tunnel(HOST, 'https://example.test');
    const closed = [];
    t.onclosed = (why) => closed.push(why);
    const connecting = t.connect();
    await vi.waitFor(() => expect(FakeWebSocket.last).not.toBeNull());
    const line = FakeWebSocket.last;
    const channel = new FakeChannel();
    t._adoptChannel(channel);
    await connecting;
    vi.useFakeTimers();
    return { t, line, channel, closed };
}

/** The path of the request a frame carries, or null for any other frame. */
function requestPath(bytes) {
    if (bytes[0] !== FRAME_REQUEST) return null;
    const headLen = new DataView(bytes.buffer, bytes.byteOffset + 5).getUint32(0, false);
    return JSON.parse(new TextDecoder().decode(bytes.subarray(9, 9 + headLen))).p;
}

/** One frame from the host, as the channel delivers it. */
function frameFromHost(t, kind, id) {
    t._onChannelMessage(
        tunnelModule.encodeFrame(kind, id, tunnelModule.encodeHead({ s: 200, h: {} })),
    );
}

describe('the signalling line dropping under an open tunnel', () => {
    beforeEach(() => {
        FakeWebSocket.last = null;
    });

    it('keeps the tunnel while the machine still answers', async () => {
        const { t, line, channel, closed } = await openTunnel();

        line.drop();

        // The machine is asked something, on the channel.
        const asked = channel.sent.map(requestPath).filter(Boolean);
        expect(asked).toEqual(['/api/health']);

        frameFromHost(t, FRAME_RESPONSE, 1);
        await vi.advanceTimersByTimeAsync(10000);

        expect(closed).toEqual([]);
        expect(t.connected).toBe(true);
    });

    it('counts any frame from the machine, not only the answer to that question', async () => {
        const { t, line, closed } = await openTunnel();

        line.drop();
        // The body of a request made before the line went, still arriving.
        frameFromHost(t, 0x03, 7);
        await vi.advanceTimersByTimeAsync(10000);

        expect(closed).toEqual([]);
    });

    it('takes the tunnel down when the machine stays silent', async () => {
        const { line, closed } = await openTunnel();

        line.drop();
        await vi.advanceTimersByTimeAsync(4900);
        expect(closed).toEqual([]);

        await vi.advanceTimersByTimeAsync(200);
        expect(closed).toHaveLength(1);
        expect(closed[0]).toMatch(/silent/);
    });

    it('leaves it to the channel when the machine closes it meanwhile', async () => {
        const { line, channel, closed } = await openTunnel();

        line.drop();
        channel.closeFromFarEnd();
        await vi.advanceTimersByTimeAsync(10000);

        // Reported once, by the channel's close, not a second time by the check.
        expect(closed).toEqual(['the channel closed']);
    });
});

describe('the signalling line dropping before the channel opens', () => {
    beforeEach(() => {
        FakeWebSocket.last = null;
    });

    it('is still a failed connection', async () => {
        const t = new tunnelModule.Tunnel(HOST, 'https://example.test');
        const connecting = t.connect();
        await vi.waitFor(() => expect(FakeWebSocket.last).not.toBeNull());

        FakeWebSocket.last.drop();

        await expect(connecting).rejects.toThrow('The introduction server hung up.');
    });
});

/* A page that keeps calling a server that is down — lineKeeper behind a stream,
   the entry page's retries — reaches the console once per try. Only a line that
   was open has something to say about why it went. */
describe('what the console hears of a line closing', () => {
    let warn;

    beforeEach(() => {
        FakeWebSocket.last = null;
        warn = vi.spyOn(console, 'warn').mockImplementation(() => {});
    });

    afterEach(() => {
        warn.mockRestore();
    });

    const closedLines = () =>
        warn.mock.calls.map((c) => String(c[0])).filter((m) => m.includes('Signalling line'));

    async function dial(act) {
        const t = new tunnelModule.Tunnel(HOST, 'https://example.test');
        const connecting = t.connect();
        await vi.waitFor(() => expect(FakeWebSocket.last).not.toBeNull());
        act(FakeWebSocket.last);
        await connecting.catch(() => {});
    }

    it('says nothing of a line that never opened', async () => {
        await dial((ws) => {
            ws.onerror();
            ws.drop(1006);
        });
        expect(closedLines()).toEqual([]);
    });

    it('gives the code of a line that was open', async () => {
        await dial((ws) => {
            ws.open();
            ws.drop(1001);
        });
        expect(closedLines()).toEqual(['[MW] Signalling line closed: code=1001 reason=(none)']);
    });
});

/* What a restart of the introduction server looks like to a page that calls
   during it: the entry page keeps calling under these, and only these. */
describe('failures that pass by themselves', () => {
    beforeEach(() => {
        FakeWebSocket.last = null;
    });

    /** The error connect() ends with when the server does `act` to its line. */
    async function failureAfter(act) {
        const t = new tunnelModule.Tunnel(HOST, 'https://example.test');
        const connecting = t.connect();
        await vi.waitFor(() => expect(FakeWebSocket.last).not.toBeNull());
        act(FakeWebSocket.last);
        return connecting.then(
            () => null,
            (e) => e,
        );
    }

    const relayError = (code) => (ws) =>
        ws.onmessage({ data: JSON.stringify({ t: 'error', code }) });

    it('marks a server that hangs up', async () => {
        const e = await failureAfter((ws) => ws.drop());
        expect(e.transient).toBe(true);
    });

    it('marks a server that cannot be reached', async () => {
        const e = await failureAfter((ws) => ws.onerror());
        expect(e.message).toBe('Could not reach the introduction server.');
        expect(e.transient).toBe(true);
    });

    it('marks a machine not online, or gone, which may be its line coming back', async () => {
        expect((await failureAfter(relayError('offline'))).transient).toBe(true);
        expect((await failureAfter(relayError('host-gone'))).transient).toBe(true);
    });

    it('leaves a full machine and an outdated page as plain answers', async () => {
        const busy = await failureAfter(relayError('busy'));
        expect(busy.message).toMatch(/as many connections/);
        expect(busy.transient).toBeUndefined();
        expect((await failureAfter(relayError('unsupported_version'))).transient).toBeUndefined();
    });
});

/* Compression of the interface's files (v0.4.2): the page asks, the host may
   deflate, the page hands back the file as served. Same vector as
   tunnelFrame.test.js and backend/tests/test_tunnel_frame.cpp. */
describe('a deflated answer', () => {
    const PLAIN = 'console.log("MoonlightWeb"); '.repeat(3).trimEnd() + '\n';
    const VECTOR =
        '789c4bcecf2bcecf49d5cbc94fd750f2cdcfcfcbc94ccf28094f4d52d2b456482657920b005b0e1e51';
    const fromHex = (h) => new Uint8Array(h.match(/../g).map((b) => parseInt(b, 16)));

    beforeEach(() => {
        FakeWebSocket.last = null;
    });

    /** The head and id of the last request the page sent. */
    function lastRequest(channel) {
        const bytes = channel.sent.filter((b) => b[0] === FRAME_REQUEST).at(-1);
        const id = new DataView(bytes.buffer, bytes.byteOffset).getUint32(1, false);
        const headLen = new DataView(bytes.buffer, bytes.byteOffset + 5).getUint32(0, false);
        return { id, head: JSON.parse(new TextDecoder().decode(bytes.subarray(9, 9 + headLen))) };
    }

    function answer(t, id, head, body) {
        t._onChannelMessage(
            tunnelModule.encodeFrame(FRAME_RESPONSE, id, tunnelModule.encodeHead(head)),
        );
        // Split across BODY frames, as the host chunks it.
        for (let i = 0; i < body.length; i += 16)
            t._onChannelMessage(tunnelModule.encodeFrame(0x03, id, body.subarray(i, i + 16)));
        t._onChannelMessage(tunnelModule.encodeFrame(0x04, id, null));
    }

    it('asks for one, and inflates it back into the file', async () => {
        const { t, channel } = await openTunnel();
        vi.useRealTimers();
        const pending = t.fetch('/js/app.js');
        const { id, head } = lastRequest(channel);
        expect(head.z).toBe('deflate');

        answer(
            t,
            id,
            { s: 200, h: { 'Content-Type': 'application/javascript' }, z: 'deflate' },
            fromHex(VECTOR),
        );
        const response = await pending;
        expect(response.status).toBe(200);
        expect(response.headers.get('content-type')).toBe('application/javascript');
        expect(await response.text()).toBe(PLAIN);
    });

    it('takes the body as it is from a host that did not deflate it', async () => {
        const { t, channel } = await openTunnel();
        vi.useRealTimers();
        const pending = t.fetch('/js/app.js');
        const { id } = lastRequest(channel);

        answer(t, id, { s: 200, h: {} }, new TextEncoder().encode(PLAIN));
        expect(await (await pending).text()).toBe(PLAIN);
    });

    it('fails the request on a body that does not inflate', async () => {
        const { t, channel } = await openTunnel();
        vi.useRealTimers();
        const pending = t.fetch('/js/app.js');
        const { id } = lastRequest(channel);

        answer(t, id, { s: 200, h: {}, z: 'deflate' }, new TextEncoder().encode(PLAIN));
        await expect(pending).rejects.toThrow(/did not inflate/);
    });

    it('fails the request on an encoding it never asked for', async () => {
        const { t, channel } = await openTunnel();
        vi.useRealTimers();
        const pending = t.fetch('/js/app.js');
        const { id } = lastRequest(channel);

        answer(t, id, { s: 200, h: {}, z: 'br' }, new TextEncoder().encode(PLAIN));
        await expect(pending).rejects.toThrow(/br/);
    });

    it('does not ask where the browser cannot inflate', async () => {
        const { t, channel } = await openTunnel();
        vi.stubGlobal('DecompressionStream', undefined);
        t.fetch('/js/app.js').catch(() => {});
        expect(lastRequest(channel).head.z).toBeUndefined();
    });
});
