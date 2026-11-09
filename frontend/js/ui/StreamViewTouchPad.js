/*
 * MoonlightWeb — browser-based Sunshine/GameStream client.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * The on-screen controller in the stream, for StreamView: when it is offered,
 * shown and plugged in. A pad and its display are two things here. Hiding it
 * leaves it plugged in, because a game that loses its controller pauses; only a
 * long press unplugs it.
 *
 * Plain prototype methods (`this` = the StreamView instance), copied onto
 * StreamView.prototype at the bottom of StreamView.js.
 */
import { t } from '../i18n/i18n.js';
import { touchEnabled, watchTouchEnabled, detectTv } from '../util/BrowserDetect.js';
import { TouchPadState } from '../stream/TouchPadState.js';
import { Icons } from './icons.js';
import { TouchPadView } from './TouchPadView.js';

/** @typedef {import('./StreamView.js').StreamView} StreamViewInstance */

/** A press of the button or the handle held this long unplugs the controller. */
const UNPLUG_HOLD_MS = 600;
/**
 * How long a blur of the keyboard's capture waits before the controller comes
 * back: a tap on the keyboard's own toolbar blurs it and refocuses at once.
 */
const KEYBOARD_GONE_MS = 200;

/** Whether a mouse or a trackpad is around, as the browser sees it. */
function hasFinePointer() {
    try {
        return !!window.matchMedia && window.matchMedia('(any-pointer: fine)').matches;
    } catch {
        return false;
    }
}

/** Whether the browser knows of any real pad, forwarded or not. */
function physicalPadPresent() {
    const pads = navigator.getGamepads ? navigator.getGamepads() : [];
    return Array.from(pads).some(Boolean);
}

export class StreamViewTouchPad {
    /**
     * What this viewer's settings say, with the defaults of a viewer who never
     * opened them.
     * @this {StreamViewInstance}
     */
    _touchPadPref(name) {
        const prefs = this._touchPadPrefs || {};
        const defaults = { mode: 'auto', scale: 1, opacity: 1, vibrate: true, outside: 'mouse' };
        return prefs[name] === undefined ? defaults[name] : prefs[name];
    }

    /**
     * Whether this viewer could ever have one, whatever the screen says right
     * now. A guest needs the invitation's gamepad; a TV never has fingers.
     * @this {StreamViewInstance}
     */
    _touchPadEligible() {
        if (this._touchPadPref('mode') === 'never' || detectTv()) return false;
        return !this._playerMode || this._gamepadAllowed === true;
    }

    /**
     * Whether it is on offer now: a touch screen the OS has on (touchEnabled),
     * asked each time, as it can be switched off during the session.
     * @this {StreamViewInstance}
     */
    _touchPadAvailable() {
        return this._touchPadEligible() && touchEnabled();
    }

    /**
     * Set the controls up for this view, and apply what the settings ask for
     * at the start. Called again once the view has its GamepadManager, which it
     * may not have yet when its picture arrives. Idempotent.
     * @this {StreamViewInstance}
     */
    _mountTouchPad() {
        if (this._standby || !this._rootEl || !this._touchPadEligible()) return;
        if (!this._stopTouchWatch) {
            this._stopTouchWatch = watchTouchEnabled((on) => this._onTouchEnabledChanged(on));
        }
        if (!this._touchPadBtn && !this._playerMode) this._mountTouchPadButton();
        if (!this._touchPadHandle) this._mountTouchPadHandle();
        this._syncTouchPad();
        if (this._gamepadManager && !this._touchPadStartQueued) {
            this._touchPadStartQueued = true;
            // After this turn: a view taking over from another is told what the
            // viewer had decided just after it activates (restoreViewState).
            queueMicrotask(() => this._startTouchPad());
        }
    }

    /**
     * A guest's existing button is their way to their pads, so only the owner
     * gets this one.
     * @this {StreamViewInstance}
     */
    _mountTouchPadButton() {
        const header = this._rootEl.querySelector('.stream-header');
        if (!header) return;
        const btn = document.createElement('button');
        btn.type = 'button';
        btn.id = 'btn-stream-touchpad';
        btn.className = 'btn-stream-pad';
        btn.innerHTML = Icons.gamepad;
        btn.title = t('stream.gamepadButton');
        btn.setAttribute('aria-label', t('stream.gamepadButton'));
        this._wireTouchPadToggle(btn);
        header.insertBefore(btn, this._rootEl.querySelector('#btn-stream-quit'));
        this._touchPadBtn = btn;
    }

