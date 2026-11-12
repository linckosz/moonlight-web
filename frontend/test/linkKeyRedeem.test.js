/*
 * MoonlightWeb — frontend TNR. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * Spending the key of the link a page was opened with (App._redeemHostKey).
 * The key is held apart from the address (js/net/linkKey.js); the application
 * says when the host has answered it, and puts it back into the address just
 * before a reload it does itself.
 */

import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';

vi.mock('../js/net/linkKey.js', () => ({
    splitLinkKey: vi.fn(),
    takeLinkKey: vi.fn(),
    carriedLinkKey: vi.fn(),
    putLinkKeyBack: vi.fn(),
    forgetLinkKey: vi.fn(),
}));

import { MoonlightApp } from '../js/app.js';
import { BackendClient } from '../js/api/BackendClient.js';
import { takeLinkKey, putLinkKeyBack, forgetLinkKey } from '../js/net/linkKey.js';
import { isHandoffKeySpent } from '../js/util/homeScreenKeys.js';

/** What the rendezvous' service worker answers by itself (bootstrap/sw.js). */
const LINE_FAILURES = [502, 503, 504];

/** What a request the host answered with a refusal throws. */
function refusal(status) {
    const err = new Error('forbidden');
    err.statusCode = status;
    return err;
}

describe('spending the key of the link', () => {
    beforeEach(() => {
        vi.clearAllMocks();
        vi.spyOn(console, 'log').mockImplementation(() => {});
        vi.spyOn(console, 'warn').mockImplementation(() => {});
    });

    afterEach(() => {
        vi.restoreAllMocks();
    });

    it('sends nothing from a page opened without one', async () => {
        takeLinkKey.mockReturnValue(null);
        const redeem = vi.spyOn(BackendClient, 'redeemHostKey').mockResolvedValue({});
        await MoonlightApp._redeemHostKey();
        expect(redeem).not.toHaveBeenCalled();
        expect(forgetLinkKey).not.toHaveBeenCalled();
    });

    it('forgets the key once the host has said yes', async () => {
        takeLinkKey.mockReturnValue({ key: 'LINE-KEY', direct: false });
        const redeem = vi.spyOn(BackendClient, 'redeemHostKey').mockResolvedValue({ status: 'ok' });
        await MoonlightApp._redeemHostKey();
        expect(redeem).toHaveBeenCalledTimes(1);
        expect(redeem).toHaveBeenCalledWith('LINE-KEY', { direct: false });
        expect(forgetLinkKey).toHaveBeenCalledTimes(1);
    });

    it('forgets the key once the host has said no', async () => {
        takeLinkKey.mockReturnValue({ key: 'LINE-KEY', direct: false });
        vi.spyOn(BackendClient, 'redeemHostKey').mockRejectedValue(refusal(403));
        await MoonlightApp._redeemHostKey();
        expect(forgetLinkKey).toHaveBeenCalledTimes(1);
    });

    it('keeps the key for the page that follows when no answer came', async () => {
        takeLinkKey.mockReturnValue({ key: 'LINE-KEY', direct: false });
        vi.spyOn(BackendClient, 'redeemHostKey').mockRejectedValue(
            new Error('the connection failed'),
        );
        await MoonlightApp._redeemHostKey();
        expect(forgetLinkKey).not.toHaveBeenCalled();
    });

    it('says so when the page is one the host serves itself', async () => {
        takeLinkKey.mockReturnValue({ key: 'LOCAL-KEY', direct: true });
        const redeem = vi.spyOn(BackendClient, 'redeemHostKey').mockResolvedValue({ status: 'ok' });
        await MoonlightApp._redeemHostKey();
        expect(redeem).toHaveBeenCalledWith('LOCAL-KEY', { direct: true });
        expect(forgetLinkKey).toHaveBeenCalledTimes(1);
    });

    // Through the rendezvous the service worker answers for a host that did
    // not: no page holds the line, the line failed, no answer came in time.
    it.each(LINE_FAILURES)('keeps the key when the line itself reports %i', async (status) => {
        takeLinkKey.mockReturnValue({ key: 'LINE-KEY', direct: false });
        vi.spyOn(BackendClient, 'redeemHostKey').mockRejectedValue(refusal(status));
        await MoonlightApp._redeemHostKey();
        expect(forgetLinkKey).not.toHaveBeenCalled();
    });

    it.each(LINE_FAILURES)(
        'takes %i for an answer on a page the host serves itself',
        async (status) => {
            takeLinkKey.mockReturnValue({ key: 'LOCAL-KEY', direct: true });
            vi.spyOn(BackendClient, 'redeemHostKey').mockRejectedValue(refusal(status));
            await MoonlightApp._redeemHostKey();
            expect(forgetLinkKey).toHaveBeenCalledTimes(1);
        },
    );
});

