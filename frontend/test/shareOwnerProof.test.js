/*
 * MoonlightWeb — frontend TNR. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * The sharing board's proof that it is the browser its session was paired with.
 * The rows that hand out a guest's way in (a link and its PIN, the rights they
 * carry) are asked for a signature by this browser's pairing key over the host's
 * single-use challenge, wherever the host's line has not proved that key already.
 */
import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import { BackendClient } from '../js/api/BackendClient.js';
import * as pairing from '../js/util/pairingCrypto.js';
import { shareOwnerDigestInput } from '../js/util/pairingCrypto.js';

const hex = (bytes) => Array.from(bytes, (b) => b.toString(16).padStart(2, '0')).join('');

describe('the bytes a board request is signed over', () => {
    it('match the host’s, byte for byte', () => {
        // The same vector is pinned in backend/tests/test_owner_proof.cpp.
        expect(hex(shareOwnerDigestInput('H', new Uint8Array([1, 2, 3]), 'activate', 2))).toBe(
            '00000016' +
                '4d572d42494e442d76317c73686172652d6f776e6572' +
                '0000000148' +
                '00000003010203' +
                '000000086163746976617465' +
                '0000000132',
        );
    });
});

describe('a board request that hands out a guest’s way in', () => {
    let posts;

    const answer = (challenge) => {
        posts = [];
        vi.spyOn(BackendClient, 'post').mockImplementation(async (path, body) => {
            posts.push({ path, body });
            if (path === '/api/share/owner-proof') return challenge;
            return { ok: true };
        });
    };

    beforeEach(() => {
        vi.spyOn(pairing, 'signShareOwnerProof').mockResolvedValue('c2ln');
    });
    afterEach(() => vi.restoreAllMocks());

    it.each([
        ['activate', () => BackendClient.shareActivate(2, { ttl_secs: 3600, host_uuid: 'h' })],
        ['credentials', () => BackendClient.shareCredentials(3)],
        ['permissions', () => BackendClient.sharePermissions(4, { gamepad: true })],
    ])('%s carries the signature when the host asks for one', async (action, call) => {
        answer({ required: true, nonce: 'bm9uY2U=', host_id: 'host-id' });
        await call();
        expect(posts[0].path).toBe('/api/share/owner-proof');
        const sent = posts[1];
        expect(sent.path).toMatch(new RegExp(`/api/share/slots/\\d/${action}$`));
        expect(sent.body.owner_proof).toEqual({ nonce: 'bm9uY2U=', sig: 'c2ln' });
        const slot = Number(sent.path.split('/')[4]);
        expect(pairing.signShareOwnerProof).toHaveBeenCalledWith(
            'host-id',
            'bm9uY2U=',
            action,
            slot,
        );
    });

    it('keeps the rest of the body as it was', async () => {
        answer({ required: true, nonce: 'bm9uY2U=', host_id: 'host-id' });
        await BackendClient.shareActivate(2, { ttl_secs: 3600, host_uuid: 'h', app_id: 7 });
        expect(posts[1].body).toMatchObject({ ttl_secs: 3600, host_uuid: 'h', app_id: 7 });
        await BackendClient.sharePermissions(2, { gamepad: true, keyboardMouse: false });
        expect(posts[3].body).toMatchObject({ gamepad: true, keyboardMouse: false });
    });

    it('signs nothing where the line proved the key already', async () => {
        answer({ required: false });
        await BackendClient.shareActivate(2, { ttl_secs: 3600 });
        expect(pairing.signShareOwnerProof).not.toHaveBeenCalled();
        expect(posts[1].body.owner_proof).toBeUndefined();
    });

    it('sends the request unsigned when this browser has no key to sign with', async () => {
        // The host then refuses it, as it refuses this browser's streams.
        pairing.signShareOwnerProof.mockResolvedValue(null);
        answer({ required: true, nonce: 'bm9uY2U=', host_id: 'host-id' });
        await BackendClient.shareCredentials(2);
        expect(posts[1].body.owner_proof).toBeUndefined();
    });
});
