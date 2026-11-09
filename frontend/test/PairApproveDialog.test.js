/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';

// A device asks in by the QR code it shows: the page names it, asks for the code
// it shows, typed from its screen, and answers only through its own buttons.
vi.mock('../js/i18n/i18n.js', () => ({
    t: (key, vars) => (vars?.name ? `${key}:${vars.name}` : key),
}));
vi.mock('../js/api/BackendClient.js', () => ({
    BackendClient: {
        getPairRequest: vi.fn(),
        approvePairRequest: vi.fn(),
        refusePairRequest: vi.fn(),
    },
}));

import {
    normalizeCode,
    openPairApprove,
    pairRequestFromPath,
    whereFrom,
} from '../js/ui/PairApproveDialog.js';
import { BackendClient } from '../js/api/BackendClient.js';

const ID = 'AbCdEfGhIjKlMnOp';
const ASKING = {
    machine_name: 'MoonlightWeb TV (MiTV-MOSR4)',
    state: 'waiting',
    ready: true,
    host_id: 'host-key-id',
    expires_in: 290,
};

const $ = (sel) => document.querySelector(sel);
const settle = () => new Promise((r) => setTimeout(r, 0));
const refusal = (statusCode, error, extra = {}) =>
    Object.assign(new Error(error), { statusCode, responseBody: { error, ...extra } });
const type = (text) => {
    const field = $('[data-code]');
    field.value = text;
    field.dispatchEvent(new Event('input'));
};

beforeEach(() => {
    vi.clearAllMocks();
});

afterEach(() => {
    document.body.innerHTML = '';
});

