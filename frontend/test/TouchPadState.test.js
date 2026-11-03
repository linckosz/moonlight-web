/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect, afterEach, vi } from 'vitest';
import {
    PAD,
    TOUCH_PAD_ID,
    TouchPadState,
    buttonsAt,
    layoutTouchPad,
    rumblePattern,
    stickAt,
    stickFromFinger,
} from '../js/stream/TouchPadState.js';
import { GamepadManager } from '../js/stream/GamepadManager.js';

afterEach(() => {
    vi.useRealTimers();
    vi.unstubAllGlobals();
});

describe('stickFromFinger', () => {
    const o = { x: 100, y: 100 };

    it('reads centred inside the dead zone', () => {
        expect(stickFromFinger(o, 103, 100, 50)).toMatchObject({ ax: 0, ay: 0, ox: 100, oy: 100 });
    });

    it('starts from zero at the edge of the dead zone, with no jump', () => {
        const { ax } = stickFromFinger(o, 104.5, 100, 50);
        expect(ax).toBeGreaterThan(0);
        expect(ax).toBeLessThan(0.02);
    });

    it('reaches full travel at the radius, on each axis and sign', () => {
        expect(stickFromFinger(o, 150, 100, 50).ax).toBeCloseTo(1);
        expect(stickFromFinger(o, 50, 100, 50).ax).toBeCloseTo(-1);
        expect(stickFromFinger(o, 100, 150, 50).ay).toBeCloseTo(1);
        expect(stickFromFinger(o, 100, 50, 50).ay).toBeCloseTo(-1);
    });

    it('holds a diagonal to the circle, not the square', () => {
        const { ax, ay } = stickFromFinger(o, 200, 200, 50);
        expect(Math.hypot(ax, ay)).toBeCloseTo(1);
        expect(ax).toBeCloseTo(ay);
    });

    it('brings the origin along a finger past the radius', () => {
        const { ox, oy } = stickFromFinger(o, 200, 100, 50);
        expect(ox).toBeCloseTo(150);
        expect(oy).toBeCloseTo(100);
    });
});

describe('rumblePattern', () => {
    it('is one long buzz at full level', () => {
        expect(rumblePattern(1000, 1)).toBe(1000);
    });

    it('pulses below that, lasting the whole slice, more on when stronger', () => {
        const faint = rumblePattern(1000, 0.1);
        const mid = rumblePattern(1000, 0.5);
        expect(faint.reduce((a, b) => a + b)).toBe(1000);
        expect(faint[0]).toBe(10);
        expect(mid[0]).toBe(25);
        expect(mid[1]).toBe(25);
    });
});

