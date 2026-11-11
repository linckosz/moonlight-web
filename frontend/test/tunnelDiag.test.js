/*
 * MoonlightWeb — frontend TNR. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * The report a failed tunnel sends its machine (bootstrap/v1/tunnel.js,
 * diagMessage; issue #48). A car's browser reached the introduction server and
 * then nothing reached its machine, and nothing on either side said where it
 * stopped. Pinned down: the report carries kinds and numbers, never an address;
 * it counts only the candidates that actually went out; and it is sent once,
 * only after this page's signed answer went out, never once the channel is up.
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

const HOST = 'a'.repeat(26);

let tunnelModule;

beforeEach(async () => {
    vi.stubGlobal('sessionStorage', memoryStorage());
    vi.stubGlobal('localStorage', memoryStorage());
    vi.resetModules();
    tunnelModule = await import('../../bootstrap/v1/tunnel.js');
});

/** A stats report as Chrome hands it over: a Map of reports by id. */
function stats(...reports) {
    return new Map(reports.map((r, i) => [`r${i}`, r]));
}

/** A Tunnel with stand-ins for the line and the peer connection. */
function tunnelOnLine({ getStats = async () => stats() } = {}) {
    const t = new tunnelModule.Tunnel(HOST, 'https://example.test');
    const sent = [];
    t._ws = { readyState: WebSocket.OPEN, send: (text) => sent.push(JSON.parse(text)) };
    t._pc = {
        iceConnectionState: 'failed',
        connectionState: 'failed',
        iceGatheringState: 'complete',
        getStats,
    };
    const diags = () => sent.filter((f) => f.d?.type === 'diag').map((f) => f.d);
    return { t, diags };
}

describe('the kind of a candidate', () => {
    const candidateKind = (c) => tunnelModule.candidateKind(c);

    it('is its type, protocol and address family', () => {
        expect(candidateKind({ type: 'host', protocol: 'udp', address: '192.168.1.20' })).toBe(
            'host/udp/v4',
        );
        expect(candidateKind({ type: 'srflx', protocol: 'udp', address: '2001:db8::7' })).toBe(
            'srflx/udp/v6',
        );
        expect(
            candidateKind({ type: 'host', protocol: 'udp', address: '4c1e0a52-1b8e.local' }),
        ).toBe('host/udp/mdns');
        expect(candidateKind({ type: 'host', protocol: 'tcp', address: '10.0.0.4' })).toBe(
            'host/tcp/v4',
        );
    });

    it('reads the address from the line when the browser leaves the field out', () => {
        expect(
            candidateKind({
                type: 'host',
                protocol: 'udp',
                candidate: 'candidate:1 1 udp 2113937151 192.168.1.20 47212 typ host',
            }),
        ).toBe('host/udp/v4');
    });

    it('is nothing for what it does not know', () => {
        expect(candidateKind(null)).toBeNull();
        expect(candidateKind({ type: 'weird', protocol: 'udp', address: '1.2.3.4' })).toBeNull();
        expect(candidateKind({ type: 'host', protocol: 'sctp', address: '1.2.3.4' })).toBeNull();
        expect(candidateKind({ type: 'host', protocol: 'udp' })).toBeNull();
    });
});

describe('the ICE counters', () => {
    it('adds up the pairs’ checks and reads nothing else', async () => {
        const pc = {
            getStats: async () =>
                stats(
                    {
                        type: 'candidate-pair',
                        requestsSent: 9,
                        responsesReceived: 0,
                        requestsReceived: 1,
                    },
                    { type: 'candidate-pair', requestsSent: 5, responsesReceived: 0 },
                    { type: 'local-candidate', address: '192.168.1.20', port: 47212 },
                    { type: 'remote-candidate', address: '203.0.113.9', port: 3478, url: 'x' },
                ),
        };
        expect(await tunnelModule.checkCounts(pc)).toEqual({ sent: 14, answered: 0, received: 1 });
    });

    it('goes without them when the browser does not answer in time', async () => {
        const pc = { getStats: () => new Promise(() => {}) };
        expect(await tunnelModule.checkCounts(pc)).toBeNull();
    });

    it('goes without them when the browser refuses', async () => {
        const pc = {
            getStats: async () => {
                throw new Error('closed');
            },
        };
        expect(await tunnelModule.checkCounts(pc)).toBeNull();
    });
});

describe('the report', () => {
    it('carries kinds and numbers, never an address', async () => {
        const { t, diags } = tunnelOnLine({
            getStats: async () =>
                stats({
                    type: 'candidate-pair',
                    requestsSent: 14,
                    responsesReceived: 0,
                    requestsReceived: 0,
                }),
        });
        t._answeredAt = performance.now() - 9800;
        t._sentKinds.set('host/udp/v4', 1);
        t._remoteCount = 6;

        await t._reportNoRoute('failed');

        expect(diags()).toHaveLength(1);
        const d = diags()[0];
        expect(d).toMatchObject({
            type: 'diag',
            why: 'failed',
            ice: 'failed',
            connection: 'failed',
            gathering: 'complete',
            cand: { 'host/udp/v4': 1 },
            recv: 6,
            checks: { sent: 14, answered: 0, received: 0 },
        });
        expect(d.ms).toBeGreaterThanOrEqual(9800);
        expect(JSON.stringify(d)).not.toMatch(/\d+\.\d+\.\d+\.\d+|::/);
    });

    it('is not sent before this page’s answer went out', async () => {
        const { t, diags } = tunnelOnLine();

        await t._reportNoRoute('deadline');

        expect(diags()).toHaveLength(0);
    });

    it('is sent once per connection', async () => {
        const { t, diags } = tunnelOnLine();
        t._answeredAt = performance.now();

        await t._reportNoRoute('deadline');
        await t._reportNoRoute('failed');

        expect(diags()).toHaveLength(1);
        expect(diags()[0].why).toBe('deadline');
    });

    it('is not sent once the channel is up', async () => {
        const { t, diags } = tunnelOnLine();
        t._answeredAt = performance.now();
        t._dc = { readyState: 'open' };

        await t._reportNoRoute('deadline');

        expect(diags()).toHaveLength(0);
    });

    it('caps what it counts', () => {
        const message = tunnelModule.diagMessage({
            why: 'failed',
            ice: 'failed',
            connection: 'failed',
            gathering: 'complete',
            ms: -5,
            sent: new Map([['host/udp/v4', 5000]]),
            received: 5000,
            checks: null,
        });
        expect(message.cand['host/udp/v4']).toBe(999);
        expect(message.recv).toBe(999);
        expect(message.ms).toBe(0);
        expect(message).not.toHaveProperty('checks');
    });
});

describe('the line', () => {
    it('says whether a signalling message went out', () => {
        const t = new tunnelModule.Tunnel(HOST, 'https://example.test');
        expect(t._sendSignal({ type: 'ice' })).toBe(false);
        t._ws = { readyState: WebSocket.OPEN, send: () => {} };
        expect(t._sendSignal({ type: 'ice' })).toBe(true);
    });
});
