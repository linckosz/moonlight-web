/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';

// A TV on plain HTTP asks in by the QR code it shows (issue #42). The
// administrator lets it in as before; a paired device that is not the
// administrator lets it in for a few hours, and says so on its button.
vi.mock('../js/i18n/i18n.js', () => ({
    t: (key, vars) => (vars ? `${key}:${JSON.stringify(vars)}` : key),
}));
vi.mock('../js/api/LanHttpApi.js', () => ({
    lanHttpRequest: vi.fn(),
    lanHttpApprove: vi.fn(),
    lanHttpRefuse: vi.fn(),
}));

import { openTvPairApprove, tvPairRequestFromPath } from '../js/ui/TvPairApproveDialog.js';
import { lanHttpApprove, lanHttpRefuse, lanHttpRequest } from '../js/api/LanHttpApi.js';

const ID = 'AbCdEfGhIjKlMnOpQrStUv';
const BY_ADMIN = { address: '192.168.1.174', expires_in: 290 };
const SHORT = {
    address: '192.168.1.174',
    expires_in: 290,
    short: true,
    host_id: 'host-key-id',
    ends_in: 8 * 3600,
};

const $ = (sel) => document.querySelector(sel);
const settle = () => new Promise((r) => setTimeout(r, 0));
const refusal = (statusCode, error) =>
    Object.assign(new Error(error), { statusCode, responseBody: { error } });
const type = (text) => {
    const field = $('[data-code]');
    field.value = text;
    field.dispatchEvent(new Event('input'));
};
const hints = () =>
    Array.from(document.querySelectorAll('.pair-approve-hint'), (p) => p.textContent);

beforeEach(() => {
    vi.clearAllMocks();
});

afterEach(() => {
    document.body.innerHTML = '';
});

describe('TvPairApproveDialog', () => {
    it('reads the request a TV draws, of 16 to 22 characters', () => {
        expect(tvPairRequestFromPath(`/tv-pair-${ID}`)).toBe(ID);
        expect(tvPairRequestFromPath('/tv-pair-AbCdEfGhIjKlMnOp')).toBe('AbCdEfGhIjKlMnOp');
        expect(tvPairRequestFromPath(`/tv-pair-${ID}A`)).toBeNull();
        expect(tvPairRequestFromPath('/tv-pair-short')).toBeNull();
    });

    it('lets the administrator answer as before', async () => {
        lanHttpRequest.mockResolvedValue(BY_ADMIN);
        lanHttpApprove.mockResolvedValue({ status: 'approved' });
        openTvPairApprove(ID);
        await settle();
        expect(hints()).toEqual(['tvPairing.hint']);
        expect($('[data-approve]').textContent).toBe('devicePairing.approve');
        type('hjds4x');
        $('[data-approve]').click();
        await settle();
        expect(lanHttpApprove).toHaveBeenCalledWith(ID, 'HJDS4X', BY_ADMIN);
        expect($('.pair-approve-body').textContent).toContain('tvPairing.approved');
    });

    it('says for how long a paired device lets the TV in, and signs for it', async () => {
        lanHttpRequest.mockResolvedValue(SHORT);
        lanHttpApprove.mockResolvedValue({ status: 'approved' });
        openTvPairApprove(ID);
        await settle();
        const [first, second] = hints();
        expect(first).toMatch(/^tvPairing\.shortHint:/);
        const vars = JSON.parse(first.slice('tvPairing.shortHint:'.length));
        expect(vars.hours).toBe(8);
        expect(vars.until).toMatch(/\d/);
        expect(second).toBe('tvPairing.shortHintEarlier');
        expect($('[data-approve]').textContent).toBe('tvPairing.shortApprove:{"hours":8}');
        type('HJDS4X');
        $('[data-approve]').click();
        await settle();
        // The request goes along: what the device signs for (host_id).
        expect(lanHttpApprove).toHaveBeenCalledWith(ID, 'HJDS4X', SHORT);
    });

    it('never says less than an hour', async () => {
        lanHttpRequest.mockResolvedValue({ ...SHORT, ends_in: 600 });
        openTvPairApprove(ID);
        await settle();
        expect($('[data-approve]').textContent).toBe('tvPairing.shortApprove:{"hours":1}');
    });

    it('signs a no as it signs a yes', async () => {
        lanHttpRequest.mockResolvedValue(SHORT);
        lanHttpRefuse.mockResolvedValue({ status: 'refused' });
        openTvPairApprove(ID);
        await settle();
        $('[data-refuse]').click();
        await settle();
        expect(lanHttpRefuse).toHaveBeenCalledWith(ID, SHORT);
    });

    it('gives one sentence for every reason a device may not let the TV in', async () => {
        lanHttpRequest.mockRejectedValue(refusal(403, 'not_allowed_short'));
        openTvPairApprove(ID);
        await settle();
        expect($('.pair-approve-body').textContent).toBe('tvPairing.notAllowedShort');
        expect(lanHttpApprove).not.toHaveBeenCalled();
    });

    it('tells a device paired before v0.5.0 to pair again, after its answer', async () => {
        lanHttpRequest.mockResolvedValue(SHORT);
        lanHttpApprove.mockRejectedValue(refusal(403, 'reappair_needed'));
        lanHttpRefuse.mockRejectedValue(refusal(403, 'reappair_needed'));
        openTvPairApprove(ID);
        await settle();
        type('HJDS4X');
        $('[data-approve]').click();
        await settle();
        expect($('.pair-approve-body').textContent).toBe('tvPairing.reappairNeeded');

        document.body.innerHTML = '';
        openTvPairApprove(ID);
        await settle();
        $('[data-refuse]').click();
        await settle();
        expect($('.pair-approve-body').textContent).toBe('tvPairing.reappairNeeded');
    });

    it('keeps the question after a wrong code', async () => {
        lanHttpRequest.mockResolvedValue(SHORT);
        lanHttpApprove.mockRejectedValue(refusal(400, 'wrong_code'));
        openTvPairApprove(ID);
        await settle();
        type('HJDS4Y');
        $('[data-approve]').click();
        await settle();
        expect($('[data-error]').hidden).toBe(false);
        expect($('[data-code]').value).toBe('');
        expect($('[data-approve]').textContent).toBe('tvPairing.shortApprove:{"hours":8}');
    });
});
