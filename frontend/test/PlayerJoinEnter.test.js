/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 *
 * Enter on the guest's join screen presses Join. The guest has just typed the
 * PIN on the keyboard; the next screen has one button, and it used to need the
 * mouse. Enter must not steal a key a focused control or a dialog is owed, and
 * it must not reach the page once the stream has replaced it.
 */
import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';

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

const INFO = {
    needs_pin: false,
    machine_name: 'ATELIER',
    state: 'idle',
    access_level: 'gamer',
    permissions: {},
};

const enter = (init = {}) =>
    document.dispatchEvent(
        new KeyboardEvent('keydown', { key: 'Enter', bubbles: true, cancelable: true, ...init }),
    );

describe('PlayerJoinView: Enter presses Join', () => {
    let container;
    let onJoin;
    let view;

    beforeEach(async () => {
        document.body.innerHTML = '<div id="player"></div>';
        container = document.getElementById('player');
        vi.clearAllMocks();
        BackendClient.playerInfo.mockResolvedValue({ ...INFO });
        onJoin = vi.fn(async () => {});
        view = new PlayerJoinView(container, 'tok', onJoin);
        await view.refresh();
    });

    afterEach(() => view._stopEnterKey());

    it('joins on Enter with nothing focused', async () => {
        enter();
        await vi.waitFor(() => expect(onJoin).toHaveBeenCalledTimes(1));
        expect(onJoin.mock.calls[0][0].height).toBe(1080);
    });

    it('joins once after the PIN, with the key that submitted it already spent', async () => {
        BackendClient.playerInfo
            .mockResolvedValueOnce({ ...INFO, needs_pin: true })
            .mockResolvedValueOnce({ ...INFO });
        BackendClient.playerPin.mockResolvedValue({});
        await view.refresh();
        const input = container.querySelector('.player-pin-input');
        input.value = '123456';
        container.querySelector('.player-pin-form').requestSubmit();
        await vi.waitFor(() => expect(container.querySelector('.player-join-btn')).not.toBeNull());
        expect(onJoin).not.toHaveBeenCalled();

        enter();
        await vi.waitFor(() => expect(onJoin).toHaveBeenCalledTimes(1));
    });

    it('leaves Enter to a focused control', async () => {
        const q720 = container.querySelector('.player-quality-btn[data-height="720"]');
        q720.focus();
        enter();
        await Promise.resolve();
        expect(onJoin).not.toHaveBeenCalled();
    });

    it('ignores a held key, a modifier and an open dialog', async () => {
        enter({ repeat: true });
        enter({ ctrlKey: true });
        const dialog = document.createElement('div');
        dialog.setAttribute('aria-modal', 'true');
        document.body.appendChild(dialog);
        enter();
        await Promise.resolve();
        expect(onJoin).not.toHaveBeenCalled();
        dialog.remove();
        enter();
        await vi.waitFor(() => expect(onJoin).toHaveBeenCalledTimes(1));
    });

    it('does not join twice while the first join is on its way', async () => {
        let release;
        onJoin.mockImplementation(() => new Promise((r) => (release = r)));
        enter();
        enter();
        await vi.waitFor(() => expect(onJoin).toHaveBeenCalledTimes(1));
        enter();
        expect(onJoin).toHaveBeenCalledTimes(1);
        release();
    });

    it('is gone once the stream has replaced the page', async () => {
        container.innerHTML = '<div class="stream"></div>';
        enter();
        await Promise.resolve();
        expect(onJoin).not.toHaveBeenCalled();
        expect(view._enterKey).toBeNull();
    });

    it('has no key to press when the slot is busy', async () => {
        BackendClient.playerInfo.mockResolvedValue({ ...INFO, state: 'streaming' });
        await view.refresh();
        expect(view._enterKey).toBeNull();
    });
});
