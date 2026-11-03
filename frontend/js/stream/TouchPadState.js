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
 * TouchPadState — what the on-screen controller is holding, with no DOM: its
 * buttons and sticks as a Gamepad-shaped object GamepadManager reads like any
 * pad (attachVirtual), the maths of its floating sticks, and where its controls
 * sit on a screen of a given size.
 *
 * Fingers are TouchPadView's business; this is told what they do. Nothing here
 * sends: the view asks GamepadManager to read once its touch event is handled,
 * and onLateChange covers what happens with no finger moving (a long press).
 */

/** The name a browser pad would show, and what its key is made from. */
export const TOUCH_PAD_ID = 'MoonlightWeb Touch Controller';

/** W3C standard button indices. */
export const PAD = {
    A: 0,
    B: 1,
    X: 2,
    Y: 3,
    LB: 4,
    RB: 5,
    LT: 6,
    RT: 7,
    VIEW: 8,
    MENU: 9,
    L3: 10,
    R3: 11,
    UP: 12,
    DOWN: 13,
    LEFT: 14,
    RIGHT: 15,
    GUIDE: 16,
};

/**
 * Inside this share of the radius a stick reads centred: a thumb resting on the
 * glass is never perfectly still, and a game would see it walk.
 */
const DEAD_ZONE = 0.08;
/** Menu held this long is the Guide button, and no Start reaches the game. */
const GUIDE_HOLD_MS = 500;
/** A tap on Menu is pressed for this long: a game reads pads, it must see it. */
const START_PULSE_MS = 80;
/** A touch on a stick that ends this soon and this close to where it began is a tap. */
const TAP_MS = 250;
const TAP_MOVE_PX = 16;
/** The second touch of a double tap comes this soon after the first ended. */
const DOUBLE_TAP_MS = 300;

/** Rumble slices: a phone's motor is on or off, so a level is how much of the time it is on. */
const RUMBLE_PERIOD_MS = 50;
const RUMBLE_MIN_ON_MS = 10;

/**
 * The on/off pattern for a motor level over `duration` ms. Full level is one
 * long buzz; anything less, pulses, so a faint engine rumble does not shake the
 * phone like an explosion.
 * @param {number} duration
 * @param {number} level 0..1
 * @returns {number|number[]} what navigator.vibrate takes
 */
export function rumblePattern(duration, level) {
    if (level >= 0.9) return duration;
    const on = Math.max(RUMBLE_MIN_ON_MS, Math.round(RUMBLE_PERIOD_MS * level));
    const off = RUMBLE_PERIOD_MS - on;
    const pattern = [];
    for (let t = 0; t < duration; t += RUMBLE_PERIOD_MS) pattern.push(on, off);
    return pattern;
}

/**
 * Stick position from where the finger is, as new origin and axes. The origin
 * follows a finger past the radius, so coming back moves the stick at once
 * rather than after a dead stretch of travel.
 * @param {{x: number, y: number}} origin
 * @param {number} x
 * @param {number} y
 * @param {number} radius
 * @returns {{ox: number, oy: number, ax: number, ay: number}}
 */
export function stickFromFinger(origin, x, y, radius) {
    let ox = origin.x;
    let oy = origin.y;
    let dx = x - ox;
    let dy = y - oy;
    const dist = Math.hypot(dx, dy);
    if (dist > radius) {
        ox = x - (dx / dist) * radius;
        oy = y - (dy / dist) * radius;
        dx = x - ox;
        dy = y - oy;
    }
    const mag = Math.min(1, dist / radius);
    if (mag < DEAD_ZONE) return { ox, oy, ax: 0, ay: 0 };
    // From the edge of the dead zone, not a jump to 8 % when leaving it.
    const travel = (mag - DEAD_ZONE) / (1 - DEAD_ZONE);
    const length = Math.hypot(dx, dy);
    return { ox, oy, ax: (dx / length) * travel, ay: (dy / length) * travel };
}