    /**
     * The button's stand-in where the header's is not at hand. Beside the
     * controller it is one of its round buttons, between View and Menu, wherever
     * the stream is shown; with the controller hidden, CSS keeps it only in
     * fullscreen, where there is no header, small at the top of the picture.
     * @this {StreamViewInstance}
     */
    _mountTouchPadHandle() {
        const area = this._rootEl.querySelector('.stream-canvas-area');
        if (!area) return;
        const handle = document.createElement('button');
        handle.type = 'button';
        handle.className = 'touch-pad-handle';
        handle.innerHTML = Icons.gamepad;
        handle.title = t('stream.gamepadButton');
        handle.setAttribute('aria-label', t('stream.gamepadButton'));
        this._wireTouchPadToggle(handle);
        area.appendChild(handle);
        this._touchPadHandle = handle;
    }

    /**
     * A press shows or hides; held, it unplugs. The trackpad's touch handlers
     * leave a button alone and click it at the end of the touch.
     * @this {StreamViewInstance}
     */
    _wireTouchPadToggle(el) {
        let timer = null;
        let unplugged = false;
        const cancel = () => {
            if (timer !== null) clearTimeout(timer);
            timer = null;
        };
        el.addEventListener('pointerdown', () => {
            cancel();
            unplugged = false;
            timer = setTimeout(() => {
                timer = null;
                unplugged = true;
                this._unplugTouchPad();
            }, UNPLUG_HOLD_MS);
        });
        for (const type of ['pointerup', 'pointercancel', 'pointerleave']) {
            el.addEventListener(type, cancel);
        }
        el.addEventListener('click', (e) => {
            e.stopPropagation();
            // The click at the end of the press that has just unplugged it.
            if (unplugged) {
                unplugged = false;
                return;
            }
            this._toggleTouchPad();
        });
    }

    /**
     * The state and layer, once, for the rest of this view.
     * @this {StreamViewInstance}
     */
    _ensureTouchPad() {
        if (this._touchPadState) return;
        const area = /** @type {HTMLElement} */ (this._rootEl.querySelector('.stream-canvas-area'));
        const vibrate =
            this._touchPadPref('vibrate') && typeof navigator.vibrate === 'function'
                ? (pattern) => navigator.vibrate(pattern)
                : null;
        const poke = () => this._gamepadManager && this._gamepadManager.pokeVirtual();
        this._touchPadState = new TouchPadState({ vibrate, onLateChange: poke });
        this._touchPadView = new TouchPadView(area, this._touchPadState, {
            scale: this._touchPadPref('scale'),
            opacity: this._touchPadPref('opacity'),
            ignoreOutside: this._touchPadPref('outside') === 'ignore',
            onChanged: poke,
            // As the trackpad does for a touch of its own.
            onTouch: () => {
                if (
                    this._shortcutsSlide &&
                    this._shortcutsSlide.style.display !== 'none' &&
                    !this._shortcutsSlide.classList.contains('fading-out')
                ) {
                    this._hideShortcutsSlide();
                }
            },
            onFourFingerTap: () => this._toggleTouchPad(),
            onLaidOut: () => this._placeTouchPadHandle(),
        });
        this._touchPadView.mount();
    }

    /**
     * Plug it in. `yields` is for one the viewer did not ask for: a real pad
     * showing up takes its place.
     * @this {StreamViewInstance}
     */
    _plugTouchPad({ yields = false } = {}) {
        const gm = this._gamepadManager;
        if (!gm || gm.hasVirtual()) return;
        this._ensureTouchPad();
        this._touchPadYields = yields;
        gm.attachVirtual(this._touchPadState, { yields });
    }

    /** @this {StreamViewInstance} */
    _unplugTouchPad() {
        const gm = this._gamepadManager;
        if (this._touchPadState) this._touchPadState.releaseAll();
        if (gm) gm.detachVirtual();
        this._touchPadShown = false;
        this._syncTouchPad();
    }

