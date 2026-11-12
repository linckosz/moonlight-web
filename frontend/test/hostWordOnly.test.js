/*
 * MoonlightWeb — frontend TNR. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * What this browser may do here is the host's to say (/api/auth/status), and
 * the page takes its word alone. The address the page was opened by gives it
 * nothing, localhost included: the admin button, the admin page, the control
 * channel and the server's defaults all wait for the host's answer.
 */

// A page opened by localhost, the address that used to stand in for the answer.
// @vitest-environment-options {"url": "https://localhost:8443/"}

import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import { MoonlightApp } from '../js/app.js';
import { BackendClient } from '../js/api/BackendClient.js';
import { AdminView } from '../js/ui/AdminView.js';
import { SettingsView } from '../js/ui/SettingsView.js';

describe('the application, on a page opened by localhost', () => {
    let saved;

    beforeEach(() => {
        saved = {
            _hasAdminAccess: MoonlightApp._hasAdminAccess,
            _isRealHostMachine: MoonlightApp._isRealHostMachine,
            _adminUnlockAvailable: MoonlightApp._adminUnlockAvailable,
            _controlWs: MoonlightApp._controlWs,
        };
        MoonlightApp._hasAdminAccess = undefined;
        MoonlightApp._isRealHostMachine = undefined;
        MoonlightApp._adminUnlockAvailable = undefined;
        MoonlightApp._controlWs = null;
        document.body.innerHTML =
            '<button id="btn-admin"></button><button id="btn-settings"></button>';
    });

    afterEach(() => {
        Object.assign(MoonlightApp, saved);
        vi.unstubAllGlobals();
        vi.restoreAllMocks();
        document.body.innerHTML = '';
    });

    it('is on localhost, as the address bar has it', () => {
        expect(window.location.hostname).toBe('localhost');
    });

    it('has nothing of the host machine before the host has answered', () => {
        expect(MoonlightApp._isHostLocal()).toBe(false);
        expect(MoonlightApp._isHostMachine()).toBe(false);
        expect(MoonlightApp._canReachAdmin()).toBe(false);
    });

    it.each([
        ['an ordinary session', { authenticated: true }, false, false, false],
        [
            "the host machine's own browser",
            { is_localhost: true, is_host_machine: true },
            true,
            true,
            true,
        ],
        [
            'a LAN machine unlocked with the admin password',
            { is_localhost: true, is_host_machine: false },
            true,
            false,
            true,
        ],
        [
            'the booth, where nobody administers',
            { is_localhost: false, is_host_machine: true, admin_locked: true },
            false,
            true,
            false,
        ],
        [
            'a LAN machine that may still unlock',
            { authenticated: true, admin_unlock_available: true },
            false,
            false,
            true,
        ],
    ])('is what the host says of %s', (_who, status, admin, machine, reach) => {
        MoonlightApp._readAccess(status);
        expect(MoonlightApp._isHostLocal()).toBe(admin);
        expect(MoonlightApp._isHostMachine()).toBe(machine);
        expect(MoonlightApp._canReachAdmin()).toBe(reach);
    });

    it('keeps the admin button hidden for a session the host gives no admin access', () => {
        MoonlightApp._readAccess({ authenticated: true });
        MoonlightApp._hideNavButtonsConditionally();
        MoonlightApp._initNavButtons();
        expect(document.getElementById('btn-admin').style.display).toBe('none');
        expect(document.getElementById('btn-settings').style.display).toBe('');
    });

    it("shows it to the host machine's own browser", () => {
        MoonlightApp._readAccess({ is_localhost: true, is_host_machine: true });
        MoonlightApp._hideNavButtonsConditionally();
        MoonlightApp._initNavButtons();
        expect(document.getElementById('btn-admin').style.display).toBe('');
    });

    describe('the control channel', () => {
        let opened;

        beforeEach(() => {
            opened = [];
            vi.stubGlobal(
                'WebSocket',
                class {
                    constructor(url) {
                        opened.push(url);
                    }
                    close() {}
                },
            );
        });

        it('is not asked for by a page the host does not call its own machine', () => {
            MoonlightApp._readAccess({ authenticated: true });
            MoonlightApp._initControlChannel();
            expect(opened).toEqual([]);
        });

        it("is asked for by the host machine's own browser", () => {
            MoonlightApp._readAccess({ is_localhost: true, is_host_machine: true });
            MoonlightApp._initControlChannel();
            expect(opened).toEqual(['wss://localhost:8443/ws/control']);
        });
    });
});

