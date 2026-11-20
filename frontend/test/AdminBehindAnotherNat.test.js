/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 *
 * A router whose own address is not the public one (CGNAT, or another router
 * in front). Internet Access keeps working there: the address goes through the
 * rendezvous line, which needs no inbound port. The page used to get this as
 * last_error, which unticks the box, so the box could never stay ticked. It is
 * a notice of its own now, shown under UPnP, and the box stays as it is.
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

describe('AdminView — a router behind another NAT', () => {
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
        return {
            box: document.querySelector('#chk-internet-enable'),
            note: document.querySelector('#internet-behind-nat'),
        };
    };

    it('keeps the box ticked and shows the notice, under UPnP', async () => {
        const { box, note } = await show({ behind_another_nat: true });
        expect(view._internetEnabled).toBe(true);
        expect(box.checked).toBe(true);
        expect(note).not.toBeNull();
        expect(note.textContent).toBe('text:admin.behindAnotherNat');
        expect(note.closest('.internet-info-box')).not.toBeNull();
        // Not the error box, and no error box at all.
        expect(document.querySelector('.internet-info-error')).toBeNull();
    });

    it('carries no address of its own', async () => {
        const { note } = await show({ behind_another_nat: true });
        expect(note.textContent).not.toMatch(/\d+\.\d+\.\d+\.\d+/);
    });

    it('is not shown on a router that is on the internet itself', async () => {
        for (const extra of [{ behind_another_nat: false }, {}]) {
            const { box, note } = await show(extra);
            expect(note).toBeNull();
            expect(box.checked).toBe(true);
        }
    });

    it('is not shown when the router is not asked, or did not answer', async () => {
        for (const extra of [{ upnp_enabled: false }, { upnp_available: false }]) {
            const { note } = await show({ behind_another_nat: true, ...extra });
            expect(note).toBeNull();
        }
    });

    it('leaves a real error what it was: the box unticks for it', async () => {
        const { box, note } = await show({
            behind_another_nat: true,
            last_error: 'Failed to detect public IP via STUN',
        });
        expect(view._internetEnabled).toBe(false);
        expect(box.checked).toBe(false);
        expect(document.querySelector('.internet-info-error').textContent).toContain(
            'Failed to detect public IP via STUN',
        );
        // The notice still says what it says.
        expect(note).not.toBeNull();
    });
});
