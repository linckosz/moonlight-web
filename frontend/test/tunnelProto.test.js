/*
 * MoonlightWeb — frontend TNR. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * The protocol revision the entry page announces to the introduction server,
 * and what it does when the server refuses it.
 *
 * The production server serves every installed version at once, so the page
 * says which revision it speaks (`?v=`) and the server answers a page it will
 * not serve with a code rather than a hang-up that looks like the network. The
 * three constants — here, in hub.go and in RendezvousClient.cpp — are kept in
 * step by hand; this file pins what the page sends and what it says.
 */

import { describe, it, expect, beforeEach, vi } from 'vitest';

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

/** A WebSocket that records its URL and lets a test feed it frames. */
class FakeWebSocket {
    static last = null;
    static OPEN = 1;
    constructor(url) {
        this.url = String(url);
        this.readyState = FakeWebSocket.OPEN;
        this.sent = [];
        FakeWebSocket.last = this;
    }
    send(s) {
        this.sent.push(s);
    }
    close() {}
    frame(obj) {
        this.onmessage({ data: JSON.stringify(obj) });
    }
}

let tunnelModule;

beforeEach(async () => {
    vi.stubGlobal('sessionStorage', memoryStorage());
    vi.stubGlobal('localStorage', memoryStorage());
    vi.stubGlobal('WebSocket', FakeWebSocket);
    FakeWebSocket.last = null;
    vi.resetModules();
    tunnelModule = await import('../../bootstrap/v1/tunnel.js');
});

describe('protocol revision', () => {
    it('is 1, and is sent on the peer socket beside the identifier', async () => {
        expect(tunnelModule.PROTO).toBe(1);

        const t = new tunnelModule.Tunnel(HOST, 'https://example.test');
        const pending = t.connect();
        pending.catch(() => {});
        // connect() awaits the identity before it opens the socket.
        await vi.waitFor(() => expect(FakeWebSocket.last).not.toBeNull());

        const url = new URL(FakeWebSocket.last.url);
        expect(url.protocol).toBe('wss:');
        expect(url.pathname).toBe('/v1/peer');
        expect(url.searchParams.get('id')).toBe(HOST);
        expect(url.searchParams.get('v')).toBe('1');

        FakeWebSocket.last.frame({ t: 'error', code: 'offline' });
        await expect(pending).rejects.toThrow('not online');
    });

    it('tells the person to reload when the server refuses this revision', async () => {
        const t = new tunnelModule.Tunnel(HOST, 'https://example.test');
        const pending = t.connect();
        await vi.waitFor(() => expect(FakeWebSocket.last).not.toBeNull());

        FakeWebSocket.last.frame({ t: 'error', code: 'unsupported_version' });
        await expect(pending).rejects.toThrow(/out of date — reload/);
    });

    it("learns the machine's revision from the ready frame, defaulting to 1", async () => {
        for (const [frame, want] of [
            [{ t: 'ready', v: 1 }, 1],
            [{ t: 'ready' }, 1], // a server that predates the field
            [{ t: 'ready', v: 'x' }, 1], // garbage is not a revision
        ]) {
            const t = new tunnelModule.Tunnel(HOST, 'https://example.test');
            const pending = t.connect();
            pending.catch(() => {});
            await vi.waitFor(() => expect(FakeWebSocket.last).not.toBeNull());

            FakeWebSocket.last.frame(frame);
            expect(t.hostProto).toBe(want);
            // ready is answered with the hello, whatever the revision.
            expect(FakeWebSocket.last.sent.map((s) => JSON.parse(s).d.type)).toEqual(['hello']);
            FakeWebSocket.last = null;
        }
    });
});
