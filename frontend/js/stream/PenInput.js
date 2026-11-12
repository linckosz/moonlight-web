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
 * A pen as a pen (issue #50): its pressure, its tilt, its eraser and its barrel
 * buttons reach a host that takes them — Sunshine's pen extension, which Wolf
 * speaks too. The relay says so in `inputcaps.pen`; until it does, a pen stays
 * what it always was here, the mouse on a desktop and a finger on a tablet.
 *
 * Pointer Events describe a pen sample by sample, each with the buttons held.
 * The GameStream pen packet describes a pen that comes over the picture, goes
 * down, moves, lifts and goes away. PenStroke turns the first into the second,
 * from the buttons of each sample alone, so it never depends on which event
 * carried a change: a tip that touches while the barrel button is already held
 * arrives as a pointermove, not a pointerdown.
 *
 * Pure: no DOM, no clock. The view (StreamViewPen.js) feeds it.
 */

/** PointerEvent.buttons: the tip touching, and the eraser end touching. */
const TIP = 1;
const ERASER = 32;
/** PointerEvent.buttons: the barrel button, and a second one some pens have. */
const BARREL = 2;
const BARREL_2 = 4;

/** The pen packet's button bits (LI_PEN_BUTTON_PRIMARY / _SECONDARY). */
export const PEN_BUTTON_PRIMARY = 0x01;
export const PEN_BUTTON_SECONDARY = 0x02;

/**
 * A pen that reports a pressure of zero while touching: the lightest touch it
 * can measure, not "unknown" — which is what zero means in the pen packet.
 * Sent as the smallest step a host keeps (1/1024 on Windows).
 */
export const MIN_CONTACT_PRESSURE = 0.001;

const DEG = Math.PI / 180;

const clamp01 = (v) => (v < 0 ? 0 : v > 1 ? 1 : v);
/** Whole degrees in 0..359; never -0, which atan2 gives for a pen leaning straight at the viewer. */
const deg360 = (v) => ((Math.round(v) % 360) + 360) % 360 || 0;
const round = (v, digits) => {
    const k = 10 ** digits;
    return Math.round(v * k) / k;
};

/**
 * The tilt the pen packet carries, from a pointer sample: how far the pen leans
 * from upright (`tilt`, 0 to 90 degrees) and the way its tip points on the
 * screen (`rotation`, 0 to 359 degrees, clockwise from the top of the screen —
 * a pen leaning towards the viewer points up, 0). That is how Sunshine and Wolf
 * read the two fields back into the tilt along x and y that Windows and Linux
 * want.
 *
 * Pointer Events give the same angles twice: tiltX/tiltY, the lean along each
 * axis, and altitudeAngle/azimuthAngle, which newer browsers add. tiltX/tiltY
 * first, since every browser fills them. Both at zero is ambiguous — a pen
 * held upright, or one that reports no tilt — and the answer is then
 * "unknown" (null) unless the altitude says it leans: a host defaults an
 * unknown tilt to upright anyway.
 *
 * @param {{tiltX?: number, tiltY?: number, altitudeAngle?: number, azimuthAngle?: number}} s
 * @returns {{tilt: number, rotation: number} | null}
 */
export function penAngles(s) {
    const tx = Number(s.tiltX) || 0;
    const ty = Number(s.tiltY) || 0;
    if (tx === 0 && ty === 0) {
        const alt = s.altitudeAngle;
        const az = s.azimuthAngle;
        if (
            typeof alt !== 'number' ||
            typeof az !== 'number' ||
            !Number.isFinite(alt) ||
            !Number.isFinite(az) ||
            alt < 0 ||
            alt >= Math.PI / 2 - 1e-3
        ) {
            return null;
        }
        // The azimuth is where the pen's cap is, clockwise from three
        // o'clock; its tip points the other way, a quarter turn back from
        // the top of the screen.
        return { tilt: Math.round(90 - alt / DEG), rotation: deg360(az / DEG - 90) };
    }
    const cx = Math.max(-90, Math.min(90, tx));
    const cy = Math.max(-90, Math.min(90, ty));
    const a = Math.tan(cx * DEG);
    const b = Math.tan(cy * DEG);
    const tilt = Math.atan(Math.hypot(a, b)) / DEG;
    // The cap leans along (a, b) — right and towards the viewer for positive
    // values; the tip points along (-a, -b). Its angle clockwise from the top
    // of the screen, y growing downwards.
    return { tilt: Math.min(90, Math.round(tilt)), rotation: deg360(Math.atan2(-a, b) / DEG) };
}

/**
 * The pen packet's buttons, from PointerEvent.buttons: the barrel button is
 * the pen's primary button, a second one its secondary. The tip and the eraser
 * are not buttons there — they are the event itself (down, move, up) and the
 * tool.
 * @param {number} buttons
 */
export function penButtons(buttons) {
    let b = 0;
    if (buttons & BARREL) b |= PEN_BUTTON_PRIMARY;
    if (buttons & BARREL_2) b |= PEN_BUTTON_SECONDARY;
    return b;
}

/**
 * @typedef {object} PenSample
 * @property {number} pointerId
 * @property {number} buttons   PointerEvent.buttons
 * @property {number} x         across the picture, 0 to 1 over it
 * @property {number} y         down the picture, 0 to 1 over it
 * @property {number} pressure  PointerEvent.pressure, 0 to 1
 * @property {{tilt: number, rotation: number} | null} angles  penAngles()
 */

/**
 * One pen, as the host was told about it.
 *
 * A stroke starts only over the picture — a tip on the bars around it does
 * nothing, as a click there does nothing — and, once started, follows the pen
 * wherever it goes, held to the picture's edge. A pen hovering off the picture
 * has left it. One pen at a time: the packet has no pointer id, so a second
 * one is ignored while the first is over the picture.
 */
export class PenStroke {
    /** @param {(msg: object) => void} send */
    constructor(send) {
        this._send = send;
        this.reset();
    }

    reset() {
        /** The pen being followed, or null. */
        this.pointerId = null;
        /** The host was told the pen is down (down, then moves, no up yet). */
        this.contact = false;
        /** The host was told the pen is over the picture (no leave since). */
        this.inRange = false;
        /** The stroke's end of the pen. */
        this.tool = 'pen';
        this._x = 0;
        this._y = 0;
    }

    /** Whether the host knows of this pen at all: down, or hovering. */
    get active() {
        return this.contact || this.inRange;
    }

    /**
     * One sample of a pen.
     * @param {PenSample} s
     * @returns {boolean} whether it was this pen's: the caller then keeps the
     *   browser's own handling of it away (the mouse events it would make up).
     */
    sample(s) {
        if (this.pointerId !== null && s.pointerId !== this.pointerId) return false;
        const over = s.x >= 0 && s.x <= 1 && s.y >= 0 && s.y <= 1;
        const touching = (s.buttons & (TIP | ERASER)) !== 0;
        const x = clamp01(s.x);
        const y = clamp01(s.y);

        if (this.contact) {
            if (touching) {
                this._emit('move', s, x, y);
                return true;
            }
            this._emit('up', s, x, y);
            this.contact = false;
            return true;
        }
        if (touching) {
            if (!over) return false;
            this.pointerId = s.pointerId;
            this.tool = s.buttons & ERASER ? 'eraser' : 'pen';
            this.contact = true;
            this.inRange = true;
            this._emit('down', s, x, y);
            return true;
        }
        if (!over) {
            if (this.inRange) this.leave();
            return false;
        }
        this.pointerId = s.pointerId;
        this.inRange = true;
        this._emit('hover', s, x, y);
        return true;
    }

    /**
     * The pen is gone: out of range, off the picture, or the page let go of it
     * (focus lost, tab hidden, gaming mode, the stream ending). A pen still
     * down is lifted first, where it last was, so no stroke is left open.
     */
    leave() {
        if (this.contact) this._emit('up', null, this._x, this._y);
        if (this.active) {
            this._send({ type: 'pen', event: 'leave', x: round(this._x, 5), y: round(this._y, 5) });
        }
        this.reset();
    }

    /**
     * @param {'down'|'move'|'up'|'hover'} event
     * @param {PenSample | null} s  null for the up of leave()
     * @param {number} x
     * @param {number} y
     */
    _emit(event, s, x, y) {
        this._x = x;
        this._y = y;
        const msg = {
            type: 'pen',
            event,
            x: round(x, 5),
            y: round(y, 5),
            buttons: s ? penButtons(s.buttons) : 0,
        };
        if (event !== 'hover') msg.tool = this.tool;
        if (event === 'down' || event === 'move') {
            const p = Number(s.pressure);
            msg.pressure = Math.max(
                MIN_CONTACT_PRESSURE,
                round(clamp01(Number.isFinite(p) ? p : 0), 4),
            );
        }
        if (s && s.angles) {
            msg.tilt = s.angles.tilt;
            msg.rotation = s.angles.rotation;
        }
        this._send(msg);
    }
}