export class TouchPadState {
    /**
     * @param {{ vibrate?: ((pattern: number|number[]) => unknown)|null,
     *           onLateChange?: (() => void)|null }} [options]
     *   `vibrate` is navigator.vibrate where there is one and the viewer wants
     *   it; without it the pad presents no motors to the host.
     *   `onLateChange` is told of a change no touch event will report.
     */
    constructor({ vibrate = null, onLateChange = null } = {}) {
        this._vibrate = vibrate;
        this._onLateChange = onLateChange;
        // The Gamepad-shaped part, what GamepadManager reads. No browser pad
        // has a negative index.
        this.index = -1;
        this.id = TOUCH_PAD_ID;
        this.mapping = 'standard';
        this.buttons = Array.from({ length: 17 }, () => ({ pressed: false, value: 0 }));
        this.axes = [0, 0, 0, 0];
        this.vibrationActuator = vibrate
            ? {
                  playEffect: (_type, { duration, strongMagnitude, weakMagnitude }) => {
                      this._rumbling = true;
                      vibrate(rumblePattern(duration, Math.max(strongMagnitude, weakMagnitude)));
                  },
                  reset: () => {
                      this._rumbling = false;
                      vibrate(0);
                  },
              }
            : null;
        this._rumbling = false;
        // Button index → fingers holding it: a finger leaving does not let go
        // of a button another one is on.
        this._holds = new Map();
        this._guideTimer = null;
        this._pulseTimer = null;
        this._guideDown = false;
        // Side ('left'/'right') → the stick being held.
        this._sticks = new Map();
        // Side → the tap that may be the first half of a double tap.
        this._lastTap = new Map();
    }

    _set(i, down) {
        this.buttons[i].pressed = down;
        this.buttons[i].value = down ? 1 : 0;
    }

    _late() {
        if (this._onLateChange) this._onLateChange();
    }

    /** One finger more on this button. Menu decides what it is when let go, or held. */
    press(i) {
        const holds = this._holds.get(i) || 0;
        this._holds.set(i, holds + 1);
        if (holds > 0) return;
        if (i !== PAD.MENU) {
            this._set(i, true);
            return;
        }
        this._endPulse();
        this._guideTimer = setTimeout(() => {
            this._guideTimer = null;
            this._guideDown = true;
            this._set(PAD.GUIDE, true);
            this._late();
        }, GUIDE_HOLD_MS);
    }

    /** One finger less on this button. */
    release(i) {
        const holds = this._holds.get(i) || 0;
        if (holds === 0) return;
        if (holds > 1) {
            this._holds.set(i, holds - 1);
            return;
        }
        this._holds.delete(i);
        if (i !== PAD.MENU) {
            this._set(i, false);
            return;
        }
        if (this._guideTimer !== null) {
            // Let go before the long press: it was Start after all.
            clearTimeout(this._guideTimer);
            this._guideTimer = null;
            this._set(PAD.MENU, true);
            this._pulseTimer = setTimeout(() => {
                this._pulseTimer = null;
                this._set(PAD.MENU, false);
                this._late();
            }, START_PULSE_MS);
        } else if (this._guideDown) {
            this._guideDown = false;
            this._set(PAD.GUIDE, false);
        }
    }

    _endPulse() {
        if (this._pulseTimer === null) return;
        clearTimeout(this._pulseTimer);
        this._pulseTimer = null;
        this._set(PAD.MENU, false);
    }

    _axisBase(side) {
        return side === 'left' ? 0 : 2;
    }

    _clickButton(side) {
        return side === 'left' ? PAD.L3 : PAD.R3;
    }

    /**
     * A finger comes down on a stick's zone, which is where its centre now is.
     * The second touch of a double tap also holds the stick's click for as long
     * as it stays down.
     * @param {'left'|'right'} side
     * @param {number} x
     * @param {number} y
     * @param {number} radius
     * @param {number} [now]
     * @returns {boolean} false if a finger is on this stick already.
     */
    stickDown(side, x, y, radius, now = performance.now()) {
        if (this._sticks.has(side)) return false;
        const tap = this._lastTap.get(side);
        const clicked =
            !!tap && now - tap.at <= DOUBLE_TAP_MS && Math.hypot(x - tap.x, y - tap.y) <= radius;
        this._lastTap.delete(side);
        if (clicked) this.press(this._clickButton(side));
        this._sticks.set(side, {
            x,
            y,
            startX: x,
            startY: y,
            fingerX: x,
            fingerY: y,
            radius,
            at: now,
            clicked,
            moved: false,
        });
        return true;
    }

    /** @param {'left'|'right'} side @param {number} x @param {number} y */
    stickMove(side, x, y) {
        const stick = this._sticks.get(side);
        if (!stick) return;
        const { ox, oy, ax, ay } = stickFromFinger(stick, x, y, stick.radius);
        stick.x = ox;
        stick.y = oy;
        stick.fingerX = x;
        stick.fingerY = y;
        if (Math.hypot(x - stick.startX, y - stick.startY) > TAP_MOVE_PX) stick.moved = true;
        const base = this._axisBase(side);
        this.axes[base] = ax;
        this.axes[base + 1] = ay;
    }

