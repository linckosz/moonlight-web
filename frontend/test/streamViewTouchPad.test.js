/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';
import { StreamViewTouchPad } from '../js/ui/StreamViewTouchPad.js';
import { StreamViewTouch } from '../js/ui/StreamViewTouch.js';
import { GamepadManager } from '../js/stream/GamepadManager.js';
import { PAD } from '../js/stream/TouchPadState.js';

const HTML = `
    <div class="stream-header"><button id="btn-stream-quit"></button></div>
    <div class="stream-canvas-area"></div>`;

const sent = (send, type) => send.mock.calls.map((c) => c[0]).filter((m) => m.type === type);
const flush = () => new Promise((resolve) => queueMicrotask(resolve));

describe('the on-screen controller in the stream', () => {
    let send;
    let pads;
    let media;
    let queries;

    /** A view made of the mixin's methods, a real manager and the DOM it reads. */
    function makeView(fields = {}, { started = true } = {}) {
        const root = document.createElement('div');
        root.innerHTML = HTML;
        document.body.appendChild(root);
        const view = Object.create(StreamViewTouchPad.prototype);
        const single = fields._playerMode === true;
        const gm = new GamepadManager(send, {
            single,
            onVirtualYield: () => view._onTouchPadYielded(),
        });
        Object.assign(view, {
            _rootEl: root,
            _gamepadManager: gm,
            _padHolds: new Set(),
            _playerMode: false,
            _gamepadAllowed: false,
            _kbdVisible: false,
            _standby: false,
            _touchPadPrefs: null,
            _toggleVirtualKeyboard: vi.fn(function () {
                view._kbdVisible = !view._kbdVisible;
            }),
            _hideShortcutsSlide: vi.fn(),
            ...fields,
        });
        if (started) gm.start();
        return view;
    }

    const padEl = (view) => view._rootEl.querySelector('.touch-pad');
    const shown = (view) => !!padEl(view) && !padEl(view).hidden;
    const button = (view) => view._rootEl.querySelector('#btn-stream-touchpad');
    const handle = (view) => view._rootEl.querySelector('.touch-pad-handle');

    function setCoarse(value) {
        media['(any-pointer: coarse)'] = value;
        for (const fn of queries['(any-pointer: coarse)'] || []) fn();
    }

    beforeEach(() => {
        send = vi.fn();
        pads = [];
        media = { '(any-pointer: coarse)': true, '(any-pointer: fine)': false };
        queries = {};
        Object.defineProperty(navigator, 'maxTouchPoints', { value: 5, configurable: true });
        Object.defineProperty(navigator, 'getGamepads', {
            value: () => pads,
            configurable: true,
            writable: true,
        });
        Object.defineProperty(navigator, 'userAgent', {
            value: 'Mozilla/5.0 (iPhone; CPU iPhone OS 17_0 like Mac OS X) Mobile',
            configurable: true,
        });
        window.matchMedia = (q) => ({
            get matches() {
                return media[q] === true;
            },
            addEventListener: (_type, fn) => {
                (queries[q] = queries[q] || new Set()).add(fn);
            },
            removeEventListener: (_type, fn) => queries[q] && queries[q].delete(fn),
        });
        vi.stubGlobal('requestAnimationFrame', () => 1);
        vi.stubGlobal('cancelAnimationFrame', vi.fn());
    });
    afterEach(() => {
        document.body.innerHTML = '';
        delete window.matchMedia;
        delete navigator.maxTouchPoints;
        delete navigator.userAgent;
        vi.unstubAllGlobals();
        vi.useRealTimers();
    });

    describe('what is on offer', () => {
        it("gives the owner a button and a handle, with the guests' gamepad icon", () => {
            const view = makeView();
            view._mountTouchPad();
            expect(button(view).querySelector('svg')).not.toBeNull();
            expect(button(view).nextElementSibling.id).toBe('btn-stream-quit');
            expect(handle(view).parentElement.className).toBe('stream-canvas-area');
            view._mountTouchPad();
            expect(view._rootEl.querySelectorAll('#btn-stream-touchpad').length).toBe(1);
            expect(view._rootEl.querySelectorAll('.touch-pad-handle').length).toBe(1);
        });

        it('gives a guest no second button: they have theirs', () => {
            const view = makeView({ _playerMode: true, _gamepadAllowed: true });
            view._mountTouchPad();
            expect(button(view)).toBeNull();
            expect(handle(view)).not.toBeNull();
        });

        it.each([
            ['a guest not invited to play', { _playerMode: true, _gamepadAllowed: false }],
            ['a viewer who said never', { _touchPadPrefs: { mode: 'never' } }],
            ['a view on standby', { _standby: true }],
        ])('offers nothing to %s', async (_who, fields) => {
            const view = makeView(fields);
            view._mountTouchPad();
            await flush();
            expect(button(view)).toBeNull();
            expect(handle(view)).toBeNull();
            expect(send).not.toHaveBeenCalled();
        });

        it('offers nothing on a TV, even with a touch screen reported', () => {
            Object.defineProperty(navigator, 'userAgent', {
                value: 'Mozilla/5.0 (Linux; Android 11; MiTV) Chrome',
                configurable: true,
            });
            const view = makeView();
            view._mountTouchPad();
            expect(button(view)).toBeNull();
        });

        it('hides the controls while the OS has the screen off, and shows them after', async () => {
            media['(any-pointer: coarse)'] = false;
            const view = makeView();
            view._mountTouchPad();
            await flush();
            expect(button(view).hidden).toBe(true);
            expect(handle(view).hidden).toBe(true);
            expect(send).not.toHaveBeenCalled();
            setCoarse(true);
            expect(button(view).hidden).toBe(false);
        });

        it('unplugs when the OS takes the screen away mid-game, and offers it back', async () => {
            const view = makeView();
            view._mountTouchPad();
            await flush();
            button(view).click();
            expect(shown(view)).toBe(true);
            setCoarse(false);
            expect(shown(view)).toBe(false);
            expect(view._gamepadManager.hasVirtual()).toBe(false);
            expect(sent(send, 'gamepaddisconnect').length).toBe(1);
            expect(button(view).hidden).toBe(true);
            setCoarse(true);
            expect(button(view).hidden).toBe(false);
            expect(view._gamepadManager.hasVirtual()).toBe(false);
        });
    });

    describe('the handle beside the controller', () => {
        const sized = (view) => {
            view._ensureTouchPad();
            padEl(view).getBoundingClientRect = () => ({
                left: 0,
                top: 0,
                width: 844,
                height: 390,
            });
        };

        it('joins its buttons while it is on screen, and goes back when it hides', () => {
            const view = makeView();
            view._mountTouchPad();
            sized(view);
            view._toggleTouchPad();
            expect(shown(view)).toBe(true);
            const rect = view._touchPadView.toggleRect();
            expect(handle(view).classList.contains('is-with-pad')).toBe(true);
            expect(handle(view).style.left).toBe(rect.x + 'px');
            expect(handle(view).style.top).toBe(rect.y + 'px');
            expect(handle(view).style.width).toBe(rect.w + 'px');
            view._toggleTouchPad();
            expect(shown(view)).toBe(false);
            expect(handle(view).classList.contains('is-with-pad')).toBe(false);
            expect(handle(view).style.left).toBe('');
            expect(handle(view).style.opacity).toBe('');
        });

        it("takes the controller's opacity", () => {
            const view = makeView({ _touchPadPrefs: { opacity: 0.4 } });
            view._mountTouchPad();
            sized(view);
            view._toggleTouchPad();
            expect(handle(view).style.opacity).toBe('0.4');
        });

        it('leaves with the controller when the keyboard opens', () => {
            const view = makeView();
            view._mountTouchPad();
            sized(view);
            view._toggleTouchPad();
            view._kbdVisible = true;
            view._syncTouchPad();
            expect(handle(view).classList.contains('is-with-pad')).toBe(false);
        });
    });

    describe('at the start', () => {
        it('Auto: plugged in with nothing else to play with, on screen at the first press', async () => {
            const view = makeView();
            view._mountTouchPad();
            await flush();
            expect(view._gamepadManager.hasVirtual()).toBe(true);
            expect(sent(send, 'gamepadconnect').length).toBe(1);
            expect(shown(view)).toBe(false);
            expect(button(view).classList.contains('is-on')).toBe(false);
            button(view).click();
            expect(shown(view)).toBe(true);
            expect(button(view).classList.contains('is-on')).toBe(true);
            // Shown, not plugged in a second time.
            expect(sent(send, 'gamepadconnect').length).toBe(1);
        });

        it('Auto: stays away from a viewer with a mouse or a trackpad', async () => {
            media['(any-pointer: fine)'] = true;
            const view = makeView();
            view._mountTouchPad();
            await flush();
            expect(shown(view)).toBe(false);
            expect(send).not.toHaveBeenCalled();
            expect(button(view).hidden).toBe(false);
        });

        it('Auto: stays away from a viewer with a real pad', async () => {
            pads = [{ index: 0, id: 'Xbox', mapping: 'standard', buttons: [], axes: [] }];
            const view = makeView();
            view._mountTouchPad();
            await flush();
            expect(view._gamepadManager.hasVirtual()).toBe(false);
        });

        it('Auto: gives its place to a real pad arriving, which gets controller 0', async () => {
            let frame = null;
            vi.stubGlobal('requestAnimationFrame', (cb) => {
                frame = cb;
                return 1;
            });
            const view = makeView();
            view._mountTouchPad();
            await flush();
            pads = [{ index: 0, id: 'Xbox', mapping: 'standard', buttons: [], axes: [] }];
            frame();
            expect(view._gamepadManager.hasVirtual()).toBe(false);
            expect(shown(view)).toBe(false);
            expect(button(view).classList.contains('is-on')).toBe(false);
            expect(sent(send, 'gamepaddisconnect').map((m) => m.index)).toEqual([0]);
            expect(sent(send, 'gamepadconnect').map((m) => m.index)).toEqual([0, 0]);
        });

        it('Always: shown even with a mouse, and stays beside a real pad', async () => {
            media['(any-pointer: fine)'] = true;
            let frame = null;
            vi.stubGlobal('requestAnimationFrame', (cb) => {
                frame = cb;
                return 1;
            });
            const view = makeView({ _touchPadPrefs: { mode: 'always' } });
            view._mountTouchPad();
            await flush();
            expect(shown(view)).toBe(true);
            pads = [{ index: 0, id: 'Xbox', mapping: 'standard', buttons: [], axes: [] }];
            frame();
            expect(view._gamepadManager.hasVirtual()).toBe(true);
            expect(sent(send, 'gamepadconnect').map((m) => m.index)).toEqual([0, 1]);
        });

        it('waits for a manager, and is told again when it has one', async () => {
            const gm = new GamepadManager(send);
            const view = makeView({ _gamepadManager: null }, { started: false });
            view._mountTouchPad();
            await flush();
            expect(button(view)).not.toBeNull();
            view._gamepadManager = gm;
            gm.start();
            view._mountTouchPad();
            await flush();
            expect(gm.hasVirtual()).toBe(true);
        });

        it('does not start for a view taken away in the meantime', async () => {
            const view = makeView();
            view._mountTouchPad();
            view._destroyTouchPad();
            await flush();
            expect(send).not.toHaveBeenCalled();
            expect(padEl(view)).toBeNull();
        });

        it("takes the viewer's settings", async () => {
            const view = makeView({
                _touchPadPrefs: { scale: 1.2, opacity: 0.3, outside: 'ignore', vibrate: false },
            });
            navigator.vibrate = vi.fn();
            view._mountTouchPad();
            await flush();
            expect(padEl(view).style.getPropertyValue('--touch-pad-opacity')).toBe('0.3');
            expect(padEl(view).classList.contains('is-ignoring')).toBe(true);
            expect(view._touchPadState.vibrationActuator).toBeNull();
            delete navigator.vibrate;
        });

        it('presents motors where the phone has one and the viewer wants them', async () => {
            navigator.vibrate = vi.fn();
            const view = makeView();
            view._mountTouchPad();
            await flush();
            expect(sent(send, 'gamepadconnect')[0].rumble).toBe(true);
            view._touchPadState.tapHaptic();
            expect(navigator.vibrate).toHaveBeenCalledWith(10);
            delete navigator.vibrate;
        });
    });

    describe('the button, the handle and the tap of four fingers', () => {
        async function plain() {
            media['(any-pointer: fine)'] = true;
            const view = makeView();
            view._mountTouchPad();
            await flush();
            return view;
        }

        it('plugs in and shows, hides without unplugging, and shows again', async () => {
            const view = await plain();
            button(view).click();
            expect(shown(view)).toBe(true);
            expect(sent(send, 'gamepadconnect').length).toBe(1);
            button(view).click();
            expect(shown(view)).toBe(false);
            expect(view._gamepadManager.hasVirtual()).toBe(true);
            expect(sent(send, 'gamepaddisconnect').length).toBe(0);
            expect(button(view).classList.contains('is-on')).toBe(false);
            handle(view).click();
            expect(shown(view)).toBe(true);
            expect(sent(send, 'gamepadconnect').length).toBe(1);
        });

        it('unplugs when held, and the click at the end does nothing more', async () => {
            vi.useFakeTimers({ toFake: ['setTimeout', 'clearTimeout'] });
            const view = await plain();
            button(view).click();
            button(view).dispatchEvent(new Event('pointerdown'));
            vi.advanceTimersByTime(600);
            expect(view._gamepadManager.hasVirtual()).toBe(false);
            expect(shown(view)).toBe(false);
            expect(sent(send, 'gamepaddisconnect').length).toBe(1);
            button(view).dispatchEvent(new Event('pointerup'));
            button(view).click();
            expect(view._gamepadManager.hasVirtual()).toBe(false);
            // The next press is an ordinary one.
            button(view).click();
            expect(view._gamepadManager.hasVirtual()).toBe(true);
        });

        it('does not unplug on a short press', async () => {
            vi.useFakeTimers({ toFake: ['setTimeout', 'clearTimeout'] });
            const view = await plain();
            button(view).click();
            button(view).dispatchEvent(new Event('pointerdown'));
            vi.advanceTimersByTime(100);
            button(view).dispatchEvent(new Event('pointerup'));
            vi.advanceTimersByTime(1000);
            expect(view._gamepadManager.hasVirtual()).toBe(true);
        });

        /** The frame loop, run by hand: a real pad is seen at the next frame. */
        function frames() {
            const loop = { next: () => {} };
            vi.stubGlobal('requestAnimationFrame', (cb) => {
                loop.next = cb;
                return 1;
            });
            return loop;
        }
        const realPad = { index: 0, id: 'Xbox', mapping: 'standard', buttons: [], axes: [] };

        it('plugged in by a press with no real pad, gives its place to one arriving', async () => {
            const loop = frames();
            const view = await plain();
            button(view).click();
            expect(view._gamepadManager.hasVirtual()).toBe(true);
            pads = [realPad];
            loop.next();
            expect(view._gamepadManager.hasVirtual()).toBe(false);
            expect(shown(view)).toBe(false);
            expect(button(view).classList.contains('is-on')).toBe(false);
            // The real pad is controller 0, not player 2.
            expect(sent(send, 'gamepadconnect').map((m) => m.index)).toEqual([0, 0]);
        });

        it('pressed for beside a real pad, is a second controller that stays', async () => {
            const loop = frames();
            pads = [realPad];
            const view = await plain();
            loop.next();
            expect(view._gamepadManager.hasPhysicalPad()).toBe(true);
            button(view).click();
            loop.next();
            expect(view._gamepadManager.hasVirtual()).toBe(true);
            expect(shown(view)).toBe(true);
            expect(sent(send, 'gamepadconnect').map((m) => m.index)).toEqual([0, 1]);
        });

        it('comes back as the second controller once a real pad took its place', async () => {
            const loop = frames();
            const view = makeView();
            view._mountTouchPad();
            await flush();
            pads = [realPad];
            loop.next();
            expect(view._gamepadManager.hasVirtual()).toBe(false);
            button(view).click();
            loop.next();
            expect(view._gamepadManager.hasVirtual()).toBe(true);
            expect(shown(view)).toBe(true);
            expect(sent(send, 'gamepadconnect').map((m) => m.index)).toEqual([0, 0, 1]);
        });

        it('leaves the trackpad out of a click on them', async () => {
            const view = await plain();
            const below = vi.fn();
            view._rootEl.addEventListener('click', below);
            button(view).click();
            expect(below).not.toHaveBeenCalled();
        });

        it('takes four fingers in the trackpad for the controller, three for the keyboard', () => {
            const end = (fingers, available) => {
                const v = {
                    _touchMaxFingers: fingers,
                    _touchMoved: false,
                    _touchStartTime: 1,
                    _touchStartX: 0,
                    _touchStartY: 0,
                    _touchTapThreshold: 8,
                    _touchTapTimeThreshold: 250,
                    _clearLongPress: vi.fn(),
                    _touchPadAvailable: () => available,
                    _toggleTouchPad: vi.fn(),
                    _toggleVirtualKeyboard: vi.fn(),
                    _startScrollMomentum: vi.fn(),
                    _scrollSamples: [],
                    _panSamples: [],
                };
                const clock = vi.spyOn(performance, 'now').mockReturnValue(100);
                StreamViewTouch.prototype.handleTouchEnd.call(v, {
                    type: 'touchend',
                    target: { closest: () => null },
                    touches: [],
                    changedTouches: [{ clientX: 0, clientY: 0 }],
                    preventDefault() {},
                });
                clock.mockRestore();
                return v;
            };
            expect(end(4, true)._toggleTouchPad).toHaveBeenCalledTimes(1);
            expect(end(4, true)._toggleVirtualKeyboard).not.toHaveBeenCalled();
            // Without a controller to offer, four fingers stay what they were.
            expect(end(4, false)._toggleVirtualKeyboard).toHaveBeenCalledTimes(1);
            expect(end(3, true)._toggleVirtualKeyboard).toHaveBeenCalledTimes(1);
            expect(end(3, true)._toggleTouchPad).not.toHaveBeenCalled();
        });

        it('takes a tap of four fingers on the layer itself', async () => {
            const view = await plain();
            button(view).click();
            view._touchPadView._onFourFingerTap();
            expect(shown(view)).toBe(false);
        });
    });

    describe('a guest choosing it in their dialog', () => {
        const guest = (fields = {}) =>
            makeView({ _playerMode: true, _gamepadAllowed: true, ...fields });

        it('offers it to a guest with fingers and the invitation, to nobody else', () => {
            const owner = makeView();
            expect(owner._touchPadDialogEntry()).toBeNull();
            expect(guest({ _gamepadAllowed: false })._touchPadDialogEntry()).toBeNull();
            const view = guest();
            expect(view._touchPadDialogEntry().active).toBe(false);
            media['(any-pointer: coarse)'] = false;
            expect(view._touchPadDialogEntry()).toBeNull();
        });

        it('says it is the one in use', () => {
            const view = guest();
            view._mountTouchPad();
            view._toggleTouchPad();
            expect(view._touchPadDialogEntry().active).toBe(true);
        });

        it('plugs it in, shows it and remembers, when picked', () => {
            const remember = vi.fn();
            const view = guest({ _touchPadRemember: remember });
            view._mountTouchPad();
            view._touchPadDialogEntry().onPick();
            expect(view._gamepadManager.hasVirtual()).toBe(true);
            expect(shown(view)).toBe(true);
            expect(remember).toHaveBeenCalledWith('always');
        });
    });

    describe('a guest', () => {
        const guest = { _playerMode: true, _gamepadAllowed: true };
        const realPad = { index: 2, id: 'Xbox', mapping: 'standard', buttons: [], axes: [] };

        it('plays as controller 0', async () => {
            const view = makeView(guest);
            view._mountTouchPad();
            await flush();
            expect(sent(send, 'gamepadconnect').map((m) => m.index)).toEqual([0]);
        });

        it('hiding it with no real pad leaves it plugged in', async () => {
            const view = makeView(guest);
            view._mountTouchPad();
            await flush();
            view._toggleTouchPad();
            expect(shown(view)).toBe(true);
            view._toggleTouchPad();
            expect(shown(view)).toBe(false);
            expect(view._gamepadManager.hasVirtual()).toBe(true);
        });

        it('hiding it gives the place back to their real pad', async () => {
            let frame = null;
            vi.stubGlobal('requestAnimationFrame', (cb) => {
                frame = cb;
                return 1;
            });
            const view = makeView({ ...guest, _touchPadPrefs: { mode: 'always' } });
            view._mountTouchPad();
            await flush();
            pads = [realPad];
            frame();
            expect(view._gamepadManager.hasPhysicalPad()).toBe(false);
            view._toggleTouchPad();
            expect(view._gamepadManager.hasVirtual()).toBe(false);
            frame();
            expect(view._gamepadManager.hasPhysicalPad()).toBe(true);
        });
    });

    describe('out of the way', () => {
        async function running() {
            const view = makeView();
            view._mountTouchPad();
            await flush();
            view._toggleTouchPad();
            return view;
        }

        it('of the soft keyboard, letting go of everything, and back when it closes', async () => {
            vi.useFakeTimers({ toFake: ['setTimeout', 'clearTimeout'] });
            const view = await running();
            view._touchPadState.press(PAD.A);
            view._gamepadManager.pokeVirtual();
            view._kbdVisible = true;
            view._touchPadKeyboardChanged();
            expect(shown(view)).toBe(false);
            expect(view._touchPadState.buttons[PAD.A].pressed).toBe(false);
            expect(sent(send, 'gamepad').at(-1).buttons).toBe(0);
            expect(view._gamepadManager.hasVirtual()).toBe(true);
            view._kbdVisible = false;
            view._touchPadKeyboardChanged();
            expect(shown(view)).toBe(false);
            vi.advanceTimersByTime(200);
            expect(shown(view)).toBe(true);
        });

        it("does not flash back when a tap on the keyboard's bar refocuses it", async () => {
            vi.useFakeTimers({ toFake: ['setTimeout', 'clearTimeout'] });
            const view = await running();
            view._kbdVisible = true;
            view._touchPadKeyboardChanged();
            view._kbdVisible = false;
            view._touchPadKeyboardChanged();
            vi.advanceTimersByTime(50);
            view._kbdVisible = true;
            view._touchPadKeyboardChanged();
            vi.advanceTimersByTime(1000);
            expect(shown(view)).toBe(false);
        });

        it('closes the keyboard when asked for while it is up', async () => {
            media['(any-pointer: fine)'] = true;
            const view = makeView();
            view._mountTouchPad();
            await flush();
            view._kbdVisible = true;
            view._toggleTouchPad();
            expect(view._toggleVirtualKeyboard).toHaveBeenCalledTimes(1);
            expect(view._gamepadManager.hasVirtual()).toBe(true);
            expect(shown(view)).toBe(true);
        });

        it('of a dialog listening to the pads', async () => {
            const view = await running();
            view._padHolds.add('remap');
            view._syncTouchPad();
            expect(shown(view)).toBe(false);
            view._padHolds.delete('remap');
            view._syncTouchPad();
            expect(shown(view)).toBe(true);
        });

        it('of the page going to the background: every held control lets go', async () => {
            const view = await running();
            view._touchPadState.press(PAD.RT);
            view._touchPadState.stickDown('left', 0, 0, 50, 0);
            view._touchPadState.stickMove('left', 50, 0);
            view._gamepadManager.pokeVirtual();
            view._touchPadLetGo();
            expect(sent(send, 'gamepad').at(-1)).toMatchObject({ buttons: 0, rt: 0, lx: 0 });
            expect(shown(view)).toBe(true);
        });

        it('of the gesture hint, which a touch dismisses as any other', async () => {
            const view = await running();
            view._shortcutsSlide = document.createElement('div');
            view._touchPadView._onTouch();
            expect(view._hideShortcutsSlide).toHaveBeenCalledTimes(1);
            view._shortcutsSlide.style.display = 'none';
            view._touchPadView._onTouch();
            expect(view._hideShortcutsSlide).toHaveBeenCalledTimes(1);
        });
    });

    describe('from one view to the next (a quality change)', () => {
        it('keeps it in use', async () => {
            const old = makeView();
            old._mountTouchPad();
            await flush();
            old._toggleTouchPad();
            const carry = old._touchPadSnapshot();
            expect(carry).toEqual({ plugged: true, shown: true, yields: true });
            const next = makeView();
            next._mountTouchPad();
            next._adoptTouchPad(carry);
            await flush();
            expect(shown(next)).toBe(true);
            expect(next._touchPadYields).toBe(true);
        });

        it('keeps it hidden but plugged in', async () => {
            const old = makeView();
            old._mountTouchPad();
            await flush();
            // Auto starts it so, until a press.
            expect(shown(old)).toBe(false);
            const next = makeView();
            next._mountTouchPad();
            next._adoptTouchPad(old._touchPadSnapshot());
            await flush();
            expect(shown(next)).toBe(false);
            expect(next._gamepadManager.hasVirtual()).toBe(true);
        });

        it('does not bring back one put away, not even for a moment', async () => {
            const old = makeView();
            old._mountTouchPad();
            await flush();
            old._unplugTouchPad();
            send.mockClear();
            const next = makeView();
            next._mountTouchPad();
            next._adoptTouchPad(old._touchPadSnapshot());
            await flush();
            expect(next._gamepadManager.hasVirtual()).toBe(false);
            expect(send).not.toHaveBeenCalled();
        });

        it('adopts what comes after its own start too', async () => {
            const next = makeView();
            next._mountTouchPad();
            await flush();
            expect(next._gamepadManager.hasVirtual()).toBe(true);
            next._adoptTouchPad({ plugged: false, shown: false, yields: false });
            expect(next._gamepadManager.hasVirtual()).toBe(false);
            next._adoptTouchPad(null);
            expect(next._gamepadManager.hasVirtual()).toBe(false);
        });

        it('has nothing to carry before it started', () => {
            const view = makeView();
            expect(view._touchPadSnapshot()).toBeNull();
        });
    });

    it('leaves nothing behind with the view', async () => {
        const view = makeView();
        view._mountTouchPad();
        await flush();
        view._destroyTouchPad();
        expect(padEl(view)).toBeNull();
        expect(button(view)).toBeNull();
        expect(handle(view)).toBeNull();
        expect(view._gamepadManager.hasVirtual()).toBe(false);
        expect(sent(send, 'gamepaddisconnect').length).toBe(1);
        expect(queries['(any-pointer: coarse)'].size).toBe(0);
        expect(() => view._destroyTouchPad()).not.toThrow();
        expect(() => view._touchPadLetGo()).not.toThrow();
        expect(() => view._touchPadKeyboardChanged()).not.toThrow();
    });

    it('takes its own fingers out of what the trackpad sees', async () => {
        const view = makeView();
        const e = { touches: [] };
        expect(view._touchesBesidePad(e)).toBe(e);
        view._mountTouchPad();
        await flush();
        expect(view._touchesBesidePad(e)).toBe(e);
    });
});
