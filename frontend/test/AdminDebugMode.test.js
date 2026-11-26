/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';

// Advanced → Debug mode: one box for the debug options and the detailed logs,
// which used to be two (debug mode, verbose logs). Saved the moment it changes,
// showing the mode in force, and locked on only by --debug.
vi.mock('../js/api/BackendClient.js', () => ({
    BackendClient: { getAdminSettings: vi.fn(), saveAdminSettings: vi.fn() },
}));
vi.mock('../js/ui/Toast.js', () => ({
    Toast: { success: vi.fn(), error: vi.fn(), warning: vi.fn() },
}));
vi.mock('../js/util/ClientLog.js', () => ({ refreshClientLog: vi.fn() }));

import { AdminView } from '../js/ui/AdminView.js';
import { BackendClient } from '../js/api/BackendClient.js';
import { Toast } from '../js/ui/Toast.js';
import { refreshClientLog } from '../js/util/ClientLog.js';

describe('AdminView — one Debug box', () => {
    let view;

    const box = () => /** @type {HTMLInputElement} */ (document.querySelector('#chk-debug-mode'));

    beforeEach(() => {
        document.body.innerHTML = '<div></div>';
        view = new AdminView(document.body, () => {});
        BackendClient.getAdminSettings.mockReset();
        BackendClient.saveAdminSettings.mockReset();
        Toast.success.mockReset();
        Toast.error.mockReset();
        refreshClientLog.mockReset();
    });

    afterEach(() => {
        view.destroy();
    });

    it('draws one box, and no verbose box', () => {
        document.body.innerHTML = view._renderAdvanced();
        expect(box()).not.toBeNull();
        expect(document.querySelector('#chk-verbose-logs')).toBeNull();
        expect(document.querySelectorAll('#admin-section-advanced input[type=checkbox]').length).toBe(1);
    });

    it('shows debug mode as the server holds it, whatever verbose says', async () => {
        BackendClient.getAdminSettings.mockResolvedValue({
            https_port: 443,
            debug_mode: false,
            verbose_logs: true,
            verbose_logs_cli: true,
        });
        await view._loadState();
        document.body.innerHTML = view._renderAdvanced();
        expect(box().checked).toBe(false);
        expect(box().disabled).toBe(false); // --verbose alone does not lock it

        BackendClient.getAdminSettings.mockResolvedValue({ https_port: 443, debug_mode: true });
        await view._loadState();
        document.body.innerHTML = view._renderAdvanced();
        expect(box().checked).toBe(true);
    });

    it('is locked on by --debug, and says so', async () => {
        BackendClient.getAdminSettings.mockResolvedValue({
            https_port: 443,
            debug_mode: true,
            debug_mode_cli: true,
        });
        await view._loadState();
        document.body.innerHTML = view._renderAdvanced();
        expect(box().checked).toBe(true);
        expect(box().disabled).toBe(true);
        expect(box().closest('.settings-field').querySelector('.settings-hint')).not.toBeNull();
    });

    it('saves debug_mode only, and shows the mode in force', async () => {
        document.body.innerHTML = view._renderAdvanced();
        box().checked = true;
        BackendClient.saveAdminSettings.mockResolvedValue({ debug_mode: true, verbose_logs: true });
        await view._saveDiagMode(box());
        expect(BackendClient.saveAdminSettings).toHaveBeenCalledWith({ debug_mode: true });
        expect(view._debugMode).toBe(true);
        expect(box().checked).toBe(true);
        expect(refreshClientLog).toHaveBeenCalled();
        expect(Toast.success).toHaveBeenCalled();

        // --debug holds it on: unticking leaves it ticked, as the server says.
        box().checked = false;
        BackendClient.saveAdminSettings.mockResolvedValue({ debug_mode: true, verbose_logs: true });
        await view._saveDiagMode(box());
        expect(box().checked).toBe(true);
    });

    it('goes back when the server refuses', async () => {
        document.body.innerHTML = view._renderAdvanced();
        box().checked = true;
        BackendClient.saveAdminSettings.mockRejectedValue(new Error('forbidden'));
        await view._saveDiagMode(box());
        expect(Toast.error).toHaveBeenCalled();
        expect(box().checked).toBe(false);
    });
});
