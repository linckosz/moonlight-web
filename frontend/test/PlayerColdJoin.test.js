/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 *
 * An invitation opened cold. The owner can now arm a row from the host's kebab
 * without streaming anything, and the guest's arrival is what starts the app.
 * The join page used to refuse that case outright ("ask them to start a game"),
 * which made the whole cold path unreachable from the only screen that could
 * use it. It must offer the button, and it must say plainly that pressing it
 * wakes a machine nobody is sitting at.
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

const BASE = {
    needs_pin: false,
    machine_name: 'ATELIER',
    state: 'idle',
    access_level: 'gamer',
    permissions: {},
};

describe('PlayerJoinView cold start', () => {
    let view;
    let container;

    const joinBtn = () => container.querySelector('.player-join-btn');
    const hint = () => container.querySelector('.player-hint');
    const refreshBtn = () => container.querySelector('.player-refresh');

    beforeEach(() => {
        document.body.innerHTML = '<div id="player"></div>';
        container = document.getElementById('player');
        vi.clearAllMocks();
        view = new PlayerJoinView(container, 'tok', async () => {});
    });

    it('offers the button with nothing running on the host', async () => {
        BackendClient.playerInfo.mockResolvedValue({
            ...BASE,
            cold_start: true,
            app_name: 'Cyberpunk 2077',
        });
        await view.refresh();

        expect(joinBtn()).not.toBeNull();
        // The dead end is gone: no error line, and no "check again" asking them
        // to wait on someone who is not there.
        expect(container.querySelector('.player-error:not([hidden])')).toBeNull();
        expect(refreshBtn()).toBeNull();
    });

    it('names the app the guest is about to launch', async () => {
        BackendClient.playerInfo.mockResolvedValue({
            ...BASE,
            cold_start: true,
            app_name: 'Cyberpunk 2077',
        });
        await view.refresh();

        expect(hint().textContent).toContain('text:player.willLaunchApp');
        expect(hint().textContent).toContain('Cyberpunk 2077');
        // The button says what it does, so nobody boots a game expecting to
        // slip into one already up.
        expect(joinBtn().textContent.trim()).toBe('text:player.launchButton');
    });

    it('stays vague rather than guessing when the app cannot be resolved', async () => {
        BackendClient.playerInfo.mockResolvedValue({ ...BASE, cold_start: true, app_name: '' });
        await view.refresh();

        expect(hint().textContent.trim()).toBe('text:player.willLaunch');
        expect(joinBtn().textContent.trim()).toBe('text:player.launchButton');
    });

    it('says nothing extra when the session is already up', async () => {
        BackendClient.playerInfo.mockResolvedValue({
            ...BASE,
            cold_start: false,
            app_name: 'Cyberpunk 2077',
        });
        await view.refresh();

        expect(hint()).toBeNull();
        expect(joinBtn().textContent.trim()).toBe('text:player.joinButton');
    });

    it('still refuses a slot someone else is streaming', async () => {
        BackendClient.playerInfo.mockResolvedValue({
            ...BASE,
            state: 'streaming',
            cold_start: true,
        });
        await view.refresh();

        expect(joinBtn()).toBeNull();
        // Here the guest really is waiting on another screen to let go.
        expect(refreshBtn()).not.toBeNull();
    });
});