describe('PairApproveDialog', () => {
    it('reads the request from the page a pairing link opens, and nothing else', () => {
        expect(pairRequestFromPath(`/pair-${ID}`)).toBe(ID);
        expect(pairRequestFromPath('/pair-short')).toBeNull();
        expect(pairRequestFromPath(`/pair-${ID}/x`)).toBeNull();
        expect(pairRequestFromPath('/admin')).toBeNull();
        expect(pairRequestFromPath(undefined)).toBeNull();
    });

    it('reads a typed code as the host does', () => {
        expect(normalizeCode(' hj-ds ')).toBe('HJDS');
        expect(normalizeCode('o1Il')).toBe('0111');
        expect(normalizeCode(undefined)).toBe('');
    });

    it('names the device and asks for its code, never showing one', async () => {
        BackendClient.getPairRequest.mockResolvedValue(ASKING);
        openPairApprove(ID, { stepMs: 1 });
        await settle();
        expect(BackendClient.getPairRequest).toHaveBeenCalledWith(ID);
        expect($('.pair-approve-lead').textContent).toBe(
            'devicePairing.asks:MoonlightWeb TV (MiTV-MOSR4)',
        );
        expect($('[data-code]').value).toBe('');
        expect(document.activeElement).toBe($('[data-code]'));
        // Yes waits for a whole code.
        expect($('[data-approve]').disabled).toBe(true);
        type('HJD');
        expect($('[data-approve]').disabled).toBe(true);
        type('HJDS');
        expect($('[data-approve]').disabled).toBe(false);
    });

    it('approves the code as typed, read as the host reads it', async () => {
        BackendClient.getPairRequest.mockResolvedValue(ASKING);
        BackendClient.approvePairRequest.mockResolvedValue({ status: 'ok' });
        const closed = openPairApprove(ID, { stepMs: 1 });
        await settle();
        type(' hj-ds');
        $('[data-approve]').click();
        await settle();
        expect(BackendClient.approvePairRequest).toHaveBeenCalledWith(ID, 'HJDS', 'host-key-id');
        expect($('.pair-approve-body').textContent).toContain('devicePairing.approved');
        $('[data-close]').click();
        await expect(closed).resolves.toBe('approved');
    });

    it('asks again after a wrong code, and gives up when the host does', async () => {
        BackendClient.getPairRequest.mockResolvedValue(ASKING);
        BackendClient.approvePairRequest
            .mockRejectedValueOnce(refusal(409, 'code_mismatch', { tries_left: 2 }))
            .mockRejectedValueOnce(refusal(409, 'too_many_tries'));
        openPairApprove(ID, { stepMs: 1 });
        await settle();
        type('ABCD');
        $('[data-approve]').click();
        await settle();
        expect($('[data-error]').hidden).toBe(false);
        expect($('[data-error]').textContent).toBe('devicePairing.wrongCode');
        expect($('[data-code]').value).toBe('');
        expect($('[data-approve]').disabled).toBe(true);
        expect($('[data-refuse]').disabled).toBe(false);

        type('ABCE');
        $('[data-approve]').click();
        await settle();
        expect($('.pair-approve-body').textContent).toBe('devicePairing.tooManyTries');
        expect($('[data-approve]')).toBeNull();
    });

    it('refuses, and a tap beside the question does not leave it unanswered', async () => {
        BackendClient.getPairRequest.mockResolvedValue(ASKING);
        BackendClient.refusePairRequest.mockResolvedValue({ status: 'ok' });
        const closed = openPairApprove(ID, { stepMs: 1 });
        await settle();
        $('.pair-approve-overlay').click();
        document.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape' }));
        expect($('.pair-approve')).not.toBeNull();
        $('[data-refuse]').click();
        await settle();
        expect(BackendClient.refusePairRequest).toHaveBeenCalledWith(ID, 'host-key-id');
        $('[data-close]').click();
        await expect(closed).resolves.toBe('refused');
    });

    it('waits for the device to send its half of the code', async () => {
        BackendClient.getPairRequest
            .mockResolvedValueOnce({ ...ASKING, ready: false })
            .mockResolvedValueOnce({ ...ASKING, ready: false })
            .mockResolvedValue(ASKING);
        openPairApprove(ID, { stepMs: 1 });
        await vi.waitFor(() => expect($('[data-code]')).not.toBeNull());
        expect(BackendClient.getPairRequest).toHaveBeenCalledTimes(3);
    });

    it('says why when the host turns the question away', async () => {
        BackendClient.getPairRequest.mockRejectedValue(refusal(410, 'expired'));
        openPairApprove(ID, { stepMs: 1 });
        await settle();
        expect($('.pair-approve-body').textContent).toBe('devicePairing.gone');
        expect($('[data-approve]')).toBeNull();

        document.body.innerHTML = '';
        BackendClient.getPairRequest.mockRejectedValue(refusal(403, 'not_allowed'));
        openPairApprove(ID, { stepMs: 1 });
        await settle();
        expect($('.pair-approve-body').textContent).toBe('devicePairing.notAllowed');

        document.body.innerHTML = '';
        BackendClient.getPairRequest.mockResolvedValue(ASKING);
        BackendClient.approvePairRequest.mockRejectedValue(refusal(403, 'bad_signature'));
        openPairApprove(ID, { stepMs: 1 });
        await settle();
        type('HJDS');
        $('[data-approve]').click();
        await settle();
        expect($('.pair-approve-body').textContent).toBe('devicePairing.noKey');

        document.body.innerHTML = '';
        BackendClient.approvePairRequest.mockRejectedValue(refusal(403, 'temporary_session'));
        openPairApprove(ID, { stepMs: 1 });
        await settle();
        type('HJDS');
        $('[data-approve]').click();
        await settle();
        expect($('.pair-approve-body').textContent).toBe('devicePairing.temporary');
    });

    it('says where a request from outside the network comes from, nothing more for one from it', async () => {
        const far = { ...ASKING, origin: 'internet', address: '203.0.113.7', city: 'Lyon' };
        BackendClient.getPairRequest.mockResolvedValue(far);
        openPairApprove(ID, { stepMs: 1 });
        await settle();
        expect($('.pair-approve-origin').textContent).toBe('devicePairing.fromElsewhere');
        expect($('[data-where]').textContent).toBe('203.0.113.7, Lyon');
        expect($('[data-where]').hidden).toBe(false);
        expect(BackendClient.getPairRequest).toHaveBeenCalledTimes(1);

        document.body.innerHTML = '';
        BackendClient.getPairRequest.mockResolvedValue({
            ...ASKING,
            origin: 'lan',
            address: '192.168.1.174',
        });
        openPairApprove(ID, { stepMs: 1 });
        await settle();
        expect($('[data-code]')).not.toBeNull();
        expect($('.pair-approve-from')).toBeNull();
    });

    it('fills in the city once the host knows it, and leaves the code as typed', async () => {
        const far = { ...ASKING, origin: 'internet', address: '2001:db8::7' };
        BackendClient.getPairRequest
            .mockResolvedValueOnce(far)
            .mockResolvedValueOnce(far)
            .mockResolvedValue({ ...far, city: 'Lyon' });
        openPairApprove(ID, { stepMs: 1 });
        await settle();
        expect($('[data-where]').textContent).toBe('2001:db8::7');
        type('HJ');
        await vi.waitFor(() => expect($('[data-where]').textContent).toBe('2001:db8::7, Lyon'));
        expect($('[data-code]').value).toBe('HJ');
        expect(document.activeElement).toBe($('[data-code]'));
        expect(BackendClient.getPairRequest).toHaveBeenCalledTimes(3);
    });

    it('looks for the city a few times only, and no more once the question is answered', async () => {
        const far = { ...ASKING, origin: 'internet', address: '203.0.113.7' };
        BackendClient.getPairRequest.mockResolvedValue(far);
        openPairApprove(ID, { stepMs: 1 });
        await vi.waitFor(() => expect(BackendClient.getPairRequest).toHaveBeenCalledTimes(6));
        await new Promise((r) => setTimeout(r, 20));
        expect(BackendClient.getPairRequest).toHaveBeenCalledTimes(6);
        expect($('[data-where]').textContent).toBe('203.0.113.7');

        document.body.innerHTML = '';
        vi.clearAllMocks();
        BackendClient.getPairRequest.mockResolvedValue(far);
        BackendClient.refusePairRequest.mockResolvedValue({ status: 'ok' });
        openPairApprove(ID, { stepMs: 5 });
        await settle();
        $('[data-refuse]').click();
        await new Promise((r) => setTimeout(r, 40));
        expect(BackendClient.getPairRequest).toHaveBeenCalledTimes(1);
    });

    it('says an address only as the host names one, and nothing when it knows nothing', async () => {
        expect(whereFrom({ address: '203.0.113.7', city: 'Lyon' })).toBe('203.0.113.7, Lyon');
        expect(whereFrom({ address: '2001:db8::7' })).toBe('2001:db8::7');
        // A line ICE has not named yet has a label, not an address.
        expect(whereFrom({ address: 'tunnel:aaaaaaaaaaaaaaaa', city: 'Lyon' })).toBe('Lyon');
        expect(whereFrom({ address: 'tunnel:aaaaaaaaaaaaaaaa' })).toBe('');
        expect(whereFrom({})).toBe('');

        BackendClient.getPairRequest.mockResolvedValue({
            ...ASKING,
            origin: 'internet',
            address: 'tunnel:aaaaaaaaaaaaaaaa',
        });
        openPairApprove(ID, { stepMs: 1 });
        await settle();
        expect($('.pair-approve-origin')).not.toBeNull();
        expect($('[data-where]').hidden).toBe(true);
    });
});
