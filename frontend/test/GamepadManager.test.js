/*
 * Moonlight-Web — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';
import { GamepadManager } from '../js/stream/GamepadManager.js';

let rafCb = null;

function fakePad({ index = 0, id = 'Xbox 360 Controller', mapping = 'standard', buttons = [], axes = [0, 0, 0, 0], vibration = false } = {}) {
    return {
        index,
        id,
        mapping,
        vibrationActuator: vibration ? { playEffect: vi.fn() } : null,
        buttons: buttons.map((b) => (typeof b === 'object' ? b : { pressed: !!b, value: b ? 1 : 0 })),
        axes,
    };
}

function setPads(list) {
    Object.defineProperty(navigator, 'getGamepads', {
        value: () => list,
        configurable: true,
        writable: true,
    });
}

describe('GamepadManager', () => {
    beforeEach(() => {
        setPads([]);
        rafCb = null;
        vi.stubGlobal('requestAnimationFrame', (cb) => {
            rafCb = cb;
            return 1;
        });
        vi.stubGlobal('cancelAnimationFrame', vi.fn());
    });
    afterEach(() => {
        vi.unstubAllGlobals();
    });

    it('does nothing on start when the Gamepad API is unavailable', () => {
        Object.defineProperty(navigator, 'getGamepads', { value: undefined, configurable: true });
        const send = vi.fn();
        const gm = new GamepadManager(send);
        gm.start();
        expect(send).not.toHaveBeenCalled();
    });

    it('announces a standard pad on connect with the detected type', () => {
        const send = vi.fn();
        const gm = new GamepadManager(send);
        gm.start();
        window.dispatchEvent(new window.Event('gamepadconnected'));
        // The connect handler reads e.gamepad — dispatch a CustomEvent carrying it.
        const evt = new window.Event('gamepadconnected');
        evt.gamepad = fakePad({ index: 0, id: 'Xbox', vibration: true });
        window.dispatchEvent(evt);
        expect(send).toHaveBeenCalledWith(
            expect.objectContaining({ type: 'gamepadconnect', index: 0, ctype: 1, rumble: true }),
        );
        gm.stop();
    });

    it('ignores non-standard controllers', () => {
        const send = vi.fn();
        const gm = new GamepadManager(send);
        gm.start();
        const evt = new window.Event('gamepadconnected');
        evt.gamepad = fakePad({ mapping: 'no-mapping' });
        window.dispatchEvent(evt);
        expect(send).not.toHaveBeenCalled();
        gm.stop();
    });

    it('polls live pads, maps buttons/axes, and skips unchanged frames', () => {
        const send = vi.fn();
        const gm = new GamepadManager(send);
        // Button 0 (A=0x1000) pressed, left stick fully right.
        const buttons = Array(17).fill(0);
        buttons[0] = 1;
        const axes = [1, 0, 0, 0];
        setPads([fakePad({ buttons, axes })]);

        gm.start(); // first poll: connect + gamepad snapshot
        const gamepadMsg = send.mock.calls.find((c) => c[0].type === 'gamepad');
        expect(gamepadMsg[0]).toMatchObject({ type: 'gamepad', index: 0, lx: 32767 });
        expect(gamepadMsg[0].buttons & 0x1000).toBe(0x1000); // A flag

        send.mockClear();
        rafCb(); // next frame, identical state → no new gamepad message
        expect(send.mock.calls.find((c) => c[0].type === 'gamepad')).toBeUndefined();
        gm.stop();
    });

    it('detects controller types from the id string', () => {
        const cases = [
            ['DualSense Wireless', 2],
            ['Nintendo Switch Pro Controller', 3],
            ['Generic USB Joystick', 0],
        ];
        for (const [id, ctype] of cases) {
            const send = vi.fn();
            const gm = new GamepadManager(send);
            gm.start();
            const evt = new window.Event('gamepadconnected');
            evt.gamepad = fakePad({ id });
            window.dispatchEvent(evt);
            expect(send.mock.calls[0][0].ctype).toBe(ctype);
            gm.stop();
        }
    });

    it('reports disconnects and clears state on stop', () => {
        const send = vi.fn();
        const gm = new GamepadManager(send);
        setPads([fakePad({ index: 2 })]);
        gm.start();
        send.mockClear();
        gm.stop();
        expect(send).toHaveBeenCalledWith(
            expect.objectContaining({ type: 'gamepaddisconnect', index: 2 }),
        );
    });

    it('triggers the vibration actuator on rumble', () => {
        const send = vi.fn();
        const gm = new GamepadManager(send);
        const pad = fakePad({ vibration: true });
        setPads([pad]);
        gm.rumble(0, 65535, 0);
        expect(pad.vibrationActuator.playEffect).toHaveBeenCalledWith(
            'dual-rumble',
            expect.objectContaining({ strongMagnitude: 1 }),
        );
    });
});