    /**
     * What the settings ask for when the stream starts, or what the viewer had
     * decided on the view this one replaces.
     * @this {StreamViewInstance}
     */
    _startTouchPad() {
        // Unmounted is taken away, in the turn it took to get here.
        if (this._touchPadStarted || !this._stopTouchWatch || !this._gamepadManager) return;
        if (!this._touchPadAvailable()) return;
        this._touchPadStarted = true;
        if (this._touchPadCarry) {
            this._applyTouchPadCarry();
            return;
        }
        const mode = this._touchPadPref('mode');
        if (mode === 'always') {
            this._plugTouchPad();
        } else if (mode === 'auto' && !hasFinePointer() && !physicalPadPresent()) {
            this._plugTouchPad({ yields: true });
        } else {
            return;
        }
        // Auto plugs it in, connected for the game, and leaves it off the
        // picture until asked for: a viewer who did not choose it is not
        // greeted by controls over the stream. Always is that choice.
        this._touchPadShown = mode === 'always';
        this._syncTouchPad();
    }

    /**
     * The button, the handle and the tap of four fingers: show or hide, plugging
     * it in if it was not. A guest hiding it gives controller 0 back to their real
     * pad if there is one, as they only have the one place.
     * @this {StreamViewInstance}
     */
    _toggleTouchPad() {
        const gm = this._gamepadManager;
        if (!gm || !this._touchPadAvailable()) return;
        this._touchPadStarted = true;
        // Asked for with no real pad in play, it stands in for one, and gives
        // way when one arrives; asked for beside one, it is a second controller
        // on purpose. A guest has the one place, and keeps it (single mode).
        const yields = !this._playerMode && !gm.hasPhysicalPad();
        if (this._kbdVisible) {
            this._toggleVirtualKeyboard();
            this._plugTouchPad({ yields });
            this._touchPadShown = true;
        } else if (!gm.hasVirtual()) {
            this._plugTouchPad({ yields });
            this._touchPadShown = true;
        } else if (!this._touchPadShown) {
            this._touchPadShown = true;
        } else if (this._playerMode && physicalPadPresent()) {
            this._unplugTouchPad();
        } else {
            this._touchPadShown = false;
        }
        this._syncTouchPad();
    }

    /**
     * A guest's way to it in their controller dialog: first in the list of pads,
     * since it is one to them. The owner has a button of their own.
     * @this {StreamViewInstance}
     */
    _touchPadDialogEntry() {
        if (!this._playerMode || !this._touchPadAvailable() || !this._gamepadManager) return null;
        return {
            label: t('settings.touchPad'),
            active: this._gamepadManager.hasVirtual(),
            onPick: () => this._chooseTouchPad(),
        };
    }

    /**
     * The guest picked it: on screen now, and at their next visit.
     * @this {StreamViewInstance}
     */
    _chooseTouchPad() {
        this._touchPadStarted = true;
        this._plugTouchPad();
        this._touchPadShown = true;
        this._syncTouchPad();
        if (this._touchPadRemember) this._touchPadRemember('always');
    }

    /**
     * Put on screen what the state says. Shown only when plugged in and the
     * viewer wants it, and out of the way of the soft keyboard and of a dialog
     * listening to the pads. Everything held is let go when it goes (setVisible),
     * and the host told.
     * @this {StreamViewInstance}
     */
    _syncTouchPad() {
        const available = this._touchPadAvailable();
        const gm = this._gamepadManager;
        const plugged = !!gm && gm.hasVirtual();
        const on = available && plugged && this._touchPadShown === true;
        for (const el of [this._touchPadBtn, this._touchPadHandle]) {
            if (!el) continue;
            el.hidden = !available;
            el.classList.toggle('is-on', on);
        }
        if (!this._touchPadView) return;
        this._touchPadView.setVisible(on && !this._kbdVisible && this._padHolds.size === 0);
        this._placeTouchPadHandle();
        if (plugged) gm.pokeVirtual();
    }

    /**
     * The handle takes its place among the controller's buttons while the
     * controller is on screen, and gives it up when it goes.
     * @this {StreamViewInstance}
     */
    _placeTouchPadHandle() {
        const handle = this._touchPadHandle;
        if (!handle) return;
        const view = this._touchPadView;
        const rect = view && view.isVisible() ? view.toggleRect() : null;
        handle.classList.toggle('is-with-pad', !!rect);
        handle.style.left = rect ? rect.x + 'px' : '';
        handle.style.top = rect ? rect.y + 'px' : '';
        handle.style.width = rect ? rect.w + 'px' : '';
        handle.style.height = rect ? rect.h + 'px' : '';
        handle.style.opacity = rect ? String(this._touchPadPref('opacity')) : '';
    }

