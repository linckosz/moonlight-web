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

// Rumble is a STATE on the host, not a pulse: XInput and the DualShock alike
// say "motors at this level" and leave them there until told otherwise. The
// Web API only offers timed effects, so a held vibration is rebuilt out of
// back-to-back slices, each re-armed just before the previous one ends. One
// slice is long enough that the seam is inaudible, short enough that a host
// that vanishes mid-rumble leaves the pad shaking for a second, not forever.
const RUMBLE_SLICE_MS = 1000;
const RUMBLE_REARM_MS = 900;
// And a ceiling on how long a level is kept alive without the host saying it
// again: a real pad stops when the game exits; ours must not buzz on after a
// crash that never sent the "off".
const RUMBLE_MAX_HOLD_MS = 15000;

// Float axis (-1..1) → signed short (-32767..32767).
function axisToShort(v) {
    let s = Math.round(v * 32767);
    if (s > 32767) s = 32767;
    if (s < -32767) s = -32767;
    return s;
}

/**
 * How often a pad held away from rest is repeated to the host even though
 * nothing changed. Under the watchdog's long grace period (3 s), so a pad
 * centred by the host on a dead link comes back within half a second of the
 * link returning — and well over the frame rate, so a moving stick is never
 * sent twice.
 */
const REEMIT_MS = 500;

/**
 * Whether a snapshot is away from rest (button, trigger or stick). The
 * threshold matches the backend's (InputWatchdog::padAtRest): deliberately
 * loose, since a barely-drifted stick counted as active costs a message and
 * a shoved one missed costs a runaway.
 */
function isActive(s) {
    const AT_REST = 4096;
    return (
        s.buttons !== 0 ||
        s.lt !== 0 ||
        s.rt !== 0 ||
        Math.abs(s.lx) >= AT_REST ||
        Math.abs(s.ly) >= AT_REST ||
        Math.abs(s.rx) >= AT_REST ||
        Math.abs(s.ry) >= AT_REST
    );
}

export class GamepadManager {
    /**
     * @param {(msg:object)=>void} sendFn — sends a JSON input message.
     * @param {{ profile?: 'auto'|'x360'|'ds4', onIgnored?: (gp: Gamepad) => void }} [options]
     *   `profile` forces the pad the host presents instead of following what we
     *   detect. It is offered in debug builds only (see SettingsView): in
     *   production the right behaviour is to guess correctly, and a visible
     *   switch would turn a detection bug into a question the user cannot
     *   answer. In debug it is what separates "the detection was wrong" from
     *   "the profile is wrong".
     *   `onIgnored` is told, once per pad, about a controller this manager will
     *   not forward (no standard mapping) — the caller decides how to say it.
     */
    constructor(sendFn, options = {}) {
        this._send = sendFn;
        this._profile = options.profile || 'auto';
        this._onIgnored = typeof options.onIgnored === 'function' ? options.onIgnored : null;
        // Indexes already reported as ignored; a pad is announced once, not
        // once per frame of the poll that keeps seeing it.
        this._ignored = new Set();
        this._running = false;
        this._rafId = null;
        // index → { last: {buttons,lt,rt,lx,ly,rx,ry}, sentAt, hasRumble:boolean }
        this._pads = new Map();
        // index → { strong, weak, since, timer } for a vibration being held.
        this._rumble = new Map();
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
        // Motors first: a pad still shaking after the stream closed would be
        // shaking for nobody.
        for (const index of Array.from(this._rumble.keys())) this._stopRumble(index);
        // Tell the host every controller is gone.
        for (const index of this._pads.keys()) {
            this._send({ type: 'gamepaddisconnect', index, mask: 0 });
        }
        this._pads.clear();
        this._ignored.clear();
    }

    /**
     * What to tell the host this pad is — which decides the virtual controller
     * it presents to the game (Xbox 360, or DualShock 4 for a PlayStation pad).
     *
     * The forced values come from the debug-only setting and are stated as the
     * same LI_CTYPE_* the detection produces, so the host has one thing to read
     * and no idea that anything was overridden.
     */
    _controllerType(gp) {
        if (this._profile === 'x360') return CTYPE.XBOX;
        if (this._profile === 'ds4') return CTYPE.PS;
        return detectType(gp.id);
    }

    /** Active controllers as a bitmask (one bit per index). */
    _mask() {
        let m = 0;
        for (const index of this._pads.keys()) m |= 1 << index;
        return m;
    }

    /**
     * A pad the browser reports without a standard mapping: it does not know
     * which button is which, and neither do we. Forwarding it anyway would give
     * a pad whose every button may be somewhere else — harder to diagnose than
     * a pad that is not there. Decided out of scope on 2026-09-02 (design §7):
     * on PC a controller offering DirectInput almost always offers XInput too,
     * so the fix is a switch on the pad, and the user has to be told that.
     */
    _noteIgnored(gp) {
        if (this._ignored.has(gp.index)) return;
        this._ignored.add(gp.index);
        if (this._onIgnored) this._onIgnored(gp);
    }

