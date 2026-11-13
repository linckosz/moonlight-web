/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 *
 * The warning at the top of the admin page when remembered devices stay signed
 * in longer than the default (remember_me_days in settings.json, issue #60).
 * It shows above the default, closes only with its button, and comes back when
 * the value changes; it is built from whole numbers alone.
 */
import { describe, it, expect, beforeEach, vi } from 'vitest';

vi.mock('../js/api/BackendClient.js', () => ({
    BackendClient: {
        getAdminSettings: vi.fn(),
        saveAdminSettings: vi.fn(async (body) => body),
    },
}));
vi.mock('../js/ui/Toast.js', () => ({
    Toast: { success: vi.fn(), error: vi.fn(), warning: vi.fn(), info: vi.fn() },
}));
// No locale is loaded in the TNR: render the key and its parameters instead.
vi.mock('../js/i18n/i18n.js', () => ({
    t: (key, params) => (params ? key + ' ' + JSON.stringify(params) : key),
    getLanguage: () => 'en',
}));

import { AdminView } from '../js/ui/AdminView.js';
import { BackendClient } from '../js/api/BackendClient.js';
import { Toast } from '../js/ui/Toast.js';

describe('AdminView: remembered devices stay signed in longer than the default', () => {
    let view;

    /** The admin page's state from GET /api/admin/settings, then the warning. */
    async function show(admin, { adminAccess = true } = {}) {
        BackendClient.getAdminSettings.mockResolvedValue(admin);
        view = new AdminView(document.body, () => {});
        view._hasAdminAccess = adminAccess;
        await view._loadState();
        document.body.innerHTML = view._rememberMeRiskHtml();
        view._bindRememberMeRisk();
        return document.getElementById('admin-remember-me-risk');
    }

    beforeEach(() => {
        vi.clearAllMocks();
        document.body.innerHTML = '';
    });

    it('says nothing at the default or below', async () => {
        expect(await show({ remember_me_days: 90, remember_me_days_default: 90 })).toBeNull();
        expect(await show({ remember_me_days: 30, remember_me_days_default: 90 })).toBeNull();
    });

    it('says nothing to an older server that does not send the value', async () => {
        expect(await show({})).toBeNull();
    });

    it('warns above the default, until acknowledged', async () => {
        const risk = await show({
            remember_me_days: 400,
            remember_me_days_default: 90,
            remember_me_days_acknowledged: 0,
        });
        expect(risk).not.toBeNull();
        expect(risk.getAttribute('role')).toBe('alert');
        expect(risk.textContent).toContain('admin.rememberMeRiskTitle {"days":"400","years":0}');
        expect(risk.textContent).toContain('admin.rememberMeRiskBody {"days":"400"}');
        expect(risk.querySelector('#btn-remember-me-ack')).not.toBeNull();
    });

    it('names whole years in the title', async () => {
        let risk = await show({ remember_me_days: 365, remember_me_days_default: 90 });
        expect(risk.textContent).toContain(
            'admin.rememberMeRiskTitleYear {"days":"365","years":1}',
        );
        risk = await show({ remember_me_days: 3650, remember_me_days_default: 90 });
        expect(risk.textContent).toContain(
            'admin.rememberMeRiskTitleYears {"days":"3,650","years":10}',
        );
    });

    it('stays closed for the value that was acknowledged', async () => {
        expect(
            await show({
                remember_me_days: 365,
                remember_me_days_default: 90,
                remember_me_days_acknowledged: 365,
            }),
        ).toBeNull();
    });

    it('comes back when the value changes', async () => {
        const risk = await show({
            remember_me_days: 730,
            remember_me_days_default: 90,
            remember_me_days_acknowledged: 365,
        });
        expect(risk).not.toBeNull();
    });

    it('acknowledges the value it shows, then goes', async () => {
        await show({ remember_me_days: 730, remember_me_days_default: 90 });
        document.getElementById('btn-remember-me-ack').click();
        await vi.waitFor(() =>
            expect(document.getElementById('admin-remember-me-risk')).toBeNull(),
        );
        expect(BackendClient.saveAdminSettings).toHaveBeenCalledWith({
            remember_me_days_acknowledged: 730,
        });
        expect(view._rememberMeDaysAck).toBe(730);
        // Drawn again, as after a reload: still closed.
        expect(view._rememberMeRiskHtml()).toBe('');
    });

    it('stays when the acknowledgment fails, and says so', async () => {
        BackendClient.saveAdminSettings.mockRejectedValueOnce(new Error('400'));
        await show({ remember_me_days: 730, remember_me_days_default: 90 });
        const button = document.getElementById('btn-remember-me-ack');
        button.click();
        await vi.waitFor(() => expect(Toast.error).toHaveBeenCalled());
        expect(document.getElementById('admin-remember-me-risk')).not.toBeNull();
        expect(button.disabled).toBe(false);
    });

    it('takes whole numbers only from the server', async () => {
        expect(
            await show({
                remember_me_days: '<img src=x onerror=alert(1)>',
                remember_me_days_default: 90,
            }),
        ).toBeNull();
        expect(await show({ remember_me_days: 365.5, remember_me_days_default: 90 })).toBeNull();
        expect(
            await show({
                remember_me_days: 365,
                remember_me_days_default: 90,
                remember_me_days_acknowledged: '365',
            }),
        ).not.toBeNull();
    });

    it('is not shown without admin access', async () => {
        expect(
            await show(
                { remember_me_days: 365, remember_me_days_default: 90 },
                { adminAccess: false },
            ),
        ).toBeNull();
    });
});
