/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 *
 * TCP 443 is asked of the router only while a browser needs it, and taken off
 * after. A router that will not let this install remove its 443 entry, under
 * this PC's address or an old one, is said on the admin page: a yes from the
 * server (router443_stale_removal_refused), a fixed text of the page's own,
 * and nothing a router answered.
 */
import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';

vi.mock('../js/api/BackendClient.js', () => ({
    BackendClient: { getInternetStatus: vi.fn(), getGamepadDriver: vi.fn(async () => ({})) },
}));
vi.mock('../js/ui/Toast.js', () => ({
    Toast: { success: vi.fn(), error: vi.fn(), warning: vi.fn() },
}));
vi.mock('../js/i18n/i18n.js', () => ({
    t: (key) => 'text:' + key,
}));

import { AdminView } from '../js/ui/AdminView.js';
import { BackendClient } from '../js/api/BackendClient.js';

const status = (extra) => ({
    internet_access_enabled: true,
    active: true,
    phase: 'active',
    upnp_enabled: true,
    upnp_available: true,
    public_ip: '203.0.113.7',
    rendezvous: { url: 'https://stream.moonlightweb.top/a1b2c3d4e5f6g7h8j9k0m1n2p3', online: true },
    ...extra,
});

describe('AdminView — a TCP 443 forward the router keeps', () => {
    let view;

    beforeEach(() => {
        document.body.innerHTML = '<div></div>';
        view = new AdminView(document.body.firstElementChild, () => {});
        view._hasAdminAccess = true;
        BackendClient.getInternetStatus.mockReset();
    });

    afterEach(() => view.destroy());

    const show = async (extra) => {
        BackendClient.getInternetStatus.mockResolvedValue(status(extra));
        await view._loadInternetState();
        view.render();
        return document.querySelector('#tcp443-stuck');
    };

    it('says so, in the words the page holds', async () => {
        const note = await show({ router443_stale_removal_refused: true });
        expect(note).not.toBeNull();
        expect(note.textContent.trim()).toBe(
            'A TCP 443 port forward to this PC could not be removed: remove it in your box.',
        );
        expect(note.textContent.trim()).toBe(AdminView.TCP443_STUCK_NOTICE);
        expect(note.classList.contains('internet-info-error')).toBe(true);
        // No address, and no port but the one it is about
        expect(note.textContent).not.toMatch(/\d+\.\d+\.\d+\.\d+/);
    });

    it('is not shown otherwise, nor for anything but a yes', async () => {
        for (const extra of [
            {},
            { router443_stale_removal_refused: false },
            { router443_stale_removal_refused: 'true' },
            { router443_stale_removal_refused: 1 },
        ])
            expect(await show(extra)).toBeNull();
    });

    it('goes once the server no longer says it', async () => {
        expect(await show({ router443_stale_removal_refused: true })).not.toBeNull();
        expect(await show({ router443_stale_removal_refused: false })).toBeNull();
    });
});
