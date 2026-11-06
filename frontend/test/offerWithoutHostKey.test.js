/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 *
 * A page that is not on the host machine takes no offer it has no host key to
 * check, signed or not: the launch ends with HOST_KEY_MISSING, and the PIN
 * brings the key. The host machine and a share-link guest are as before.
 */

// A page reached by a public name, as through the tunnel.
// @vitest-environment-options {"url": "https://stream.example.test/"}

import { describe, it, expect, vi, afterEach } from 'vitest';
import { WebRtcDataChannel } from '../js/api/WebRtcDataChannel.js';
import { WebRtcMedia } from '../js/api/WebRtcMedia.js';
import {
    HOST_KEY_MISSING,
    isPrivateLanHostname,
    mayTakeUncheckedOffer,
    noteLocalPrivilege,
} from '../js/util/pairingCrypto.js';

const withoutHostKey = () => ({
    hostPublicKey: null,
    hostId: 'host-a',
    keyId: 'k',
    nonceB: new Uint8Array(32),
});

const signedOffer = {
    type: 'offer',
    sdp: 'v=0\r\na=fingerprint:sha-256 AA:BB\r\n',
    host_id: 'host-a',
    nonce: 'bm9uY2U=',
    sig: 'c2ln',
};
const unsignedOffer = { type: 'offer', sdp: 'v=0\r\n' };

afterEach(() => {
    noteLocalPrivilege(false);
    vi.unstubAllGlobals();
    vi.restoreAllMocks();
});

describe('which page may take an offer it cannot check', () => {
    it('is the host machine by its own address, whatever the host says', () => {
        for (const h of ['localhost', '127.0.0.1', '::1', '[::1]', 'LOCALHOST']) {
            expect(mayTakeUncheckedOffer(h)).toBe(true);
        }
    });

    it('is a private LAN address only with local privilege there', () => {
        for (const h of ['192.168.1.66', '10.0.0.5', '172.16.0.1', '172.31.255.1', '[fd12::1]']) {
            noteLocalPrivilege(false);
            expect(mayTakeUncheckedOffer(h)).toBe(false);
            noteLocalPrivilege(true);
            expect(mayTakeUncheckedOffer(h)).toBe(true);
        }
    });

    it('is never a public address or a name, local privilege or not', () => {
        noteLocalPrivilege(true);
        for (const h of ['stream.example.test', '8.8.8.8', '172.32.0.1', '[2001:db8::1]', '']) {
            expect(mayTakeUncheckedOffer(h)).toBe(false);
        }
    });

    it('reads private LAN literals and nothing else', () => {
        expect(isPrivateLanHostname('192.168.0.1')).toBe(true);
        expect(isPrivateLanHostname('[fe80::1]')).toBe(true);
        expect(isPrivateLanHostname('192.169.0.1')).toBe(false);
        expect(isPrivateLanHostname('my.192.168.1.1.example')).toBe(false);
        expect(isPrivateLanHostname('fdroid.example')).toBe(false);
    });

    it('defaults to this page, here on a public name', () => {
        noteLocalPrivilege(true);
        expect(mayTakeUncheckedOffer()).toBe(false);
    });
});

describe.each([
    ['WebRtcDataChannel', () => new WebRtcDataChannel('ws://test.invalid')],
    ['WebRtcMedia', () => new WebRtcMedia('ws://test.invalid')],
])('%s — off the host machine, without the host key', (_name, make) => {
    const arrange = (mwBind) => {
        const t = make();
        const onError = vi.fn();
        t.onError = onError;
        const createPc = vi.spyOn(t, '_createPeerConnection').mockImplementation(() => {});
        vi.spyOn(console, 'error').mockImplementation(() => {});
        vi.spyOn(console, 'log').mockImplementation(() => {});
        t._mwBind = mwBind;
        return { t, onError, createPc };
    };

    it.each([
        ['an unsigned offer, with a key of its own', withoutHostKey, unsignedOffer],
        ['an unsigned offer, with no key at all', () => null, unsignedOffer],
        ['a signed offer', withoutHostKey, signedOffer],
    ])('refuses %s, with HOST_KEY_MISSING', async (_what, mwBind, offer) => {
        const { t, onError, createPc } = arrange(mwBind());

        await t._handleSdpOffer({ ...offer });

        expect(onError).toHaveBeenCalledTimes(1);
        expect(onError.mock.calls[0][0].message).toBe(HOST_KEY_MISSING);
        expect(createPc).not.toHaveBeenCalled();
        expect(t.pc).toBeNull();
    });

    it('refuses it even with local privilege said, the page being on a public name', async () => {
        noteLocalPrivilege(true);
        const { t, onError } = arrange(withoutHostKey());

        await t._handleSdpOffer({ ...unsignedOffer });

        expect(onError.mock.calls[0][0].message).toBe(HOST_KEY_MISSING);
    });

    it("still takes a share-link guest's unsigned offer", async () => {
        const { t, onError } = arrange(undefined);
        t._unpairedGuest = true;
        const pc = { setRemoteDescription: vi.fn(async () => {}) };
        t.pc = pc;
        vi.spyOn(console, 'warn').mockImplementation(() => {});
        vi.stubGlobal(
            'RTCSessionDescription',
            class {
                constructor(init) {
                    Object.assign(this, init);
                }
            },
        );

        await t._handleSdpOffer({ ...unsignedOffer });

        expect(pc.setRemoteDescription).toHaveBeenCalledTimes(1);
        const reasons = onError.mock.calls.map((c) => c[0].message);
        expect(reasons).not.toContain(HOST_KEY_MISSING);
    });
});