    /**
     * The OS switched the touch screen off (the controller goes, a game must not
     * be left holding a stick) or on (it is on offer again, nothing more).
     * @this {StreamViewInstance}
     */
    _onTouchEnabledChanged(on) {
        if (on) {
            this._startTouchPad();
        } else {
            this._unplugTouchPad();
        }
        this._syncTouchPad();
    }

    /**
     * A real pad took the place of a controller nobody asked for.
     * @this {StreamViewInstance}
     */
    _onTouchPadYielded() {
        this._touchPadShown = false;
        this._syncTouchPad();
    }

    /**
     * The soft keyboard opened or closed: StreamViewKeyboard reads no further
     * than this. Coming back waits a moment (KEYBOARD_GONE_MS).
     * @this {StreamViewInstance}
     */
    _touchPadKeyboardChanged() {
        if (!this._touchPadView) return;
        if (this._touchPadKbdTimer) clearTimeout(this._touchPadKbdTimer);
        this._touchPadKbdTimer = null;
        if (this._kbdVisible) {
            this._syncTouchPad();
        } else {
            this._touchPadKbdTimer = setTimeout(() => {
                this._touchPadKbdTimer = null;
                this._syncTouchPad();
            }, KEYBOARD_GONE_MS);
        }
    }

    /**
     * The page went to the background: no end will come for the fingers on it.
     * @this {StreamViewInstance}
     */
    _touchPadLetGo() {
        if (!this._touchPadView) return;
        this._touchPadView.releaseAll();
        if (this._gamepadManager) this._gamepadManager.pokeVirtual();
    }

    /**
     * `e` as the trackpad's handlers should see it (TouchPadView.withoutOwnFingers).
     * @this {StreamViewInstance}
     */
    _touchesBesidePad(e) {
        return this._touchPadView ? this._touchPadView.withoutOwnFingers(e) : e;
    }

    /**
     * What the viewer decided, for the view that replaces this one.
     * @this {StreamViewInstance}
     */
    _touchPadSnapshot() {
        if (!this._touchPadStarted || !this._gamepadManager) return null;
        return {
            plugged: this._gamepadManager.hasVirtual(),
            shown: this._touchPadShown === true,
            yields: this._touchPadYields === true,
        };
    }

    /**
     * Adopt a replaced view's _touchPadSnapshot: a quality change is no reason
     * to bring back a controller the viewer put away, or to lose one in use.
     * @this {StreamViewInstance}
     */
    _adoptTouchPad(carry) {
        if (!carry) return;
        this._touchPadCarry = carry;
        if (this._touchPadStarted) this._applyTouchPadCarry();
    }

    /** @this {StreamViewInstance} */
    _applyTouchPadCarry() {
        const carry = this._touchPadCarry;
        this._touchPadCarry = null;
        if (carry.plugged) {
            this._plugTouchPad({ yields: carry.yields });
            this._touchPadShown = carry.shown;
            this._syncTouchPad();
        } else {
            this._unplugTouchPad();
        }
    }

    /**
     * Take it all away with the view. Its manager tells the host on its own.
     * @this {StreamViewInstance}
     */
    _destroyTouchPad() {
        if (this._stopTouchWatch) this._stopTouchWatch();
        this._stopTouchWatch = null;
        if (this._touchPadKbdTimer) clearTimeout(this._touchPadKbdTimer);
        this._touchPadKbdTimer = null;
        if (this._touchPadView) this._touchPadView.destroy();
        this._touchPadView = null;
        this._touchPadState = null;
        if (this._gamepadManager) this._gamepadManager.detachVirtual();
        for (const el of [this._touchPadBtn, this._touchPadHandle]) {
            if (el) el.remove();
        }
        this._touchPadBtn = null;
        this._touchPadHandle = null;
        this._touchPadStarted = false;
        this._touchPadStartQueued = false;
        this._touchPadShown = false;
    }
}
