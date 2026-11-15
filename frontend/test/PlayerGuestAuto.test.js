/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 *
 * A guest's Auto (issue #67). On a host where each guest has a stream of its
 * own, the join page offers Auto first and picks it, says what it follows
 * while it is the choice, and keeps a rung the guest picked for their next
 * visit. Auto sends no height: only this screen's size and whether it is a
 * TV's, which the host works the height out from.
 */
import { describe, it, expect, beforeEach, vi } from 'vitest';

vi.mock('../js/api/BackendClient.js', () => ({
    BackendClient: { playerInfo: vi.fn(), playerPin: vi.fn() },
}));
vi.mock('../js/i18n/i18n.js', () => ({
    t: (key) => 'text:' + key,
}));

import { PlayerJoinView, guestScreen } from '../js/ui/PlayerJoinView.js';
import { BackendClient } from '../js/api/BackendClient.js';

const BASE = {
    needs_pin: false,
    machine_name: 'ATELIER',
    state: 'idle',
    cold_start: false,
    access_level: 'viewer',
    permissions: {},
};

describe("PlayerJoinView: a guest's Auto", () => {
    let container;

    const buttons = () => [...container.querySelectorAll('.player-quality-btn')];
    const selected = () => container.querySelector('.player-quality-btn.is-selected');
    const hint = () => container.querySelector('.player-quality-auto-hint');
    const pick = (q) =>
        /** @type {HTMLElement} */ (container.querySelector(`[data-quality="${q}"]`)).click();

    async function mount(info = {}) {
        BackendClient.playerInfo.mockResolvedValue({ ...BASE, ...info });
        const onJoin = vi.fn(async () => {});
        const view = new PlayerJoinView(container, 'tok', onJoin);
        await view.refresh();
        return onJoin;
    }

    async function join(onJoin) {
        container.querySelector('.player-join-btn').click();
        await vi.waitFor(() => expect(onJoin).toHaveBeenCalled());
        return onJoin.mock.calls[0][0];
    }

    beforeEach(() => {
        document.body.innerHTML = '<div id="player"></div>';
        container = document.getElementById('player');
        localStorage.clear();
        vi.clearAllMocks();
    });

    it('offers Auto first, then the three rungs, Auto picked', async () => {
        await mount();
        expect(buttons().map((b) => b.dataset.quality)).toEqual(['auto', '720', '1080', '1440']);
        expect(buttons()[0].textContent).toBe('text:player.qualityAuto');
        expect(selected().dataset.quality).toBe('auto');
    });

    it('says what Auto follows while it is the choice, and only then', async () => {
        await mount();
        expect(hint().textContent).toBe('text:player.qualityAutoHint');
        expect(hint().hidden).toBe(false);
        pick(720);
        expect(hint().hidden).toBe(true);
        pick('auto');
        expect(hint().hidden).toBe(false);
    });

    it('joins on Auto with this screen and no height of its own', async () => {
        const onJoin = await mount();
        const info = await join(onJoin);
        expect(info.height).toBe('auto');
        expect(info).toMatchObject(guestScreen());
        expect(typeof info.tv).toBe('boolean');
    });

    it('joins at a rung the guest picked', async () => {
        const onJoin = await mount();
        pick(1440);
        expect(selected().dataset.quality).toBe('1440');
        const info = await join(onJoin);
        expect(info.height).toBe(1440);
    });

    it('keeps the pick for the next visit', async () => {
        await mount();
        pick(720);
        document.body.innerHTML = '<div id="player"></div>';
        container = document.getElementById('player');
        const onJoin = await mount();
        expect(selected().dataset.quality).toBe('720');
        expect(hint().hidden).toBe(true);
        expect((await join(onJoin)).height).toBe(720);
    });

    it('falls back to Auto on a kept choice it does not know', async () => {
        localStorage.setItem('mw_player_prefs', JSON.stringify({ quality: 2160 }));
        await mount();
        expect(selected().dataset.quality).toBe('auto');
    });

    it('offers nothing where the owner set one picture for every guest', async () => {
        const onJoin = await mount({ feed_height: 1080 });
        expect(buttons()).toEqual([]);
        expect(hint()).toBeNull();
        expect((await join(onJoin)).height).toBe(1080);
    });

    it('reads the screen in device pixels, held landscape', () => {
        const restore = { screen: window.screen, dpr: window.devicePixelRatio };
        Object.defineProperty(window, 'screen', {
            configurable: true,
            value: { width: 390, height: 844 },
        });
        Object.defineProperty(window, 'devicePixelRatio', { configurable: true, value: 3 });
        try {
            expect(guestScreen().screen).toBe('2532x1170');
        } finally {
            Object.defineProperty(window, 'screen', { configurable: true, value: restore.screen });
            Object.defineProperty(window, 'devicePixelRatio', {
                configurable: true,
                value: restore.dpr,
            });
        }
    });
});
