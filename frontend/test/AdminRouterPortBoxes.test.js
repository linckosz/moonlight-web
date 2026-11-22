/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 *
 * The two boxes of admin › Internet: "One shared port for streams"
 * (ice_udp_mux), applied by a restart of the server once no stream runs, and
 * "Also through TCP 443" (ice_tcp_443), applied at once and shown only while
 * the shared ports run where it can apply.
 */
import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';

vi.mock('../js/api/BackendClient.js', () => ({
    BackendClient: {
        getAdminSettings: vi.fn(),
        saveAdminSettings: vi.fn(),
        getAuthStatus: vi.fn(),
        getGamepadDriver: vi.fn(async () => ({})),
    },
}));
vi.mock('../js/ui/Toast.js', () => ({
    Toast: { success: vi.fn(), error: vi.fn(), warning: vi.fn(), info: vi.fn() },
}));
vi.mock('../js/i18n/i18n.js', () => ({
    t: (key) => 'text:' + key,
}));

import { AdminView } from '../js/ui/AdminView.js';
import { BackendClient } from '../js/api/BackendClient.js';
import { Toast } from '../js/ui/Toast.js';

const admin = (extra) => ({
    ice_udp_mux: true,
    ice_tcp_443: true,
    shared_ports_running: true,
    tcp443_applies: true,
    restart_pending: false,
    ...extra,
});

describe('AdminView — the boxes of the router ports', () => {
    let view;

    beforeEach(() => {
        document.body.innerHTML = '<div></div>';
        view = new AdminView(document.body.firstElementChild, () => {});
        view._hasAdminAccess = true;
        BackendClient.getAdminSettings.mockReset();
        BackendClient.saveAdminSettings.mockReset();
        Toast.info.mockReset();
    });

    afterEach(() => view.destroy());

    const show = async (extra) => {
        BackendClient.getAdminSettings.mockResolvedValue(admin(extra));
        await view._loadState();
        view.render();
        view.bindEvents();
        return {
            shared: document.querySelector('#chk-shared-ports'),
            tcp443: document.querySelector('#chk-tcp443'),
            waiting: document.querySelector('#shared-ports-waiting'),
        };
    };

    it('shows both boxes as the file asks while the shared ports run', async () => {
        const { shared, tcp443, waiting } = await show({});
        expect(shared.checked).toBe(true);
        expect(tcp443.checked).toBe(true);
        expect(waiting).toBeNull();
        const off = await show({ ice_udp_mux: false, ice_tcp_443: false });
        expect(off.shared.checked).toBe(false);
        expect(off.tcp443.checked).toBe(false);
    });

    it('hides 443 while the shared ports do not run, or where it cannot apply', async () => {
        for (const extra of [{ shared_ports_running: false }, { tcp443_applies: false }]) {
            const { shared, tcp443 } = await show(extra);
            expect(shared).not.toBeNull();
            expect(tcp443).toBeNull();
        }
        // The stored choice is not lost while it is hidden.
        await show({ shared_ports_running: false, ice_tcp_443: false });
        expect(view._tcp443).toBe(false);
    });

    it('says the restart waits for the streams, and only then', async () => {
        const { waiting } = await show({ restart_pending: true });
        expect(waiting.textContent).toBe('text:admin.sharedPortsRestartWaiting');
    });

    it('writes the shared port box alone, and waits when streams run', async () => {
        const { shared } = await show({});
        BackendClient.saveAdminSettings.mockResolvedValue({ restart: 'after_streams' });
        shared.checked = false;
        shared.dispatchEvent(new Event('change'));
        await vi.waitFor(() =>
            expect(document.querySelector('#shared-ports-waiting')).not.toBeNull(),
        );
        expect(BackendClient.saveAdminSettings).toHaveBeenCalledWith({ ice_udp_mux: false });
        expect(Toast.info).not.toHaveBeenCalled();
    });

    it('says the server restarts, and reconnects', async () => {
        const { shared } = await show({});
        const reconnect = vi.spyOn(view, '_reconnectAfterRestart').mockResolvedValue();
        BackendClient.saveAdminSettings.mockResolvedValue({ restart: 'now' });
        shared.checked = false;
        shared.dispatchEvent(new Event('change'));
        await vi.waitFor(() => expect(reconnect).toHaveBeenCalled());
        expect(Toast.info).toHaveBeenCalledWith('text:admin.sharedPortsRestarting');
    });

    it('writes the 443 box alone, and puts it back on a refusal', async () => {
        const { tcp443 } = await show({});
        BackendClient.saveAdminSettings.mockResolvedValue({ ice_tcp_443: false });
        tcp443.checked = false;
        tcp443.dispatchEvent(new Event('change'));
        await vi.waitFor(() =>
            expect(BackendClient.saveAdminSettings).toHaveBeenCalledWith({ ice_tcp_443: false }),
        );
        BackendClient.saveAdminSettings.mockRejectedValue(new Error('refused'));
        tcp443.checked = true;
        tcp443.dispatchEvent(new Event('change'));
        await vi.waitFor(() => expect(tcp443.checked).toBe(false));
    });

    it('cannot be clicked from a view that may not write them', async () => {
        view._hasAdminAccess = false;
        const html = view._renderRouterPortBoxes();
        document.body.innerHTML = html;
        expect(document.querySelector('#chk-shared-ports').disabled).toBe(true);
    });
});
