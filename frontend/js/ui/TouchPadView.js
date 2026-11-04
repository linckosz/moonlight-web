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
 * TouchPadView — the on-screen controller's layer: draws what layoutTouchPad
 * lays out, and turns fingers into what TouchPadState holds.
 *
 * Each finger is followed by its identifier, on one stick or over some buttons.
 * A touch event stays with the element it began on, however far the finger
 * goes, so a finger is hit-tested against the layout itself: that is what lets
 * it slide from A to B, or round the cross through its diagonals.
 *
 * No touch that begins here reaches the stream's trackpad: every event stops at
 * this layer. The rest of the screen is the trackpad's, unless the layer is told
 * to take it too (`ignoreOutside`). The stream still sees those fingers in the
 * touches of its own events, hence withoutOwnFingers.
 */
import { PAD, buttonsAt, layoutTouchPad, stickAt } from '../stream/TouchPadState.js';

/** Fingers down, at most this long, that are a tap of the whole hand. */
const FOUR_FINGER_TAP_MS = 600;

const LABELS = {
    lt: 'LT',
    lb: 'LB',
    rt: 'RT',
    rb: 'RB',
    a: 'A',
    b: 'B',
    x: 'X',
    y: 'Y',
    l3: 'L3',
    r3: 'R3',
    view: '⧉',
    menu: '≡',
};

/** The cross's keys: the button each draws, its place and its arrow. */
const DPAD_ARMS = [
    [PAD.UP, 'up', '▲'],
    [PAD.DOWN, 'down', '▼'],
    [PAD.LEFT, 'left', '◀'],
    [PAD.RIGHT, 'right', '▶'],
];

function place(el, x, y, w, h) {
    el.style.left = x + 'px';
    el.style.top = y + 'px';
    el.style.width = w + 'px';
    el.style.height = h + 'px';
}

export class TouchPadView {
    /**
     * @param {HTMLElement} parent the positioned element it covers.
     * @param {import('../stream/TouchPadState.js').TouchPadState} state
     * @param {{ scale?: number, opacity?: number, ignoreOutside?: boolean,
     *           onChanged?: () => void, onTouch?: () => void,
     *           onFourFingerTap?: () => void, onLaidOut?: () => void }} [options]
     *   `onChanged` is told when a touch event changed what is held, once that
     *   event is fully handled, so a slide from A to B is one state to send.
     *   `onTouch` is told of any finger coming down here.
     *   `onFourFingerTap`: a tap of four fingers, most of them landing here when
     *   the controller covers the screen.
     *   `onLaidOut`: the controls have just moved, and with them toggleRect.
     */
    constructor(parent, state, options = {}) {
        this._parent = parent;
        this._state = state;
        this._scale = options.scale || 1;
        this._opacity = options.opacity === undefined ? 1 : options.opacity;
        this._ignoreOutside = options.ignoreOutside === true;
        this._onChanged = options.onChanged || null;
        this._onTouch = options.onTouch || null;
        this._onFourFingerTap = options.onFourFingerTap || null;
        this._onLaidOut = options.onLaidOut || null;
        // Touch.identifier → { buttons: number[], side: 'left'|'right'|null }
        this._fingers = new Map();
        this._gestureAt = 0;
        this._gestureMax = 0;
        this._controls = [];
        this._toggle = null;
        this._rect = null;
        // What the layout was last made for (see relayout).
        this._laidOutFor = '';
        // Button index → the element that draws it.
        this._buttonEls = new Map();
        this._rings = {};
        this._el = null;
        this._resizeObserver = null;
        this._onEvent = (e) => this._handle(e);
    }

    /** Build the layer, hidden. Idempotent. */
    mount() {
        if (this._el) return;
        const el = document.createElement('div');
        el.className = 'touch-pad';
        if (this._ignoreOutside) el.classList.add('is-ignoring');
        el.setAttribute('aria-hidden', 'true');
        el.hidden = true;
        el.style.setProperty('--touch-pad-opacity', String(this._opacity));
        // What the notch and the home bar take, as the browser reckons it.
        this._insetsProbe = document.createElement('div');
        this._insetsProbe.className = 'touch-pad-insets';
        el.appendChild(this._insetsProbe);
        for (const type of ['touchstart', 'touchmove', 'touchend', 'touchcancel']) {
            el.addEventListener(type, this._onEvent, { passive: false });
        }
        this._el = el;
        this._parent.appendChild(el);
        if (typeof ResizeObserver === 'function') {
            this._resizeObserver = new ResizeObserver(() => this.relayout());
            this._resizeObserver.observe(el);
        }
    }

