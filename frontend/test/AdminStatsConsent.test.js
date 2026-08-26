/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 *
 * Withdrawing (or giving) the statistics consent from the admin page. GDPR
 * asks that taking it back be as easy as giving it, so this switch is the other
 * half of the first-launch bar — and, like it, its failure mode has to be the
 * quiet one: a checkbox showing a state the backend does not hold would have
 * the user believe they opted out when they did not.
 */
import { describe, it, expect, beforeEach, vi } from 'vitest';

vi.mock('../js/api/BackendClient.js', () => ({
    BackendClient: {
        getMetricsConsent: vi.fn(),
        setMetricsConsent: vi.fn(async () => ({ decision: 'granted' })),
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

describe('AdminView statistics consent', () => {
    let view;

    beforeEach(() => {
        document.body.innerHTML = '<div id="admin"></div>';
        vi.clearAllMocks();
        view = new AdminView(document.getElementById('admin'), () => {});
    });

    it('starts with no section at all, rather than a wrong one', () => {
        expect(view._statsAvailable).toBe(false);
        expect(view._statsGranted).toBe(false);
    });

    it('reads the current answer, and whether this build reports at all', async () => {
        BackendClient.getMetricsConsent.mockResolvedValue({
            decision: 'granted',
            available: true,
        });
        await view._loadStatsConsent();
        expect(view._statsAvailable).toBe(true);
        expect(view._statsGranted).toBe(true);
    });

    it('treats a refusal as not granted, on a build that can report', async () => {
        BackendClient.getMetricsConsent.mockResolvedValue({ decision: 'denied', available: true });
        await view._loadStatsConsent();
        expect(view._statsAvailable).toBe(true);
        expect(view._statsGranted).toBe(false);
    });

    it('hides the section when the backend will not say — never blocks the page', async () => {
        BackendClient.getMetricsConsent.mockRejectedValue(new Error('403'));
        await view._loadStatsConsent();
        expect(view._statsAvailable).toBe(false);
        expect(view._statsGranted).toBe(false);
    });

    it('withdraws consent, recording the wording shown beside the switch', async () => {
        const box = { checked: false, disabled: false };
        await view._setStatsConsent(box);
        const [granted, message, source] = BackendClient.setMetricsConsent.mock.calls[0];
        expect(granted).toBe(false);
        expect(source).toBe('admin');
        expect(message).toContain('text:stats.adminToggle');
        expect(message).toContain('text:stats.adminDesc');
        expect(view._statsGranted).toBe(false);
        expect(box.disabled).toBe(false);
    });

    it('gives consent the same way', async () => {
        const box = { checked: true, disabled: false };
        await view._setStatsConsent(box);
        expect(BackendClient.setMetricsConsent.mock.calls[0][0]).toBe(true);
        expect(view._statsGranted).toBe(true);
    });

    it('puts the switch back when the choice could not be saved', async () => {
        BackendClient.setMetricsConsent.mockRejectedValue(new Error('offline'));
        const box = { checked: false, disabled: false };
        await view._setStatsConsent(box);
        // The backend still holds "granted", so the box must not claim otherwise.
        expect(box.checked).toBe(true);
        expect(box.disabled).toBe(false);
    });
});
