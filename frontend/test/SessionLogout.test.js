/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 *
 * Leaving a machine you do not own. Someone logging in from an internet café
 * needs two things MoonlightWeb had neither of: a way to say "do not remember
 * me" on the way in, and a way out on the way back. These tests pin down both —
 * that the choice reaches the server, that it survives a mistyped PIN, and that
 * the Log out button only exists where there is a session to end.
 */
import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';

vi.mock('../js/api/BackendClient.js', () => ({
    BackendClient: {
        getAuthStatus: vi.fn(),
        getServerStatus: vi.fn(),
        getStreamingSettings: vi.fn(async () => ({})),
        validatePin: vi.fn(async () => ({ status: 'ok' })),
        validateCertificate: vi.fn(async () => ({ status: 'ok' })),
        logout: vi.fn(async () => ({ status: 'ok', had_session: true })),
    },
}));
vi.mock('../js/ui/Toast.js', () => ({
    Toast: { success: vi.fn(), error: vi.fn(), warning: vi.fn(), info: vi.fn() },
}));
vi.mock('../js/i18n/i18n.js', () => ({
    t: (key, params) => (params ? key + ' ' + JSON.stringify(params) : key),
    getLanguage: () => 'en',
    setLanguage: vi.fn(),
    AVAILABLE_LANGUAGES: [{ code: 'en', label: 'English' }],
}));

import { LoginView } from '../js/ui/LoginView.js';
import { SettingsView } from '../js/ui/SettingsView.js';
import { BackendClient } from '../js/api/BackendClient.js';

describe('LoginView "remember me"', () => {
    let view;
    let container;

    const typePin = (digits) => {
        const input = container.querySelector('#login-pin-input');
        input.value = digits;
        input.dispatchEvent(new Event('input'));
        return input;
    };

    beforeEach(async () => {
        vi.clearAllMocks();
        BackendClient.getAuthStatus.mockResolvedValue({
            authenticated: false,
            cert_auth_enabled: true,
            remaining: 3,
        });
        document.body.innerHTML = '<div id="host"></div>';
        container = document.getElementById('host');
        view = new LoginView(container, () => {});
        await view.start();
    });

    afterEach(() => view.destroy());

    it('offers the choice, checked, on both authentication forms', () => {
        expect(container.querySelector('#login-remember').checked).toBe(true);

        container.querySelector('#btn-toggle-auth-method').click();
        expect(container.querySelector('#login-cert-input')).not.toBeNull();
        expect(container.querySelector('#login-remember').checked).toBe(true);
    });

    it('remembers by default, so an unchanged form behaves as before', async () => {
        typePin('123456');
        container.querySelector('#btn-login-unlock').click();
        await vi.waitFor(() => expect(BackendClient.validatePin).toHaveBeenCalled());
        expect(BackendClient.validatePin).toHaveBeenCalledWith('123456', expect.any(String), true);
    });

    it('carries the refusal to the server when the box is unchecked', async () => {
        const box = container.querySelector('#login-remember');
        box.checked = false;
        box.dispatchEvent(new Event('change'));

        typePin('123456');
        container.querySelector('#btn-login-unlock').click();
        await vi.waitFor(() => expect(BackendClient.validatePin).toHaveBeenCalled());
        expect(BackendClient.validatePin).toHaveBeenCalledWith('123456', expect.any(String), false);
    });

    it('keeps the refusal through a wrong PIN, which re-renders the form', async () => {
        const box = container.querySelector('#login-remember');
        box.checked = false;
        box.dispatchEvent(new Event('change'));

        const err = new Error('nope');
        err.statusCode = 401;
        err.responseBody = { remaining: 2 };
        BackendClient.validatePin.mockRejectedValueOnce(err);

        typePin('000000');
        container.querySelector('#btn-login-unlock').click();
        await vi.waitFor(() =>
            expect(container.querySelector('#login-error').textContent).not.toBe(''),
        );

        // The re-rendered form must not quietly re-tick the box the user cleared.
        expect(container.querySelector('#login-remember').checked).toBe(false);

        view._submitting = false;
        typePin('123456');
        container.querySelector('#btn-login-unlock').click();
        await vi.waitFor(() => expect(BackendClient.validatePin).toHaveBeenCalledTimes(2));
        expect(BackendClient.validatePin).toHaveBeenLastCalledWith(
            '123456',
            expect.any(String),
            false,
        );
    });

    it('passes the choice through the certificate form too', async () => {
        container.querySelector('#btn-toggle-auth-method').click();
        const box = container.querySelector('#login-remember');
        box.checked = false;
        box.dispatchEvent(new Event('change'));

        await view._submitCertificate(
            new File(['token'], 'mw.cert'),
            null,
            container.querySelector('#btn-login-cert-auth'),
            container.querySelector('#login-cert-input'),
        );
        expect(BackendClient.validateCertificate).toHaveBeenCalledWith('token', '', false);
    });
});