    /** Take the layer away, with everything it was holding let go. */
    destroy() {
        if (!this._el) return;
        this.releaseAll();
        if (this._resizeObserver) this._resizeObserver.disconnect();
        this._resizeObserver = null;
        this._el.remove();
        this._el = null;
    }

    isVisible() {
        return !!this._el && !this._el.hidden;
    }

    /** Showing lays it out for the screen as it is now; hiding lets go of everything. */
    setVisible(visible) {
        if (!this._el || visible === this.isVisible()) return;
        if (!visible) this.releaseAll();
        this._el.hidden = !visible;
        if (visible) this.relayout();
    }

    /**
     * Where the button that hides the controller goes, in the layer's pixels; null
     * before the layer first has a size.
     */
    toggleRect() {
        return this._toggle;
    }

    /** Let go of every control: its fingers are forgotten until they come down again. */
    releaseAll() {
        const held = this._fingers.size > 0;
        this._fingers.clear();
        this._state.releaseAll();
        this._render();
        return held;
    }

    /**
     * `e` as the stream's trackpad should see it: without the fingers on this
     * layer among its touches, or two thumbs on the sticks and a third finger
     * on the picture would read as a three-finger gesture.
     * @param {TouchEvent} e
     */
    withoutOwnFingers(e) {
        if (this._fingers.size === 0) return e;
        const touches = Array.from(e.touches).filter((t) => !this._fingers.has(t.identifier));
        return new Proxy(e, {
            get(target, prop) {
                if (prop === 'touches') return touches;
                const value = target[prop];
                return typeof value === 'function' ? value.bind(target) : value;
            },
        });
    }

    _insets() {
        const style = getComputedStyle(this._insetsProbe);
        const px = (v) => parseFloat(v) || 0;
        return {
            top: px(style.paddingTop),
            right: px(style.paddingRight),
            bottom: px(style.paddingBottom),
            left: px(style.paddingLeft),
        };
    }

    /** Lay the controls out for the layer's size, and redraw them. */
    relayout() {
        if (!this.isVisible()) return;
        this._rect = this._el.getBoundingClientRect();
        const { width, height } = this._rect;
        if (!width || !height) return;
        const insets = this._insets();
        // Asked more often than anything changes, and a finger may be down.
        const laidOutFor = JSON.stringify([width, height, insets]);
        if (laidOutFor === this._laidOutFor) return;
        this._laidOutFor = laidOutFor;
        // A rotation under a finger: the controls move, the finger does not.
        this.releaseAll();
        const layout = layoutTouchPad({ width, height, scale: this._scale, insets });
        this._controls = layout.controls;
        this._toggle = layout.toggle;
        this._draw(layout);
        if (this._onLaidOut) this._onLaidOut();
        if (this._onChanged) this._onChanged();
    }

    _draw(layout) {
        for (const child of Array.from(this._el.children)) {
            if (child !== this._insetsProbe) child.remove();
        }
        this._buttonEls.clear();
        this._rings = {};
        this._el.style.fontSize = Math.round(layout.d * 0.3) + 'px';
        for (const c of layout.controls) {
            if (c.kind === 'stick') this._drawStick(c, layout);
            else if (c.kind === 'dpad') this._drawDpad(c);
            else this._drawButton(c);
        }
    }

    _drawButton(c) {
        const el = document.createElement('div');
        el.className = 'touch-pad-btn' + (c.r ? ' is-round' : '') + (c.pill ? ' is-pill' : '');
        el.dataset.control = c.id;
        el.textContent = LABELS[c.id];
        place(el, c.x, c.y, c.w, c.h);
        this._el.appendChild(el);
        for (const b of c.buttons) this._buttonEls.set(b, el);
    }

    _drawDpad(c) {
        const el = document.createElement('div');
        el.className = 'touch-pad-dpad';
        el.dataset.control = c.id;
        place(el, c.x, c.y, c.w, c.h);
        for (const [button, name, arrow] of DPAD_ARMS) {
            const arm = document.createElement('div');
            arm.className = 'touch-pad-dpad-arm arm-' + name;
            arm.textContent = arrow;
            el.appendChild(arm);
            this._buttonEls.set(button, arm);
        }
        this._el.appendChild(el);
    }

    _drawStick(c, layout) {
        // The zone goes under the buttons: it takes what they leave.
        const zone = document.createElement('div');
        zone.className = 'touch-pad-zone';
        zone.dataset.control = c.id;
        place(zone, c.x, c.y, c.w, c.h);
        this._el.insertBefore(zone, this._el.firstChild);
        const size = layout.radius * 2;
        const ring = document.createElement('div');
        ring.className = 'touch-pad-ring';
        place(ring, 0, 0, size, size);
        const inner = document.createElement('div');
        inner.className = 'touch-pad-ring-inner';
        ring.appendChild(inner);
        const knob = document.createElement('div');
        knob.className = 'touch-pad-knob';
        ring.appendChild(knob);
        this._el.appendChild(ring);
        this._rings[c.side] = { ring, knob, control: c, radius: layout.radius };
        this._drawRing(c.side);
    }

