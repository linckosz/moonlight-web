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

/** @typedef {import('./StreamView.js').StreamView} StreamViewInstance */

import { PenStroke, penAngles } from '../stream/PenInput.js';

/**
 * The pen on the picture, for StreamView (issue #50): what stream/PenInput.js
 * sends, from the pointer events of the input layer.
 *
 * Only once the relay says the host takes a pen (`inputcaps.pen`, a GameStream
 * host with Sunshine's pen extension), only in desktop mode, and only while
 * this viewer has the mouse. Otherwise nothing changes: the pen goes on being
 * the mouse on a desktop and a finger on a tablet.
 *
 * When it is on, the pen is taken away from the two paths that used to carry
 * it, or the host would get every stroke twice:
 *  - the mouse events the browser makes up from a pen: the mouse handlers ask
 *    _penOwnsMouseEvent(). A real mouse's events come after a pointer event of
 *    its own type, a pen's after one of the pen's, so the last pointer type
 *    seen tells them apart;
 *  - the touch events a tablet makes up from a pen as well: _touchesBesidePen()
 *    takes them out of what the trackpad's handlers see.
 */

/** Pointer events on the input layer the pen listens to. */
const PEN_EVENTS = [
    'pointerdown',
    'pointermove',
    'pointerup',
    'pointercancel',
    'pointerleave',
    'lostpointercapture',
];

/**
 * A touch whose browser does not say it is a stylus (touchType, Safari) is the
 * pen's when it starts where the pen pointer just was: the two are the same
 * contact, and the pointer event comes first.
 */
const PEN_TOUCH_PX = 2;
const PEN_POINT_MS = 100;

/**
 * Raw reports (pointerrawupdate) carry the pen's motion while they arrive; the
 * coalesced pointermove then stands down, as the mouse's does (_rawPointerLive).
 */
const PEN_RAW_LIVE_MS = 250;

export class StreamViewPen {
    /**
     * Listen to the pen on the input layer. Bound with the mouse, whatever the
     * host: until it is known to take a pen, the handler only notes which kind
     * of pointer spoke last.
     * @this {StreamViewInstance}
     */
    _bindPen() {
        this._penStroke = new PenStroke((msg) => this._sendToHost(msg));
        this._penTouchIds = new Set();
        this._penPoint = null;
        this._penRawMs = 0;
        this._lastPointerType = '';
        if (!this.inputEl) return;
        this._onPenPointer = (e) => this._handlePenPointer(e);
        for (const type of PEN_EVENTS) this.inputEl.addEventListener(type, this._onPenPointer);
        // Same switch as the mouse's raw path (mw_raw_pointer): bound after it.
        if (this._rawPointer) {
            this._onPenRaw = (e) => {
                if (e.pointerType !== 'pen') return;
                this._penRawMs = performance.now();
                this._handlePenPointer(e);
            };
            this.inputEl.addEventListener('pointerrawupdate', this._onPenRaw);
        }
    }

    /** @this {StreamViewInstance} */
    _unbindPen() {
        this._penLift();
        if (!this.inputEl) return;
        if (this._onPenPointer) {
            for (const type of PEN_EVENTS)
                this.inputEl.removeEventListener(type, this._onPenPointer);
        }
        if (this._onPenRaw) this.inputEl.removeEventListener('pointerrawupdate', this._onPenRaw);
        this._onPenPointer = null;
        this._onPenRaw = null;
    }

    /**
     * Whether the pen goes to the host as a pen right now.
     * @this {StreamViewInstance}
     */
    _penOn() {
        return (
            this._penSupported === true &&
            !this._gamingMode &&
            this._mouseAllowed !== false &&
            !this._quitting
        );
    }

    /**
     * The relay's word on the pen (inputcaps). Taken back, the pen is lifted.
     * @this {StreamViewInstance}
     * @param {boolean} supported
     */
    _setPenSupported(supported) {
        // Unset reads as "no": a host without one says nothing new.
        if ((this._penSupported === true) === supported) return;
        this._penSupported = supported;
        console.log(
            supported
                ? '[StreamView] Pen: pressure, tilt and eraser go to the host'
                : '[StreamView] Pen: drawn as the mouse, or a finger',
        );
        if (!supported) this._penLift();
    }

    /**
     * Let go of the pen: lifted if it was down, then gone from the picture.
     * @this {StreamViewInstance}
     */
    _penLift() {
        if (this._penStroke && this._penStroke.active) this._penStroke.leave();
    }

    /**
     * A mouse event the browser made up from the pen, which the mouse handlers
     * leave alone while the pen speaks for itself.
     * @this {StreamViewInstance}
     */
    _penOwnsMouseEvent() {
        return this._lastPointerType === 'pen' && this._penOn();
    }

