/*
 * MoonlightWeb — frontend TNR. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * After a PIN reached through the introduction server, the page reloads once,
 * so that a home-screen shortcut made from it carries the machine's name and
 * its key: on an iPhone, one made without the reload carried neither
 * (06/11/2026, Safari 26.5).
 */

// Through the introduction server, where the shortcut is made.
// @vitest-environment-options {"url": "https://stream.moonlightweb.top/"}

import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import { MoonlightApp } from '../js/app.js';
import { BackendClient } from '../js/api/BackendClient.js';
import { Toast } from '../js/ui/Toast.js';

describe('the reload after a PIN, for the home-screen shortcut', () => {
    let signedIn;
    let saved;

    beforeEach(() => {
        saved = {
            loginView: MoonlightApp.loginView,
            state: MoonlightApp.state,
            _hasAdminAccess: MoonlightApp._hasAdminAccess,
            _isRealHostMachine: MoonlightApp._isRealHostMachine,
            _adminUnlockAvailable: MoonlightApp._adminUnlockAvailable,
        };
        signedIn = false;
        localStorage.clear();
        sessionStorage.clear();
        document.body.innerHTML =
            '<button id="btn-admin" style="display: none"></button>' +
            '<button id="btn-settings" style="display: none"></button>' +
            '<div id="main-content"></div>';
        vi.spyOn(BackendClient, 'getAuthStatus').mockImplementation(async () => ({
            authenticated: signedIn,
            is_localhost: false,
            remaining: 3,
        }));
        vi.spyOn(BackendClient, 'validatePin').mockImplementation(async () => {
            signedIn = true;
            return { status: 'ok' };
        });
        vi.spyOn(Toast, 'success').mockImplementation(() => {});
        for (const step of ['_initNavButtons', '_initRouter', '_initAsync', '_reloadPage'])
            vi.spyOn(MoonlightApp, step).mockImplementation(() => {});
        vi.spyOn(console, 'log').mockImplementation(() => {});
    });

    afterEach(() => {
        vi.restoreAllMocks();
        Object.assign(MoonlightApp, saved);
        document.body.innerHTML = '';
        sessionStorage.clear();
    });

    /** The login page and a right PIN; resolves once the page has settled. */
    async function signIn() {
        const settled = () =>
            MoonlightApp._reloadPage.mock.calls.length + MoonlightApp._initRouter.mock.calls.length;
        const before = settled();
        signedIn = false;
        expect(await MoonlightApp._checkAuth()).toBe(false);
        await vi.waitFor(() => expect(document.getElementById('login-pin-input')).not.toBe(null));
        const pin = /** @type {HTMLInputElement} */ (document.getElementById('login-pin-input'));
        pin.value = '123456';
        pin.dispatchEvent(new Event('input'));
        document.getElementById('btn-login-unlock').click();
        await vi.waitFor(() => expect(settled()).toBeGreaterThan(before));
    }

    it('reloads once the signed-in manifest has been asked for, and draws nothing first', async () => {
        vi.spyOn(MoonlightApp, '_offerHomeScreenHandoff').mockResolvedValue('ready');
        await signIn();
        expect(MoonlightApp._reloadPage).toHaveBeenCalledTimes(1);
        expect(MoonlightApp._initRouter).not.toHaveBeenCalled();
    });

    it('reloads too when the manifest could not be prepared: the reload asks again', async () => {
        vi.spyOn(MoonlightApp, '_offerHomeScreenHandoff').mockResolvedValue('failed');
        await signIn();
        expect(MoonlightApp._reloadPage).toHaveBeenCalledTimes(1);
    });

    it('never reloads a second time in the same tab', async () => {
        vi.spyOn(MoonlightApp, '_offerHomeScreenHandoff').mockResolvedValue('ready');
        await signIn();
        expect(MoonlightApp._reloadPage).toHaveBeenCalledTimes(1);

        await signIn();
        expect(MoonlightApp._reloadPage).toHaveBeenCalledTimes(1);
        expect(MoonlightApp._initRouter).toHaveBeenCalledTimes(1);
    });

    it('does not reload where there is no shortcut to prepare', async () => {
        vi.spyOn(MoonlightApp, '_offerHomeScreenHandoff').mockResolvedValue(null);
        await signIn();
        expect(MoonlightApp._reloadPage).not.toHaveBeenCalled();
        expect(MoonlightApp._initRouter).toHaveBeenCalledTimes(1);
    });

    it('reloads anyway when the manifest takes too long, and says so', async () => {
        const warn = vi.spyOn(console, 'warn').mockImplementation(() => {});
        // Stuck in the tunnel: never answers.
        vi.spyOn(MoonlightApp, '_offerHomeScreenHandoff').mockReturnValue(new Promise(() => {}));
        // The real bound, made short so the test does not wait 5 s.
        const within = MoonlightApp._offerHomeScreenHandoffWithin;
        vi.spyOn(MoonlightApp, '_offerHomeScreenHandoffWithin').mockImplementation(function () {
            return within.call(this, 20);
        });
        await signIn();
        expect(MoonlightApp._reloadPage).toHaveBeenCalledTimes(1);
        expect(MoonlightApp._initRouter).not.toHaveBeenCalled();
        expect(warn).toHaveBeenCalledWith(expect.stringContaining('not prepared within 20 ms'));
    });

    it('does not reload without a session storage to keep the mark in', () => {
        vi.spyOn(Storage.prototype, 'getItem').mockImplementation(() => {
            throw new Error('storage refused');
        });
        expect(MoonlightApp._reloadOnceForHomeScreen()).toBe(false);
        expect(MoonlightApp._reloadPage).not.toHaveBeenCalled();
    });
});

describe('the bounded wait for the home-screen manifest', () => {
    afterEach(() => {
        vi.useRealTimers();
        vi.restoreAllMocks();
    });

    it('gives up at 5 s, not before, and leaves no timer behind', async () => {
        vi.useFakeTimers();
        const warn = vi.spyOn(console, 'warn').mockImplementation(() => {});
        vi.spyOn(MoonlightApp, '_offerHomeScreenHandoff').mockReturnValue(new Promise(() => {}));
        let settled = null;
        MoonlightApp._offerHomeScreenHandoffWithin(5000).then((r) => (settled = r));
        await vi.advanceTimersByTimeAsync(4999);
        expect(settled).toBe(null);
        await vi.advanceTimersByTimeAsync(1);
        expect(settled).toBe('timeout');
        expect(warn).toHaveBeenCalledWith(expect.stringContaining('not prepared within 5000 ms'));
        expect(vi.getTimerCount()).toBe(0);
    });

    it("answers with the handoff's own result when it comes first", async () => {
        vi.useFakeTimers();
        const warn = vi.spyOn(console, 'warn').mockImplementation(() => {});
        vi.spyOn(MoonlightApp, '_offerHomeScreenHandoff').mockResolvedValue('ready');
        expect(await MoonlightApp._offerHomeScreenHandoffWithin(5000)).toBe('ready');
        expect(warn).not.toHaveBeenCalled();
        expect(vi.getTimerCount()).toBe(0);
    });
});
