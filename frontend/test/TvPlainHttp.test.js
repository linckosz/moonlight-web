/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 *
 * A TV on plain HTTP (issue #42): it tells itself apart from any other browser
 * by the status route, signs in by the QR code and code it shows, and the
 * administrator lets it in from the page that QR code opens.
 */
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';

vi.mock('../js/i18n/i18n.js', () => ({
    t: (key, vars) => (vars ? `${key}:${JSON.stringify(vars)}` : key),
}));
vi.mock('../js/api/BackendClient.js', () => ({
    BackendClient: { get: vi.fn(), post: vi.fn() },
}));

import { BackendClient } from '../js/api/BackendClient.js';
import { lanHttpPoll, lanHttpStatus } from '../js/api/LanHttpApi.js';
import { clockOf, spacedCode, TvSignInView } from '../js/ui/TvSignInView.js';
import { openTvPairApprove, tvPairRequestFromPath } from '../js/ui/TvPairApproveDialog.js';

const ID = 'AbCdEfGhIjKlMnOp';
const $ = (sel) => document.querySelector(sel);
const settle = () => new Promise((r) => setTimeout(r, 0));
const answer = (status, body) =>
    new Response(JSON.stringify(body), {
        status,
        headers: { 'Content-Type': 'application/json' },
    });
const refusal = (statusCode, error) =>
    Object.assign(new Error(error), { statusCode, responseBody: { error } });

beforeEach(() => {
    vi.clearAllMocks();
});

afterEach(() => {
    vi.unstubAllGlobals();
    vi.useRealTimers();
    document.body.innerHTML = '';
});

describe('LanHttpApi', () => {
    it('is a TV only where the status route answers', async () => {
        vi.stubGlobal(
            'fetch',
            vi.fn(async () => answer(404, { error: 'Not Found' })),
        );
        expect(await lanHttpStatus()).toBeNull();
        vi.stubGlobal(
            'fetch',
            vi.fn(async () => answer(200, { signed_in: false })),
        );
        expect(await lanHttpStatus()).toEqual({ signed_in: false });
        vi.stubGlobal(
            'fetch',
            vi.fn(async () => answer(200, { signed_in: true })),
        );
        expect(await lanHttpStatus()).toEqual({ signed_in: true });
        vi.stubGlobal(
            'fetch',
            vi.fn(async () => {
                throw new Error('offline');
            }),
        );
        expect(await lanHttpStatus()).toBeNull();
    });

    it('asks where its request stands with the claim in a header, never in the address', async () => {
        const fetchMock = vi.fn(async () => answer(200, { state: 'pending' }));
        vi.stubGlobal('fetch', fetchMock);
        expect(await lanHttpPoll(ID, 'the-claim')).toBe('pending');
        const [path, init] = fetchMock.mock.calls[0];
        expect(path).toBe(`/api/lan-http/pair/${ID}`);
        expect(path).not.toContain('the-claim');
        expect(init.headers['X-MW-Claim']).toBe('the-claim');
        vi.stubGlobal(
            'fetch',
            vi.fn(async () => answer(404, { state: 'gone' })),
        );
        expect(await lanHttpPoll(ID, 'the-claim')).toBe('gone');
        vi.stubGlobal(
            'fetch',
            vi.fn(async () => answer(200, { state: 'something else' })),
        );
        expect(await lanHttpPoll(ID, 'the-claim')).toBe('gone');
    });
});

describe('TvSignInView', () => {
    const opened = (code = 'K7F3QX') => ({
        request: ID,
        code,
        claim: 'the-claim',
        expires_in: 300,
        approve_url: `https://192.168.1.10:8443/tv-pair-${ID}`,
    });

    it('reads across a room: the code in two halves, the time left', () => {
        expect(spacedCode('k7f3qx')).toBe('K7F 3QX');
        expect(spacedCode('ABC')).toBe('ABC');
        expect(clockOf(292)).toBe('4:52');
        expect(clockOf(-3)).toBe('0:00');
    });

    it('shows its QR code and code, then signs in once approved', async () => {
        BackendClient.post.mockResolvedValue(opened());
        const states = ['pending', 'approved'];
        vi.stubGlobal(
            'fetch',
            vi.fn(async () => answer(200, { state: states.shift() })),
        );
        const onSignedIn = vi.fn();
        const qr = vi.fn(() => '<svg data-qr></svg>');
        const view = new TvSignInView(document.body, { onSignedIn, qr, pollMs: 1 });
        view.start();
        await settle();
        expect(BackendClient.post).toHaveBeenCalledWith('/api/lan-http/pair', {});
        expect(qr).toHaveBeenCalledWith(`https://192.168.1.10:8443/tv-pair-${ID}`);
        expect($('[data-qr]')).not.toBeNull();
        expect($('.tv-sign-in-code').textContent).toBe('K7F 3QX');
        // Nothing to type on the TV.
        expect(document.querySelector('input')).toBeNull();
        await vi.waitFor(() => expect(onSignedIn).toHaveBeenCalledTimes(1));
        view.stop();
    });

    it('shows the address when it has no QR code to draw', async () => {
        BackendClient.post.mockResolvedValue(opened());
        vi.stubGlobal(
            'fetch',
            vi.fn(async () => answer(200, { state: 'pending' })),
        );
        const view = new TvSignInView(document.body, { onSignedIn: vi.fn(), pollMs: 1000 });
        view.start();
        await settle();
        expect($('.tv-sign-in-address').textContent).toBe(
            `https://192.168.1.10:8443/tv-pair-${ID}`,
        );
        view.stop();
    });

    it('replaces a request the server no longer knows with a new code', async () => {
        BackendClient.post
            .mockResolvedValueOnce(opened('AAAAAA'))
            .mockResolvedValue(opened('BBBBBB'));
        vi.stubGlobal(
            'fetch',
            vi.fn(async () => answer(404, { state: 'gone' })),
        );
        const view = new TvSignInView(document.body, { onSignedIn: vi.fn(), pollMs: 1 });
        view.start();
        await vi.waitFor(() => expect($('.tv-sign-in-code')?.textContent).toBe('BBB BBB'));
        view.stop();
    });

    it('says a refusal, and asks again only when told to', async () => {
        BackendClient.post.mockResolvedValue(opened());
        vi.stubGlobal(
            'fetch',
            vi.fn(async () => answer(200, { state: 'refused' })),
        );
        const view = new TvSignInView(document.body, { onSignedIn: vi.fn(), pollMs: 1 });
        view.start();
        await vi.waitFor(() => expect($('[data-tv-again]')).not.toBeNull());
        expect($('.tv-sign-in-message').textContent).toBe('tvSignIn.refused');
        expect(BackendClient.post).toHaveBeenCalledTimes(1);
        $('[data-tv-again]').click();
        await settle();
        expect(BackendClient.post).toHaveBeenCalledTimes(2);
        view.stop();
    });

    it('waits when the server is busy', async () => {
        BackendClient.post.mockRejectedValue(refusal(429, 'busy'));
        const view = new TvSignInView(document.body, { onSignedIn: vi.fn() });
        view.start();
        await settle();
        expect($('.tv-sign-in-message').textContent).toBe('tvSignIn.busy');
        view.stop();
    });
});

describe('TvPairApproveDialog', () => {
    const type = (text) => {
        const field = $('[data-code]');
        field.value = text;
        field.dispatchEvent(new Event('input'));
    };

    it('reads the request from the page a TV QR code opens, and nothing else', () => {
        expect(tvPairRequestFromPath(`/tv-pair-${ID}`)).toBe(ID);
        expect(tvPairRequestFromPath(`/pair-${ID}`)).toBeNull();
        expect(tvPairRequestFromPath('/tv-pair-short')).toBeNull();
        expect(tvPairRequestFromPath(`/tv-pair-${ID}/x`)).toBeNull();
        expect(tvPairRequestFromPath(undefined)).toBeNull();
    });

    it('names the TV by its address, says it is on plain HTTP, and sends the code typed', async () => {
        BackendClient.get.mockResolvedValue({ address: '192.168.1.50', expires_in: 280 });
        BackendClient.post.mockResolvedValue({ status: 'approved' });
        const closed = openTvPairApprove(ID);
        await settle();
        expect(BackendClient.get).toHaveBeenCalledWith(`/api/lan-http/approve/${ID}`);
        expect($('.pair-approve-lead').textContent).toContain('192.168.1.50');
        expect($('.tv-pair-plain-http').textContent).toBe('tvPairing.plainHttp');
        const approve = $('[data-approve]');
        type('k7f 3q');
        expect(approve.disabled).toBe(true);
        type('k7f 3qx');
        expect(approve.disabled).toBe(false);
        approve.click();
        await settle();
        expect(BackendClient.post).toHaveBeenCalledWith(`/api/lan-http/approve/${ID}`, {
            code: 'K7F3QX',
        });
        expect($('.pair-approve-body').textContent).toBe('tvPairing.approved');
        $('[data-close]').click();
        expect(await closed).toBe('approved');
    });

    it('keeps the question after a wrong code', async () => {
        BackendClient.get.mockResolvedValue({ address: '192.168.1.50', expires_in: 280 });
        BackendClient.post.mockRejectedValue(refusal(400, 'wrong_code'));
        openTvPairApprove(ID);
        await settle();
        type('AAAAAA');
        $('[data-approve]').click();
        await settle();
        expect($('[data-error]').hidden).toBe(false);
        expect($('[data-error]').textContent).toBe('tvPairing.wrongCode');
        expect($('[data-code]').value).toBe('');
    });

    it('tells anyone but the administrator that it is not theirs', async () => {
        BackendClient.get.mockRejectedValue(refusal(403, 'not_allowed'));
        openTvPairApprove(ID);
        await settle();
        expect($('.pair-approve-body').textContent).toBe('tvPairing.notAllowed');
        expect($('[data-code]')).toBeNull();
    });

    it('refuses on the refusal route', async () => {
        BackendClient.get.mockResolvedValue({ address: '192.168.1.50', expires_in: 280 });
        BackendClient.post.mockResolvedValue({ status: 'refused' });
        const closed = openTvPairApprove(ID);
        await settle();
        $('[data-refuse]').click();
        await settle();
        expect(BackendClient.post).toHaveBeenCalledWith(`/api/lan-http/refuse/${ID}`, {});
        $('[data-close]').click();
        expect(await closed).toBe('refused');
    });
});