    /**
     * @this {StreamViewInstance}
     * @param {PointerEvent} e
     */
    _handlePenPointer(e) {
        if (e.type !== 'pointerrawupdate') this._lastPointerType = e.pointerType;
        if (e.pointerType !== 'pen') return;
        this._penPoint = { x: e.clientX, y: e.clientY, t: performance.now() };
        if (!this._penOn()) return;
        const stroke = this._penStroke;
        switch (e.type) {
            case 'pointerleave':
            case 'pointercancel':
                if (stroke.pointerId === null || e.pointerId === stroke.pointerId) stroke.leave();
                return;
            case 'lostpointercapture':
                // It follows every pointerup: only a capture lost under a pen
                // still down is a pen gone.
                if (stroke.contact && e.pointerId === stroke.pointerId) stroke.leave();
                return;
            case 'pointermove':
                if (performance.now() - this._penRawMs < PEN_RAW_LIVE_MS) return;
                break;
            default:
                break;
        }
        const moving = e.type === 'pointermove' || e.type === 'pointerrawupdate';
        const coalesced =
            moving && typeof e.getCoalescedEvents === 'function' ? e.getCoalescedEvents() : [];
        let mine = false;
        for (const s of coalesced.length ? coalesced : [e]) {
            if (stroke.sample(this._penSample(s, e.pointerId))) mine = true;
        }
        if (e.type !== 'pointerdown' || !mine) return;
        // No mouse events made up from it, no text selected under it.
        e.preventDefault();
        if (stroke.contact) {
            try {
                this.inputEl.setPointerCapture(e.pointerId);
            } catch (err) {
                /* a pointer the browser no longer knows: its up is on its way */
            }
        }
    }

    /**
     * A pointer sample as PenStroke reads it: its position across the picture
     * (0 to 1 over it, outside that over the bars).
     * @this {StreamViewInstance}
     * @param {PointerEvent} s
     * @param {number} pointerId
     */
    _penSample(s, pointerId) {
        const rect = this._mediaRect();
        return {
            pointerId,
            buttons: s.buttons | 0,
            x: rect.width > 0 ? (s.clientX - rect.left) / rect.width : -1,
            y: rect.height > 0 ? (s.clientY - rect.top) / rect.height : -1,
            pressure: s.pressure,
            angles: penAngles(s),
        };
    }

    /**
     * Whether the pen held the lift-on-silence heartbeat open: a pen held
     * still on the glass sends nothing, and the host must not take that for a
     * link gone quiet.
     * @this {StreamViewInstance}
     */
    _penHeld() {
        return !!(this._penStroke && this._penStroke.contact);
    }

    /**
     * `e` as the trackpad's handlers should see it: without the pen's touches,
     * which the pen has already sent. Null when the event is the pen's alone;
     * the browser's own handling of it is then prevented here, as the trackpad
     * would have.
     *
     * Only touches that start on the input layer: a pen on a button, the stats
     * or the on-screen controller is a finger there, as before.
     * @this {StreamViewInstance}
     * @param {TouchEvent} e
     * @returns {TouchEvent | null}
     */
    _touchesBesidePen(e) {
        const ids = this._penTouchIds;
        if (!ids) return e;
        if (e.type === 'touchstart' && this._penOn()) {
            for (const t of Array.from(e.changedTouches)) {
                if (this._touchIsPen(t)) ids.add(t.identifier);
            }
        }
        if (ids.size === 0) return e;
        const changed = Array.from(e.changedTouches);
        const others = changed.filter((t) => !ids.has(t.identifier));
        const touches = Array.from(e.touches).filter((t) => !ids.has(t.identifier));
        if (e.type === 'touchend' || e.type === 'touchcancel') {
            for (const t of changed) ids.delete(t.identifier);
        }
        if (others.length === 0) {
            e.preventDefault();
            return null;
        }
        if (others.length === changed.length && touches.length === e.touches.length) return e;
        return new Proxy(e, {
            get(target, prop) {
                if (prop === 'touches') return touches;
                if (prop === 'changedTouches') return others;
                const value = target[prop];
                return typeof value === 'function' ? value.bind(target) : value;
            },
        });
    }

    /**
     * @this {StreamViewInstance}
     * @param {Touch} t
     */
    _touchIsPen(t) {
        const target = /** @type {Node} */ (/** @type {unknown} */ (t.target));
        if (!this.inputEl || !target || !this.inputEl.contains(target)) return false;
        if (/** @type {any} */ (t).touchType === 'stylus') return true;
        const p = this._penPoint;
        return (
            !!p &&
            performance.now() - p.t < PEN_POINT_MS &&
            Math.abs(t.clientX - p.x) <= PEN_TOUCH_PX &&
            Math.abs(t.clientY - p.y) <= PEN_TOUCH_PX
        );
    }
}