    /** @param {'left'|'right'} side @param {number} [now] */
    stickUp(side, now = performance.now()) {
        const stick = this._sticks.get(side);
        if (!stick) return;
        this._sticks.delete(side);
        const base = this._axisBase(side);
        this.axes[base] = 0;
        this.axes[base + 1] = 0;
        if (stick.clicked) {
            this.release(this._clickButton(side));
        } else if (!stick.moved && now - stick.at <= TAP_MS) {
            this._lastTap.set(side, { x: stick.startX, y: stick.startY, at: now });
        }
    }

    /**
     * Where to draw a stick being held, or null. `x`/`y` is its centre, `kx`/`ky`
     * its knob, and the knob is held inside the ring.
     * @param {'left'|'right'} side
     */
    stickView(side) {
        const stick = this._sticks.get(side);
        if (!stick) return null;
        return { x: stick.x, y: stick.y, kx: stick.fingerX, ky: stick.fingerY };
    }

    /**
     * Everything back to rest: the touches were taken away (a notification over
     * the page, a rotation, the keyboard opening) and no end will come for them.
     * Forgets a pending double tap too.
     */
    releaseAll() {
        if (this._guideTimer !== null) clearTimeout(this._guideTimer);
        if (this._pulseTimer !== null) clearTimeout(this._pulseTimer);
        this._guideTimer = null;
        this._pulseTimer = null;
        this._guideDown = false;
        this._holds.clear();
        this._sticks.clear();
        this._lastTap.clear();
        for (const b of this.buttons) {
            b.pressed = false;
            b.value = 0;
        }
        this.axes.fill(0);
    }

    /** A short tick under the finger, unless the game's rumble has the motor. */
    tapHaptic() {
        if (this._vibrate && !this._rumbling) this._vibrate(10);
    }
}

/**
 * Where the controls sit, laid out as an Xbox pad is: on the left the stick above
 * the cross, on the right the face buttons above the stick. Upright, everything
 * gathers in the lower part of the screen, where the thumbs are and where the
 * picture is not. Pure, in the layer's own pixels.
 *
 * A `button` control is a circle (`r`) or a box; a `dpad` splits its box into
 * eight directions; a `stick` is a zone a floating stick starts in, with the
 * place its ring rests at when no one holds it.
 *
 * @param {{width: number, height: number, scale?: number,
 *          insets?: {top?: number, right?: number, bottom?: number, left?: number}}} opts
 */
