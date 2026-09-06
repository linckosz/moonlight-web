/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';

// The rendezvous address is claimed AFTER /api/internet/enable answers, so the
// admin page has to look again. Before this, it read the status once, found no
// address, and left the Internet box empty until the user reloaded by hand.
vi.mock('../js/api/BackendClient.js', () => ({
    BackendClient: { getInternetStatus: vi.fn(), enableInternet: vi.fn() },
}));
vi.mock('../js/ui/Toast.js', () => ({
    Toast: { success: vi.fn(), error: vi.fn(), warning: vi.fn() },
}));

import { AdminView } from '../js/ui/AdminView.js';
import { BackendClient } from '../js/api/BackendClient.js';

describe('AdminView — waiting for the rendezvous address', () => {
    let view;
    let renders;

    beforeEach(() => {
        vi.useFakeTimers();
        document.body.innerHTML = '<div></div>';
        view = new AdminView(document.body, () => {});
        renders = 0;
        view.render = () => {
            renders++;
        };
        view.bindEvents = () => {};
        BackendClient.getInternetStatus.mockReset();
    });

    afterEach(() => {
        view.destroy();
        vi.useRealTimers();
    });

    const statusWith = (url) => ({
        internet_access_enabled: true,
        active: true,
        rendezvous: url ? { url, online: true } : {},
    });

    it('keeps polling until the address is claimed, then renders it once', async () => {
        BackendClient.getInternetStatus
            .mockResolvedValueOnce(statusWith(''))
            .mockResolvedValueOnce(statusWith(''))
            .mockResolvedValue(statusWith('https://stream.moonlightweb.top/abc'));

        view._awaitRendezvousUrl();
        expect(view._awaitingRdv).toBe(true);

        await vi.advanceTimersByTimeAsync(700);
        expect(view._rendezvousUrl).toBe('');
        await vi.advanceTimersByTimeAsync(700);
        expect(view._rendezvousUrl).toBe('');

        await vi.advanceTimersByTimeAsync(700);
        expect(view._rendezvousUrl).toBe('https://stream.moonlightweb.top/abc');
        expect(view._awaitingRdv).toBe(false);
        expect(renders).toBe(1);

        // And it stops: no further polling once the address is on screen.
        const calls = BackendClient.getInternetStatus.mock.calls.length;
        await vi.advanceTimersByTimeAsync(5000);
        expect(BackendClient.getInternetStatus.mock.calls.length).toBe(calls);
    });

    it('gives up at the deadline rather than spinning on a build with no rendezvous', async () => {
        BackendClient.getInternetStatus.mockResolvedValue(statusWith(''));

        view._awaitRendezvousUrl(2000);
        await vi.advanceTimersByTimeAsync(2100);

        expect(view._awaitingRdv).toBe(false);
        expect(view._rendezvousUrl).toBe('');
        expect(renders).toBe(1);
        const calls = BackendClient.getInternetStatus.mock.calls.length;
        await vi.advanceTimersByTimeAsync(5000);
        expect(BackendClient.getInternetStatus.mock.calls.length).toBe(calls);
    });

    it('does not start a wait when the address is already known', () => {
        view._rendezvousUrl = 'https://stream.moonlightweb.top/already';
        view._awaitRendezvousUrl();
        expect(view._awaitingRdv).toBe(false);
        expect(BackendClient.getInternetStatus).not.toHaveBeenCalled();
    });

    it('survives a status call that fails and keeps waiting', async () => {
        BackendClient.getInternetStatus
            .mockRejectedValueOnce(new Error('backend busy'))
            .mockResolvedValue(statusWith('https://stream.moonlightweb.top/xyz'));

        view._awaitRendezvousUrl();
        await vi.advanceTimersByTimeAsync(700);
        expect(view._awaitingRdv).toBe(true);
        await vi.advanceTimersByTimeAsync(700);
        expect(view._rendezvousUrl).toBe('https://stream.moonlightweb.top/xyz');
    });

    // The wiring, not just the helper: enabling is where the empty box came from.
    it('starts the wait when enabling returns without an address', async () => {
        BackendClient.enableInternet.mockResolvedValue({ status: 'enabled', domain: '' });
        BackendClient.getInternetStatus.mockResolvedValue(statusWith(''));

        await view._enableInternet();

        expect(view._awaitingRdv).toBe(true);
        BackendClient.getInternetStatus.mockResolvedValue(
            statusWith('https://stream.moonlightweb.top/late'),
        );
        await vi.advanceTimersByTimeAsync(700);
        expect(view._rendezvousUrl).toBe('https://stream.moonlightweb.top/late');
        expect(view._awaitingRdv).toBe(false);
    });

    it('does not wait when enabling already answered with the address', async () => {
        BackendClient.enableInternet.mockResolvedValue({ status: 'enabled', domain: '' });
        BackendClient.getInternetStatus.mockResolvedValue(
            statusWith('https://stream.moonlightweb.top/known'),
        );

        await view._enableInternet();

        expect(view._rendezvousUrl).toBe('https://stream.moonlightweb.top/known');
        expect(view._awaitingRdv).toBe(false);
        expect(view._rdvPollTimer).toBe(null);
    });

    it('is stopped by destroy(), so a closed page leaves no timer behind', async () => {
        BackendClient.getInternetStatus.mockResolvedValue(statusWith(''));
        view._awaitRendezvousUrl();
        view.destroy();
        expect(view._awaitingRdv).toBe(false);
        const calls = BackendClient.getInternetStatus.mock.calls.length;
        await vi.advanceTimersByTimeAsync(3000);
        expect(BackendClient.getInternetStatus.mock.calls.length).toBe(calls);
    });
});
