/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';

vi.mock('../js/api/BackendClient.js', () => ({
    BackendClient: { getAppList: vi.fn() },
}));

import { HostListView } from '../js/ui/HostListView.js';
import { Host } from '../js/models/Host.js';
import { BackendClient } from '../js/api/BackendClient.js';

const READY_HOST = { uuid: 'host-a', name: 'DUALRTX', state: 'online', pairState: 'paired' };

const ok = (apps) => ({ status: 'ok', apps });
const names = (container) =>
    Array.from(container.querySelectorAll('.app-card-name')).map((el) => el.textContent);
const grid = (container) => container.querySelector('.host-apps');

// Waiting for the background confirmation to land. The fetch is a resolved
// promise chained a few times, so draining the microtask queue is enough.
const settle = async () => {
    for (let i = 0; i < 6; i++) await Promise.resolve();
};

/**
 * The app grid on a host card. What matters here is *when* the cards are on
 * screen: a host that just went "ready" must show the list this browser already
 * knows, not a spinner, and the fetch that confirms it must stay invisible
 * unless the host actually disagrees.
 */
describe('HostListView app grid', () => {
    let view;
    let container;

    beforeEach(() => {
        localStorage.clear();
        vi.clearAllMocks();
        document.body.innerHTML = '<div id="root"></div>';
        container = document.getElementById('root');
    });

    afterEach(() => {
        if (view) view.destroy();
        view = null;
    });

    const mount = () => {
        view = new HostListView(container);
        view.hosts = [new Host(READY_HOST)];
        view.renderList();
    };

    const remember = (apps) =>
        localStorage.setItem(
            'mw-host-apps',
            JSON.stringify({ 'host-a': { ts: Date.now(), apps } }),
        );

    it('paints a remembered list synchronously, with no spinner in between', () => {
        remember([
            { id: 1, name: 'Desktop' },
            { id: 2, name: 'Steam' },
        ]);
        BackendClient.getAppList.mockReturnValue(new Promise(() => {})); // never answers

        mount();

        // Same tick as the render — nothing awaited.
        expect(names(container)).toEqual(['Desktop', 'Steam']);
        expect(grid(container).querySelector('.host-apps-loading')).toBeNull();
    });

    it('still confirms that list against the host', async () => {
        remember([{ id: 1, name: 'Desktop' }]);
        BackendClient.getAppList.mockResolvedValue(ok([{ id: 1, name: 'Desktop' }]));

        mount();
        await settle();

        expect(BackendClient.getAppList).toHaveBeenCalledWith('host-a');
    });

    it('does not repaint when the host agrees — the confirmation must be invisible', async () => {
        remember([{ id: 1, name: 'Desktop' }]);
        BackendClient.getAppList.mockResolvedValue(ok([{ id: 1, name: 'Desktop' }]));

        mount();
        const painted = container.querySelector('.app-card');
        await settle();

        expect(container.querySelector('.app-card')).toBe(painted); // same node, never redrawn
    });

    it('repaints when the host disagrees', async () => {
        remember([{ id: 1, name: 'Desktop' }]);
        BackendClient.getAppList.mockResolvedValue(
            ok([
                { id: 1, name: 'Desktop' },
                { id: 5, name: 'Cyberpunk 2077' },
            ]),
        );

        mount();
        expect(names(container)).toEqual(['Desktop']);
        await settle();

        expect(names(container)).toEqual(['Desktop', 'Cyberpunk 2077']);
    });

    it('keeps a remembered grid up when the host is momentarily unreachable', async () => {
        remember([{ id: 1, name: 'Desktop' }]);
        BackendClient.getAppList.mockRejectedValue(new Error('network'));
        const err = vi.spyOn(console, 'error').mockImplementation(() => {});

        mount();
        await settle();

        expect(names(container)).toEqual(['Desktop']);
        expect(grid(container).querySelector('.host-apps-error')).toBeNull();
        err.mockRestore();
    });

    // A host that answers 4xx has given a verdict, not hit a hiccup. Retrying
    // returns the same answer for ever, so the spinner must come down and say
    // what the host said — the symptom this pins is a card stuck on "Loading
    // applications..." because a dropped pairing was reported as a 5xx.
    it('shows the reason when the host rejects the request, and stops retrying', async () => {
        const rejection = Object.assign(new Error('Host is no longer paired. Please pair again.'), {
            statusCode: 401,
        });
        BackendClient.getAppList.mockRejectedValue(rejection);
        const err = vi.spyOn(console, 'error').mockImplementation(() => {});

        mount();
        await settle();

        expect(grid(container).querySelector('.host-apps-loading')).toBeNull();
        expect(grid(container).querySelector('.host-apps-error').textContent).toBe(
            'Host is no longer paired. Please pair again.',
        );
        err.mockRestore();
    });

    it('keeps retrying behind the spinner when the failure is transient', async () => {
        const rejection = Object.assign(new Error('gateway'), { statusCode: 502 });
        BackendClient.getAppList.mockRejectedValue(rejection);
        const err = vi.spyOn(console, 'error').mockImplementation(() => {});

        mount();
        await settle();

        expect(grid(container).querySelector('.host-apps-loading')).not.toBeNull();
        expect(grid(container).querySelector('.host-apps-error')).toBeNull();
        err.mockRestore();
    });

    it('shows the spinner for a host it has never seen, then its apps', async () => {
        BackendClient.getAppList.mockResolvedValue(ok([{ id: 1, name: 'Desktop' }]));

        mount();
        expect(grid(container).querySelector('.host-apps-loading')).not.toBeNull();
        await settle();

        expect(names(container)).toEqual(['Desktop']);
    });

    it('remembers what the host answered, for the next visit', async () => {
        BackendClient.getAppList.mockResolvedValue(ok([{ id: 1, name: 'Desktop' }]));

        mount();
        await settle();

        expect(JSON.parse(localStorage.getItem('mw-host-apps'))['host-a'].apps).toEqual([
            { id: 1, name: 'Desktop', hdrSupported: false },
        ]);
    });

    describe('an app that no longer exists on the host', () => {
        it('is reported gone, and disappears from the grid', async () => {
            remember([
                { id: 1, name: 'Desktop' },
                { id: 2, name: 'Uninstalled Game' },
            ]);
            BackendClient.getAppList.mockResolvedValue(ok([{ id: 1, name: 'Desktop' }]));
            mount();

            const gone = await view.confirmAppExists(view.hosts[0], { id: 2, name: 'Uninstalled' });

            expect(gone).toBe(false);
            expect(names(container)).toEqual(['Desktop']);
        });

        it('is not blamed when the host simply could not answer', async () => {
            remember([{ id: 2, name: 'Steam' }]);
            BackendClient.getAppList.mockRejectedValue(new Error('timeout'));
            const err = vi.spyOn(console, 'error').mockImplementation(() => {});
            mount();

            const exists = await view.confirmAppExists(view.hosts[0], { id: 2, name: 'Steam' });

            expect(exists).toBe(true); // an unreachable host proves nothing
            err.mockRestore();
        });

        it('confirms an app the host still lists', async () => {
            remember([{ id: 2, name: 'Steam' }]);
            BackendClient.getAppList.mockResolvedValue(ok([{ id: 2, name: 'Steam' }]));
            mount();

            expect(await view.confirmAppExists(view.hosts[0], { id: 2, name: 'Steam' })).toBe(true);
        });
    });
});
