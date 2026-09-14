/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
/*
 * Start at login in the first-run wizard: applied by default with the rest of
 * the setup, then shown as a ticked box on the last page — above the button
 * that leaves — where unticking it removes the login item on the spot through
 * the same route the admin page uses. Never offered where the server has no
 * login item to write (a service install).
 */
import { describe, it, expect, beforeEach, vi } from 'vitest';

vi.mock('../js/api/BackendClient.js', () => ({
    BackendClient: {
        getSetupStatus: vi.fn(),
        applySetup: vi.fn(async () => ({ ok: true, internet_active: false, autostart: true })),
        saveAdminSettings: vi.fn(async (body) => ({ autostart_enabled: !!body.autostart_enabled })),
        checkSunshineCredentials: vi.fn(async () => ({ ok: true })),
    },
}));
vi.mock('../js/i18n/i18n.js', () => ({
    t: (key) => 'text:' + key,
}));

import { SetupView } from '../js/ui/SetupView.js';
import { BackendClient } from '../js/api/BackendClient.js';

function status(overrides = {}) {
    return {
        os: 'Linux',
        setup_completed: false,
        headless: false,
        https_port: 443,
        autostart_supported: true,
        autostart_installed: false,
        display_sleep: { supported: false, kept_awake: false },
        internet: { active: true, domain: '' },
        native: { available: true, reason: 'available', needs_permission: false, possible: true },
        sunshine: { installed: false, paired: false, can_auto_install: false, running: false },
        ...overrides,
    };
}

const flush = () => new Promise((r) => setTimeout(r, 0));

describe('SetupView — start at login', () => {
    let container;
    let view;

    async function start(st) {
        BackendClient.getSetupStatus.mockResolvedValue(st);
        view = new SetupView(container, () => {});
        await view.start();
        return view;
    }

    beforeEach(() => {
        vi.clearAllMocks();
        document.body.innerHTML = '<div id="setup"></div>';
        container = document.getElementById('setup');
    });

    it('asks nothing on the options page and applies it by default', async () => {
        await start(status());
        expect(container.querySelector('#chk-autostart')).toBeNull();

        container.querySelector('#btn-setup-start').click();
        await flush();
        await flush();

        const sent = BackendClient.applySetup.mock.calls[0][0];
        expect(sent.autostart).toBe(true);
        expect(view._step).toBe('done');
    });

    it('shows it ticked on the last page, above the way out, and unticking removes it', async () => {
        await start(status());
        container.querySelector('#btn-setup-start').click();
        await flush();
        await flush();

        const box = container.querySelector('#chk-autostart');
        expect(box).not.toBeNull();
        expect(box.checked).toBe(true);
        // Above the button that leaves the wizard.
        const finish = container.querySelector('#btn-setup-finish');
        expect(box.compareDocumentPosition(finish) & Node.DOCUMENT_POSITION_FOLLOWING).toBeTruthy();

        box.checked = false;
        box.dispatchEvent(new Event('change'));
        await flush();
        expect(BackendClient.saveAdminSettings).toHaveBeenCalledWith({ autostart_enabled: false });
        expect(box.checked).toBe(false);
        expect(view._autostartInstalled).toBe(false);
    });

    it('does not re-apply over a login item already there', async () => {
        await start(status({ autostart_installed: true }));
        container.querySelector('#btn-setup-start').click();
        await flush();
        await flush();

        expect(BackendClient.applySetup.mock.calls[0][0].autostart).toBe(false);
        expect(container.querySelector('#chk-autostart').checked).toBe(true);
    });

    it('offers no box where the server cannot write one', async () => {
        await start(status({ autostart_supported: false }));
        container.querySelector('#btn-setup-start').click();
        await flush();
        await flush();

        expect(BackendClient.applySetup.mock.calls[0][0].autostart).toBe(false);
        expect(view._step).toBe('done');
        expect(container.querySelector('#chk-autostart')).toBeNull();
    });
});
