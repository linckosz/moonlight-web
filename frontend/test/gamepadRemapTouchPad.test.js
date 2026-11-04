/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 *
 * The on-screen controller in a guest's controller dialog, and what they chose
 * kept for their next visit.
 */
import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';
import { GamepadRemapDialog } from '../js/ui/GamepadRemapDialog.js';
import { guestTouchPadMode, rememberGuestTouchPadMode } from '../js/ui/PlayerJoinView.js';

const realPad = (index, id) => ({
    index,
    id,
    mapping: 'standard',
    connected: true,
    buttons: Array.from({ length: 17 }, () => ({ pressed: false, value: 0 })),
    axes: [0, 0, 0, 0],
});

describe('the on-screen controller in the controller dialog', () => {
    let pads;
    let dialog;

    const open = (touchPad, onPadChange = vi.fn()) => {
        dialog = new GamepadRemapDialog({ padKey: null, mode: 'test', touchPad, onPadChange });
        dialog.open();
        return onPadChange;
    };
    const picker = () =>
        document.querySelector('.gamepad-remap-pad select:not(.gamepad-remap-kind-select)');
    const pick = (value) => {
        picker().value = value;
        picker().dispatchEvent(new Event('change'));
    };

    beforeEach(() => {
        pads = [];
        Object.defineProperty(navigator, 'getGamepads', {
            value: () => pads,
            configurable: true,
            writable: true,
        });
        vi.stubGlobal('requestAnimationFrame', () => 1);
        vi.stubGlobal('cancelAnimationFrame', vi.fn());
        vi.stubGlobal(
            'fetch',
            vi.fn(async () => ({ ok: false, json: async () => ({}) })),
        );
    });
    afterEach(() => {
        if (dialog) dialog.close();
        document.body.innerHTML = '';
        vi.unstubAllGlobals();
    });

    it('is the only entry with no real pad, so a phone can still pick it', () => {
        open({ label: 'On screen', active: false, onPick: vi.fn() });
        expect(Array.from(picker().options).map((o) => o.textContent)).toEqual(['On screen']);
    });

    it('comes first, before the real pads', () => {
        pads = [
            realPad(0, 'Xbox Wireless Controller (STANDARD GAMEPAD Vendor: 045e Product: 02fd)'),
        ];
        open({ label: 'On screen', active: false, onPick: vi.fn() });
        expect(picker().options.length).toBe(2);
        expect(picker().options[0].textContent).toBe('On screen');
    });

    it('stays selected while in use: a pad lying there does not take over', () => {
        pads = [
            realPad(0, 'Xbox Wireless Controller (STANDARD GAMEPAD Vendor: 045e Product: 02fd)'),
        ];
        const onPadChange = open({ label: 'On screen', active: true, onPick: vi.fn() });
        expect(picker().value).toBe('touch-pad');
        expect(onPadChange).not.toHaveBeenCalled();
        expect(dialog._selectedPad()).toBeNull();
    });

    it('calls back and closes when picked', () => {
        const onPick = vi.fn();
        pads = [
            realPad(0, 'Xbox Wireless Controller (STANDARD GAMEPAD Vendor: 045e Product: 02fd)'),
        ];
        const onPadChange = open({ label: 'On screen', active: false, onPick });
        onPadChange.mockClear();
        pick('touch-pad');
        expect(onPick).toHaveBeenCalledTimes(1);
        expect(onPadChange).not.toHaveBeenCalled();
        expect(dialog.isOpen).toBe(false);
    });

    it('gives way to a real pad when one is picked', () => {
        const onPick = vi.fn();
        pads = [
            realPad(0, 'Xbox Wireless Controller (STANDARD GAMEPAD Vendor: 045e Product: 02fd)'),
        ];
        const onPadChange = open({ label: 'On screen', active: true, onPick });
        const real = picker().options[1].value;
        pick(real);
        expect(onPadChange).toHaveBeenCalledWith(real);
        expect(onPick).not.toHaveBeenCalled();
        expect(dialog._selectedPad()).toBe(pads[0]);
    });

    it('changes nothing for whoever is not offered it', () => {
        pads = [
            realPad(0, 'Xbox Wireless Controller (STANDARD GAMEPAD Vendor: 045e Product: 02fd)'),
        ];
        open(null);
        expect(picker()).toBeNull();
    });
});

describe("a guest's choice", () => {
    beforeEach(() => localStorage.clear());
    afterEach(() => localStorage.clear());

    it('starts at auto, and is kept next to the rest of their preferences', () => {
        expect(guestTouchPadMode()).toBe('auto');
        localStorage.setItem('mw_player_prefs', JSON.stringify({ padKey: 'name:x' }));
        rememberGuestTouchPadMode('always');
        expect(guestTouchPadMode()).toBe('always');
        const saved = JSON.parse(localStorage.getItem('mw_player_prefs'));
        expect(saved.padKey).toBe('name:x');
        rememberGuestTouchPadMode('auto');
        expect(guestTouchPadMode()).toBe('auto');
    });

    it('survives a storage it cannot read', () => {
        localStorage.setItem('mw_player_prefs', '{not json');
        expect(guestTouchPadMode()).toBe('auto');
    });
});