describe('TouchPadState', () => {
    it('is shaped like a standard Gamepad no browser could also report', () => {
        const s = new TouchPadState();
        expect(s).toMatchObject({ index: -1, id: TOUCH_PAD_ID, mapping: 'standard' });
        expect(s.buttons.length).toBe(17);
        expect(s.axes).toEqual([0, 0, 0, 0]);
        expect(s.vibrationActuator).toBeNull();
    });

    it('holds a button for as long as one finger is on it', () => {
        const s = new TouchPadState();
        s.press(PAD.A);
        s.press(PAD.A);
        s.release(PAD.A);
        expect(s.buttons[PAD.A]).toEqual({ pressed: true, value: 1 });
        s.release(PAD.A);
        expect(s.buttons[PAD.A]).toEqual({ pressed: false, value: 0 });
        s.release(PAD.A);
        expect(s.buttons[PAD.A].pressed).toBe(false);
    });

    it('presses triggers all the way', () => {
        const s = new TouchPadState();
        s.press(PAD.RT);
        expect(s.buttons[PAD.RT].value).toBe(1);
    });

    describe('Menu', () => {
        it('is Start on a tap, pressed long enough for a game to read', () => {
            vi.useFakeTimers();
            const onLateChange = vi.fn();
            const s = new TouchPadState({ onLateChange });
            s.press(PAD.MENU);
            expect(s.buttons[PAD.MENU].pressed).toBe(false);
            vi.advanceTimersByTime(100);
            s.release(PAD.MENU);
            expect(s.buttons[PAD.MENU].pressed).toBe(true);
            expect(onLateChange).not.toHaveBeenCalled();
            vi.advanceTimersByTime(80);
            expect(s.buttons[PAD.MENU].pressed).toBe(false);
            expect(onLateChange).toHaveBeenCalledTimes(1);
            expect(s.buttons[PAD.GUIDE].pressed).toBe(false);
        });

        it('is Guide when held, with no Start reaching the game', () => {
            vi.useFakeTimers();
            const onLateChange = vi.fn();
            const s = new TouchPadState({ onLateChange });
            s.press(PAD.MENU);
            vi.advanceTimersByTime(500);
            expect(s.buttons[PAD.GUIDE].pressed).toBe(true);
            expect(s.buttons[PAD.MENU].pressed).toBe(false);
            expect(onLateChange).toHaveBeenCalledTimes(1);
            s.release(PAD.MENU);
            expect(s.buttons[PAD.GUIDE].pressed).toBe(false);
            expect(s.buttons[PAD.MENU].pressed).toBe(false);
            vi.advanceTimersByTime(1000);
            expect(s.buttons[PAD.MENU].pressed).toBe(false);
        });

        it('takes a second tap during the first one', () => {
            vi.useFakeTimers();
            const s = new TouchPadState();
            s.press(PAD.MENU);
            s.release(PAD.MENU);
            s.press(PAD.MENU);
            expect(s.buttons[PAD.MENU].pressed).toBe(false);
            s.release(PAD.MENU);
            expect(s.buttons[PAD.MENU].pressed).toBe(true);
            vi.advanceTimersByTime(80);
            expect(s.buttons[PAD.MENU].pressed).toBe(false);
        });
    });

    describe('sticks', () => {
        it('centre under the finger, and move from there', () => {
            const s = new TouchPadState();
            expect(s.stickDown('left', 300, 200, 50, 0)).toBe(true);
            expect(s.axes).toEqual([0, 0, 0, 0]);
            s.stickMove('left', 350, 200);
            expect(s.axes[0]).toBeCloseTo(1);
            s.stickMove('left', 350, 150);
            expect(Math.hypot(s.axes[0], s.axes[1])).toBeCloseTo(1);
            expect(s.axes[1]).toBeLessThan(0);
        });

        it('have one finger each, and the right one its own axes', () => {
            const s = new TouchPadState();
            s.stickDown('right', 0, 0, 50, 0);
            expect(s.stickDown('right', 40, 0, 50, 0)).toBe(false);
            s.stickMove('right', 0, 50);
            expect(s.axes[3]).toBeCloseTo(1);
            expect(s.axes[1]).toBe(0);
        });

        it('go back to centre when let go', () => {
            const s = new TouchPadState();
            s.stickDown('left', 0, 0, 50, 0);
            s.stickMove('left', 50, 0);
            s.stickUp('left', 1000);
            expect(s.axes).toEqual([0, 0, 0, 0]);
            expect(s.stickView('left')).toBeNull();
        });

        it('say where to draw them', () => {
            const s = new TouchPadState();
            s.stickDown('left', 10, 20, 50, 0);
            s.stickMove('left', 110, 20);
            expect(s.stickView('left')).toEqual({ x: 60, y: 20, kx: 110, ky: 20 });
        });

        it('click on the second touch of a double tap, for as long as it stays', () => {
            const s = new TouchPadState();
            s.stickDown('left', 100, 100, 50, 0);
            s.stickUp('left', 100);
            expect(s.buttons[PAD.L3].pressed).toBe(false);
            s.stickDown('left', 110, 105, 50, 300);
            expect(s.buttons[PAD.L3].pressed).toBe(true);
            s.stickMove('left', 150, 105);
            expect(s.buttons[PAD.L3].pressed).toBe(true);
            s.stickUp('left', 900);
            expect(s.buttons[PAD.L3].pressed).toBe(false);
        });

        it('do not chain a click into the next tap', () => {
            const s = new TouchPadState();
            s.stickDown('right', 0, 0, 50, 0);
            s.stickUp('right', 50);
            s.stickDown('right', 0, 0, 50, 100);
            s.stickUp('right', 150);
            expect(s.buttons[PAD.R3].pressed).toBe(false);
            s.stickDown('right', 0, 0, 50, 200);
            expect(s.buttons[PAD.R3].pressed).toBe(false);
        });

        it.each([
            ['too slow to be a tap', 0, 400, 500, 0, 0],
            ['a stick moved, not a tap', 0, 100, 200, 30, 0],
            ['too long after the first', 0, 100, 500, 0, 0],
            ['too far from the first', 0, 100, 200, 0, 200],
        ])('do not click: %s', (_why, downAt, upAt, secondAt, moveBy, jumpBy) => {
            const s = new TouchPadState();
            s.stickDown('left', 100, 100, 50, downAt);
            if (moveBy) s.stickMove('left', 100 + moveBy, 100);
            s.stickUp('left', upAt);
            s.stickDown('left', 100 + jumpBy, 100, 50, secondAt);
            expect(s.buttons[PAD.L3].pressed).toBe(false);
        });

        it('ignore moves and ends with no finger on them', () => {
            const s = new TouchPadState();
            s.stickMove('left', 50, 50);
            s.stickUp('left');
            expect(s.axes).toEqual([0, 0, 0, 0]);
        });
    });

    it('lets go of everything at once, pending long press and double tap included', () => {
        vi.useFakeTimers();
        const onLateChange = vi.fn();
        const s = new TouchPadState({ onLateChange });
        s.press(PAD.A);
        s.press(PAD.A);
        s.press(PAD.MENU);
        s.stickDown('right', 0, 0, 50, 0);
        s.stickUp('right', 50);
        s.stickDown('left', 0, 0, 50, 0);
        s.stickMove('left', 50, 0);
        s.releaseAll();
        expect(s.buttons.every((b) => !b.pressed && b.value === 0)).toBe(true);
        expect(s.axes).toEqual([0, 0, 0, 0]);
        expect(s.stickView('left')).toBeNull();
        vi.advanceTimersByTime(2000);
        expect(s.buttons[PAD.GUIDE].pressed).toBe(false);
        expect(onLateChange).not.toHaveBeenCalled();
        // The finger's end arriving afterwards changes nothing, and no count is left over.
        s.release(PAD.A);
        s.press(PAD.A);
        s.release(PAD.A);
        expect(s.buttons[PAD.A].pressed).toBe(false);
        // And the tap before it is no first half of a double tap.
        s.stickDown('right', 0, 0, 50, 60);
        expect(s.buttons[PAD.R3].pressed).toBe(false);
    });

    it('also ends a Start pulse under way', () => {
        vi.useFakeTimers();
        const s = new TouchPadState();
        s.press(PAD.MENU);
        s.release(PAD.MENU);
        s.releaseAll();
        expect(s.buttons[PAD.MENU].pressed).toBe(false);
        vi.advanceTimersByTime(200);
        expect(s.buttons[PAD.MENU].pressed).toBe(false);
    });

    describe('vibration', () => {
        it("turns the host's rumble into the phone's, and stops it", () => {
            const vibrate = vi.fn();
            const s = new TouchPadState({ vibrate });
            s.vibrationActuator.playEffect('dual-rumble', {
                duration: 1000,
                strongMagnitude: 1,
                weakMagnitude: 0,
            });
            expect(vibrate).toHaveBeenLastCalledWith(1000);
            s.vibrationActuator.playEffect('dual-rumble', {
                duration: 1000,
                strongMagnitude: 0,
                weakMagnitude: 0.5,
            });
            expect(vibrate.mock.lastCall[0][0]).toBe(25);
            s.vibrationActuator.reset();
            expect(vibrate).toHaveBeenLastCalledWith(0);
        });

        it("ticks at a touch, but never cuts the game's rumble short", () => {
            const vibrate = vi.fn();
            const s = new TouchPadState({ vibrate });
            s.tapHaptic();
            expect(vibrate).toHaveBeenLastCalledWith(10);
            vibrate.mockClear();
            s.vibrationActuator.playEffect('dual-rumble', {
                duration: 1000,
                strongMagnitude: 1,
                weakMagnitude: 1,
            });
            vibrate.mockClear();
            s.tapHaptic();
            expect(vibrate).not.toHaveBeenCalled();
            s.vibrationActuator.reset();
            vibrate.mockClear();
            s.tapHaptic();
            expect(vibrate).toHaveBeenCalledTimes(1);
        });

        it('stays silent without a motor', () => {
            expect(() => new TouchPadState().tapHaptic()).not.toThrow();
        });
    });

    it('is read by GamepadManager as an Xbox pad, buttons and sticks alike', () => {
        const send = vi.fn();
        vi.stubGlobal('requestAnimationFrame', () => 1);
        vi.stubGlobal('cancelAnimationFrame', vi.fn());
        Object.defineProperty(navigator, 'getGamepads', {
            value: () => [],
            configurable: true,
            writable: true,
        });
        const s = new TouchPadState({ vibrate: vi.fn() });
        const gm = new GamepadManager(send);
        gm.start();
        gm.attachVirtual(s);
        expect(send).toHaveBeenCalledWith(
            expect.objectContaining({ type: 'gamepadconnect', index: 0, ctype: 1, rumble: true }),
        );
        s.press(PAD.A);
        s.press(PAD.LT);
        s.stickDown('left', 0, 0, 50, 0);
        s.stickMove('left', 0, -50);
        gm.pokeVirtual();
        expect(send).toHaveBeenLastCalledWith(
            expect.objectContaining({ type: 'gamepad', buttons: 0x1000, lt: 255, ly: 32767 }),
        );
        s.releaseAll();
        gm.pokeVirtual();
        expect(send).toHaveBeenLastCalledWith(
            expect.objectContaining({ type: 'gamepad', buttons: 0, lt: 0, lx: 0 }),
        );
        gm.stop();
    });
});

