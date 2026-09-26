/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';

vi.mock('../js/api/BackendClient.js', () => ({
    BackendClient: { getAppList: vi.fn(), stopHostSession: vi.fn() },
}));

import { HostListView } from '../js/ui/HostListView.js';
import { Host } from '../js/models/Host.js';
import { BackendClient } from '../js/api/BackendClient.js';

/**
 * Issue #24 — Moonlight's model on a host whose app outlives the stream: Stop
 * leaves the game running, and its card is where the viewer gets back to it
 * (click = resume) or closes it (Quit). Only that one card changes, and only
 * where the server says the app is ours to keep.
 */
describe('the running app card', () => {
    let view;
    let container;

    const APPS = [
        { id: 1, name: 'Desktop' },
        { id: 2, name: 'Cyberpunk 2077' },
    ];

    const mount = (hostData) => {
        localStorage.setItem(
            'mw-host-apps',
            JSON.stringify({ 'host-a': { ts: Date.now(), apps: APPS } }),
        );
        document.body.innerHTML = '<div id="root"></div>';
        container = document.getElementById('root');
        view = new HostListView(container);
        view.hosts = [
            new Host({
                uuid: 'host-a',
                name: 'BENCH-DESK',
                state: 'online',
                pairState: 'paired',
                ...hostData,
            }),
        ];
        view.renderList();
    };

    const cardOf = (id) => container.querySelector(`.app-card[data-app-id="${id}"]`);

    beforeEach(() => {
        vi.clearAllMocks();
        BackendClient.getAppList.mockReturnValue(new Promise(() => {})); // never answers
    });

    afterEach(() => {
        if (view) view.destroy();
        view = null;
        localStorage.clear();
        vi.restoreAllMocks();
    });

    it('marks the running app, and only that one, with a badge and a Quit button', () => {
        mount({ resumableApps: true, currentGameId: 2 });

        const running = cardOf(2);
        expect(running.classList.contains('app-card--running')).toBe(true);
        expect(running.querySelector('.app-card-badge')).not.toBeNull();
        expect(running.querySelector('.app-card-quit')).not.toBeNull();
        // Reads as a resume, not a launch (i18n keys: no locale is loaded here).
        expect(running.getAttribute('aria-label')).toBe('apps.resumeAria');

        const idle = cardOf(1);
        expect(idle.classList.contains('app-card--running')).toBe(false);
        expect(idle.querySelector('.app-card-quit')).toBeNull();
        expect(idle.getAttribute('aria-label')).toBe('apps.launchAria');
    });

    it('shows nothing where the app is not kept (Wolf, the native host)', () => {
        mount({ resumableApps: false, currentGameId: 2 });
        expect(container.querySelector('.app-card--running')).toBeNull();
        expect(container.querySelector('.app-card-quit')).toBeNull();
    });

    it('shows nothing when no app runs', () => {
        mount({ resumableApps: true, currentGameId: 0 });
        expect(container.querySelector('.app-card--running')).toBeNull();
    });

    it('Quit asks first, closes the app on the host, and never launches it', async () => {
        mount({ resumableApps: true, currentGameId: 2 });
        const onLaunch = vi.fn();
        view.onLaunchApp = onLaunch;
        vi.spyOn(window, 'confirm').mockReturnValue(true);
        BackendClient.stopHostSession.mockResolvedValue({ status: 'stopped', currentGameId: 0 });
        vi.spyOn(view, 'refresh').mockResolvedValue(undefined);

        cardOf(2).querySelector('.app-card-quit').click();
        await vi.waitFor(() => expect(view.hosts[0].currentGameId).toBe(0));

        expect(window.confirm).toHaveBeenCalledTimes(1);
        expect(BackendClient.stopHostSession).toHaveBeenCalledWith('host-a');
        expect(onLaunch).not.toHaveBeenCalled();
        await vi.waitFor(() => expect(container.querySelector('.app-card--running')).toBeNull());
    });

    it('Quit declined leaves the app running', () => {
        mount({ resumableApps: true, currentGameId: 2 });
        view.onLaunchApp = vi.fn();
        vi.spyOn(window, 'confirm').mockReturnValue(false);

        cardOf(2).querySelector('.app-card-quit').click();

        expect(BackendClient.stopHostSession).not.toHaveBeenCalled();
        expect(view.onLaunchApp).not.toHaveBeenCalled();
        expect(cardOf(2).classList.contains('app-card--running')).toBe(true);
    });
});
