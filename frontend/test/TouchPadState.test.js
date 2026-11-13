/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect, afterEach, vi } from 'vitest';
import {
    PAD,
    TOUCH_PAD_ID,
    TOUCH_PAD_PRESETS,
    TouchPadState,
    buttonsAt,
    layoutOrientation,
    touchPadLayouts,
    touchPadSettings,
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

describe('touchPadSettings', () => {
    const defaultLayout = { preset: 'default', landscape: {}, portrait: {} };

    it('gives the defaults of a viewer who never opened them', () => {
        expect(touchPadSettings()).toEqual({
            mode: 'auto',
            scale: 1,
            opacity: 1,
            vibrate: true,
            outside: 'mouse',
            layout: defaultLayout,
            style: 'xbox',
        });
    });

    it('takes what was saved', () => {
        expect(
            touchPadSettings({
                touch_pad_mode: 'never',
                touch_pad_scale: 1.4,
                touch_pad_opacity: 0.2,
                touch_pad_vibrate: false,
                touch_pad_outside: 'ignore',
                touch_pad_style: 'switch',
            }),
        ).toEqual({
            mode: 'never',
            scale: 1.4,
            opacity: 0.2,
            vibrate: false,
            outside: 'ignore',
            layout: defaultLayout,
            style: 'switch',
        });
    });

    it('refuses what no setting could have saved', () => {
        const odd = touchPadSettings({
            touch_pad_mode: 'sometimes',
            touch_pad_scale: 3,
            touch_pad_opacity: 0,
            touch_pad_outside: 'elsewhere',
            touch_pad_style: 'gamecube',
        });
        expect(odd).toEqual(touchPadSettings());
        expect(touchPadSettings({ touch_pad_opacity: '0.5' }).opacity).toBe(1);
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
    const byId = (layout, id) => layout.controls.find((c) => c.id === id);
    // What is drawn: the cross is four round keys, the toggle one more.
    const drawn = (layout) => {
        const list = [];
        const disc = (cx, cy, r) => ({ x: cx - r, y: cy - r, w: r * 2, h: r * 2, disc: true });
        for (const c of layout.controls) {
            if (c.kind === 'dpad') {
                const r = layout.d / 2;
                const far = c.w / 2 - r;
                list.push(disc(c.cx, c.cy - far, r), disc(c.cx, c.cy + far, r));
                list.push(disc(c.cx - far, c.cy, r), disc(c.cx + far, c.cy, r));
            } else if (c.kind === 'button') {
                list.push(c.r ? disc(c.cx, c.cy, c.w / 2) : c);
            }
        }
        const t = layout.toggle;
        list.push(disc(t.x + t.w / 2, t.y + t.h / 2, t.w / 2));
        return list;
    };
    const overlap = (a, b) => {
        if (a.disc && b.disc) {
            const dist = Math.hypot(a.x + a.w / 2 - b.x - b.w / 2, a.y + a.h / 2 - b.y - b.h / 2);
            return dist < (a.w + b.w) / 2 - 0.001;
        }
        return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
    };

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
                const t = layout.toggle;
                expect(t.y).toBeGreaterThanOrEqual(0);
                expect(t.y + t.h).toBeLessThanOrEqual(height);
                expect(t.w).toBeGreaterThanOrEqual(44);
            });

            it('rests each ring on screen', () => {
                for (const side of ['stick-left', 'stick-right']) {
                    const c = byId(layout, side);
                    expect(c.restX - c.radius).toBeGreaterThanOrEqual(0);
                    expect(c.restX + c.radius).toBeLessThanOrEqual(width);
                    expect(c.restY + c.radius).toBeLessThanOrEqual(height);
                }
            });

            it('never lays one control over another', () => {
                const list = drawn(layout);
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
        expect(byId(layout, 'rt').x + byId(layout, 'rt').w).toBeCloseTo(844 - 47);
        expect(byId(layout, 'l3').y + byId(layout, 'l3').h).toBe(390 - 21);
        expect(byId(layout, 'r3').y + byId(layout, 'r3').h).toBe(390 - 21);
        expect(byId(layout, 'stick-left').restY + layout.radius).toBe(390 - 21);
    });

    it('gathers in the lower part of the screen held upright', () => {
        const layout = layoutTouchPad({ width: 390, height: 844 });
        expect(byId(layout, 'lt').y).toBeGreaterThan(844 / 2 - 50);
    });

    it('keeps the outer places of GeForce NOW, within a pixel or two, on that shape', () => {
        // Measured on a capture of 2000x923, brought down to 845x390. The cross
        // and the face buttons are tightened from these places, by decision.
        const layout = layoutTouchPad({ width: 845, height: 390 });
        const places = {
            lt: [49, 62],
            rt: [795, 62],
            b: [795, 195],
            l3: [49, 340],
            r3: [795, 340],
        };
        for (const [id, [x, y]] of Object.entries(places)) {
            expect(Math.abs(byId(layout, id).cx - x)).toBeLessThan(3);
            expect(Math.abs(byId(layout, id).cy - y)).toBeLessThan(3);
        }
        const stick = byId(layout, 'stick-left');
        expect(Math.abs(stick.restX - 219)).toBeLessThan(3);
        expect(Math.abs(stick.restY - 307)).toBeLessThan(3);
    });

    it('draws the cross and the face buttons a sliver apart on their diagonals', () => {
        const layout = layoutTouchPad({ width: 844, height: 390 });
        const y = byId(layout, 'y');
        const x = byId(layout, 'x');
        const between = Math.hypot(y.cx - x.cx, y.cy - x.cy) - layout.d;
        expect(between).toBeGreaterThan(0);
        expect(between).toBeLessThan(layout.d * 0.25);
        expect(byId(layout, 'dpad').w).toBeCloseTo(layout.d * 2.7);
        expect(x.cx + layout.d * 1.7).toBeCloseTo(byId(layout, 'b').cx);
    });

    it('sits the toggle between View and Menu, on top', () => {
        const wide = layoutTouchPad({ width: 844, height: 390 });
        const middle = (c) => c.y + c.h / 2;
        expect(middle(wide.toggle)).toBeCloseTo(middle(byId(wide, 'view')));
        expect(byId(wide, 'view').x + byId(wide, 'view').w).toBeLessThan(wide.toggle.x);
        expect(byId(wide, 'menu').x).toBeGreaterThan(wide.toggle.x + wide.toggle.w);
        expect(wide.toggle.y + wide.toggle.h).toBeLessThan(byId(wide, 'stick-left').restY);
    });

    describe('upright', () => {
        const layout = layoutTouchPad({ width: 390, height: 790, insets: { top: 20, bottom: 21 } });

        it('takes View, the toggle and Menu to the top of the screen', () => {
            expect(layout.toggle.y).toBe(20);
            const middle = layout.toggle.y + layout.toggle.h / 2;
            expect(byId(layout, 'view').y + byId(layout, 'view').h / 2).toBeCloseTo(middle);
            expect(byId(layout, 'menu').y + byId(layout, 'menu').h / 2).toBeCloseTo(middle);
        });

        it('brings the rest down, right above the sticks', () => {
            const a = byId(layout, 'a');
            const ringTop = byId(layout, 'stick-right').restY - layout.radius;
            expect(a.y + a.h).toBeLessThan(ringTop);
            expect(ringTop - (a.y + a.h)).toBeLessThan(layout.d * 0.5);
        });

        it('says the same on a tablet', () => {
            const tablet = layoutTouchPad({ width: 768, height: 1024 });
            expect(tablet.toggle.y).toBeCloseTo(8);
            expect(byId(tablet, 'lt').y).toBeGreaterThan(1024 / 2);
        });

        it('stays with the shoulders when the screen is not tall enough for that', () => {
            const squat = layoutTouchPad({ width: 400, height: 420 });
            expect(squat.toggle.y).toBeGreaterThan(byId(squat, 'lt').y);
        });
    });

    it('goes below the cross on its side when the shoulders leave no room', () => {
        const small = layoutTouchPad({ width: 568, height: 320, scale: 1.4 });
        const cross = byId(small, 'dpad');
        expect(small.toggle.y).toBeGreaterThan(cross.y + cross.h);
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
            ['l3', PAD.L3],
            ['r3', PAD.R3],
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
            const far = layout.d * 0.85;
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

        it('leaves the corner of the cross under the ring to the stick', () => {
            const dpad = byId(layout, 'dpad');
            const corner = [dpad.x + dpad.w - 1, dpad.y + dpad.h - 1];
            expect(buttonsAt(layout.controls, ...corner)).toBeNull();
            expect(stickAt(layout.controls, ...corner)).toBe(byId(layout, 'stick-left'));
        });

        it('widens a thin View to a finger, beyond what it draws', () => {
            const view = byId(layout, 'view');
            const [cx] = centre(view);
            expect(view.h).toBeLessThan(44);
            expect(buttonsAt(layout.controls, cx, view.y - view.slack + 0.5)).toEqual([PAD.VIEW]);
            expect(buttonsAt(layout.controls, cx, view.y + view.h + view.slack - 0.5)).toEqual([
                PAD.VIEW,
            ]);
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

describe('layoutOrientation', () => {
    it('is decided by which side is longer', () => {
        expect(layoutOrientation(1180, 820)).toBe('landscape');
        expect(layoutOrientation(820, 1180)).toBe('portrait');
    });

    it('takes a square screen as the one on its side', () => {
        expect(layoutOrientation(800, 800)).toBe('landscape');
    });
});

describe('touchPadLayouts', () => {
    it('gives a viewer who moved nothing the layout as drawn', () => {
        expect(touchPadLayouts()).toEqual({ preset: 'default', landscape: {}, portrait: {} });
        expect(touchPadLayouts(null)).toEqual(touchPadLayouts());
        expect(touchPadLayouts('nonsense')).toEqual(touchPadLayouts());
    });

    it('keeps what an editor saves, per orientation', () => {
        const saved = touchPadLayouts({
            preset: 'left',
            landscape: { a: { x: 0.9, y: 0.7, s: 1.2 } },
            portrait: { a: { x: 0.5, y: 0.6, s: 1 } },
        });
        expect(saved.preset).toBe('left');
        expect(saved.landscape.a).toEqual({ x: 0.9, y: 0.7, s: 1.2 });
        expect(saved.portrait.a).toEqual({ x: 0.5, y: 0.6, s: 1 });
    });

    it('drops what no editor could have saved', () => {
        const saved = touchPadLayouts({
            preset: 'sideways',
            landscape: {
                a: { x: 1.2, y: 0.5, s: 1 },
                b: { x: 0.5, y: -0.1, s: 1 },
                c: { x: 0.5, y: 0.5, s: 1 },
                d: { x: 0.5, y: 0.5, s: 1 },
                x: 'nonsense',
            },
            portrait: null,
        });
        expect(saved.preset).toBe('default');
        expect(saved.landscape).toEqual({});
        expect(saved.portrait).toEqual({});
    });

    it('keeps a size no editor offers at the plain one, and the place it was given', () => {
        const saved = touchPadLayouts({ landscape: { y: { x: 0.25, y: 0.5, s: 9 } } });
        expect(saved.landscape.y).toEqual({ x: 0.25, y: 0.5, s: 1 });
        expect(touchPadLayouts({ landscape: { y: { x: 0.25, y: 0.5 } } }).landscape.y.s).toBe(1);
    });

    it('names only the controls a layout may move', () => {
        const saved = touchPadLayouts({
            landscape: {
                lt: { x: 0.1, y: 0.1, s: 1 },
                'stick-left': { x: 0.2, y: 0.8, s: 1 },
                toggle: { x: 0.5, y: 0.5, s: 1 },
            },
        });
        expect(Object.keys(saved.landscape).sort()).toEqual(['lt', 'stick-left']);
    });
});

describe('a layout a viewer arranged', () => {
    const W = 1180;
    const H = 820;
    const byId = (layout, id) => layout.controls.find((c) => c.id === id);
    const centre = (c) => [
        c.cx !== undefined ? c.cx : c.x + c.w / 2,
        c.cy !== undefined ? c.cy : c.y + c.h / 2,
    ];
    const onScreen = (layout, w, h) =>
        layout.controls.every(
            (c) =>
                c.x >= -0.001 && c.y >= -0.001 && c.x + c.w <= w + 0.001 && c.y + c.h <= h + 0.001,
        );

    it('is the default layout when nothing was moved', () => {
        const plain = layoutTouchPad({ width: W, height: H });
        expect(layoutTouchPad({ width: W, height: H, preset: 'default', overrides: {} })).toEqual(
            plain,
        );
    });

    it('takes the place it was given, as a share of the layer', () => {
        const layout = layoutTouchPad({
            width: W,
            height: H,
            overrides: { a: { x: 0.5, y: 0.5, s: 1 } },
        });
        const [cx, cy] = centre(byId(layout, 'a'));
        expect(cx).toBeCloseTo(W / 2);
        expect(cy).toBeCloseTo(H / 2);
    });

    it('grows a control with the size it was given, keeping its shape', () => {
        const plain = layoutTouchPad({ width: W, height: H });
        const layout = layoutTouchPad({
            width: W,
            height: H,
            overrides: { a: { x: 0.9, y: 0.7, s: 1.3 } },
        });
        const a = byId(layout, 'a');
        expect(a.w).toBeCloseTo(byId(plain, 'a').w * 1.3);
        expect(a.h).toBeCloseTo(a.w);
        expect(a.r).toBeCloseTo(a.w * 0.62);
    });

    it('stays inside the safe areas however far it is pushed', () => {
        const insets = { top: 12, right: 47, bottom: 21, left: 47 };
        const layout = layoutTouchPad({
            width: W,
            height: H,
            insets,
            overrides: {
                a: { x: 1, y: 1, s: 1 },
                lt: { x: 0, y: 0, s: 1 },
                'stick-left': { x: 0, y: 1, s: 1 },
            },
        });
        const a = byId(layout, 'a');
        expect(a.x + a.w).toBeCloseTo(W - insets.right);
        expect(a.y + a.h).toBeCloseTo(H - insets.bottom);
        const lt = byId(layout, 'lt');
        expect(lt.x).toBeCloseTo(insets.left);
        expect(lt.y).toBeCloseTo(insets.top);
        const stick = byId(layout, 'stick-left');
        expect(stick.restX - stick.radius).toBeGreaterThanOrEqual(-0.001);
        expect(stick.restY + stick.radius).toBeLessThanOrEqual(H - insets.bottom + 0.001);
    });

    it('moves a stick ring but never its zone', () => {
        const plain = layoutTouchPad({ width: W, height: H });
        const layout = layoutTouchPad({
            width: W,
            height: H,
            overrides: { 'stick-left': { x: 0.15, y: 0.6, s: 1 } },
        });
        const zone = byId(layout, 'stick-left');
        const before = byId(plain, 'stick-left');
        expect(zone.x).toBe(before.x);
        expect(zone.w).toBe(before.w);
        expect(zone.restX).toBeCloseTo(0.15 * W);
        expect(zone.restY).toBeCloseTo(0.6 * H);
        expect(stickAt(layout.controls, zone.restX, zone.restY)).toBe(zone);
    });

    it('keeps a ring out of the middle strip, and inside its own side', () => {
        const layout = layoutTouchPad({
            width: W,
            height: H,
            overrides: { 'stick-right': { x: 0.5, y: 0.5, s: 1 } },
        });
        const zone = byId(layout, 'stick-right');
        expect(zone.restX - zone.radius).toBeGreaterThanOrEqual(zone.x - 0.001);
        expect(stickAt(layout.controls, zone.restX, zone.restY)).toBe(zone);
    });

    it('keeps the toggle between the two pills it sits between', () => {
        const layout = layoutTouchPad({
            width: W,
            height: H,
            overrides: {
                view: { x: 0.3, y: 0.2, s: 1 },
                menu: { x: 0.5, y: 0.2, s: 1 },
            },
        });
        const view = byId(layout, 'view');
        const menu = byId(layout, 'menu');
        const mid = (centre(view)[0] + centre(menu)[0]) / 2;
        expect(layout.toggle.x + layout.toggle.w / 2).toBeCloseTo(mid);
        expect(layout.toggle.y + layout.toggle.h / 2).toBeCloseTo(centre(view)[1]);
    });

    it('reads a moved dpad in eight directions where it now is', () => {
        const layout = layoutTouchPad({
            width: W,
            height: H,
            overrides: { dpad: { x: 0.5, y: 0.5, s: 1 } },
        });
        const dpad = byId(layout, 'dpad');
        const [cx, cy] = centre(dpad);
        expect(buttonsAt(layout.controls, cx, cy)).toEqual([]);
        expect(buttonsAt(layout.controls, cx, cy - dpad.w / 2 + 2)).toEqual([PAD.UP]);
        expect(buttonsAt(layout.controls, cx - dpad.w / 2 + 2, cy)).toEqual([PAD.LEFT]);
    });

    it('mirrors everything for a left-handed thumb', () => {
        const plain = layoutTouchPad({ width: W, height: H });
        const left = layoutTouchPad({ width: W, height: H, preset: 'left' });
        for (const id of ['a', 'y', 'lt', 'rb', 'view', 'l3']) {
            expect(centre(byId(left, id))[0]).toBeCloseTo(W - centre(byId(plain, id))[0]);
        }
        for (const id of ['stick-left', 'stick-right']) {
            expect(byId(left, id).restX).toBeCloseTo(W - byId(plain, id).restX);
            expect(byId(left, id).x).toBe(byId(plain, id).x);
        }
        expect(onScreen(left, W, H)).toBe(true);
    });

    it('pulls a compact layout inward and draws less', () => {
        const plain = layoutTouchPad({ width: W, height: H });
        const compact = layoutTouchPad({ width: W, height: H, preset: 'compact' });
        const a = byId(compact, 'a');
        const before = byId(plain, 'a');
        expect(a.w).toBeLessThan(before.w);
        // Toward the middle of the screen, whichever side of it the control was.
        const inward = (c) => Math.abs(centre(c)[0] - W / 2) + Math.abs(centre(c)[1] - H / 2);
        expect(inward(a)).toBeLessThan(inward(before));
        expect(onScreen(compact, W, H)).toBe(true);
    });

    it('keeps every preset on screen, on every shape, at every size', () => {
        const screens = [
            [844, 390],
            [390, 844],
            [1180, 820],
            [820, 1180],
            [568, 320],
        ];
        for (const [w, h] of screens) {
            for (const scale of [0.8, 1, 1.4]) {
                for (const preset of TOUCH_PAD_PRESETS) {
                    const layout = layoutTouchPad({ width: w, height: h, scale, preset });
                    expect(onScreen(layout, w, h)).toBe(true);
                    for (const side of ['stick-left', 'stick-right']) {
                        const c = byId(layout, side);
                        expect(c.restX - c.radius).toBeGreaterThanOrEqual(-0.001);
                        expect(c.restX + c.radius).toBeLessThanOrEqual(w + 0.001);
                        expect(c.restY + c.radius).toBeLessThanOrEqual(h + 0.001);
                    }
                }
            }
        }
    });

    it('lays itself out on the orientation it is actually in', () => {
        const saved = touchPadLayouts({
            landscape: { a: { x: 0.9, y: 0.6, s: 1 } },
            portrait: { a: { x: 0.5, y: 0.8, s: 1 } },
        });
        const wide = layoutTouchPad({
            width: 1180,
            height: 820,
            overrides: saved[layoutOrientation(1180, 820)],
        });
        const tall = layoutTouchPad({
            width: 820,
            height: 1180,
            overrides: saved[layoutOrientation(820, 1180)],
        });
        expect(centre(byId(wide, 'a'))[0]).toBeCloseTo(0.9 * 1180);
        expect(centre(byId(tall, 'a'))[1]).toBeCloseTo(0.8 * 1180);
    });
});
