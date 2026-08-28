/*
 * MoonlightWeb — frontend TNR. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * Trickled candidates that arrive before there is anywhere to put them.
 *
 * This is not a corner case, it is the ordinary order of events. The host builds
 * its peer connection the moment the rendezvous opens the session and its local
 * addresses are ready within milliseconds, while this end still has a signature
 * to verify, an answer to make and an answer to sign before it has a remote
 * description to attach anything to. Measured against a real host: nine of ten
 * candidates arrived in that gap — every LAN address, every IPv6 address — and
 * every one of them was dropped on the floor.
 *
 * What was left was the single reflexive candidate, which arrives a second or
 * two later because it costs a round trip to a STUN server. A connection with a
 * perfectly good path over the local network was therefore staking everything on
 * the one address that has to come back in through a NAT: fine on a router with
 * hairpin, nothing at all from a guest's own network. The symptom was a page
 * sitting at "Opening a direct connection…" until it gave up.
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
const ice = (address) => ({
    candidate: `candidate:1 1 UDP 2114 ${address} 54034 typ host`,
    mid: '0',
});

let tunnelModule;

beforeEach(async () => {
    vi.stubGlobal('sessionStorage', memoryStorage());
    vi.stubGlobal('localStorage', memoryStorage());
    vi.resetModules();
    tunnelModule = await import('../../bootstrap/tunnel.js');
});

/** A Tunnel with a stand-in for the peer connection, which jsdom has none of. */
function tunnelWithFakePc(remoteDescription = null) {
    const t = new tunnelModule.Tunnel(HOST, 'https://example.test');
    const added = [];
    t._pc = {
        remoteDescription,
        addIceCandidate: async (c) => {
            added.push(c.candidate);
        },
    };
    return { t, added };
}

describe('candidates that arrive before the offer has been applied', () => {
    it('holds them instead of dropping them', async () => {
        const { t, added } = tunnelWithFakePc(null);

        await t._onRemoteCandidate(ice('192.168.1.66'));
        await t._onRemoteCandidate(ice('2a01:e0a:ac9:df50::1'));

        // Nothing could be added yet — that is not the bug. The bug was that
        // nothing was kept either.
        expect(added).toHaveLength(0);
        expect(t._earlyCandidates).toHaveLength(2);
    });

    it('hands them over once the offer is in, in the order they came', async () => {
        const { t, added } = tunnelWithFakePc(null);

        await t._onRemoteCandidate(ice('192.168.1.66'));
        await t._onRemoteCandidate(ice('10.0.0.4'));

        t._pc.remoteDescription = { type: 'offer' };
        await t._flushEarlyCandidates();

        expect(added.map((c) => c.split(' ')[4])).toEqual(['192.168.1.66', '10.0.0.4']);
        expect(t._earlyCandidates).toHaveLength(0);
    });

    it('passes a later one straight through', async () => {
        const { t, added } = tunnelWithFakePc({ type: 'offer' });

        await t._onRemoteCandidate(ice('82.67.150.202'));

        expect(added).toHaveLength(1);
        expect(t._earlyCandidates).toHaveLength(0);
    });

    // Flushed exactly once: a second flush must not replay what it already
    // handed over, or a renegotiation would re-add stale addresses.
    it('keeps nothing back after a flush', async () => {
        const { t, added } = tunnelWithFakePc(null);
        await t._onRemoteCandidate(ice('192.168.1.66'));

        t._pc.remoteDescription = { type: 'offer' };
        await t._flushEarlyCandidates();
        await t._flushEarlyCandidates();

        expect(added).toHaveLength(1);
    });

    // The queue is filled from the relay BEFORE the far end has been
    // authenticated, so whoever holds that session can push into it. Bounded far
    // above what a host emits and far below anything that costs memory.
    it('refuses to grow without limit before anyone has been verified', async () => {
        const { t } = tunnelWithFakePc(null);

        for (let i = 0; i < 500; i++) await t._onRemoteCandidate(ice(`10.0.0.${i % 250}`));

        expect(t._earlyCandidates.length).toBeLessThanOrEqual(64);
    });

    // A candidate can arrive before there is even a peer connection: the host
    // starts gathering when the session opens, and this end builds one only
    // after it has checked who it is talking to.
    it('holds one that arrives before there is a peer connection at all', async () => {
        const t = new tunnelModule.Tunnel(HOST, 'https://example.test');

        await t._onRemoteCandidate(ice('192.168.1.66'));

        expect(t._earlyCandidates).toHaveLength(1);
    });
});
