/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
/*
 * The first-run wizard on a machine that hosts itself. Since the native engine
 * reached macOS and Linux (design §19, §20) the usual first launch has nothing
 * to install, and the wizard must stop asking for a second streaming server's
 * credentials — while still offering Sunshine on the machines that genuinely
 * need it (an AppImage, a box with no encoder).
 *
 * The failure this guards is silent in both directions: a wizard that keeps
 * asking for Sunshine sends the user to install something they do not need,
 * and one that stops asking on a machine with no engine leaves it with no host
 * at all.
 */
import { describe, it, expect, beforeEach, vi } from 'vitest';

vi.mock('../js/api/BackendClient.js', () => ({
    BackendClient: {
        getSetupStatus: vi.fn(),
        applySetup: vi.fn(async () => ({ ok: true, internet_active: false })),
        checkSunshineCredentials: vi.fn(async () => ({ ok: true })),
    },
}));
vi.mock('../js/i18n/i18n.js', () => ({
    t: (key) => 'text:' + key,
}));

import { SetupView } from '../js/ui/SetupView.js';
import { BackendClient } from '../js/api/BackendClient.js';

// The shape of /api/setup/status, with only what this file varies.
function status(overrides = {}) {
    return {
        os: 'Linux',
        setup_completed: false,
        headless: false,
        https_port: 443,
        autostart_installed: true,
        display_sleep: { supported: false, kept_awake: false },
        internet: { active: true, domain: '' },
        native: {
            available: false,
            reason: 'probe failed',
            needs_permission: false,
            possible: false,
        },
        sunshine: { installed: false, paired: false, can_auto_install: true, running: false },
        ...overrides,
    };
}

describe('SetupView — a machine that streams itself', () => {
    let container;
    let view;

    async function start(st) {
        BackendClient.getSetupStatus.mockResolvedValue(st);
        view = new SetupView(container, () => {});
        await view.start();
        return view;
    }

    const html = () => container.innerHTML;

    beforeEach(() => {
        vi.clearAllMocks();
        document.body.innerHTML = '<div id="setup"></div>';
        container = document.getElementById('setup');
    });

    it('says the machine hosts itself, and asks nothing about Sunshine', async () => {
        await start(
            status({
                native: {
                    available: true,
                    reason: 'available',
                    needs_permission: false,
                    possible: true,
                },
            }),
        );

        expect(html()).toContain('text:setup.hostNative');
        expect(html()).not.toContain('text:setup.sunshineTitle');
        expect(container.querySelector('#setup-user')).toBeNull();
        expect(container.querySelector('#setup-pass')).toBeNull();
        expect(container.querySelector('#chk-install')).toBeNull();
    });

    it('never asks to install Sunshine, even when one is installed here already', async () => {
        // The engine is here AND an unpaired Sunshine is installed: pairing it
        // belongs to the hosts page, on the user's initiative — not to a
        // first-run step that would demand credentials to get past.
        await start(
            status({
                native: {
                    available: true,
                    reason: 'available',
                    needs_permission: false,
                    possible: true,
                },
                sunshine: { installed: true, paired: false, can_auto_install: true, running: true },
            }),
        );

        expect(container.querySelector('#setup-pass')).toBeNull();
        await view._apply();

        expect(BackendClient.checkSunshineCredentials).not.toHaveBeenCalled();
        const sent = BackendClient.applySetup.mock.calls[0][0];
        expect(sent.sunshine.install).toBe(false);
        expect(view._activeSteps).not.toContain('install');
        expect(view._activeSteps).not.toContain('pairing');
    });

    it('points at the missing macOS permission instead of offering Sunshine', async () => {
        // Screen Recording is not granted yet, so the engine reports
        // unavailable — but it is one checkbox away, not a reason to install a
        // second streaming server.
        await start(
            status({
                os: 'macOS',
                native: {
                    available: false,
                    reason: 'screen capture permission not granted',
                    needs_permission: true,
                    possible: true,
                },
            }),
        );

        expect(html()).toContain('text:setup.hostPermission');
        expect(html()).not.toContain('text:setup.sunshineNotDetected');
        expect(container.querySelector('#chk-install')).toBeNull();

        // …and the done screen names the same permission, for MoonlightWeb.
        view._step = 'done';
        view.render();
        expect(html()).toContain('text:setup.donePermissionsNative');
        expect(html()).not.toContain('text:setup.donePermissions"');
    });

    it('still offers Sunshine where no engine can run — an AppImage, no encoder', async () => {
        await start(
            status({
                native: {
                    available: false,
                    reason: 'architecture not supported',
                    needs_permission: false,
                    possible: false,
                },
            }),
        );

        expect(html()).toContain('text:setup.sunshineNotDetected');
        expect(container.querySelector('#chk-install')).not.toBeNull();
        expect(container.querySelector('#setup-user')).not.toBeNull();
        expect(view._installSunshine).toBe(true);
    });

    it('treats a status with no native object as "cannot host itself"', async () => {
        // An older server, or a probe that answered nothing: offer help rather
        // than withhold it.
        const st = status();
        delete st.native;
        await start(st);

        expect(html()).toContain('text:setup.sunshineNotDetected');
        expect(view._nativePossible).toBe(false);
    });
});
