/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 *
 * The login screen used to be a dead end. Someone with two PCs who follows a
 * link to the one that is switched off — or whose session there has expired —
 * lands on a PIN prompt with no route to the other machine. These tests pin
 * down the way out, and the one thing it must never do: name the machine it is
 * standing in front of.
 */
import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';

vi.mock('../js/api/BackendClient.js', () => ({
    BackendClient: {
        getAuthStatus: vi.fn(),
        validatePin: vi.fn(async () => ({ status: 'ok' })),
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
    tunnelHostId: () => 'bakdp5qszx8kqgg4kbg0mdb93y',
}));

import { LoginView } from '../js/ui/LoginView.js';
import { BackendClient } from '../js/api/BackendClient.js';

const HERE = 'bakdp5qszx8kqgg4kbg0mdb93y';
const THERE = '9tvfm1r4xj2w0dcyq7hnkp63ae';

function register(entries) {
    localStorage.setItem('mw-instances', JSON.stringify(entries));
}

function entry(id, name) {
    return { id, name, url: `https://stream.moonlightweb.top/${id}`, seen: Date.now() };
}

describe('LoginView server selection', () => {
    let view;
    let container;

    const open = async () => {
        document.body.innerHTML = '<div id="host"></div>';
        container = document.getElementById('host');
        view = new LoginView(container, () => {});
        await view.start();
    };

    beforeEach(() => {
        vi.clearAllMocks();
        localStorage.clear();
        BackendClient.getAuthStatus.mockResolvedValue({
            authenticated: false,
            cert_auth_enabled: true,
            remaining: 3,
        });
    });

    afterEach(() => view && view.destroy());

    it('offers nothing when this browser has been nowhere else', async () => {
        register([entry(HERE, 'DualRTX')]);
        await open();
        expect(container.querySelector('#login-server-select')).toBeNull();
    });

    it('lists the other machines, on both authentication forms', async () => {
        register([entry(HERE, 'DualRTX'), entry(THERE, 'UM790Pro')]);
        await open();

        const select = container.querySelector('#login-server-select');
        expect(select).not.toBeNull();
        const options = [...select.options].map((o) => o.textContent.trim());
        expect(options).toEqual(['login.serverCurrent', 'UM790Pro']);

        container.querySelector('#btn-toggle-auth-method').click();
        expect(container.querySelector('#login-cert-input')).not.toBeNull();
        expect(container.querySelector('#login-server-select')).not.toBeNull();
    });

    it('never names the machine being unlocked', async () => {
        register([entry(HERE, 'DualRTX'), entry(THERE, 'UM790Pro')]);
        await open();
        expect(container.innerHTML).not.toContain('DualRTX');
    });

    it('names a machine this browser has never reached no more than any other', async () => {
        // The identifier in the address is not in the register: every entry is
        // somewhere else, and the selected row still says nothing about here.
        register([entry(THERE, 'UM790Pro')]);
        await open();
        const select = container.querySelector('#login-server-select');
        expect([...select.options].map((o) => o.value)).toEqual([
            '',
            `https://stream.moonlightweb.top/${THERE}`,
        ]);
        expect(select.value).toBe('');
    });

    it('leaves for the chosen machine, and does nothing for the current row', async () => {
        register([entry(HERE, 'DualRTX'), entry(THERE, 'UM790Pro')]);
        await open();

        const assign = [];
        const select = container.querySelector('#login-server-select');

        const original = Object.getOwnPropertyDescriptor(window, 'location');
        delete window.location;
        window.location = {
            get href() {
                return 'https://stream.moonlightweb.top/';
            },
            set href(v) {
                assign.push(v);
            },
        };

        select.value = '';
        select.dispatchEvent(new Event('change'));
        expect(assign).toEqual([]);

        select.value = `https://stream.moonlightweb.top/${THERE}`;
        select.dispatchEvent(new Event('change'));
        expect(assign).toEqual([`https://stream.moonlightweb.top/${THERE}`]);

        if (original) Object.defineProperty(window, 'location', original);
    });
});