    _drawRing(side) {
        const r = this._rings[side];
        if (!r) return;
        const view = this._state.stickView(side);
        const x = view ? view.x : r.control.restX;
        const y = view ? view.y : r.control.restY;
        r.ring.style.transform = `translate(${x - r.radius}px, ${y - r.radius}px)`;
        r.ring.classList.toggle('is-held', !!view);
        const kx = view ? view.kx - x : 0;
        const ky = view ? view.ky - y : 0;
        r.knob.style.transform = `translate(calc(-50% + ${kx}px), calc(-50% + ${ky}px))`;
    }

    /** Draw what the fingers are holding. */
    _render() {
        if (!this._el) return;
        const held = new Set();
        for (const finger of this._fingers.values()) {
            for (const b of finger.buttons) held.add(b);
        }
        for (const [button, el] of this._buttonEls) {
            el.classList.toggle('is-down', held.has(button));
        }
        for (const side of Object.keys(this._rings)) this._drawRing(side);
    }

    /** Fingers whose end never came (a system gesture took them): let go of them now. */
    _forgetLost(touches) {
        const alive = new Set(Array.from(touches).map((t) => t.identifier));
        for (const id of Array.from(this._fingers.keys())) {
            if (!alive.has(id)) this._lift(id);
        }
    }

    _lift(id) {
        const finger = this._fingers.get(id);
        if (!finger) return;
        this._fingers.delete(id);
        if (finger.side) this._state.stickUp(finger.side);
        for (const b of finger.buttons) this._state.release(b);
    }

    /** Move this finger's hold onto `buttons`; true if it pressed anything new. */
    _holdButtons(finger, buttons) {
        let pressed = false;
        for (const b of buttons) {
            if (!finger.buttons.includes(b)) {
                this._state.press(b);
                pressed = true;
            }
        }
        for (const b of finger.buttons) {
            if (!buttons.includes(b)) this._state.release(b);
        }
        finger.buttons = buttons;
        return pressed;
    }

    _down(touch, x, y) {
        const finger = { buttons: [], side: null };
        this._fingers.set(touch.identifier, finger);
        const buttons = buttonsAt(this._controls, x, y);
        if (buttons) {
            if (this._holdButtons(finger, buttons)) this._state.tapHaptic();
            return;
        }
        const zone = stickAt(this._controls, x, y);
        if (zone && this._state.stickDown(zone.side, x, y, zone.radius)) {
            finger.side = zone.side;
            this._state.tapHaptic();
        }
    }

    _move(touch, x, y) {
        const finger = this._fingers.get(touch.identifier);
        if (!finger) return;
        if (finger.side) {
            this._state.stickMove(finger.side, x, y);
            return;
        }
        // Over buttons, or on its way to some. Never onto a stick: sliding off a
        // button into the zone around it must not grab one.
        const buttons = buttonsAt(this._controls, x, y) || [];
        if (this._holdButtons(finger, buttons)) this._state.tapHaptic();
    }

    _handle(e) {
        e.preventDefault();
        e.stopPropagation();
        // A page moved by the OS (iOS's viewport) moves the layer with no resize.
        if (e.type === 'touchstart') this._rect = this._el.getBoundingClientRect();
        if (!this._rect) return;
        const local = (t) => [t.clientX - this._rect.left, t.clientY - this._rect.top];
        const changed = Array.from(e.changedTouches);
        const now = performance.now();
        if (e.type === 'touchstart') {
            this._forgetLost(e.touches);
            if (this._fingers.size === 0) {
                this._gestureAt = now;
                this._gestureMax = 0;
            }
            for (const t of changed) this._down(t, ...local(t));
            this._gestureMax = Math.max(this._gestureMax, this._fingers.size);
            if (this._onTouch) this._onTouch();
        } else if (e.type === 'touchmove') {
            for (const t of changed) this._move(t, ...local(t));
        } else {
            for (const t of changed) this._lift(t.identifier);
            this._forgetLost(e.touches);
            if (
                e.type === 'touchend' &&
                this._fingers.size === 0 &&
                this._gestureMax >= 4 &&
                now - this._gestureAt < FOUR_FINGER_TAP_MS &&
                this._onFourFingerTap
            ) {
                this._gestureMax = 0;
                this._onFourFingerTap();
            }
        }
        this._render();
        if (this._onChanged) this._onChanged();
    }
}
