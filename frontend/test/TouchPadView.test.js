/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';
import { TouchPadView } from '../js/ui/TouchPadView.js';
import { PAD, TouchPadState, layoutTouchPad } from '../js/stream/TouchPadState.js';

const W = 844;
const H = 390;
const layout = layoutTouchPad({ width: W, height: H });
const control = (id) => layout.controls.find((c) => c.id === id);
const centre = (id) => {
    const c = control(id);
    return [c.x + c.w / 2, c.y + c.h / 2];
};

/** A real touch event's surface: what is down now, and what just changed. */
function fire(target, type, changed, all) {
    const touch = ([identifier, clientX, clientY]) => ({ identifier, clientX, clientY });
    const e = new Event(type, { bubbles: true, cancelable: true });
    e.changedTouches = changed.map(touch);
    e.touches = all.map(touch);
    target.dispatchEvent(e);
    return e;
}

describe('TouchPadView', () => {
    let parent;
    let state;
    let view;
    let onChanged;
    let onTouch;
    let onFourFingerTap;
    let el;
    // What is down, by identifier.
    let down;

    const start = (id, x, y) => {
        down.set(id, [id, x, y]);
        return fire(el, 'touchstart', [[id, x, y]], [...down.values()]);
    };
    const move = (id, x, y) => {
        down.set(id, [id, x, y]);
        return fire(el, 'touchmove', [[id, x, y]], [...down.values()]);
    };
    const end = (id, type = 'touchend') => {
        const t = down.get(id);
        down.delete(id);
        return fire(el, type, [t], [...down.values()]);
    };

    function build(options = {}) {
        state = new TouchPadState({ vibrate: options.vibrate || null });
        onChanged = vi.fn();
        onTouch = vi.fn();
        onFourFingerTap = vi.fn();
        view = new TouchPadView(parent, state, {
            onChanged,
            onTouch,
            onFourFingerTap,
            ...options,
        });
        view.mount();
        el = parent.querySelector('.touch-pad');
        el.getBoundingClientRect = () => ({ left: 0, top: 0, width: W, height: H });
        view.setVisible(true);
        onChanged.mockClear();
    }

    beforeEach(() => {
        parent = document.createElement('div');
        document.body.appendChild(parent);
        down = new Map();
        build();
    });
    afterEach(() => {
        parent.remove();
        vi.useRealTimers();
    });

    it('builds hidden, once, and shows every control when told', () => {
        const again = new TouchPadView(parent, state);
        again.mount();
        again.mount();
        const mine = parent.querySelectorAll('.touch-pad');
        expect(mine.length).toBe(2);
        expect(mine[1].hidden).toBe(true);
        again.destroy();
        expect(view.isVisible()).toBe(true);
        const ids = [
            'a',
            'b',
            'x',
            'y',
            'lb',
            'lt',
            'rb',
            'rt',
            'view',
            'menu',
            'dpad',
            'l3',
            'r3',
        ];
        for (const id of ids) {
            expect(el.querySelector(`[data-control="${id}"]`)).not.toBeNull();
        }
        expect(el.querySelectorAll('.touch-pad-zone').length).toBe(2);
        expect(el.querySelectorAll('.touch-pad-ring').length).toBe(2);
    });

    it('takes the scale and the opacity', () => {
        view.destroy();
        build({ scale: 1.2, opacity: 0.3 });
        expect(el.style.getPropertyValue('--touch-pad-opacity')).toBe('0.3');
        const a = el.querySelector('[data-control="a"]');
        const big = layoutTouchPad({ width: W, height: H, scale: 1.2 }).d;
        expect(parseFloat(a.style.width)).toBeCloseTo(big);
    });

    it('gives the toggle its place, and says when the controls have moved', () => {
        expect(view.toggleRect()).toEqual(layout.toggle);
        view.destroy();
        const onLaidOut = vi.fn();
        build({ onLaidOut });
        expect(onLaidOut).toHaveBeenCalledTimes(1);
        view.relayout();
        expect(onLaidOut).toHaveBeenCalledTimes(1);
    });

    it('is at full opacity unless told otherwise', () => {
        expect(el.style.getPropertyValue('--touch-pad-opacity')).toBe('1');
    });

    it('draws L3 and R3 as buttons, as held as the double tap', () => {
        start(1, ...centre('l3'));
        expect(el.querySelector('[data-control="l3"]').classList.contains('is-down')).toBe(true);
        expect(state.buttons[PAD.L3].pressed).toBe(true);
        end(1);
        expect(state.buttons[PAD.L3].pressed).toBe(false);
    });

    it('takes the rest of the screen only when told to', () => {
        expect(el.classList.contains('is-ignoring')).toBe(false);
        view.destroy();
        build({ ignoreOutside: true });
        expect(el.classList.contains('is-ignoring')).toBe(true);
    });

    it('stops every touch at the layer, so the trackpad never sees it', () => {
        const below = vi.fn();
        parent.addEventListener('touchstart', below);
        parent.addEventListener('touchmove', below);
        parent.addEventListener('touchend', below);
        const events = [start(1, ...centre('a')), move(1, ...centre('b')), end(1)];
        expect(below).not.toHaveBeenCalled();
        for (const e of events) expect(e.defaultPrevented).toBe(true);
    });

    it('presses and lets go of a button, and says so once per event', () => {
        start(1, ...centre('a'));
        expect(state.buttons[PAD.A].pressed).toBe(true);
        expect(onChanged).toHaveBeenCalledTimes(1);
        expect(onTouch).toHaveBeenCalledTimes(1);
        expect(el.querySelector('[data-control="a"]').classList.contains('is-down')).toBe(true);
        end(1);
        expect(state.buttons[PAD.A].pressed).toBe(false);
        expect(onChanged).toHaveBeenCalledTimes(2);
        expect(el.querySelector('[data-control="a"]').classList.contains('is-down')).toBe(false);
    });

    it('holds as many buttons as there are fingers', () => {
        start(1, ...centre('a'));
        start(2, ...centre('rt'));
        start(3, ...centre('lb'));
        expect(state.buttons[PAD.A].pressed).toBe(true);
        expect(state.buttons[PAD.RT].pressed).toBe(true);
        expect(state.buttons[PAD.LB].pressed).toBe(true);
        end(2);
        expect(state.buttons[PAD.RT].pressed).toBe(false);
        expect(state.buttons[PAD.A].pressed).toBe(true);
    });

    it('slides from one button to the next, with one state to send', () => {
        start(1, ...centre('a'));
        onChanged.mockClear();
        move(1, ...centre('b'));
        expect(state.buttons[PAD.A].pressed).toBe(false);
        expect(state.buttons[PAD.B].pressed).toBe(true);
        expect(onChanged).toHaveBeenCalledTimes(1);
    });

    it('lets go when sliding off into nothing, and presses again on the way back', () => {
        start(1, ...centre('a'));
        move(1, W / 2, H / 2);
        expect(state.buttons[PAD.A].pressed).toBe(false);
        move(1, ...centre('x'));
        expect(state.buttons[PAD.X].pressed).toBe(true);
        end(1);
        expect(state.buttons.every((b) => !b.pressed)).toBe(true);
    });

    it('keeps a button another finger is on when one slides away', () => {
        start(1, ...centre('a'));
        start(2, ...centre('a'));
        move(1, ...centre('b'));
        expect(state.buttons[PAD.A].pressed).toBe(true);
        end(2);
        expect(state.buttons[PAD.A].pressed).toBe(false);
    });

    it('goes round the cross through its diagonals', () => {
        const [cx, cy] = centre('dpad');
        const far = layout.d * 0.85;
        start(1, cx, cy - far);
        expect(state.buttons[PAD.UP].pressed).toBe(true);
        move(1, cx + far, cy - far);
        expect(state.buttons[PAD.UP].pressed).toBe(true);
        expect(state.buttons[PAD.RIGHT].pressed).toBe(true);
        move(1, cx + far, cy);
        expect(state.buttons[PAD.UP].pressed).toBe(false);
        expect(state.buttons[PAD.RIGHT].pressed).toBe(true);
        move(1, cx, cy);
        expect(state.buttons[PAD.RIGHT].pressed).toBe(false);
        expect(el.querySelector('.touch-pad-dpad-arm.arm-up').classList.contains('is-down')).toBe(
            false,
        );
    });

    it('floats a stick under the finger, one per side, each with its own finger', () => {
        const left = control('stick-left');
        const right = control('stick-right');
        start(1, left.restX, left.restY);
        start(2, right.restX, right.restY);
        move(1, left.restX + layout.radius, left.restY);
        move(2, right.restX, right.restY - layout.radius);
        expect(state.axes[0]).toBeCloseTo(1);
        expect(state.axes[3]).toBeCloseTo(-1);
        const ring = el.querySelector('.touch-pad-ring.is-held');
        expect(ring).not.toBeNull();
        end(1);
        expect(state.axes[0]).toBe(0);
        expect(state.axes[3]).toBeCloseTo(-1);
        end(2, 'touchcancel');
        expect(state.axes).toEqual([0, 0, 0, 0]);
        expect(el.querySelector('.touch-pad-ring.is-held')).toBeNull();
    });

    it("gives a second finger in a held stick's zone no second stick", () => {
        const left = control('stick-left');
        start(1, left.restX, left.restY);
        start(2, left.restX + 40, left.restY);
        move(2, left.restX + 40 + layout.radius, left.restY);
        expect(state.axes[0]).toBe(0);
        end(2);
        expect(state.stickView('left')).not.toBeNull();
    });

    it("lets a stick's finger cross buttons without pressing them", () => {
        const left = control('stick-left');
        start(1, left.restX, left.restY);
        move(1, ...centre('lt'));
        expect(state.buttons[PAD.LT].pressed).toBe(false);
        expect(state.stickView('left')).not.toBeNull();
    });

    it('never starts a stick from a finger sliding off a button', () => {
        const left = control('stick-left');
        start(1, ...centre('lt'));
        move(1, left.restX, left.restY);
        expect(state.buttons[PAD.LT].pressed).toBe(false);
        expect(state.stickView('left')).toBeNull();
    });

    it("double taps a stick's zone into its click", () => {
        const left = control('stick-left');
        start(1, left.restX, left.restY);
        end(1);
        start(2, left.restX, left.restY);
        expect(state.buttons[PAD.L3].pressed).toBe(true);
        end(2);
        expect(state.buttons[PAD.L3].pressed).toBe(false);
    });

    it('lets go on a cancelled touch', () => {
        start(1, ...centre('a'));
        end(1, 'touchcancel');
        expect(state.buttons[PAD.A].pressed).toBe(false);
    });

    it('lets go of a finger whose end never came, as soon as it can tell', () => {
        start(1, ...centre('a'));
        // The system took finger 1 away; the next event no longer lists it.
        down.delete(1);
        start(2, ...centre('b'));
        expect(state.buttons[PAD.A].pressed).toBe(false);
        expect(state.buttons[PAD.B].pressed).toBe(true);
    });

    it('lets go of everything when hidden, and again when laid out anew', () => {
        start(1, ...centre('a'));
        view.setVisible(false);
        expect(state.buttons[PAD.A].pressed).toBe(false);
        expect(view.isVisible()).toBe(false);
        // The finger's end arrives after the fact: nothing to let go of.
        end(1);
        view.setVisible(true);
        start(2, ...centre('b'));
        el.getBoundingClientRect = () => ({ left: 0, top: 0, width: H, height: W });
        view.relayout();
        expect(state.buttons[PAD.B].pressed).toBe(false);
        expect(onChanged).toHaveBeenCalled();
    });

    it('keeps a finger down when asked to lay out for the same screen', () => {
        start(1, ...centre('a'));
        view.relayout();
        expect(state.buttons[PAD.A].pressed).toBe(true);
    });

    it("hit-tests in the layer's own pixels, wherever it has moved to", () => {
        el.getBoundingClientRect = () => ({ left: 100, top: 50, width: W, height: H });
        const [x, y] = centre('a');
        start(1, x + 100, y + 50);
        expect(state.buttons[PAD.A].pressed).toBe(true);
    });

    it('stays out of the notch', () => {
        const spy = vi.spyOn(window, 'getComputedStyle').mockReturnValue({
            paddingTop: '0px',
            paddingRight: '47px',
            paddingBottom: '21px',
            paddingLeft: '47px',
        });
        view.relayout();
        expect(parseFloat(el.querySelector('[data-control="lt"]').style.left)).toBe(47);
        spy.mockRestore();
    });

    it('does nothing with a layer of no size', () => {
        el.getBoundingClientRect = () => ({ left: 0, top: 0, width: 0, height: 0 });
        expect(() => view.relayout()).not.toThrow();
    });

    describe('a tap of four fingers', () => {
        const tap = (n) => {
            for (let i = 1; i <= n; i++) start(i, 400 + i, 10);
            for (let i = 1; i <= n; i++) end(i);
        };

        it('is told, once', () => {
            tap(4);
            expect(onFourFingerTap).toHaveBeenCalledTimes(1);
        });

        it('is not three fingers, a cancelled touch, or a long hold', () => {
            tap(3);
            for (let i = 1; i <= 4; i++) start(i, 400 + i, 10);
            for (let i = 1; i <= 4; i++) end(i, 'touchcancel');
            let now = 0;
            const clock = vi.spyOn(performance, 'now').mockImplementation(() => now);
            for (let i = 1; i <= 4; i++) start(i, 400 + i, 10);
            now = 1000;
            for (let i = 1; i <= 4; i++) end(i);
            clock.mockRestore();
            expect(onFourFingerTap).not.toHaveBeenCalled();
        });
    });

    describe('withoutOwnFingers', () => {
        const event = (touches) => ({
            touches,
            changedTouches: [touches[0]],
            type: 'touchmove',
            preventDefault: vi.fn(function () {
                return this;
            }),
        });

        it('hands the event as it is when no finger is here', () => {
            const e = event([{ identifier: 9 }]);
            expect(view.withoutOwnFingers(e)).toBe(e);
        });

        it("takes this layer's fingers out of the touches, and nothing else", () => {
            start(1, ...centre('a'));
            start(2, ...centre('b'));
            const e = event([{ identifier: 1 }, { identifier: 9 }, { identifier: 2 }]);
            const seen = view.withoutOwnFingers(e);
            expect(seen.touches.map((t) => t.identifier)).toEqual([9]);
            expect(seen.touches.length).toBe(1);
            expect(seen.type).toBe('touchmove');
            expect(seen.changedTouches).toBe(e.changedTouches);
            seen.preventDefault();
            expect(e.preventDefault).toHaveBeenCalledTimes(1);
            expect(e.preventDefault.mock.contexts[0]).toBe(e);
        });
    });

    describe('haptics', () => {
        it('ticks on a press, a slide to another button and a stick, never on a lift', () => {
            view.destroy();
            const vibrate = vi.fn();
            build({ vibrate });
            start(1, ...centre('a'));
            expect(vibrate).toHaveBeenCalledTimes(1);
            move(1, ...centre('b'));
            expect(vibrate).toHaveBeenCalledTimes(2);
            end(1);
            expect(vibrate).toHaveBeenCalledTimes(2);
            const left = control('stick-left');
            start(2, left.restX, left.restY);
            expect(vibrate).toHaveBeenCalledTimes(3);
        });
    });

    it('takes itself away, holding nothing', () => {
        start(1, ...centre('a'));
        view.destroy();
        expect(parent.querySelector('.touch-pad')).toBeNull();
        expect(state.buttons[PAD.A].pressed).toBe(false);
        expect(view.isVisible()).toBe(false);
        expect(() => view.destroy()).not.toThrow();
        expect(() => view.setVisible(true)).not.toThrow();
    });
});