    _handleConnect(gp) {
        if (!gp) return;
        if (gp.mapping !== 'standard') {
            this._noteIgnored(gp);
            return;
        }
        if (this._pads.has(gp.index)) return;
        const hasRumble = !!gp.vibrationActuator;
        this._pads.set(gp.index, { last: null, sentAt: 0, hasRumble });
        this._send({
            type: 'gamepadconnect',
            index: gp.index,
            mask: this._mask(),
            ctype: this._controllerType(gp),
            rumble: hasRumble,
        });
    }

    _handleDisconnect(gp) {
        if (!gp) return;
        // Unplugged and plugged back in still the wrong mode deserves the
        // message again; the same index may also be a different pad by then.
        this._ignored.delete(gp.index);
        if (!this._pads.has(gp.index)) return;
        this._stopRumble(gp.index);
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
            if (!gp) continue;
            if (gp.mapping !== 'standard') {
                // Also seen here, not only on the connect event: a pad plugged
                // in before start() never fires one.
                this._noteIgnored(gp);
                continue;
            }
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
            const now = performance.now();
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
                // Unchanged — don't flood the input channel. Except that a pad
                // held away from rest is repeated now and then: the host's
                // watchdog centres it when the link goes quiet for too long,
                // and nothing else would put it back until the stick MOVES.
                // The state is idempotent on every host, so a repeat costs
                // two small messages a second and never misleads.
                if (!isActive(cur) || now - entry.sentAt < REEMIT_MS) continue;
            }
            entry.last = cur;
            entry.sentAt = now;
            this._send({ type: 'gamepad', index: gp.index, mask: this._mask(), ...cur });
        }
    }

    /**
     * True when any pad is away from rest (button, trigger or stick).
     *
     * A pad only reports on change, so a stick shoved and held goes silent
     * exactly like a held key — and the host's input watchdog reads silence as
     * a dead link. StreamView's held-input heartbeat asks this so it keeps
     * beating while a stick is pushed.
     */
    hasActiveState() {
        for (const entry of this._pads.values()) {
            if (entry.last && isActive(entry.last)) return true;
        }
        return false;
    }

    /**
     * Set the matching controller's motors to what the host asked for, and
     * keep them there until the host says otherwise (see RUMBLE_SLICE_MS).
     *
     * Zero on both motors is the "off" the host sends when the game releases
     * the pad; anything else replaces whatever was being held.
     */
    rumble(index, low, high) {
        // Limelight motors are 16-bit; the Web API wants 0..1 magnitudes.
        const strong = Math.min(1, (low || 0) / 65535);
        const weak = Math.min(1, (high || 0) / 65535);

        if (strong === 0 && weak === 0) {
            this._stopRumble(index);
            return;
        }

        const previous = this._rumble.get(index);
        if (previous && previous.timer) clearTimeout(previous.timer);
        this._rumble.set(index, { strong, weak, since: Date.now(), timer: null });
        this._playRumbleSlice(index);
    }

    /** One slice of the vibration being held on `index`, and the next armed. */
    _playRumbleSlice(index) {
        const entry = this._rumble.get(index);
        if (!entry) return;

        const pads = navigator.getGamepads ? navigator.getGamepads() : [];
        const gp = pads[index];
        // No actuator (Safari, Firefox, a pad without motors): nothing to
        // hold, and nothing to keep re-arming for.
        if (!gp || !gp.vibrationActuator) {
            this._rumble.delete(index);
            return;
        }
        if (Date.now() - entry.since > RUMBLE_MAX_HOLD_MS) {
            this._stopRumble(index);
            return;
        }

        try {
            gp.vibrationActuator.playEffect('dual-rumble', {
                duration: RUMBLE_SLICE_MS,
                strongMagnitude: entry.strong,
                weakMagnitude: entry.weak,
            });
        } catch (e) {
            /* unsupported actuator type */
        }
        entry.timer = setTimeout(() => this._playRumbleSlice(index), RUMBLE_REARM_MS);
    }

    /** Motors off on `index`, and no slice left armed. */
    _stopRumble(index) {
        const entry = this._rumble.get(index);
        if (entry && entry.timer) clearTimeout(entry.timer);
        this._rumble.delete(index);

        const pads = navigator.getGamepads ? navigator.getGamepads() : [];
        const gp = pads[index];
        const actuator = gp && gp.vibrationActuator;
        if (!actuator) return;
        try {
            // reset() cuts the running slice short; a browser without it just
            // lets the current slice run out, which is at most a second.
            if (typeof actuator.reset === 'function') actuator.reset();
        } catch (e) {
            /* nothing to cut short */
        }
    }
}
