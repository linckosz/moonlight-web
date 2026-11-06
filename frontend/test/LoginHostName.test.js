/*
 * MoonlightWeb — frontend TNR. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * The PIN screen names the machine it unlocks when the host gave its name,
 * which the host does to a caller on its own home network only
 * (/api/auth/status's display_name). Without that name, as over the Internet,
 * the screen is the one it has always been and names no machine.
 */

import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';

vi.mock('../js/api/BackendClient.js', () => ({
    BackendClient: {
        getAuthStatus: vi.fn(),
        validatePin: vi.fn(),
        validateCertificate: vi.fn(async () => ({ status: 'ok' })),
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
vi.mock('../js/net/tunnelBridge.js', () => ({
    tunnelHostId: () => null,
}));

import { LoginView } from '../js/ui/LoginView.js';
import { BackendClient } from '../js/api/BackendClient.js';

describe('the machine named on the PIN screen', () => {
    let view;

    const host = () => document.getElementById('host');
    const line = () => document.getElementById('login-host-name');
    const open = async (status) => {
        BackendClient.getAuthStatus.mockResolvedValue({
            authenticated: false,
            remaining: 3,
            cert_auth_enabled: true,
            ...status,
        });
        document.body.innerHTML = '<div id="host"></div>';
        view = new LoginView(host(), () => {});
        await view.start();
    };

    beforeEach(() => {
        vi.clearAllMocks();
        localStorage.clear();
    });
    afterEach(() => view && view.destroy());

    it('is named when the host gave its name', async () => {
        await open({ display_name: 'DualRTX' });
        expect(line().textContent).toBe('login.serverName {"name":"DualRTX"}');
        // Under the title, above the form.
        expect(line().previousElementSibling.classList.contains('login-title')).toBe(true);
        expect(host().querySelector('#login-form-area #login-host-name')).toBeNull();
    });

    it('is not named without it, and the screen is otherwise the same', async () => {
        await open({});
        expect(line()).toBeNull();
        expect(host().innerHTML).not.toContain('login.serverName');
        const plain = host().innerHTML;

        view.destroy();
        await open({ display_name: 'DualRTX' });
        line().remove();
        expect(host().innerHTML).toBe(plain);
    });

    it('is not named by a blank name, or by anything but a string', async () => {
        for (const display_name of ['', '   ', 42, null, { name: 'DualRTX' }]) {
            await open({ display_name });
            expect(line()).toBeNull();
            view.destroy();
        }
        view = null;
    });

    it('keeps its name on the certificate form, and after a wrong PIN', async () => {
        await open({ display_name: 'DualRTX' });

        const err = new Error('Invalid PIN');
        err.statusCode = 401;
        err.responseBody = { remaining: 2 };
        BackendClient.validatePin.mockRejectedValueOnce(err);
        const pin = /** @type {HTMLInputElement} */ (document.getElementById('login-pin-input'));
        pin.value = '000000';
        pin.dispatchEvent(new Event('input'));
        document.getElementById('btn-login-unlock').click();
        await vi.waitFor(() =>
            expect(document.getElementById('login-error').textContent).not.toBe(''),
        );
        expect(line().textContent).toBe('login.serverName {"name":"DualRTX"}');

        document.getElementById('btn-toggle-auth-method').click();
        expect(document.getElementById('login-cert-input')).not.toBeNull();
        expect(line().textContent).toBe('login.serverName {"name":"DualRTX"}');
    });

    it('shows the name as text, never as markup', async () => {
        await open({ display_name: '<img src=x onerror=alert(1)>' });
        expect(host().querySelector('img')).toBeNull();
        expect(line().textContent).toBe('login.serverName {"name":"<img src=x onerror=alert(1)>"}');
    });
});
