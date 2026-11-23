/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 *
 * The two boxes of admin › Internet: "One shared port for streams"
 * (ice_udp_mux), applied by a restart of the server once no stream runs, after
 * an Apply under the box (Cancel or leaving the page writes nothing), and
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
        Toast.error.mockReset();
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

    const untick = (shared) => {
        shared.checked = !shared.checked;
        shared.dispatchEvent(new Event('change'));
    };

    it('only asks when the shared port box is ticked: nothing is written', async () => {
        const { shared } = await show({});
        expect(document.querySelector('#shared-ports-confirm')).toBeNull();
        untick(shared);
        const confirm = document.querySelector('#shared-ports-confirm');
        expect(confirm).not.toBeNull();
        expect(confirm.textContent).toContain('text:admin.sharedPortsConfirm');
        expect(document.querySelector('#chk-shared-ports').checked).toBe(false);
        expect(BackendClient.saveAdminSettings).not.toHaveBeenCalled();
        // Ticked back: no question any more, still nothing written.
        untick(document.querySelector('#chk-shared-ports'));
        expect(document.querySelector('#shared-ports-confirm')).toBeNull();
        expect(BackendClient.saveAdminSettings).not.toHaveBeenCalled();
    });

    it('Cancel writes nothing and puts the box back', async () => {
        const { shared } = await show({});
        untick(shared);
        document.querySelector('#btn-shared-ports-cancel').click();
        expect(document.querySelector('#shared-ports-confirm')).toBeNull();
        expect(document.querySelector('#chk-shared-ports').checked).toBe(true);
        expect(view._sharedPorts).toBe(true);
        expect(BackendClient.saveAdminSettings).not.toHaveBeenCalled();
        expect(Toast.info).not.toHaveBeenCalled();
    });

    it('leaving or reloading the page drops the question, unwritten', async () => {
        const { shared } = await show({});
        untick(shared);
        view.destroy();
        document.body.innerHTML = '<div></div>';
        view = new AdminView(document.body.firstElementChild, () => {});
        view._hasAdminAccess = true;
        const again = await show({});
        expect(again.shared.checked).toBe(true);
        expect(document.querySelector('#shared-ports-confirm')).toBeNull();
        expect(BackendClient.saveAdminSettings).not.toHaveBeenCalled();
    });

    it('a field edited and not saved survives the question, its Cancel and its Apply', async () => {
        const { shared } = await show({});
        const name = document.querySelector('#admin-instance-name');
        const port = document.querySelector('#admin-https-port');
        expect(name).not.toBeNull();
        expect(port).not.toBeNull();
        name.value = 'typed, not saved';
        port.value = '8443';
        untick(shared);
        document.querySelector('#btn-shared-ports-cancel').click();
        untick(document.querySelector('#chk-shared-ports'));
        BackendClient.saveAdminSettings.mockResolvedValue({ restart: 'after_streams' });
        document.querySelector('#btn-shared-ports-apply').click();
        await vi.waitFor(() =>
            expect(document.querySelector('#shared-ports-waiting')).not.toBeNull(),
        );
        // The same fields, never drawn again, with what was typed in them.
        expect(document.querySelector('#admin-instance-name')).toBe(name);
        expect(name.value).toBe('typed, not saved');
        expect(document.querySelector('#admin-https-port')).toBe(port);
        expect(port.value).toBe('8443');
        expect(BackendClient.saveAdminSettings).toHaveBeenCalledTimes(1);
    });

    it('Apply writes the shared port box alone, and waits when streams run', async () => {
        const { shared } = await show({});
        BackendClient.saveAdminSettings.mockResolvedValue({ restart: 'after_streams' });
        untick(shared);
        document.querySelector('#btn-shared-ports-apply').click();
        await vi.waitFor(() =>
            expect(document.querySelector('#shared-ports-waiting')).not.toBeNull(),
        );
        expect(BackendClient.saveAdminSettings).toHaveBeenCalledTimes(1);
        expect(BackendClient.saveAdminSettings).toHaveBeenCalledWith({ ice_udp_mux: false });
        expect(document.querySelector('#shared-ports-confirm')).toBeNull();
        expect(document.querySelector('#chk-shared-ports').checked).toBe(false);
        expect(Toast.info).not.toHaveBeenCalled();
    });

    it('Apply with no stream: says the server restarts, and reconnects', async () => {
        const { shared } = await show({});
        const reconnect = vi.spyOn(view, '_reconnectAfterRestart').mockResolvedValue();
        BackendClient.saveAdminSettings.mockResolvedValue({ restart: 'now' });
        untick(shared);
        expect(reconnect).not.toHaveBeenCalled();
        document.querySelector('#btn-shared-ports-apply').click();
        await vi.waitFor(() => expect(reconnect).toHaveBeenCalled());
        expect(BackendClient.saveAdminSettings).toHaveBeenCalledWith({ ice_udp_mux: false });
        expect(Toast.info).toHaveBeenCalledWith('text:admin.sharedPortsRestarting');
    });

    it('a refused Apply keeps the question open', async () => {
        const { shared } = await show({});
        BackendClient.saveAdminSettings.mockRejectedValue(new Error('refused'));
        untick(shared);
        const apply = document.querySelector('#btn-shared-ports-apply');
        apply.click();
        await vi.waitFor(() => expect(Toast.error).toHaveBeenCalled());
        expect(document.querySelector('#shared-ports-confirm')).not.toBeNull();
        expect(view._sharedPorts).toBe(true);
        expect(apply.disabled).toBe(false);
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
