/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect, beforeEach, vi } from 'vitest';

/**
 * The guest's say on their page's error reports: a quiet box at the bottom of
 * the join page, ticked by default, shown only when the machine reports at all,
 * and remembered with the guest's other options.
 */

vi.mock('../js/api/BackendClient.js', () => ({
    BackendClient: {
        playerInfo: vi.fn(),
        playerPin: vi.fn(),
    },
}));
vi.mock('../js/i18n/i18n.js', () => ({
    t: (key) => 'text:' + key,
}));
const allowedFn = vi.hoisted(() => ({ current: null }));
vi.mock('../js/util/ErrorTrail.js', () => ({
    setErrorTrailAllowed: (fn) => {
        allowedFn.current = fn;
    },
}));

import { PlayerJoinView } from '../js/ui/PlayerJoinView.js';
import { BackendClient } from '../js/api/BackendClient.js';

const JOINABLE = {
    needs_pin: false,
    machine_name: 'ATELIER',
    state: 'idle',
    access_level: 'full',
    permissions: {},
    cold_start: false,
};

describe('PlayerJoinView error-report box', () => {
    let view;
    let container;
    const box = () => container.querySelector('#player-errors-box');

    beforeEach(() => {
        localStorage.clear();
        document.body.innerHTML = '<div id="player"></div>';
        container = document.getElementById('player');
        vi.clearAllMocks();
        view = new PlayerJoinView(container, 'tok', async () => {});
    });

    it('is ticked by default when the machine reports, with its text', async () => {
        BackendClient.playerInfo.mockResolvedValue({ ...JOINABLE, stats_reporting: true });
        await view.refresh();
        expect(box()).not.toBeNull();
        expect(box().checked).toBe(true);
        expect(container.textContent).toContain('text:stats.playerErrors');
        expect(container.textContent).toContain('text:stats.playerErrorsDesc');
        expect(allowedFn.current()).toBe(true);
    });

    it('is not shown when the machine reports nothing', async () => {
        BackendClient.playerInfo.mockResolvedValue({ ...JOINABLE, stats_reporting: false });
        await view.refresh();
        expect(box()).toBeNull();
    });

    it('unticked, the page stops sending and remembers it', async () => {
        BackendClient.playerInfo.mockResolvedValue({ ...JOINABLE, stats_reporting: true });
        await view.refresh();
        box().checked = false;
        box().dispatchEvent(new Event('change'));
        expect(allowedFn.current()).toBe(false);
        expect(JSON.parse(localStorage.getItem('mw_player_prefs')).errorReports).toBe(false);
        // Back on the same link: still unticked.
        const again = new PlayerJoinView(container, 'tok', async () => {});
        await again.refresh();
        expect(box().checked).toBe(false);
    });
});
