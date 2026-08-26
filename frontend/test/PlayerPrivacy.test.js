/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 *
 * The guest's corner button. An invited player has no account, no Settings
 * page and no way to reach Admin, yet their session is one of the ones the
 * census counts — so this is the only place they can read what is kept and
 * what is sent. It must never become a wall: it opens on demand, it closes,
 * and joining never requires touching it.
 */
import { describe, it, expect, beforeEach, vi } from 'vitest';

vi.mock('../js/api/BackendClient.js', () => ({
    BackendClient: {
        playerInfo: vi.fn(),
        playerPin: vi.fn(),
    },
}));
vi.mock('../js/i18n/i18n.js', () => ({
    t: (key, params) => (params ? 'text:' + key + JSON.stringify(params) : 'text:' + key),
}));

import { PlayerJoinView } from '../js/ui/PlayerJoinView.js';
import { BackendClient } from '../js/api/BackendClient.js';

const JOINABLE = {
    needs_pin: false,
    machine_name: 'ATELIER',
    state: 'idle',
    access_level: 'full',
    permissions: {},
    owner_streaming: true,
};

describe('PlayerJoinView privacy corner', () => {
    let view;
    let container;

    const btn = () => container.querySelector('#player-privacy-btn');
    const panel = () => container.querySelector('.player-privacy-panel');

    beforeEach(() => {
        document.body.innerHTML = '<div id="player"></div>';
        container = document.getElementById('player');
        vi.clearAllMocks();
        view = new PlayerJoinView(container, 'tok', async () => {});
    });

    it('is offered on the selection page', async () => {
        BackendClient.playerInfo.mockResolvedValue({ ...JOINABLE, stats_reporting: true });
        await view.refresh();
        expect(btn()).not.toBeNull();
        expect(btn().textContent.trim()).toBe('text:stats.cookiesButton');
        // Closed until asked for: nothing greets a guest with a consent wall.
        expect(panel()).toBeNull();
        expect(btn().getAttribute('aria-expanded')).toBe('false');
    });

    it('opens and closes again, blocking nothing behind it', async () => {
        BackendClient.playerInfo.mockResolvedValue({ ...JOINABLE, stats_reporting: true });
        await view.refresh();

        btn().click();
        expect(panel()).not.toBeNull();
        expect(panel().getAttribute('aria-modal')).toBeNull();
        expect(document.body.style.overflow).toBe('');
        // The Join button stays reachable the whole time.
        expect(container.querySelector('.player-join-btn')).not.toBeNull();

        panel().querySelector('.player-privacy-close').click();
        expect(panel()).toBeNull();
        expect(btn().getAttribute('aria-expanded')).toBe('false');
    });

    it('names what this page keeps in the guest browser, always', async () => {
        BackendClient.playerInfo.mockResolvedValue({ ...JOINABLE, stats_reporting: false });
        await view.refresh();
        btn().click();
        expect(panel().textContent).toContain('text:stats.playerCookies');
    });

    // The guest cannot change the answer, so the panel must at least be honest
    // about which one the machine gave.
    it('spells out the census when the machine reports', async () => {
        BackendClient.playerInfo.mockResolvedValue({ ...JOINABLE, stats_reporting: true });
        await view.refresh();
        btn().click();
        const text = panel().textContent;
        expect(text).toContain('text:stats.playerStatsOn');
        expect(text).toContain('text:stats.statsSent1');
        expect(text).toContain('text:stats.statsNever3');
    });

    it('says nothing is counted when nothing is, and lists nothing', async () => {
        BackendClient.playerInfo.mockResolvedValue({ ...JOINABLE, stats_reporting: false });
        await view.refresh();
        btn().click();
        const text = panel().textContent;
        expect(text).toContain('text:stats.playerStatsOff');
        expect(text).not.toContain('text:stats.statsSent1');
    });

    // A dead link never told us what the machine does, so the panel must not
    // guess — the browser-storage half is still true and still shown.
    it('claims nothing about the census on a dead link', async () => {
        BackendClient.playerInfo.mockRejectedValue(new Error('404'));
        await view.refresh();
        btn().click();
        const text = panel().textContent;
        expect(text).toContain('text:stats.playerCookies');
        expect(text).not.toContain('text:stats.playerStatsOff');
        expect(text).not.toContain('text:stats.playerStatsOn');
    });

    it('is offered before the PIN, too', async () => {
        BackendClient.playerInfo.mockResolvedValue({ needs_pin: true });
        await view.refresh();
        expect(btn()).not.toBeNull();
    });
});
