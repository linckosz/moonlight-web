/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 *
 * The statistics answer, given or withdrawn from the Settings page. It lives
 * there rather than in Admin because it concerns everyone who streams through
 * this machine — so the disclosure has to reach a plain user, while the switch
 * itself stays the machine's own. Both halves are privacy properties: each one
 * failing is silent at runtime, leaving a checkbox that shows a state the
 * backend does not hold, or a user who was never told what is counted.
 */
import { describe, it, expect, beforeEach, vi } from 'vitest';

vi.mock('../js/api/BackendClient.js', () => ({
    BackendClient: {
        getAuthStatus: vi.fn(async () => ({ has_session: false })),
        getStreamingSettings: vi.fn(async () => ({})),
        getMetricsConsent: vi.fn(),
        setMetricsConsent: vi.fn(async () => ({ decision: 'granted' })),
    },
}));
vi.mock('../js/ui/Toast.js', () => ({
    Toast: { success: vi.fn(), error: vi.fn(), warning: vi.fn(), info: vi.fn() },
}));
vi.mock('../js/i18n/i18n.js', () => ({
    t: (key) => 'text:' + key,
    getLanguage: () => 'en',
    setLanguage: vi.fn(),
    AVAILABLE_LANGUAGES: [{ code: 'en', label: 'English' }],
}));

import { SettingsView } from '../js/ui/SettingsView.js';
import { BackendClient } from '../js/api/BackendClient.js';

describe('SettingsView privacy section', () => {
    let view;

    const load = async (state) => {
        BackendClient.getMetricsConsent.mockResolvedValue(state);
        await view._loadStatsConsent();
        return view._renderPrivacySection();
    };

    beforeEach(() => {
        document.body.innerHTML = '<div id="settings"></div>';
        vi.clearAllMocks();
        view = new SettingsView(document.getElementById('settings'), () => {});
    });

    it('starts with no section at all, rather than a wrong one', () => {
        expect(view._statsLoaded).toBe(false);
        expect(view._renderPrivacySection()).toBe('');
    });

    it('shows the switch, ticked, to the machine that may answer', async () => {
        const html = await load({ decision: 'granted', available: true, writable: true });
        expect(html).toContain('settings-stats-consent');
        expect(html).toContain('checked');
        expect(html).not.toContain('disabled');
        expect(html).not.toContain('text:stats.ownerOnly');
    });

    it('treats a refusal as not ticked', async () => {
        const html = await load({ decision: 'denied', available: true, writable: true });
        expect(html).toContain('settings-stats-consent');
        expect(html).not.toContain('checked');
    });

    // The whole reason it moved out of Admin: a signed-in user on another
    // machine gets to read what is counted about their streams. What they do
    // not get is the answer — that speaks for the machine, not for them.
    it('shows a remote user the same disclosure, with the switch locked', async () => {
        const html = await load({ decision: 'granted', available: true, writable: false });
        expect(html).toContain('disabled');
        expect(html).toContain('text:stats.ownerOnly');
        expect(html).toContain('text:stats.statsSent1');
        expect(html).toContain('text:stats.statsNever3');
    });

    it('says so plainly on a build that reports nothing, instead of a switch that lies', async () => {
        const html = await load({ decision: '', available: false, writable: true });
        expect(html).toContain('text:stats.unavailable');
        expect(html).not.toContain('settings-stats-consent');
    });

    it('always carries the full disclosure, whatever the answer', async () => {
        const html = await load({ decision: 'denied', available: true, writable: true });
        for (const key of ['sentIntro', 'statsSent1', 'neverIntro', 'statsNever3', 'necessary']) {
            expect(html).toContain('text:stats.' + key);
        }
    });

    it('hides the section when the backend will not say — never blocks the page', async () => {
        BackendClient.getMetricsConsent.mockRejectedValue(new Error('offline'));
        await view._loadStatsConsent();
        expect(view._statsLoaded).toBe(false);
        expect(view._renderPrivacySection()).toBe('');
    });

    it('withdraws consent, recording the wording shown around the switch', async () => {
        const box = { checked: false, disabled: false };
        await view._setStatsConsent(box);
        const [granted, message, source] = BackendClient.setMetricsConsent.mock.calls[0];
        expect(granted).toBe(false);
        expect(source).toBe('settings');
        // The record must name both the promise and what is never sent.
        for (const key of ['sectionTitle', 'toggle', 'toggleDesc', 'statsNever3', 'necessary']) {
            expect(message).toContain('text:stats.' + key);
        }
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
