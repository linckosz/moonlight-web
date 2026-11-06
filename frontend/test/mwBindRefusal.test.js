/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect, vi } from 'vitest';
import { WebRtcDataChannel } from '../js/api/WebRtcDataChannel.js';
import { WebRtcMedia } from '../js/api/WebRtcMedia.js';
import {
    HOST_KEY_MISSING,
    IDENTITY_REFUSED,
    isBindRefusal,
    isBindRefusedClose,
} from '../js/util/pairingCrypto.js';

/**
 * A host that fails MW-BIND must end the launch with IDENTITY_REFUSED — the one
 * reason MoonlightApp._onTransportFailed treats as final. Any other message there
 * is an ordinary transport failure, and the chain then walks down to wss, which
 * carries the stream with no DTLS and so nothing to bind: the refusal would be
 * walked around. Seen on the bench 21/09/2026 — three refused offers, then a
 * stream over wss, and no word to the user.
 */
describe.each([
    ['WebRtcDataChannel', () => new WebRtcDataChannel('ws://test.invalid')],
    ['WebRtcMedia', () => new WebRtcMedia('ws://test.invalid')],
])('%s — an offer that fails MW-BIND', (_name, make) => {
    it('reports IDENTITY_REFUSED and never negotiates', async () => {
        const t = make();
        const onError = vi.fn();
        t.onError = onError;
        const createPc = vi.spyOn(t, '_createPeerConnection').mockImplementation(() => {});
        // A paired identity, and an offer that names another host: refused
        // before the signature is even looked at.
        t._mwBind = {
            hostPublicKey: 'AAAA',
            hostId: 'host-a',
            keyId: 'k',
            nonceB: new Uint8Array(32),
        };

        await t._handleSdpOffer({
            type: 'offer',
            sdp: 'v=0\r\na=fingerprint:sha-256 AA:BB\r\n',
            host_id: 'host-b',
            nonce: 'bm9uY2U=',
            sig: 'c2ln',
        });

        expect(onError).toHaveBeenCalledTimes(1);
        expect(onError.mock.calls[0][0].message).toBe(IDENTITY_REFUSED);
        // No DTLS state was created for the peer that failed to prove itself.
        expect(createPc).not.toHaveBeenCalled();
        expect(t.pc).toBeNull();
    });

    // A browser whose stored record holds its own key and none of the host's.
    const withoutHostKey = () => ({
        hostPublicKey: null,
        hostId: 'host-a',
        keyId: 'k',
        nonceB: new Uint8Array(32),
    });

    it('reports HOST_KEY_MISSING for a signed offer it holds no host key to check', async () => {
        const t = make();
        const onError = vi.fn();
        t.onError = onError;
        const createPc = vi.spyOn(t, '_createPeerConnection').mockImplementation(() => {});
        vi.spyOn(console, 'error').mockImplementation(() => {});
        t._mwBind = withoutHostKey();

        await t._handleSdpOffer({
            type: 'offer',
            sdp: 'v=0\r\na=fingerprint:sha-256 AA:BB\r\n',
            host_id: 'host-a',
            nonce: 'bm9uY2U=',
            sig: 'c2ln',
        });

        expect(onError).toHaveBeenCalledTimes(1);
        expect(onError.mock.calls[0][0].message).toBe(HOST_KEY_MISSING);
        expect(isBindRefusal(HOST_KEY_MISSING)).toBe(true);
        expect(createPc).not.toHaveBeenCalled();
        expect(t.pc).toBeNull();
        vi.restoreAllMocks();
    });

    it('still takes an unsigned offer, as the host machine is sent', async () => {
        const t = make();
        const onError = vi.fn();
        t.onError = onError;
        // Stands in for the negotiation, which is not what this looks at.
        // Kept apart: the error the missing rest of it raises closes the transport.
        const pc = { setRemoteDescription: vi.fn(async () => {}) };
        t.pc = pc;
        vi.stubGlobal(
            'RTCSessionDescription',
            class {
                constructor(init) {
                    Object.assign(this, init);
                }
            },
        );
        vi.spyOn(t, '_createPeerConnection').mockImplementation(() => {});
        vi.spyOn(console, 'log').mockImplementation(() => {});
        vi.spyOn(console, 'error').mockImplementation(() => {});
        vi.spyOn(console, 'warn').mockImplementation(() => {});
        t._mwBind = withoutHostKey();

        await t._handleSdpOffer({ type: 'offer', sdp: 'v=0\r\n' });

        expect(pc.setRemoteDescription).toHaveBeenCalledTimes(1);
        const reasons = onError.mock.calls.map((c) => c[0].message);
        expect(reasons).not.toContain(HOST_KEY_MISSING);
        expect(reasons).not.toContain(IDENTITY_REFUSED);
        vi.unstubAllGlobals();
        vi.restoreAllMocks();
    });
});

/**
 * The other half: the HOST refusing our key closes the signalling socket with
 * 1008. Seen 04/10/2026 on the UM790Pro's DEV after it changed port — a session
 * cookie (cookies ignore the port) bound to the key of the old origin. Every
 * WebRTC rung was refused, and the stream came up over wss with a doubled
 * pointer and no word to the user.
 */
describe('isBindRefusedClose', () => {
    it("is the host's MW-BIND refusal, and nothing else", () => {
        expect(isBindRefusedClose({ code: 1008, reason: 'pairing verification failed' })).toBe(
            true,
        );
        expect(isBindRefusedClose({ code: 1008, reason: 'something else' })).toBe(false);
        expect(isBindRefusedClose({ code: 1006, reason: '' })).toBe(false);
        expect(isBindRefusedClose(undefined)).toBe(false);
    });
});