describe('layoutTouchPad', () => {
    const screens = [
        ['a phone on its side', 844, 390],
        ['a small phone on its side', 568, 320],
        ['a phone held upright', 390, 844],
        ['a narrow phone held upright', 360, 740],
        ['a tablet', 1180, 820],
    ];
    // Face buttons touch by design; only rounding could read that as overlapping.
    const EDGE = 0.001;
    const byId = (layout, id) => layout.controls.find((c) => c.id === id);
    const buttons = (layout) =>
        layout.controls.filter((c) => c.kind === 'button' || c.kind === 'dpad');
    const overlap = (a, b) =>
        a.x < b.x + b.w - EDGE &&
        b.x < a.x + a.w - EDGE &&
        a.y < b.y + b.h - EDGE &&
        b.y < a.y + a.h - EDGE;

    describe.each(screens)('on %s', (_name, width, height) => {
        describe.each([0.8, 1, 1.4])('at scale %s', (scale) => {
            const layout = layoutTouchPad({ width, height, scale });

            it('keeps every control on screen', () => {
                for (const c of layout.controls) {
                    expect(c.x).toBeGreaterThanOrEqual(0);
                    expect(c.y).toBeGreaterThanOrEqual(0);
                    expect(c.x + c.w).toBeLessThanOrEqual(width + 0.001);
                    expect(c.y + c.h).toBeLessThanOrEqual(height + 0.001);
                }
            });

            it('never lays one control over another', () => {
                const list = buttons(layout);
                for (let i = 0; i < list.length; i++) {
                    for (let j = i + 1; j < list.length; j++) {
                        expect(overlap(list[i], list[j])).toBe(false);
                    }
                }
            });

            it('leaves a strip down the middle to the mouse', () => {
                const x = width / 2;
                expect(stickAt(layout.controls, x, layout.d)).toBeNull();
                expect(buttonsAt(layout.controls, x, height / 2)).toBeNull();
            });

            it('rests each stick inside its own zone', () => {
                for (const side of ['stick-left', 'stick-right']) {
                    const c = byId(layout, side);
                    expect(stickAt(layout.controls, c.restX, c.restY)).toBe(c);
                }
            });
        });
    });

    it('grows with the scale, then gives way before the sides meet', () => {
        const at = (scale, w = 844, h = 390) => layoutTouchPad({ width: w, height: h, scale }).d;
        expect(at(1.4)).toBeGreaterThan(at(1));
        expect(at(1)).toBeGreaterThan(at(0.8));
        expect(at(1.4, 300, 700)).toBeLessThanOrEqual(300 / 7);
    });

    it('stays out of the notch and the home bar', () => {
        const insets = { left: 47, right: 47, bottom: 21 };
        const layout = layoutTouchPad({ width: 844, height: 390, insets });
        expect(byId(layout, 'lt').x).toBe(47);
        expect(byId(layout, 'rt').x + byId(layout, 'rt').w).toBe(844 - 47);
        expect(byId(layout, 'dpad').y + byId(layout, 'dpad').h).toBe(390 - 21);
    });

    it('gathers in the lower part of the screen held upright', () => {
        const layout = layoutTouchPad({ width: 390, height: 844 });
        expect(byId(layout, 'lt').y).toBeGreaterThan(844 / 2 - 50);
    });

    describe('under a finger', () => {
        const layout = layoutTouchPad({ width: 844, height: 390 });
        const centre = (c) => [c.x + c.w / 2, c.y + c.h / 2];

        it.each([
            ['a', PAD.A],
            ['b', PAD.B],
            ['x', PAD.X],
            ['y', PAD.Y],
            ['lb', PAD.LB],
            ['lt', PAD.LT],
            ['rb', PAD.RB],
            ['rt', PAD.RT],
            ['view', PAD.VIEW],
            ['menu', PAD.MENU],
        ])('finds %s', (id, button) => {
            expect(buttonsAt(layout.controls, ...centre(byId(layout, id)))).toEqual([button]);
        });

        it('takes the nearest face button, and one just off its edge', () => {
            const a = byId(layout, 'a');
            const b = byId(layout, 'b');
            expect(buttonsAt(layout.controls, a.cx + 2, a.cy - 2)).toEqual([PAD.A]);
            expect(buttonsAt(layout.controls, b.cx - 2, b.cy + 2)).toEqual([PAD.B]);
            expect(buttonsAt(layout.controls, a.cx, a.cy + a.r - 1)).toEqual([PAD.A]);
        });

        it('reads the cross in eight directions, with a dead middle', () => {
            const dpad = byId(layout, 'dpad');
            const [cx, cy] = centre(dpad);
            const far = dpad.w * 0.4;
            const at = (dx, dy) => buttonsAt(layout.controls, cx + dx, cy + dy);
            expect(at(0, 0)).toEqual([]);
            expect(at(0, -far)).toEqual([PAD.UP]);
            expect(at(0, far)).toEqual([PAD.DOWN]);
            expect(at(-far, 0)).toEqual([PAD.LEFT]);
            expect(at(far, 0)).toEqual([PAD.RIGHT]);
            expect(at(far, -far).sort()).toEqual([PAD.UP, PAD.RIGHT].sort());
            expect(at(far, far).sort()).toEqual([PAD.DOWN, PAD.RIGHT].sort());
            expect(at(-far, far).sort()).toEqual([PAD.DOWN, PAD.LEFT].sort());
            expect(at(-far, -far).sort()).toEqual([PAD.UP, PAD.LEFT].sort());
        });

        it('says the stick zones are where no button is', () => {
            const left = byId(layout, 'stick-left');
            const right = byId(layout, 'stick-right');
            expect(buttonsAt(layout.controls, left.restX, left.restY)).toBeNull();
            expect(stickAt(layout.controls, left.restX, left.restY)).toBe(left);
            expect(buttonsAt(layout.controls, right.restX, right.restY)).toBeNull();
            expect(stickAt(layout.controls, right.restX, right.restY)).toBe(right);
        });

        it('says nothing above the shoulders line, in the middle', () => {
            expect(buttonsAt(layout.controls, 422, 195)).toBeNull();
            expect(stickAt(layout.controls, 422, 195)).toBeNull();
        });
    });
});