describe('SettingsView log out', () => {
    let view;
    let container;
    let replace;

    const build = (canLogout) => {
        document.body.innerHTML = '<div id="host"></div>';
        container = document.getElementById('host');
        view = new SettingsView(container, () => {});
        view._canLogout = canLogout;
        view.render();
        view.bindEvents();
    };

    beforeEach(() => {
        vi.clearAllMocks();
        replace = vi.fn();
        // jsdom refuses real navigation; the button's whole job is to trigger one.
        delete window.location;
        window.location = { pathname: '/', replace };
    });

    afterEach(() => vi.unstubAllGlobals());

    it('is absent on a browser with no session to end, such as the host page', () => {
        build(false);
        expect(container.querySelector('#btn-settings-logout')).toBeNull();
    });

    it('reads the session flag from the server, and fails closed', async () => {
        const v = new SettingsView(document.body, () => {});
        BackendClient.getAuthStatus.mockResolvedValueOnce({ has_session: true });
        expect(await v._checkSession()).toBe(true);
        BackendClient.getAuthStatus.mockResolvedValueOnce({ has_session: false });
        expect(await v._checkSession()).toBe(false);
        BackendClient.getAuthStatus.mockRejectedValueOnce(new Error('offline'));
        expect(await v._checkSession()).toBe(false);
    });

    it('spares the host machine, which holds a session but must never sign out', async () => {
        const v = new SettingsView(document.body, () => {});

        // The host reaching its own server through the public domain: it carries
        // a real host-key session, so has_session alone would offer the button —
        // and clicking it would drop the host onto a PIN prompt on its own PC.
        BackendClient.getAuthStatus.mockResolvedValueOnce({
            has_session: true,
            is_host_machine: true,
            is_localhost: true,
        });
        expect(await v._checkSession()).toBe(false);

        // A LAN device that unlocked admin with the password is somebody else's
        // computer: it has admin rights but is not the host, and keeps its way out.
        BackendClient.getAuthStatus.mockResolvedValueOnce({
            has_session: true,
            is_host_machine: false,
            is_localhost: true,
        });
        expect(await v._checkSession()).toBe(true);
    });

    it('does nothing at all when the confirmation is declined', async () => {
        build(true);
        vi.stubGlobal(
            'confirm',
            vi.fn(() => false),
        );
        await view._logout(container.querySelector('#btn-settings-logout'));
        expect(BackendClient.logout).not.toHaveBeenCalled();
        expect(replace).not.toHaveBeenCalled();
    });

    it('ends the session and reloads, dropping any socket still open', async () => {
        build(true);
        vi.stubGlobal(
            'confirm',
            vi.fn(() => true),
        );
        await view._logout(container.querySelector('#btn-settings-logout'));
        expect(BackendClient.logout).toHaveBeenCalled();
        expect(replace).toHaveBeenCalledWith('/');
    });

    it('reloads even when the logout call fails, so a stale cookie cannot strand the user', async () => {
        build(true);
        vi.stubGlobal(
            'confirm',
            vi.fn(() => true),
        );
        BackendClient.logout.mockRejectedValueOnce(new Error('gone'));
        await view._logout(container.querySelector('#btn-settings-logout'));
        expect(replace).toHaveBeenCalledWith('/');
    });
});
