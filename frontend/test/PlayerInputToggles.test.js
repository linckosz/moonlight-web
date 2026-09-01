/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 *
 * The guest's mouse-mode row. It was gated on access_level === 'full', which
 * silently stopped being the right test the day "desktop" was split out of it:
 * a guest granted keyboard and mouse but no gamepad lost both toggles, with no
 * error and nothing on screen to hint that a choice existed.
 */
import { describe, it, expect, beforeEach, vi } from 'vitest';

vi.mock('../js/api/BackendClient.js', () => ({
    BackendClient: { playerInfo: vi.fn(), playerPin: vi.fn() },
}));
vi.mock('../js/i18n/i18n.js', () => ({
    t: (key) => 'text:' + key,
}));
vi.mock('../js/util/BrowserDetect.js', () => ({
    IS_MOBILE_OR_TABLET: false,
    IS_TOUCH_DEVICE: false,
}));

import { PlayerJoinView } from '../js/ui/PlayerJoinView.js';
import { BackendClient } from '../js/api/BackendClient.js';

const BASE = {
    needs_pin: false,
    machine_name: 'ATELIER',
    state: 'idle',
    cold_start: false,
    permissions: {},
};

describe('PlayerJoinView input toggles', () => {
    let view;
    let container;

    const mouseRow = () => container.querySelector('.player-toggle[data-pref="gaming"]');

    const render = async (accessLevel) => {
        BackendClient.playerInfo.mockResolvedValue({ ...BASE, access_level: accessLevel });
        await view.refresh();
    };

    beforeEach(() => {
        document.body.innerHTML = '<div id="player"></div>';
        container = document.getElementById('player');
        localStorage.clear();
        vi.clearAllMocks();
        view = new PlayerJoinView(container, 'tok', async () => {});
    });

    it('offers the mouse mode to a guest who has a mouse but no gamepad', async () => {
        await render('desktop');
        expect(mouseRow()).not.toBeNull();
    });

    it('offers it to a guest granted everything', async () => {
        await render('full');
        expect(mouseRow()).not.toBeNull();
    });

    it('leaves it out where there is no mouse to shape', async () => {
        await render('gamer');
        expect(mouseRow()).toBeNull();
        await render('viewer');
        expect(mouseRow()).toBeNull();
    });

    it('says what the two buttons are choosing between', async () => {
        await render('desktop');
        const label = mouseRow().querySelector('.player-field-label');
        expect(label.textContent).toBe('text:player.mouseMode');

        // Named on screen, and named to a screen reader through the same node
        // rather than a second copy that could drift from it.
        const group = mouseRow().querySelector('.player-toggle-choice');
        expect(group.getAttribute('aria-labelledby')).toBe(label.id);
        expect(label.id).toBeTruthy();

        // "Desktop" and "Gaming" do not say which one keeps the pointer in the
        // frame; the hover hints are where that is spelled out.
        const [off, on] = mouseRow().querySelectorAll('.player-toggle-btn');
        expect(off.getAttribute('title')).toBe('text:player.desktopHint');
        expect(on.getAttribute('title')).toBe('text:player.gamingHint');
    });

    it('remembers the pick for the next visit', async () => {
        await render('desktop');
        const [, on] = mouseRow().querySelectorAll('.player-toggle-btn');
        on.click();
        expect(on.classList.contains('is-selected')).toBe(true);
        expect(JSON.parse(localStorage.getItem('mw_player_prefs')).gaming).toBe(true);
    });
});
