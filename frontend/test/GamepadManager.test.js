/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';
import { GamepadManager, isTvRemotePad } from '../js/stream/GamepadManager.js';

let rafCb = null;

function fakePad({
    index = 0,
    id = 'Xbox 360 Controller',
    mapping = 'standard',
    buttons = [],
    axes = [0, 0, 0, 0],
    vibration = false,
} = {}) {
    return {
        index,
        id,
        mapping,
        vibrationActuator: vibration ? { playEffect: vi.fn() } : null,
        buttons: buttons.map((b) =>
            typeof b === 'object' ? b : { pressed: !!b, value: b ? 1 : 0 },
        ),
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

    it('never forwards a pad the HID passthrough sends as itself, and takes it back when let go', () => {
        const send = vi.fn();
        const wheel = fakePad({ id: 'G923 (STANDARD GAMEPAD Vendor: 046d Product: c26e)' });
        setPads([wheel]);
        const gm = new GamepadManager(send);
        gm.start();
        expect(send).toHaveBeenCalledWith(
            expect.objectContaining({ type: 'gamepadconnect', index: 0 }),
        );
        send.mockClear();
        gm.setExcluded(['usb:046d:c26e']);
        expect(send).toHaveBeenCalledWith(
            expect.objectContaining({ type: 'gamepaddisconnect', index: 0 }),
        );
        send.mockClear();
        rafCb && rafCb(0);
        expect(send.mock.calls.some((c) => c[0].type === 'gamepadconnect')).toBe(false);
        gm.setExcluded([]);
        const evt = new window.Event('gamepadconnected');
        evt.gamepad = wheel;
        window.dispatchEvent(evt);
        expect(send).toHaveBeenCalledWith(
            expect.objectContaining({ type: 'gamepadconnect', index: 0 }),
        );
        gm.stop();
    });

    it('ignores a non-standard controller nothing maps, and says so once', () => {
        const send = vi.fn();
        const onIgnored = vi.fn();
        const gm = new GamepadManager(send, {
            onIgnored,
            platform: 'win',
            db: null,
            user: () => null,
        });
        gm.start();
        const evt = new window.Event('gamepadconnected');
        evt.gamepad = fakePad({ mapping: '' });
        window.dispatchEvent(evt);
        window.dispatchEvent(evt);
        expect(send).not.toHaveBeenCalled();
        expect(onIgnored).toHaveBeenCalledTimes(1);
        gm.stop();
    });

    it('forwards an unknown Chrome Android pad as standard, and says it was guessed', () => {
        const send = vi.fn();
        const onMapped = vi.fn();
        const buttons = Array(17).fill(0);
        buttons[1] = 1; // B
        setPads([fakePad({ id: 'GameSir-G8+', mapping: '', buttons })]);
        const gm = new GamepadManager(send, { onMapped, platform: 'android', user: () => null });
        gm.start();
        const msg = send.mock.calls.find((c) => c[0].type === 'gamepad');
        expect(msg[0].buttons & 0x2000).toBe(0x2000);
        expect(onMapped).toHaveBeenCalledWith(
            expect.anything(),
            expect.objectContaining({ source: 'android' }),
        );
        gm.stop();
    });

    it('reads a database-mapped pad through its bindings', () => {
        const send = vi.fn();
        const db = { win: { '1234:5678': [['Pad', 'a:b3,leftx:a1,dpup:h0.1']] } };
        const buttons = Array(16).fill(0);
        buttons[3] = 1;
        const axes = [0, 1, 0, 0, 0, 0, 0, 0, 0, -1]; // hat up
        setPads([fakePad({ id: 'Pad (Vendor: 1234 Product: 5678)', mapping: '', buttons, axes })]);
        const gm = new GamepadManager(send, { platform: 'win', db, user: () => null });
        gm.start();
        const msg = send.mock.calls.find((c) => c[0].type === 'gamepad')[0];
        expect(msg.buttons & 0x1000).toBe(0x1000); // A from raw button 3
        expect(msg.buttons & 0x0001).toBe(0x0001); // d-pad up from the hat
        expect(msg.lx).toBe(32767); // left X from raw axis 1
        gm.stop();
    });

    it('forwards an EdgeTX radio from its built-in profile, and says it was laid out for the user', () => {
        const send = vi.fn();
        const onMapped = vi.fn();
        // Throttle at the bottom, CH5 unmixed (0), CH6 switch down (+1).
        setPads([
            fakePad({
                id: 'Radiomaster TX12 Joystick (Vendor: 1209 Product: 4f54)',
                mapping: '',
                buttons: Array(24).fill(0),
                axes: [0, 0, -1, 0, 0, 1, 0],
            }),
        ]);
        const gm = new GamepadManager(send, { onMapped, platform: 'win', user: () => null });
        gm.start();
        const msg = send.mock.calls.find((c) => c[0].type === 'gamepad')[0];
        expect(msg).toMatchObject({ ly: -32767, lt: 128, rt: 255, lx: 0, rx: 0 });
        expect(onMapped).toHaveBeenCalledWith(
            expect.anything(),
            expect.objectContaining({ source: 'builtin' }),
        );
        gm.stop();
    });

    it('loads the database before deciding on a desktop pad', async () => {
        const send = vi.fn();
        setPads([fakePad({ id: 'Mystery (Vendor: 0001 Product: 0002)', mapping: '' })]);
        const onIgnored = vi.fn();
        const gm = new GamepadManager(send, { onIgnored, platform: 'win', user: () => null });
        gm.start();
        // Not decided yet: no announce, no warning.
        expect(onIgnored).not.toHaveBeenCalled();
        await vi.waitFor(() => {
            rafCb();
            expect(onIgnored).toHaveBeenCalledTimes(1);
        });
        gm.stop();
    });

    it("prefers the user's own mapping, and picks up a change live", () => {
        const send = vi.fn();
        let saved = null;
        const buttons = Array(17).fill(0);
        buttons[0] = 1;
        setPads([fakePad({ buttons })]);
        const gm = new GamepadManager(send, { platform: 'win', user: () => saved });
        gm.start();
        expect(send.mock.calls.find((c) => c[0].type === 'gamepad')[0].buttons).toBe(0x1000);

        // Swapped A and B, saved elsewhere (the dialog).
        saved = { bindings: { b: { t: 'b', i: 0 }, a: { t: 'b', i: 1 } } };
        gm.refreshMappings();
        send.mockClear();
        rafCb();
        expect(send.mock.calls.find((c) => c[0].type === 'gamepad')[0].buttons).toBe(0x2000);
        gm.stop();
    });

    it('in single mode forwards one pad as controller 0, the preferred one first', () => {
        const send = vi.fn();
        const a = fakePad({ index: 0, id: 'A (Vendor: 0001 Product: 0001)' });
        const b = fakePad({ index: 1, id: 'B (Vendor: 0002 Product: 0002)', vibration: true });
        setPads([a, b]);
        const gm = new GamepadManager(send, {
            single: true,
            preferredKey: 'usb:0002:0002',
            platform: 'win',
            user: () => null,
        });
        gm.start();
        const connects = send.mock.calls.filter((c) => c[0].type === 'gamepadconnect');
        // Pad A came first and took the place; B (preferred) took it over.
        expect(connects.map((c) => c[0].index)).toEqual([0, 0]);
        expect(send).toHaveBeenCalledWith(
            expect.objectContaining({ type: 'gamepaddisconnect', index: 0 }),
        );
        expect(gm.forwardedKey()).toBe('usb:0002:0002');
        const states = send.mock.calls.filter((c) => c[0].type === 'gamepad').map((c) => c[0]);
        expect(states.every((m) => m.index === 0 && m.mask === 1)).toBe(true);

        // The host's controller 0 rumbles the pad in hand (browser index 1).
        gm.rumble(0, 65535, 0);
        expect(b.vibrationActuator.playEffect).toHaveBeenCalled();

        // Switching pads releases the current one.
        send.mockClear();
        gm.setPreferredPad('usb:0001:0001');
        rafCb();
        expect(gm.forwardedKey()).toBe('usb:0001:0001');
        gm.stop();
    });

    it('while paused puts pads at rest once and sends nothing', () => {
        const send = vi.fn();
        const pad = fakePad({ axes: [1, 0, 0, 0] });
        setPads([pad]);
        const gm = new GamepadManager(send, { platform: 'win', user: () => null });
        gm.start();
        send.mockClear();
        gm.setPaused(true);
        expect(send).toHaveBeenCalledWith(
            expect.objectContaining({ type: 'gamepad', lx: 0, buttons: 0 }),
        );
        send.mockClear();
        pad.axes = [-1, 0, 0, 0];
        rafCb();
        expect(send).not.toHaveBeenCalled();
        gm.setPaused(false);
        rafCb();
        expect(send).toHaveBeenCalledWith(expect.objectContaining({ type: 'gamepad', lx: -32767 }));
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

    it('repeats a pad held away from rest, never one at rest', () => {
        const send = vi.fn();
        const gm = new GamepadManager(send);
        let now = 1000;
        vi.spyOn(performance, 'now').mockImplementation(() => now);
        const pad = fakePad({ index: 0, axes: [1, 0, 0, 0] });
        setPads([pad]);

        gm.start(); // first poll: connect + the pushed stick
        send.mockClear();

        // The same frame again, too soon: silence.
        now += 100;
        rafCb();
        expect(send.mock.calls.find((c) => c[0].type === 'gamepad')).toBeUndefined();

        // Half a second later, still pushed: said again, unchanged. This is
        // what puts a pad back after the host centred it on a dead link.
        now += 400;
        rafCb();
        const repeat = send.mock.calls.find((c) => c[0].type === 'gamepad');
        expect(repeat[0]).toMatchObject({ type: 'gamepad', index: 0, lx: 32767 });

        // Back to rest: reported once, then never repeated however long it sits.
        send.mockClear();
        pad.axes = [0, 0, 0, 0];
        now += 100;
        rafCb();
        expect(send.mock.calls.filter((c) => c[0].type === 'gamepad')).toHaveLength(1);
        send.mockClear();
        now += 5000;
        rafCb();
        expect(send.mock.calls.find((c) => c[0].type === 'gamepad')).toBeUndefined();
        gm.stop();
    });

    it('repeats every pad once after resendAll, at rest or not', () => {
        const send = vi.fn();
        const gm = new GamepadManager(send);
        let now = 1000;
        vi.spyOn(performance, 'now').mockImplementation(() => now);
        const pushed = fakePad({ index: 0, axes: [1, 0, 0, 0] });
        const resting = fakePad({ index: 1 });
        setPads([pushed, resting]);

        gm.start(); // first poll: both announced and reported
        send.mockClear();
        now += 100;
        rafCb(); // unchanged, too soon: silence
        expect(send.mock.calls.find((c) => c[0].type === 'gamepad')).toBeUndefined();

        // The tab comes back from the background: the host may have centred
        // the pushed pad, so both are said again on the very next poll.
        gm.resendAll();
        now += 10;
        rafCb();
        const said = send.mock.calls.filter((c) => c[0].type === 'gamepad').map((c) => c[0]);
        expect(said).toHaveLength(2);
        expect(said.find((m) => m.index === 0)).toMatchObject({ lx: 32767 });
        expect(said.find((m) => m.index === 1)).toMatchObject({ lx: 0, buttons: 0 });

        // And then it is quiet again for the resting one.
        send.mockClear();
        now += 10;
        rafCb();
        expect(send.mock.calls.find((c) => c[0].index === 1)).toBeUndefined();
        gm.stop();
    });

    describe('between two frames', () => {
        function setVisibility(state) {
            Object.defineProperty(document, 'visibilityState', {
                value: state,
                configurable: true,
            });
            document.dispatchEvent(new window.Event('visibilitychange'));
        }

        beforeEach(() => {
            vi.useFakeTimers({ toFake: ['setInterval', 'clearInterval'] });
        });
        afterEach(() => {
            vi.useRealTimers();
            // Back to jsdom's own getter.
            delete document.visibilityState;
        });

        it('reads a forwarded pad every 4 ms, and sends only what changed', () => {
            const send = vi.fn();
            const pad = fakePad({ index: 0 });
            setPads([pad]);
            const gm = new GamepadManager(send, { platform: 'win', user: () => null });
            gm.start();
            send.mockClear();

            // The stick moves; no frame comes: the next read, 4 ms on, sends it.
            pad.axes = [1, 0, 0, 0];
            vi.advanceTimersByTime(4);
            expect(send).toHaveBeenCalledWith(
                expect.objectContaining({ type: 'gamepad', index: 0, lx: 32767 }),
            );

            // Ten more reads of the same state: nothing.
            send.mockClear();
            vi.advanceTimersByTime(40);
            expect(send).not.toHaveBeenCalled();
            gm.stop();
        });

        it('leaves the frame loop alone while no pad is forwarded', () => {
            // A pad nothing maps is seen, not forwarded.
            setPads([fakePad({ mapping: '' })]);
            const gm = new GamepadManager(vi.fn(), { platform: 'win', db: null, user: () => null });
            gm.start();
            expect(vi.getTimerCount()).toBe(0);
            gm.stop();
        });

        it('stops on pause, on a hidden page, on the last pad gone and on stop()', () => {
            const pad = fakePad({ index: 0 });
            setPads([pad]);
            const gm = new GamepadManager(vi.fn(), { platform: 'win', user: () => null });
            gm.start();
            expect(vi.getTimerCount()).toBe(1);

            gm.setPaused(true);
            expect(vi.getTimerCount()).toBe(0);
            gm.setPaused(false);
            expect(vi.getTimerCount()).toBe(1);

            setVisibility('hidden');
            expect(vi.getTimerCount()).toBe(0);
            setVisibility('visible');
            expect(vi.getTimerCount()).toBe(1);

            const gone = new window.Event('gamepaddisconnected');
            gone.gamepad = pad;
            setPads([]);
            window.dispatchEvent(gone);
            expect(vi.getTimerCount()).toBe(0);

            setPads([pad]);
            rafCb();
            expect(vi.getTimerCount()).toBe(1);
            gm.stop();
            expect(vi.getTimerCount()).toBe(0);
        });

        it('tells the probe how far apart the reads are, and each state sent', () => {
            let now = 1000;
            const clock = vi.spyOn(performance, 'now').mockImplementation(() => now);
            const probe = { notePadRead: vi.fn(), notePadSend: vi.fn() };
            const pad = fakePad({ index: 0 });
            setPads([pad]);
            const gm = new GamepadManager(vi.fn(), { platform: 'win', user: () => null, probe });

            gm.start(); // the pad is found and its state sent: no gap yet
            expect(probe.notePadRead).not.toHaveBeenCalled();
            expect(probe.notePadSend).toHaveBeenCalledTimes(1);

            now += 4;
            vi.advanceTimersByTime(4);
            expect(probe.notePadRead).toHaveBeenCalledWith(4);
            expect(probe.notePadSend).toHaveBeenCalledTimes(1); // unchanged: nothing sent

            // A pause is no gap: the reads after it start over.
            gm.setPaused(true);
            now += 1000;
            gm.setPaused(false);
            probe.notePadRead.mockClear();
            now += 4;
            vi.advanceTimersByTime(4);
            now += 4;
            vi.advanceTimersByTime(4);
            expect(probe.notePadRead.mock.calls).toEqual([[4]]);
            gm.stop();
            clock.mockRestore();
        });
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

    it('reports an at-rest pad as inactive and a pushed one as active', () => {
        const send = vi.fn();
        const gm = new GamepadManager(send);
        const pad = fakePad({ index: 0 });
        setPads([pad]);
        gm.start();
        // A connected pad sitting at rest must not keep the held-input
        // heartbeat alive — that is what tells the host the link is idle.
        expect(gm.hasActiveState()).toBe(false);

        // Stick shoved and left there: the pad stops reporting, so only this
        // is left to keep the host's watchdog from releasing it.
        pad.axes = [1, 0, 0, 0];
        rafCb();
        expect(gm.hasActiveState()).toBe(true);

        pad.axes = [0, 0, 0, 0];
        rafCb();
        expect(gm.hasActiveState()).toBe(false);
        gm.stop();
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

/**
 * A TV remote that Android calls a joystick (the Freebox's B16C): once the
 * page reads the pads, its arrows come as pad buttons 12-15. It is no
 * controller for a game — its arrows go out as keys, and the host never
 * hears of a pad.
 */
describe('GamepadManager and a TV remote seen as a pad', () => {
    const remote = (pressed = []) =>
        fakePad({
            index: 1,
            id: 'B16C (Vendor: 7545 Product: 0183)',
            buttons: Array.from({ length: 17 }, (_, i) => (pressed.includes(i) ? 1 : 0)),
        });

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
        vi.restoreAllMocks();
    });

    it('knows the Freebox remote, and not a game pad', () => {
        expect(isTvRemotePad({ id: 'B16C (Vendor: 7545 Product: 0183)' })).toBe(true);
        expect(isTvRemotePad({ id: 'SHIELD Remote' })).toBe(true);
        expect(isTvRemotePad({ id: 'Xbox Wireless Controller' })).toBe(false);
        expect(isTvRemotePad({ id: 'DualSense Wireless Controller' })).toBe(false);
    });

    it('sends its arrows as keys and never announces it as a pad', () => {
        const send = vi.fn();
        const onRemoteKey = vi.fn();
        const gm = new GamepadManager(send, { onRemoteKey, platform: 'win', user: () => null });
        setPads([remote()]);
        gm.start();
        setPads([remote([13])]);
        rafCb();
        expect(onRemoteKey).toHaveBeenCalledWith('down', true, false);
        setPads([remote()]);
        rafCb();
        expect(onRemoteKey).toHaveBeenLastCalledWith('down', false, false);
        expect(send.mock.calls.find((c) => /^gamepad/.test(c[0].type))).toBeUndefined();
        // Its connect event is no pad either.
        const evt = new window.Event('gamepadconnected');
        evt.gamepad = remote();
        window.dispatchEvent(evt);
        expect(send).not.toHaveBeenCalled();
        gm.stop();
    });

    it('repeats an arrow held down, as a keyboard would', () => {
        let now = 1000;
        vi.spyOn(performance, 'now').mockImplementation(() => now);
        const onRemoteKey = vi.fn();
        const gm = new GamepadManager(vi.fn(), { onRemoteKey, platform: 'win', user: () => null });
        setPads([remote([15])]);
        gm.start();
        expect(onRemoteKey).toHaveBeenCalledTimes(1);
        now += 200;
        rafCb();
        expect(onRemoteKey).toHaveBeenCalledTimes(1);
        now += 300;
        rafCb();
        expect(onRemoteKey).toHaveBeenLastCalledWith('right', true, true);
        gm.stop();
        // Stopping lets go of it on the host.
        expect(onRemoteKey).toHaveBeenLastCalledWith('right', false, false);
    });

    it('lets go of a held arrow when paused', () => {
        const onRemoteKey = vi.fn();
        const gm = new GamepadManager(vi.fn(), { onRemoteKey, platform: 'win', user: () => null });
        setPads([remote([12])]);
        gm.start();
        gm.setPaused(true);
        expect(onRemoteKey).toHaveBeenLastCalledWith('up', false, false);
        gm.stop();
    });

    it('tells the menu chord of a real pad once per press', () => {
        const onMenuChord = vi.fn();
        const gm = new GamepadManager(vi.fn(), { onMenuChord });
        const chord = Array(17).fill(0);
        for (const i of [4, 5, 8, 9]) chord[i] = 1;
        setPads([fakePad({ buttons: chord })]);
        gm.start();
        rafCb();
        expect(onMenuChord).toHaveBeenCalledTimes(1);
        setPads([fakePad({ buttons: Array(17).fill(0) })]);
        rafCb();
        setPads([fakePad({ buttons: chord })]);
        rafCb();
        expect(onMenuChord).toHaveBeenCalledTimes(2);
        gm.stop();
    });
});

describe('GamepadManager and an on-screen controller', () => {
    /** A Gamepad-shaped on-screen controller: no browser index, always current. */
    function touchPad(extra = {}) {
        return {
            index: -1,
            id: 'MoonlightWeb Touch Controller',
            mapping: 'standard',
            vibrationActuator: null,
            buttons: Array.from({ length: 17 }, () => ({ pressed: false, value: 0 })),
            axes: [0, 0, 0, 0],
            ...extra,
        };
    }
    const sent = (send, type) => send.mock.calls.map((c) => c[0]).filter((m) => m.type === type);

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
        vi.useRealTimers();
    });

    it('is announced as an Xbox pad, controller 0, when nothing else is there', () => {
        const send = vi.fn();
        const gm = new GamepadManager(send);
        gm.start();
        gm.attachVirtual(touchPad());
        expect(sent(send, 'gamepadconnect')).toEqual([
            { type: 'gamepadconnect', index: 0, mask: 1, ctype: 1, rumble: false },
        ]);
        expect(gm.hasVirtual()).toBe(true);
        expect(gm.hasPhysicalPad()).toBe(false);
    });

    it('waits for start, like any pad', () => {
        const send = vi.fn();
        const gm = new GamepadManager(send);
        gm.attachVirtual(touchPad());
        expect(send).not.toHaveBeenCalled();
        gm.start();
        rafCb();
        expect(sent(send, 'gamepadconnect').length).toBe(1);
    });

    it('runs without the Gamepad API, from the moment it is plugged in', () => {
        Object.defineProperty(navigator, 'getGamepads', { value: undefined, configurable: true });
        const send = vi.fn();
        const gm = new GamepadManager(send);
        gm.start();
        expect(send).not.toHaveBeenCalled();
        gm.attachVirtual(touchPad());
        expect(sent(send, 'gamepadconnect').length).toBe(1);
        gm.stop();
        expect(sent(send, 'gamepaddisconnect').length).toBe(1);
    });

    it('takes the lowest number free beside a real pad', () => {
        setPads([fakePad({ index: 0 })]);
        const send = vi.fn();
        const gm = new GamepadManager(send);
        gm.start();
        gm.attachVirtual(touchPad());
        expect(sent(send, 'gamepadconnect').map((m) => m.index)).toEqual([0, 1]);
        expect(sent(send, 'gamepadconnect')[1].mask).toBe(3);
    });

    it('moves a real pad off the number it holds, and renumbers nobody', () => {
        const send = vi.fn();
        const gm = new GamepadManager(send);
        gm.start();
        const v = touchPad();
        gm.attachVirtual(v);
        setPads([fakePad({ index: 0, buttons: [1] })]);
        rafCb();
        const connects = sent(send, 'gamepadconnect');
        expect(connects.map((m) => m.index)).toEqual([0, 1]);
        // The real pad's state goes out under its own number.
        expect(sent(send, 'gamepad').at(-1)).toMatchObject({ index: 1, buttons: 0x1000 });
        // The on-screen one going away leaves the real pad where it is.
        gm.detachVirtual();
        expect(sent(send, 'gamepaddisconnect')).toEqual([
            { type: 'gamepaddisconnect', index: 0, mask: 2 },
        ]);
        send.mockClear();
        setPads([fakePad({ index: 0, buttons: [0, 1] })]);
        rafCb();
        expect(sent(send, 'gamepad')[0]).toMatchObject({ index: 1, buttons: 0x2000 });
    });

    it('leaves the browser index of a real pad alone without one', () => {
        setPads([null, null, fakePad({ index: 2 })]);
        const send = vi.fn();
        const gm = new GamepadManager(send);
        gm.start();
        expect(sent(send, 'gamepadconnect').map((m) => m.index)).toEqual([2]);
    });

    it('sends at the touch, and nothing when nothing changed', () => {
        const send = vi.fn();
        const gm = new GamepadManager(send);
        gm.start();
        const v = touchPad();
        gm.attachVirtual(v);
        send.mockClear();
        gm.pokeVirtual();
        expect(send).not.toHaveBeenCalled();
        v.buttons[0] = { pressed: true, value: 1 };
        v.buttons[7] = { pressed: true, value: 1 };
        v.axes[1] = -1;
        gm.pokeVirtual();
        expect(send.mock.calls.length).toBe(1);
        expect(send.mock.calls[0][0]).toMatchObject({
            type: 'gamepad',
            index: 0,
            buttons: 0x1000,
            rt: 255,
            ly: 32767,
        });
        gm.pokeVirtual();
        expect(send.mock.calls.length).toBe(1);
        expect(gm.hasActiveState()).toBe(true);
    });

    it('is repeated by the frame loop while held, and not while at rest', () => {
        vi.useFakeTimers({ toFake: ['performance'] });
        const send = vi.fn();
        const gm = new GamepadManager(send);
        gm.start();
        const v = touchPad();
        gm.attachVirtual(v);
        v.axes[0] = 1;
        gm.pokeVirtual();
        send.mockClear();
        vi.advanceTimersByTime(600);
        rafCb();
        expect(sent(send, 'gamepad').length).toBe(1);
        v.axes[0] = 0;
        gm.pokeVirtual();
        send.mockClear();
        vi.advanceTimersByTime(600);
        rafCb();
        expect(send).not.toHaveBeenCalled();
    });

    it('alone arms no 4 ms reader; beside a real pad, one is armed', () => {
        vi.useFakeTimers();
        const interval = vi.spyOn(globalThis, 'setInterval');
        const gm = new GamepadManager(vi.fn());
        gm.start();
        gm.attachVirtual(touchPad());
        expect(interval).not.toHaveBeenCalled();
        setPads([fakePad({ index: 0 })]);
        rafCb();
        expect(interval).toHaveBeenCalledTimes(1);
        gm.stop();
        interval.mockRestore();
    });

    it('is put at rest and silenced by a pause, then repeated', () => {
        const send = vi.fn();
        const gm = new GamepadManager(send);
        gm.start();
        const v = touchPad();
        gm.attachVirtual(v);
        v.buttons[0] = { pressed: true, value: 1 };
        gm.pokeVirtual();
        gm.setPaused(true);
        expect(sent(send, 'gamepad').at(-1)).toMatchObject({ buttons: 0 });
        send.mockClear();
        gm.pokeVirtual();
        expect(send).not.toHaveBeenCalled();
        gm.setPaused(false);
        gm.pokeVirtual();
        expect(sent(send, 'gamepad')[0]).toMatchObject({ buttons: 0x1000 });
    });

    it('comes back after a stop and a start, still plugged in', () => {
        const send = vi.fn();
        const gm = new GamepadManager(send);
        gm.start();
        gm.attachVirtual(touchPad());
        gm.stop();
        expect(sent(send, 'gamepaddisconnect').length).toBe(1);
        gm.start();
        rafCb();
        expect(sent(send, 'gamepadconnect').length).toBe(2);
    });

    it('sends nothing more once unplugged', () => {
        const send = vi.fn();
        const gm = new GamepadManager(send);
        gm.start();
        gm.attachVirtual(touchPad());
        gm.detachVirtual();
        expect(gm.hasVirtual()).toBe(false);
        send.mockClear();
        gm.pokeVirtual();
        rafCb();
        expect(send).not.toHaveBeenCalled();
        gm.detachVirtual();
        expect(send).not.toHaveBeenCalled();
    });

    it('plugging a second one in replaces the first', () => {
        const send = vi.fn();
        const gm = new GamepadManager(send);
        gm.start();
        gm.attachVirtual(touchPad());
        gm.attachVirtual(touchPad());
        expect(sent(send, 'gamepadconnect').map((m) => m.index)).toEqual([0, 0]);
        expect(sent(send, 'gamepaddisconnect').map((m) => m.mask)).toEqual([0]);
    });

    describe('when it yields to a real pad', () => {
        it('is unplugged first, so the real pad takes controller 0', () => {
            const send = vi.fn();
            const onVirtualYield = vi.fn();
            const gm = new GamepadManager(send, { onVirtualYield });
            gm.start();
            gm.attachVirtual(touchPad(), { yields: true });
            setPads([fakePad({ index: 0 })]);
            rafCb();
            expect(onVirtualYield).toHaveBeenCalledTimes(1);
            expect(gm.hasVirtual()).toBe(false);
            expect(sent(send, 'gamepaddisconnect').map((m) => m.index)).toEqual([0]);
            expect(sent(send, 'gamepadconnect').map((m) => m.index)).toEqual([0, 0]);
            expect(sent(send, 'gamepadconnect')[1].mask).toBe(1);
        });

        it('does not yield to a pad nothing maps, nor when not asked to', () => {
            const send = vi.fn();
            const onVirtualYield = vi.fn();
            const gm = new GamepadManager(send, { onVirtualYield, platform: 'ios' });
            gm.start();
            gm.attachVirtual(touchPad(), { yields: true });
            setPads([fakePad({ index: 0, id: 'Mystery', mapping: '' })]);
            rafCb();
            expect(gm.hasVirtual()).toBe(true);
            gm.attachVirtual(touchPad());
            setPads([fakePad({ index: 0 })]);
            rafCb();
            expect(gm.hasVirtual()).toBe(true);
            expect(onVirtualYield).not.toHaveBeenCalled();
        });
    });

    describe('in single mode (a guest)', () => {
        it('takes controller 0 from the real pad, and gives it back', () => {
            setPads([fakePad({ index: 1 })]);
            const send = vi.fn();
            const gm = new GamepadManager(send, { single: true, preferredKey: 'name:other' });
            gm.start();
            expect(sent(send, 'gamepadconnect').map((m) => m.index)).toEqual([0]);
            gm.attachVirtual(touchPad());
            expect(sent(send, 'gamepaddisconnect').map((m) => m.index)).toEqual([0]);
            expect(sent(send, 'gamepadconnect').map((m) => m.index)).toEqual([0, 0]);
            expect(gm.forwardedKey()).toBe('name:moonlightweb touch controller');
            // The real pad does not get it back while the on-screen one is there.
            rafCb();
            expect(sent(send, 'gamepadconnect').length).toBe(2);
            gm.detachVirtual();
            rafCb();
            expect(sent(send, 'gamepadconnect').length).toBe(3);
            expect(gm.forwardedKey()).not.toBe('name:moonlightweb touch controller');
        });

        it('gives its place to a real pad when it yields', () => {
            const send = vi.fn();
            const onVirtualYield = vi.fn();
            const gm = new GamepadManager(send, { single: true, onVirtualYield });
            gm.start();
            gm.attachVirtual(touchPad(), { yields: true });
            setPads([fakePad({ index: 3 })]);
            rafCb();
            expect(onVirtualYield).toHaveBeenCalledTimes(1);
            expect(sent(send, 'gamepadconnect').map((m) => m.index)).toEqual([0, 0]);
            expect(gm.forwardedKey()).not.toBe('name:moonlightweb touch controller');
        });
    });

    describe('rumble', () => {
        it('reaches the on-screen controller through its own number', () => {
            vi.useFakeTimers();
            const playEffect = vi.fn();
            const reset = vi.fn();
            const gm = new GamepadManager(vi.fn());
            gm.start();
            gm.attachVirtual(touchPad({ vibrationActuator: { playEffect, reset } }));
            gm.rumble(0, 65535, 0);
            expect(playEffect).toHaveBeenCalledTimes(1);
            expect(playEffect.mock.calls[0][1]).toMatchObject({ strongMagnitude: 1 });
            gm.rumble(0, 0, 0);
            expect(reset).toHaveBeenCalledTimes(1);
            vi.advanceTimersByTime(2000);
            expect(playEffect).toHaveBeenCalledTimes(1);
        });

        it('stops when the controller is unplugged', () => {
            vi.useFakeTimers();
            const playEffect = vi.fn();
            const reset = vi.fn();
            const gm = new GamepadManager(vi.fn());
            gm.start();
            gm.attachVirtual(touchPad({ vibrationActuator: { playEffect, reset } }));
            gm.rumble(0, 65535, 0);
            gm.detachVirtual();
            expect(reset).toHaveBeenCalledTimes(1);
            vi.advanceTimersByTime(2000);
            expect(playEffect).toHaveBeenCalledTimes(1);
        });

        it('finds the real pad behind its moved number', () => {
            vi.useFakeTimers();
            const send = vi.fn();
            const gm = new GamepadManager(send);
            gm.start();
            const v = touchPad();
            gm.attachVirtual(v);
            const real = fakePad({ index: 0, vibration: true });
            setPads([real]);
            rafCb();
            gm.rumble(1, 65535, 0);
            expect(real.vibrationActuator.playEffect).toHaveBeenCalledTimes(1);
            gm.stop();
        });

        it('leaves a real pad alone when a gone controller had its browser index', () => {
            const send = vi.fn();
            const gm = new GamepadManager(send);
            gm.start();
            gm.attachVirtual(touchPad());
            const real = fakePad({ index: 0, vibration: true });
            setPads([real]);
            rafCb();
            gm.detachVirtual();
            // Controller 0 is gone; the real pad is browser index 0, controller 1.
            gm.rumble(0, 65535, 0);
            expect(real.vibrationActuator.playEffect).not.toHaveBeenCalled();
        });
    });
});
