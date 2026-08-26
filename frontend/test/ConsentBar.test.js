/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
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

import { ConsentBar } from '../js/ui/ConsentBar.js';
import { BackendClient } from '../js/api/BackendClient.js';

// The statistics consent bar. These are privacy properties, not features: each
// one of them failing is silent at runtime — the app keeps working while
// reporting something nobody agreed to, or asking someone who has no business
// answering.
describe('ConsentBar', () => {
    const bar = () => document.getElementById('consent-bar');
    const hostLocal = { isHostLocal: () => true };

    beforeEach(() => {
        document.body.innerHTML = '';
        vi.clearAllMocks();
        BackendClient.getMetricsConsent.mockResolvedValue({
            decision: '',
            available: true,
            reporting: false,
        });
    });

    it('asks on the host machine when nothing has been answered yet', async () => {
        await ConsentBar.maybeShow(hostLocal);
        expect(bar()).not.toBeNull();
        expect(bar().querySelector('#consent-accept')).not.toBeNull();
        expect(bar().querySelector('#consent-decline')).not.toBeNull();
    });

    it('never asks a remote viewer — it is not their machine to answer for', async () => {
        await ConsentBar.maybeShow({ isHostLocal: () => false });
        expect(bar()).toBeNull();
        expect(BackendClient.getMetricsConsent).not.toHaveBeenCalled();
    });

    it('does not ask twice, whichever way it was answered', async () => {
        for (const decision of ['granted', 'denied']) {
            document.body.innerHTML = '';
            BackendClient.getMetricsConsent.mockResolvedValue({ decision, available: true });
            await ConsentBar.maybeShow(hostLocal);
            expect(bar()).toBeNull();
        }
    });

    it('stays quiet on a build that reports nothing anyway', async () => {
        BackendClient.getMetricsConsent.mockResolvedValue({ decision: '', available: false });
        await ConsentBar.maybeShow(hostLocal);
        expect(bar()).toBeNull();
    });

    it('stays quiet when the backend will not answer', async () => {
        BackendClient.getMetricsConsent.mockRejectedValue(new Error('nope'));
        await ConsentBar.maybeShow(hostLocal);
        expect(bar()).toBeNull();
    });

    it('records a refusal as such, and closes', async () => {
        await ConsentBar.maybeShow(hostLocal);
        bar().querySelector('#consent-decline').click();
        await vi.waitFor(() => expect(BackendClient.setMetricsConsent).toHaveBeenCalled());
        const [granted, , source] = BackendClient.setMetricsConsent.mock.calls[0];
        expect(granted).toBe(false);
        expect(source).toBe('banner');
        await vi.waitFor(() => expect(bar()).toBeNull());
    });

    it('sends the text that was on screen, so the record says what was agreed to', async () => {
        await ConsentBar.maybeShow(hostLocal);
        bar().querySelector('#consent-accept').click();
        await vi.waitFor(() => expect(BackendClient.setMetricsConsent).toHaveBeenCalled());
        const [granted, message] = BackendClient.setMetricsConsent.mock.calls[0];
        expect(granted).toBe(true);
        // Everything the bar displays: the promise, what is sent, and what
        // never is. A record missing the last part would be worthless.
        for (const key of [
            'stats.title',
            'stats.body',
            'stats.statsSent1',
            'stats.statsNever3',
            'stats.necessary',
        ]) {
            expect(message).toContain('text:' + key);
        }
    });

    it('closes even when the answer could not be saved — nothing is reported either way', async () => {
        BackendClient.setMetricsConsent.mockRejectedValue(new Error('offline'));
        await ConsentBar.maybeShow(hostLocal);
        bar().querySelector('#consent-accept').click();
        await vi.waitFor(() => expect(bar()).toBeNull());
    });

    it('blocks nothing: no overlay, no focus trap, no modal role', async () => {
        await ConsentBar.maybeShow(hostLocal);
        expect(bar().getAttribute('role')).toBe('region');
        expect(bar().getAttribute('aria-modal')).toBeNull();
        expect(document.querySelector('.pairing-overlay')).toBeNull();
        expect(document.body.style.overflow).toBe('');
    });
});
