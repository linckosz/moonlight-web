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

const menu = () => document.getElementById('instance-menu');
const names = () =>
    [...menu().querySelectorAll('.instance-item-name')].map((n) => n.textContent.trim());

describe('LoginView server selection', () => {
    let view;
    let container;

    const open = async () => {
        document.body.innerHTML =
            '<header><div class="instance-menu" id="instance-menu" hidden></div></header><div id="host"></div>';
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
        expect(menu().hidden).toBe(true);
        expect(menu().querySelector('.instance-btn')).toBeNull();
    });

    it('lives in the page header, not in the login box', async () => {
        register([entry(HERE, 'DualRTX'), entry(THERE, 'UM790Pro')]);
        await open();
        expect(menu().hidden).toBe(false);
        expect(menu().querySelector('.instance-btn')).not.toBeNull();
        expect(container.querySelector('.instance-menu, #login-server-select')).toBeNull();

        // Switching to the certificate form leaves the header alone.
        container.querySelector('#btn-toggle-auth-method').click();
        expect(container.querySelector('#login-cert-input')).not.toBeNull();
        expect(menu().querySelector('.instance-btn')).not.toBeNull();
    });

    it('lists this machine under a neutral label, then the others', async () => {
        register([entry(HERE, 'DualRTX'), entry(THERE, 'UM790Pro')]);
        await open();
        expect(names()).toEqual(['login.serverCurrent', 'UM790Pro']);
        expect(menu().querySelector('.instance-name').textContent).toBe('login.serverCurrent');
    });

    it('never names the machine being unlocked', async () => {
        // The host gave no name (no display_name), as it does over the Internet.
        register([entry(HERE, 'DualRTX'), entry(THERE, 'UM790Pro')]);
        await open();
        expect(document.body.innerHTML).not.toContain('DualRTX');
    });

    it('names a machine this browser has never reached no more than any other', async () => {
        // The identifier in the address is not in the register: every entry is
        // somewhere else, and the current row still says nothing about here.
        register([entry(THERE, 'UM790Pro')]);
        await open();
        expect(names()).toEqual(['login.serverCurrent', 'UM790Pro']);
    });

    it('leaves for the chosen machine, and does nothing for the current row', async () => {
        register([entry(HERE, 'DualRTX'), entry(THERE, 'UM790Pro')]);
        await open();

        const assign = [];
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

        const items = menu().querySelectorAll('.instance-item');
        items[0].click();
        expect(assign).toEqual([]);

        items[1].click();
        expect(assign).toEqual([`https://stream.moonlightweb.top/${THERE}`]);

        if (original) Object.defineProperty(window, 'location', original);
    });

    it('hands the header back empty when the login screen goes', async () => {
        register([entry(HERE, 'DualRTX'), entry(THERE, 'UM790Pro')]);
        await open();
        view.destroy();
        expect(menu().hidden).toBe(true);
        expect(menu().innerHTML).toBe('');
    });
});