export function layoutTouchPad({ width: w, height: h, scale = 1, insets = {} }) {
    const left = Math.max(8, insets.left || 0);
    const right = Math.max(8, insets.right || 0);
    const bottom = Math.max(8, insets.bottom || 0);
    const short = Math.min(w, h);
    // A face button's diameter. Past what fits, the scale gives way, so the two
    // sides never meet on a phone held upright.
    const d = Math.min(Math.max(40, Math.min(64, short * 0.135)) * scale, w / 7, h / 5.5);
    // Upright, the controls keep to the part of the screen a landscape one has.
    const top = Math.max(Math.max(8, insets.top || 0), h - w * 1.1);
    const shoulderW = d * 1.5;
    const shoulderH = d * 0.8;
    const gap = d * 0.15;
    const belowShoulders = top + shoulderH + gap;
    const controls = [];

    const box = (id, x, y, bw, bh, buttons) =>
        controls.push({ id, kind: 'button', x, y, w: bw, h: bh, buttons });
    const round = (id, cx, cy, button) =>
        controls.push({
            id,
            kind: 'button',
            x: cx - d / 2,
            y: cy - d / 2,
            w: d,
            h: d,
            cx,
            cy,
            r: d * 0.62,
            buttons: [button],
        });

    box('lt', left, top, shoulderW, shoulderH, [PAD.LT]);
    box('lb', left + shoulderW + gap, top, shoulderW, shoulderH, [PAD.LB]);
    box('rt', w - right - shoulderW, top, shoulderW, shoulderH, [PAD.RT]);
    box('rb', w - right - shoulderW * 2 - gap, top, shoulderW, shoulderH, [PAD.RB]);

    // Face buttons: a diamond under the right shoulders.
    const faceCx = w - right - d * 1.5;
    const faceCy = belowShoulders + d * 1.5;
    round('y', faceCx, faceCy - d, PAD.Y);
    round('x', faceCx - d, faceCy, PAD.X);
    round('b', faceCx + d, faceCy, PAD.B);
    round('a', faceCx, faceCy + d, PAD.A);

    // The cross, in the bottom left corner.
    const dpadSize = d * 2.4;
    controls.push({
        id: 'dpad',
        kind: 'dpad',
        x: left,
        y: h - bottom - dpadSize,
        w: dpadSize,
        h: dpadSize,
    });

    // View and Menu, side by side at the bottom centre.
    const centerW = d * 1.05;
    const centerH = d * 0.65;
    // Upright, the cross reaches the middle: they go just above it.
    const crossInTheWay = w / 2 - centerW - gap < left + dpadSize;
    const centerY = crossInTheWay ? h - bottom - dpadSize - gap - centerH : h - bottom - centerH;
    box('view', w / 2 - centerW - gap, centerY, centerW, centerH, [PAD.VIEW]);
    box('menu', w / 2 + gap, centerY, centerW, centerH, [PAD.MENU]);

    // Stick zones: everything of each side that is not a button (hit-tested
    // after them), up to the centre strip, which stays the mouse's.
    const zoneTop = belowShoulders;
    const zoneW = w * 0.42;
    const radius = d * 1.1;
    const leftRestY = (zoneTop + (h - bottom - dpadSize)) / 2;
    controls.push({
        id: 'stick-left',
        kind: 'stick',
        side: 'left',
        x: 0,
        y: zoneTop,
        w: zoneW,
        h: h - zoneTop,
        radius,
        restX: left + dpadSize / 2,
        restY: leftRestY,
    });
    controls.push({
        id: 'stick-right',
        kind: 'stick',
        side: 'right',
        x: w - zoneW,
        y: zoneTop,
        w: zoneW,
        h: h - zoneTop,
        radius,
        restX: w - right - d * 1.5,
        restY: Math.max(faceCy + d * 1.5 + radius * 0.5, h - bottom - radius),
    });
    return { d, radius, controls };
}

/** The buttons a dpad's box means under a finger, diagonals included. */
function dpadButtons(c, x, y) {
    const dx = x - (c.x + c.w / 2);
    const dy = y - (c.y + c.h / 2);
    // The middle says nothing: a thumb resting there presses no direction.
    if (Math.hypot(dx, dy) < c.w * 0.12) return [];
    const octant = Math.round(Math.atan2(dy, dx) / (Math.PI / 4));
    return {
        0: [PAD.RIGHT],
        1: [PAD.RIGHT, PAD.DOWN],
        2: [PAD.DOWN],
        3: [PAD.DOWN, PAD.LEFT],
        4: [PAD.LEFT],
        '-4': [PAD.LEFT],
        '-3': [PAD.LEFT, PAD.UP],
        '-2': [PAD.UP],
        '-1': [PAD.UP, PAD.RIGHT],
    }[octant];
}

function inBox(c, x, y) {
    return x >= c.x && x <= c.x + c.w && y >= c.y && y <= c.y + c.h;
}

/**
 * The buttons under a point: the nearest round one in reach, or the box or
 * cross the point is in. `[]` over the cross's dead middle, null elsewhere.
 * @param {Array<any>} controls
 * @param {number} x
 * @param {number} y
 * @returns {number[]|null}
 */
export function buttonsAt(controls, x, y) {
    let nearest = null;
    let best = Infinity;
    for (const c of controls) {
        if (c.kind !== 'button') continue;
        if (c.r) {
            const dist = Math.hypot(x - c.cx, y - c.cy);
            if (dist <= c.r && dist < best) {
                best = dist;
                nearest = c;
            }
        } else if (inBox(c, x, y)) {
            return c.buttons;
        }
    }
    if (nearest) return nearest.buttons;
    for (const c of controls) {
        if (c.kind === 'dpad' && inBox(c, x, y)) return dpadButtons(c, x, y);
    }
    return null;
}

/**
 * The stick zone under a point, or null. Asked after buttonsAt: a finger coming
 * down on a button is a button, and only a touch starts a stick, never a slide.
 * @param {Array<any>} controls
 * @param {number} x
 * @param {number} y
 */
export function stickAt(controls, x, y) {
    for (const c of controls) {
        if (c.kind === 'stick' && inBox(c, x, y)) return c;
    }
    return null;
}
