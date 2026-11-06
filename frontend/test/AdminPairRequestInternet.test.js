/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';

// The box under the PIN that takes QR code pairing requests from outside the
// LAN too: unticked until the server says otherwise, saved the moment it
// changes, and showing what the server kept.
vi.mock('../js/api/BackendClient.js', () => ({
    BackendClient: { getAdminSettings: vi.fn(), saveAdminSettings: vi.fn() },
}));
vi.mock('../js/ui/Toast.js', () => ({
    Toast: { success: vi.fn(), error: vi.fn(), warning: vi.fn() },
}));

import { AdminView } from '../js/ui/AdminView.js';
import { BackendClient } from '../js/api/BackendClient.js';
import { Toast } from '../js/ui/Toast.js';

describe('AdminView — QR code requests from the internet', () => {
    let view;

    const box = () => document.querySelector('#chk-pair-request-internet');

    beforeEach(() => {
        document.body.innerHTML = '<div></div>';
        view = new AdminView(document.body, () => {});
        BackendClient.getAdminSettings.mockReset();
        BackendClient.saveAdminSettings.mockReset();
        Toast.success.mockReset();
        Toast.error.mockReset();
    });

    afterEach(() => {
        view.destroy();
    });

    it('is unticked until the server says otherwise, an older backend included', async () => {
        expect(view._pairRequestInternet).toBe(false);
        BackendClient.getAdminSettings.mockResolvedValue({ https_port: 443 });
        await view._loadState();
        expect(view._pairRequestInternet).toBe(false);

        BackendClient.getAdminSettings.mockResolvedValue({
            https_port: 443,
            pair_request_internet: true,
        });
        await view._loadState();
        expect(view._pairRequestInternet).toBe(true);
    });

    it('shows the setting as the server holds it', () => {
        view._pairRequestInternet = true;
        document.body.innerHTML = view._renderPairRequestInternet();
        expect(box().checked).toBe(true);
        view._pairRequestInternet = false;
        document.body.innerHTML = view._renderPairRequestInternet();
        expect(box().checked).toBe(false);
    });

    it('saves a change on the spot, through the admin settings', async () => {
        document.body.innerHTML = view._renderPairRequestInternet();
        BackendClient.saveAdminSettings.mockResolvedValue({ pair_request_internet: true });
        await view._togglePairRequestInternet(true);
        expect(BackendClient.saveAdminSettings).toHaveBeenCalledWith({
            pair_request_internet: true,
        });
        expect(view._pairRequestInternet).toBe(true);
        expect(box().checked).toBe(true);
        expect(Toast.success).toHaveBeenCalled();
    });

    it('goes back to what the server kept when it refuses', async () => {
        document.body.innerHTML = view._renderPairRequestInternet();
        box().checked = true;
        BackendClient.saveAdminSettings.mockRejectedValue(new Error('forbidden'));
        await view._togglePairRequestInternet(true);
        expect(Toast.error).toHaveBeenCalled();
        expect(view._pairRequestInternet).toBe(false);
        expect(box().checked).toBe(false);
    });
});