describe('the admin page, opened by localhost', () => {
    let view;

    beforeEach(() => {
        document.body.innerHTML = '<div id="admin"></div>';
        view = new AdminView(document.getElementById('admin'), () => {});
        vi.spyOn(BackendClient, 'checkForUpdate').mockImplementation(() => {});
        vi.spyOn(console, 'warn').mockImplementation(() => {});
    });

    afterEach(() => {
        view.destroy?.();
        vi.restoreAllMocks();
        document.body.innerHTML = '';
    });

    it('is nobody the host has not named', () => {
        expect(view._isLocalhost()).toBe(false);
    });

    it('shows a session without admin access the notice, and asks nothing host-only', async () => {
        vi.spyOn(BackendClient, 'getAuthStatus').mockResolvedValue({ authenticated: true });
        const hostOnly = [
            '_loadState',
            '_loadStreamingState',
            '_loadHostApps',
            '_loadSessions',
        ].map((step) => vi.spyOn(view, step).mockImplementation(async () => {}));

        await view.start();

        expect(view._isLocalhost()).toBe(false);
        for (const step of hostOnly) expect(step).not.toHaveBeenCalled();
        expect(document.getElementById('view-admin')).not.toBeNull();
        expect(document.getElementById('admin-section-internet')).toBeNull();
    });

    it('shows nothing more when the host does not answer', async () => {
        vi.spyOn(BackendClient, 'getAuthStatus').mockRejectedValue(new Error('down'));
        const state = vi.spyOn(view, '_loadState').mockImplementation(async () => {});

        await view.start();

        expect(view._isLocalhost()).toBe(false);
        expect(state).not.toHaveBeenCalled();
    });

    it('is the admin the host says it is', async () => {
        vi.spyOn(BackendClient, 'getAuthStatus').mockResolvedValue({ is_localhost: true });
        await view._loadAuthStatus();
        expect(view._isLocalhost()).toBe(true);
    });
});

describe("Settings, on a page opened by localhost: the server's defaults", () => {
    let view;
    let sent;

    beforeEach(() => {
        localStorage.clear();
        document.body.innerHTML = '<div id="settings"></div>';
        view = new SettingsView(document.getElementById('settings'), () => {});
        sent = vi.spyOn(BackendClient, 'saveStreamingSettings').mockResolvedValue({});
        vi.spyOn(console, 'warn').mockImplementation(() => {});
    });

    afterEach(() => {
        view.destroy?.();
        vi.restoreAllMocks();
        localStorage.clear();
        document.body.innerHTML = '';
    });

    it('are left alone by a session the host gives no admin access', async () => {
        vi.spyOn(BackendClient, 'getAuthStatus').mockResolvedValue({ authenticated: true });

        await view._saveToStorage();

        expect(localStorage.getItem('mw-streaming-settings')).not.toBeNull();
        expect(sent).not.toHaveBeenCalled();
    });

    it('are left alone when the host does not answer', async () => {
        vi.spyOn(BackendClient, 'getAuthStatus').mockRejectedValue(new Error('down'));

        await view._saveToStorage();

        expect(localStorage.getItem('mw-streaming-settings')).not.toBeNull();
        expect(sent).not.toHaveBeenCalled();
    });

    it("are written by the host machine's own browser, asked who it is once", async () => {
        const asked = vi
            .spyOn(BackendClient, 'getAuthStatus')
            .mockResolvedValue({ is_localhost: true, is_host_machine: true });

        await view._saveToStorage();
        await view._saveToStorage();

        expect(sent).toHaveBeenCalledTimes(2);
        expect(asked).toHaveBeenCalledTimes(1);
    });
});
