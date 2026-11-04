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
 * The controller's settings as saved (Settings → On-screen controller, per
 * device), with the defaults of a viewer who never opened them.
 * @param {Record<string, any>} [data]
 */
export function touchPadSettings(data = {}) {
    const within = (v, min, max, fallback) =>
        typeof v === 'number' && v >= min && v <= max ? v : fallback;
    return {
        mode: ['always', 'never'].includes(data.touch_pad_mode) ? data.touch_pad_mode : 'auto',
        scale: within(data.touch_pad_scale, 0.8, 1.4, 1),
        opacity: within(data.touch_pad_opacity, 0.2, 1, 1),
        vibrate: data.touch_pad_vibrate !== false,
        outside: data.touch_pad_outside === 'ignore' ? 'ignore' : 'mouse',
    };
}

/**
 * Where the controls sit: on each side the buttons high and outside, the stick
 * low and inside, L3 and R3 in the bottom corners. These are the places and
 * proportions of GeForce NOW's controller, the one players of this kind of
 * stream already have in their thumbs, with the cross and the face buttons drawn
 * a little tighter. Upright, View, the toggle and Menu go to the top of the screen
 * and the rest gathers at the bottom, where the thumbs are. Pure, in the layer's
 * own pixels.
 *
 * A `button` control is a circle (`r`) or a box (`slack` widens what a finger can
 * hit beyond what is drawn); a `dpad` splits a disc of `reach` into eight
 * directions; a `stick` is a zone a floating stick starts in, with the place its
 * ring rests at when no one holds it. `toggle` is where the button that hides the
 * controller goes between View and Menu: it is not one of the pad's controls.
 *
 * @param {{width: number, height: number, scale?: number,
 *          insets?: {top?: number, right?: number, bottom?: number, left?: number}}} opts
 */
export function layoutTouchPad({ width: w, height: h, scale = 1, insets = {} }) {
    const short = Math.min(w, h);
    // A button's diameter. Past what fits, the scale gives way, so the two sides
    // never meet on a phone held upright.
    const d = Math.min(Math.max(40, Math.min(66, short * 0.158)) * scale, w / 7, h / 5.5);
    const edge = Math.max(8, d * 0.3);
    const left = Math.max(edge, insets.left || 0);
    const right = Math.max(edge, insets.right || 0);
    const gap = d * 0.15;
    const radius = d * 1.1;
    const restY = h - Math.max(8, d * 0.23, insets.bottom || 0) - radius;
    const toggleSize = Math.max(44, d * 0.76);
    // From the top of the shoulders to the bottom of the A button.
    const buttonsH = d * 4;
    const screenTop = Math.max(8, insets.top || 0);
    // Upright, with the height for it: the buttons right above the sticks.
    const lowered = restY - radius - gap - buttonsH;
    const stacked = h > w && lowered >= screenTop + toggleSize + gap;
    // Otherwise the part of the screen a landscape one has.
    const top = stacked ? lowered : Math.max(8, short * 0.08, insets.top || 0, h - w * 1.1);
    // Columns from each edge: the cross and the face buttons, a sliver apart on
    // their diagonals, and the shoulders in line with them.
    const pitch = d * 0.85;
    const fromLeft = (n) => left + d / 2 + n * pitch;
    const fromRight = (n) => w - right - d / 2 - n * pitch;
    const shouldersY = top + d / 2;
    const clusterY = shouldersY + d * 2.15;
    const controls = [];

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

    round('lt', fromLeft(0), shouldersY, PAD.LT);
    round('lb', fromLeft(2), shouldersY, PAD.LB);
    round('rt', fromRight(0), shouldersY, PAD.RT);
    round('rb', fromRight(2), shouldersY, PAD.RB);

    round('y', fromRight(1), clusterY - pitch, PAD.Y);
    round('x', fromRight(2), clusterY, PAD.X);
    round('b', fromRight(0), clusterY, PAD.B);
    round('a', fromRight(1), clusterY + pitch, PAD.A);

    // The cross answers on a disc, not its box: a corner of the box lies under
    // the ring of the stick, where a thumb means the stick.
    const dpadCx = fromLeft(1);
    const dpadSize = pitch * 2 + d;
    controls.push({
        id: 'dpad',
        kind: 'dpad',
        x: dpadCx - dpadSize / 2,
        y: clusterY - dpadSize / 2,
        w: dpadSize,
        h: dpadSize,
        cx: dpadCx,
        cy: clusterY,
        reach: dpadSize / 2 + d * 0.05,
    });

    const cornerY = h - Math.max(8, d * 0.31, insets.bottom || 0) - d / 2;
    round('l3', fromLeft(0), cornerY, PAD.L3);
    round('r3', fromRight(0), cornerY, PAD.R3);

    // View, the toggle and Menu: at the top of the screen upright, else on top
    // between the shoulders, else below the cross when these leave them no room.
    const pillW = d * 1.03;
    const pillH = Math.max(34, d * 0.51);
    const roomOnTop = w / 2 - d * 1.28 - pillW / 2 >= Math.max(left, right) + pitch * 2 + d + gap;
    let middleY = clusterY + pitch + d / 2 + gap + toggleSize / 2;
    if (stacked) middleY = screenTop + toggleSize / 2;
    else if (roomOnTop) middleY = top + d * 0.25;
    const pill = (id, cx, button) =>
        controls.push({
            id,
            kind: 'button',
            pill: true,
            x: cx - pillW / 2,
            y: middleY - pillH / 2,
            w: pillW,
            h: pillH,
            slack: Math.max(0, (44 - pillH) / 2),
            buttons: [button],
        });
    pill('view', w / 2 - d * 1.28, PAD.VIEW);
    pill('menu', w / 2 + d * 1.28, PAD.MENU);

    // Stick zones: everything of each side that is not a button (hit-tested
    // after them), up to the centre strip, which stays the mouse's.
    const zoneTop = top + d;
    const zoneW = w * 0.42;
    const restInward = Math.min(left + d * 3.24, w / 2 - radius - gap);
    controls.push({
        id: 'stick-left',
        kind: 'stick',
        side: 'left',
        x: 0,
        y: zoneTop,
        w: zoneW,
        h: h - zoneTop,
        radius,
        restX: restInward,
        restY,
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
        restX: w - restInward,
        restY,
    });
    return {
        d,
        radius,
        controls,
        toggle: {
            x: w / 2 - toggleSize / 2,
            y: middleY - toggleSize / 2,
            w: toggleSize,
            h: toggleSize,
        },
    };
}

/** The buttons a dpad means under a finger, diagonals included. */
function dpadButtons(c, x, y) {
    const dx = x - c.cx;
    const dy = y - c.cy;
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
    const slack = c.slack || 0;
    return x >= c.x && x <= c.x + c.w && y >= c.y - slack && y <= c.y + c.h + slack;
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
        if (c.kind === 'dpad' && Math.hypot(x - c.cx, y - c.cy) <= c.reach) {
            return dpadButtons(c, x, y);
        }
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