describe("spending a home-screen shortcut's key", () => {
    const OWN = 'hs-' + 'A'.repeat(22);

    beforeEach(() => {
        vi.clearAllMocks();
        localStorage.clear();
        vi.spyOn(console, 'log').mockImplementation(() => {});
        vi.spyOn(console, 'warn').mockImplementation(() => {});
    });

    afterEach(() => {
        vi.restoreAllMocks();
        localStorage.clear();
    });

    it('remembers the key as answered once the host has refused it', async () => {
        takeLinkKey.mockReturnValue({ key: OWN, direct: false });
        const redeem = vi
            .spyOn(BackendClient, 'redeemHomeScreenKey')
            .mockRejectedValue(refusal(403));
        await MoonlightApp._redeemHostKey();
        expect(redeem).toHaveBeenCalledTimes(1);
        expect(redeem.mock.calls[0][0]).toBe(OWN);
        expect(await isHandoffKeySpent(OWN)).toBe(true);
        expect(forgetLinkKey).toHaveBeenCalledTimes(1);
    });

    it.each(LINE_FAILURES)(
        'keeps the key for a later launch when the line itself reports %i',
        async (status) => {
            takeLinkKey.mockReturnValue({ key: OWN, direct: false });
            vi.spyOn(BackendClient, 'redeemHomeScreenKey').mockRejectedValue(refusal(status));
            await MoonlightApp._redeemHostKey();
            expect(await isHandoffKeySpent(OWN)).toBe(false);
            expect(forgetLinkKey).not.toHaveBeenCalled();
        },
    );
});

describe('a reload this page does itself', () => {
    let order;
    let original;

    beforeEach(() => {
        vi.clearAllMocks();
        order = [];
        putLinkKeyBack.mockImplementation(() => order.push('key back'));
        original = window.location;
        Object.defineProperty(window, 'location', {
            value: { reload: () => order.push('reload') },
            configurable: true,
        });
        vi.spyOn(console, 'log').mockImplementation(() => {});
        vi.spyOn(console, 'warn').mockImplementation(() => {});
    });

    afterEach(() => {
        Object.defineProperty(window, 'location', { value: original, configurable: true });
        vi.restoreAllMocks();
        document.body.innerHTML = '';
    });

    it('puts the key back into the address first', () => {
        MoonlightApp._reloadPage();
        expect(order).toEqual(['key back', 'reload']);
    });

    it('does so from the retry button of a page that could not reach the host', async () => {
        const saved = MoonlightApp.state;
        document.body.innerHTML = '<div id="main-content"></div>';
        vi.spyOn(BackendClient, 'getAuthStatus').mockRejectedValue(
            new Error('the connection failed'),
        );
        expect(await MoonlightApp._checkAuth()).toBe(false);
        MoonlightApp.state = saved;
        /** @type {HTMLButtonElement} */ (document.querySelector('.btn-reload')).click();
        expect(order).toEqual(['key back', 'reload']);
    });
});
