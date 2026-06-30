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
 * GamepadManager — bridges the browser Gamepad API to the Moonlight input DC.
 *
 * The Gamepad API exposes "standard mapping" controllers (Xbox, PlayStation,
 * most modern pads) with a fixed layout that maps 1:1 to Limelight's button
 * flags and axes. We poll the live state every frame and send a snapshot over
 * the input transport only when it changes (anti-spam).
 *
 * Non-standard controllers (racing wheels, HOTAS) are ignored on purpose:
 * they need a per-device remap that does not exist yet (phase 2).
 *
 * Protocol (browser → backend):
 *   {type:"gamepadconnect", index, mask, ctype, rumble}
 *   {type:"gamepad", index, mask, buttons, lt, rt, lx, ly, rx, ry}
 *   {type:"gamepaddisconnect", index, mask}
 * Backend → browser: {type:"rumble", index, low, high}
 */

// Limelight button flags (must match Limelight.h).
const BTN = {
    A: 0x1000,
    B: 0x2000,
    X: 0x4000,
    Y: 0x8000,
    UP: 0x0001,
    DOWN: 0x0002,
    LEFT: 0x0004,
    RIGHT: 0x0008,
    LB: 0x0100,
    RB: 0x0200,
    PLAY: 0x0010,
    BACK: 0x0020,
    LS_CLK: 0x0040,
    RS_CLK: 0x0080,
    SPECIAL: 0x0400,
};

// W3C standard gamepad button index → Limelight flag.
// Indices 6/7 (triggers) are analog and handled separately.
const BUTTON_MAP = {
    0: BTN.A,
    1: BTN.B,
    2: BTN.X,
    3: BTN.Y,
    4: BTN.LB,
    5: BTN.RB,
    8: BTN.BACK,
    9: BTN.PLAY,
    10: BTN.LS_CLK,
    11: BTN.RS_CLK,
    12: BTN.UP,
    13: BTN.DOWN,
    14: BTN.LEFT,
    15: BTN.RIGHT,
    16: BTN.SPECIAL,
};

// LI_CTYPE_* (Limelight.h)
const CTYPE = { UNKNOWN: 0, XBOX: 1, PS: 2, NINTENDO: 3 };

function detectType(id) {
    const s = (id || '').toLowerCase();
    if (/xbox|xinput|microsoft/.test(s)) return CTYPE.XBOX;
    if (/dualsense|dualshock|playstation|sony|0ce6|054c/.test(s)) return CTYPE.PS;
    if (/nintendo|switch|joy-?con|pro controller|057e/.test(s)) return CTYPE.NINTENDO;
    return CTYPE.UNKNOWN;
}

// Float axis (-1..1) → signed short (-32767..32767).
function axisToShort(v) {
    let s = Math.round(v * 32767);
    if (s > 32767) s = 32767;
    if (s < -32767) s = -32767;
    return s;
}

export class GamepadManager {
    /** @param {(msg:object)=>void} sendFn — sends a JSON input message. */
    constructor(sendFn) {
        this._send = sendFn;
        this._running = false;
        this._rafId = null;
        // index → { last: {buttons,lt,rt,lx,ly,rx,ry}, hasRumble:boolean }
        this._pads = new Map();
        this._onConnect = (e) => this._handleConnect(e.gamepad);
        this._onDisconnect = (e) => this._handleDisconnect(e.gamepad);
    }

    start() {
        if (this._running || !navigator.getGamepads) return;
        this._running = true;
        window.addEventListener('gamepadconnected', this._onConnect);
        window.addEventListener('gamepaddisconnected', this._onDisconnect);
        // Pads connected before start() won't fire an event — pick them up on
        // the first poll.
        this._loop();
    }

    stop() {
        if (!this._running) return;
        this._running = false;
        window.removeEventListener('gamepadconnected', this._onConnect);
        window.removeEventListener('gamepaddisconnected', this._onDisconnect);
        if (this._rafId !== null) cancelAnimationFrame(this._rafId);
        this._rafId = null;
        // Tell the host every controller is gone.
        for (const index of this._pads.keys()) {
            this._send({ type: 'gamepaddisconnect', index, mask: 0 });
        }
        this._pads.clear();
    }

    /** Active controllers as a bitmask (one bit per index). */
    _mask() {
        let m = 0;
        for (const index of this._pads.keys()) m |= 1 << index;
        return m;
    }

    _handleConnect(gp) {
        if (!gp || gp.mapping !== 'standard') return; // wheels/HOTAS: phase 2
        if (this._pads.has(gp.index)) return;
        const hasRumble = !!gp.vibrationActuator;
        this._pads.set(gp.index, { last: null, hasRumble });
        this._send({
            type: 'gamepadconnect',
            index: gp.index,
            mask: this._mask(),
            ctype: detectType(gp.id),
            rumble: hasRumble,
        });
    }

    _handleDisconnect(gp) {
        if (!gp || !this._pads.has(gp.index)) return;
        this._pads.delete(gp.index);
        this._send({ type: 'gamepaddisconnect', index: gp.index, mask: this._mask() });
    }

    _loop() {
        if (!this._running) return;
        this._poll();
        this._rafId = requestAnimationFrame(() => this._loop());
    }

    _poll() {
        const pads = navigator.getGamepads ? navigator.getGamepads() : [];
        for (const gp of pads) {
            if (!gp || gp.mapping !== 'standard') continue;
            // Late-arriving pad (no connect event yet).
            if (!this._pads.has(gp.index)) this._handleConnect(gp);

            const entry = this._pads.get(gp.index);
            if (!entry) continue;

            let buttons = 0;
            for (const i in BUTTON_MAP) {
                if (gp.buttons[i] && gp.buttons[i].pressed) buttons |= BUTTON_MAP[i];
            }
            const lt = gp.buttons[6] ? Math.round(gp.buttons[6].value * 255) : 0;
            const rt = gp.buttons[7] ? Math.round(gp.buttons[7].value * 255) : 0;
            // Y axes inverted: Limelight expects up = positive.
            const lx = axisToShort(gp.axes[0] || 0);
            const ly = axisToShort(-(gp.axes[1] || 0));
            const rx = axisToShort(gp.axes[2] || 0);
            const ry = axisToShort(-(gp.axes[3] || 0));

            const cur = { buttons, lt, rt, lx, ly, rx, ry };
            const p = entry.last;
            if (
                p &&
                p.buttons === buttons &&
                p.lt === lt &&
                p.rt === rt &&
                p.lx === lx &&
                p.ly === ly &&
                p.rx === rx &&
                p.ry === ry
            ) {
                continue; // unchanged — don't flood the input channel
            }
            entry.last = cur;
            this._send({ type: 'gamepad', index: gp.index, mask: this._mask(), ...cur });
        }
    }

    /** Trigger vibration on the matching controller (host rumble request). */
    rumble(index, low, high) {
        const pads = navigator.getGamepads ? navigator.getGamepads() : [];
        const gp = pads[index];
        if (!gp || !gp.vibrationActuator) return;
        // Limelight motors are 16-bit; the Web API wants 0..1 magnitudes.
        const strong = Math.min(1, (low || 0) / 65535);
        const weak = Math.min(1, (high || 0) / 65535);
        try {
            gp.vibrationActuator.playEffect('dual-rumble', {
                duration: 200,
                strongMagnitude: strong,
                weakMagnitude: weak,
            });
        } catch (e) {
            /* unsupported actuator type */
        }
    }
}
