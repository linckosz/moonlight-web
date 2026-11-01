/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 *
 * The host list drawn from the last visit (01/11/2026: on an iPhone through the
 * rendezvous, the Host page stayed empty for 5 to 6 s while the machine
 * answered its own loopback in tens of milliseconds). Pinned down: the last
 * list is drawn before the machine answers, per machine; the answer replaces it
 * and drops a host it no longer has; a remembered game is never offered to
 * resume; an empty answer is never remembered.
 */
import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';

vi.mock('../js/api/BackendClient.js', () => ({
    BackendClient: {
        getHosts: vi.fn(),
        getAppList: vi.fn(),
        scanHosts: vi.fn(async () => ({})),
        checkForUpdate: vi.fn(async () => ({})),
    },
}));

import { HostListView } from '../js/ui/HostListView.js';
import { BackendClient } from '../js/api/BackendClient.js';
import { loadCachedHosts, saveCachedHosts } from '../js/util/hostListCache.js';

const DESK = { uuid: 'h1', name: 'BENCH-DESK', state: 'online', pairState: 'paired' };
const DEN = { uuid: 'h2', name: 'DEN-PC', state: 'online', pairState: 'paired', currentGameId: 7 };

describe('hostListCache', () => {
    afterEach(() => localStorage.clear());

    it('keeps one list per machine', () => {
        saveCachedHosts('machine-a', [DESK]);
        saveCachedHosts('machine-b', [DEN]);
        expect(loadCachedHosts('machine-a')).toEqual([DESK]);
        expect(loadCachedHosts('machine-b')).toEqual([DEN]);
        expect(loadCachedHosts('machine-c')).toBe(null);
        expect(loadCachedHosts('')).toBe(null);
    });

    it('forgets a machine that answered with no host', () => {
        saveCachedHosts('machine-a', [DESK]);
        saveCachedHosts('machine-a', []);
        expect(loadCachedHosts('machine-a')).toBe(null);
    });

    it('lets a month-old list go', () => {
        saveCachedHosts('machine-a', [DESK]);
        const map = JSON.parse(localStorage.getItem('mw-host-list'));
        map['machine-a'].ts = Date.now() - 31 * 24 * 60 * 60 * 1000;
        localStorage.setItem('mw-host-list', JSON.stringify(map));
        expect(loadCachedHosts('machine-a')).toBe(null);
    });
});

describe('HostListView, the list drawn from the last visit', () => {
    let view;
    let container;

    beforeEach(() => {
        vi.clearAllMocks();
        BackendClient.getAppList.mockReturnValue(new Promise(() => {}));
        document.body.innerHTML = '<div id="root"></div>';
        container = document.getElementById('root');
        view = new HostListView(container);
        view.renderShell();
    });

    afterEach(() => {
        if (view) view.destroy();
        view = null;
        localStorage.clear();
    });

    const cards = () =>
        [...container.querySelectorAll('.host-card')].map((c) => c.getAttribute('data-uuid'));

    it('draws the cards before the machine has answered', () => {
        saveCachedHosts('machine-a', [DESK, DEN]);
        BackendClient.getHosts.mockReturnValue(new Promise(() => {}));
        view.memoryKey = 'machine-a';
        view.start();
        expect(cards().sort()).toEqual(['h1', 'h2']);
        // No resume offered on the strength of a memory.
        expect(view.hosts.find((h) => h.uuid === 'h2').currentGameId).toBe(0);
    });

    it("draws nothing from another machine's list", () => {
        saveCachedHosts('machine-b', [DEN]);
        BackendClient.getHosts.mockReturnValue(new Promise(() => {}));
        view.memoryKey = 'machine-a';
        view.start();
        expect(cards()).toEqual([]);
    });

    it('gives way to the answer, which drops a host it no longer has', async () => {
        saveCachedHosts('machine-a', [DESK, DEN]);
        let answer;
        BackendClient.getHosts.mockReturnValue(new Promise((r) => (answer = r)));
        view.memoryKey = 'machine-a';
        view.start();
        await vi.waitFor(() => expect(BackendClient.getHosts).toHaveBeenCalled());

        answer({ hosts: [DESK] });
        await vi.waitFor(() => expect(cards()).toEqual(['h1']));
        expect(loadCachedHosts('machine-a')).toEqual([DESK]);
        view.stop();
    });

    it('remembers nothing without a machine to file it under', async () => {
        BackendClient.getHosts.mockResolvedValue({ hosts: [DESK] });
        view._active = true;
        await view.refresh();
        expect(localStorage.getItem('mw-host-list')).toBe(null);
    });
});
