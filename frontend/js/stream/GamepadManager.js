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
 * flags and axes. A pad is never an event — not even its buttons, unlike a key
 * or a click: it is seen when it is read. The frame loop finds the pads; once
 * one is forwarded it is also read every POLL_MS, the rate Chrome samples pads
 * at. A snapshot goes over the input transport only when it changes
 * (anti-spam).
 *
 * A controller the browser reports without a standard layout is read through
 * a mapping (gamepadMapping.resolveMapping): the user's own, a built-in
 * profile (EdgeTX / OpenTX radios), Chrome Android's pre-sorted layout, or
 * SDL_GameControllerDB. One that nothing maps is not
 * forwarded — every button could be somewhere else — and the caller is told
 * so it can offer the remap wizard.
 *
 * `single` mode (share-link guests) forwards ONE pad, always as controller 0:
 * the host shifts a guest's pads past the owner's and the other guests'
 * (gamepadOffset), so a guest pad at browser index 1 would land on the next
 * guest's virtual controller.
 *
 * An on-screen controller (attachVirtual) is one more pad: it answers to the
 * same anti-spam, repeats, pause and teardown, and the host cannot tell it from
 * another Xbox pad. It has no browser index, so it takes the lowest controller
 * number free, and a real pad whose number it holds takes the next one. It is
 * read on the frame loop and sent at the touch itself (pokeVirtual).
 *
 * Protocol (browser → backend):
 *   {type:"gamepadconnect", index, mask, ctype, rumble}
 *   {type:"gamepad", index, mask, buttons, lt, rt, lx, ly, rx, ry}
 *   {type:"gamepaddisconnect", index, mask}
 * Backend → browser: {type:"rumble", index, low, high}
 */

import {
    resolveMapping,
    readVirtualPad,
    loadGamepadDb,
    detectPlatform,
    isAutoMapped,
    isTvRemoteId,
    padKey,
} from './gamepadMapping.js';
import { getMapping, CHANGED_EVENT } from '../util/gamepadMappingsStore.js';

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
 * A TV remote seen as a pad (gamepadMapping.isTvRemoteId): StreamView gets its
 * direction pad as arrow keys instead (onRemoteKey), like any remote of the
 * keyboard kind, and the host is never told a pad is there.
 * @param {{id?: string}} gp
 */
export function isTvRemotePad(gp) {
    return isTvRemoteId(gp && gp.id);
}

/** Standard-mapping direction pad buttons, as the arrows they stand for. */
const REMOTE_DIRS = { 12: 'up', 13: 'down', 14: 'left', 15: 'right' };
/**
 * A remote's arrow held this long repeats, then this often — the typematic a
 * keyboard's controller would make: a key injected on the host presses once
 * however long it is held (see StreamView.handleKeyDown).
 */
const REMOTE_REPEAT_DELAY_MS = 420;
const REMOTE_REPEAT_MS = 40;
/** The menu chord on a real pad: Select + Start + LB + RB (moonlight-qt's quit chord). */
const MENU_CHORD = [8, 9, 4, 5];

/**
 * How often a forwarded pad is read between two frames. Chrome samples pads
 * at 250 Hz; read once a frame, a stick or a button waited for the next frame
 * — 8 ms on average at 60 Hz, sticks and buttons alike. Read every 4 ms, it
 * waits 2. Not a setting: latency comes first, and a pad that does not move
 * still sends nothing (see REEMIT_MS for the one exception).
 *
 * Armed only while a pad is forwarded, the page is visible and the stream not
 * paused (_armPoll); the frame loop alone keeps looking for pads otherwise.
 */
const POLL_MS = 4;

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
     * @param {{ profile?: 'auto'|'x360'|'ds4',
     *           onIgnored?: (gp: Gamepad) => void,
     *           onMapped?: (gp: Gamepad, res: object) => void,
     *           single?: boolean, preferredKey?: string|null,
     *           probe?: {notePadRead: (gapMs: number) => void, notePadSend: () => void}|null,
     *           onVirtualYield?: () => void,
     *           platform?: string, db?: object|null, user?: (key: string) => object|null }} [options]
     *   `profile` forces the pad the host presents instead of following what we
     *   detect. It is offered in debug builds only (see SettingsView): in
     *   production the right behaviour is to guess correctly, and a visible
     *   switch would turn a detection bug into a question the user cannot
     *   answer. In debug it is what separates "the detection was wrong" from
     *   "the profile is wrong".
     *   `onIgnored` is told, once per pad, about a controller nothing maps —
     *   the caller decides how to say it (and offers the wizard).
     *   `onMapped` is told, once per pad, about one laid out without the user
     *   (a built-in profile, Chrome Android, SDL database), so they can check it.
     *   `single` forwards one pad only, as controller 0 (share-link guests);
     *   `preferredKey` is the pad (gamepadMapping.padKey) to take when present.
     *   `probe` (diagnostics only, PipelineDiag's MainThreadProbe) is told how
     *   far apart the reads of a forwarded pad are, and of every state sent.
     *   `platform`, `db` and `user` replace the live sources (tests).
     *   `onRemoteKey(dir, down, repeat)` is told the arrows of a TV remote seen
     *   as a pad (isTvRemotePad), which is then never forwarded as a pad.
     *   `onMenuChord()` is told when a forwarded pad presses Select + Start +
     *   LB + RB together (StreamView opens the remote's menu with it on a TV).
     *   `onVirtualYield()` is told when a real pad took the place of an
     *   on-screen controller attached with `yields`.
     */
    constructor(sendFn, options = {}) {
        this._send = sendFn;
        this._onRemoteKey = typeof options.onRemoteKey === 'function' ? options.onRemoteKey : null;
        this._onMenuChord = typeof options.onMenuChord === 'function' ? options.onMenuChord : null;
        this._onVirtualYield =
            typeof options.onVirtualYield === 'function' ? options.onVirtualYield : null;
        // Browser index → { buttonIndex → {since, lastAt} } for a remote's held arrows.
        this._remotes = new Map();
        // Pads whose menu chord is down, so it fires once per press.
        this._chordDown = new Set();
        this._profile = options.profile || 'auto';
        this._onIgnored = typeof options.onIgnored === 'function' ? options.onIgnored : null;
        this._onMapped = typeof options.onMapped === 'function' ? options.onMapped : null;
        this._single = options.single === true;
        this._preferredKey = options.preferredKey || null;
        this._platform = options.platform || detectPlatform();
        // undefined = SDL database not loaded yet; null = unavailable.
        this._db = 'db' in options ? options.db : undefined;
        this._dbLoading = false;
        this._user = typeof options.user === 'function' ? options.user : getMapping;
        // Indexes already reported as ignored / guessed; a pad is announced
        // once, not once per frame of the poll that keeps seeing it.
        this._ignored = new Set();
        this._announced = new Set();
        // Browser index → { id, res } — how each pad is read, until it is
        // replugged or a mapping changes.
        this._resolved = new Map();
        this._paused = false;
        this._running = false;
        // start() was asked for: a page without the Gamepad API only runs once
        // it has an on-screen controller to forward.
        this._wanted = false;
        // The on-screen controller (a Gamepad-shaped object, always current).
        this._virtual = null;
        this._virtualYields = false;
        // Single mode: the pad preferred before the on-screen controller was.
        this._keyBeforeVirtual = null;
        this._rafId = null;
        // The POLL_MS reader, while armed (_armPoll).
        this._pollTimer = null;
        this._probe = options.probe || null;
        // When the previous read of a forwarded pad happened (diagnostics).
        this._lastReadAt = 0;
        // Browser index → { last: {buttons,lt,rt,lx,ly,rx,ry}, sentAt,
        //   hasRumble, bindings, hostIndex, key }
        this._pads = new Map();
        // Keys (`usb:vid:pid`) of pads the HID passthrough sends as themselves.
        this._excluded = new Set();
        // Browser index → { strong, weak, since, timer } for a vibration being held.
        this._rumble = new Map();
        this._onConnect = (e) => this._handleConnect(e.gamepad);
        this._onDisconnect = (e) => this._handleDisconnect(e.gamepad);
        this._onMappingsChanged = () => this.refreshMappings();
        // A hidden page stops the frame loop and the reads with it, as it
        // always did; StreamView repeats every pad on the way back (resendAll).
        this._onVisibility = () => this._armPoll();
    }

    start() {
        this._wanted = true;
        if (this._running || (!navigator.getGamepads && !this._virtual)) return;
        this._running = true;
        window.addEventListener('gamepadconnected', this._onConnect);
        window.addEventListener('gamepaddisconnected', this._onDisconnect);
        window.addEventListener(CHANGED_EVENT, this._onMappingsChanged);
        document.addEventListener('visibilitychange', this._onVisibility);
        // Pads connected before start() won't fire an event — pick them up on
        // the first poll.
        this._loop();
    }

    stop() {
        this._wanted = false;
        if (!this._running) return;
        this._running = false;
        window.removeEventListener('gamepadconnected', this._onConnect);
        window.removeEventListener('gamepaddisconnected', this._onDisconnect);
        window.removeEventListener(CHANGED_EVENT, this._onMappingsChanged);
        document.removeEventListener('visibilitychange', this._onVisibility);
        if (this._rafId !== null) cancelAnimationFrame(this._rafId);
        this._rafId = null;
        this._armPoll();
        this._releaseRemotes();
        this._chordDown.clear();
        // Motors first: a pad still shaking after the stream closed would be
        // shaking for nobody.
        for (const index of Array.from(this._rumble.keys())) this._stopRumble(index);
        // Tell the host every controller is gone.
        for (const entry of this._pads.values()) {
            this._send({ type: 'gamepaddisconnect', index: entry.hostIndex, mask: 0 });
        }
        this._pads.clear();
        this._ignored.clear();
        this._announced.clear();
        this._resolved.clear();
    }

    /**
     * Forget how every pad is read, so the next poll resolves them again —
     * after the user saved or reset a mapping. A pad that became unmapped is
     * released on the host; one that became mapped is announced.
     */
    refreshMappings() {
        this._resolved.clear();
        this._ignored.clear();
    }

    /**
     * Stop forwarding while the remap wizard listens to the pad: every pad is
     * put back at rest on the host once, then nothing is sent until resumed.
     * Pressing A to map "A" must not press A in the game.
     */
    setPaused(paused) {
        paused = !!paused;
        if (paused === this._paused) return;
        this._paused = paused;
        this._armPoll();
        if (paused) {
            this._releaseRemotes();
            const rest = { buttons: 0, lt: 0, rt: 0, lx: 0, ly: 0, rx: 0, ry: 0 };
            for (const entry of this._pads.values()) {
                entry.last = rest;
                entry.sentAt = performance.now();
                this._send({
                    type: 'gamepad',
                    index: entry.hostIndex,
                    mask: this._mask(),
                    ...rest,
                });
            }
        } else {
            this.resendAll();
        }
    }

    /**
     * Single mode: the pad to forward from now on. The current one is
     * released on the host and the chosen one takes controller 0 on the next
     * poll (if it is connected and mapped).
     */
    setPreferredPad(key) {
        this._preferredKey = key || null;
        if (!this._single) return;
        for (const [index, entry] of this._pads) {
            if (entry.key !== this._preferredKey) this._release(index);
        }
    }

    /** The pad forwarded in single mode (its key), or null. */
    forwardedKey() {
        for (const entry of this._pads.values()) return entry.key;
        return null;
    }

    /**
     * Plug an on-screen controller in: a Gamepad-shaped object, standard
     * mapping, always current, with an `index` no browser pad can have. It is
     * forwarded from now on, or as soon as the manager starts. In single mode it
     * becomes the preferred pad, and detachVirtual gives the place back.
     *
     * `yields`: a real pad showing up takes its place — it is unplugged first,
     * so the real pad gets its own number (controller 0, not player 2) — and
     * onVirtualYield is told. For a controller nobody explicitly asked for.
     *
     * @param {any} pad
     * @param {{yields?: boolean}} [options]
     */
    attachVirtual(pad, { yields = false } = {}) {
        if (this._virtual) this.detachVirtual();
        this._virtual = pad;
        this._virtualYields = yields;
        if (this._single) {
            this._keyBeforeVirtual = this._preferredKey;
            this.setPreferredPad(padKey(pad));
        }
        if (this._wanted) this.start();
        this.pokeVirtual();
    }

    /** Unplug the on-screen controller: taken off the host, its motors stopped. */
    detachVirtual() {
        const pad = this._virtual;
        if (!pad) return;
        this._release(pad.index);
        this._chordDown.delete(pad.index);
        this._virtual = null;
        if (this._single) {
            this._preferredKey = this._keyBeforeVirtual;
            this._keyBeforeVirtual = null;
        }
    }

    /** Whether an on-screen controller is plugged in. */
    hasVirtual() {
        return !!this._virtual;
    }

    /**
     * Read the on-screen controller now: its state changed at a touch, and
     * waiting for the next frame would add latency for nothing.
     */
    pokeVirtual() {
        if (this._running && this._virtual) this._readPad(this._virtual);
    }

    /** Whether a real pad is being forwarded, as opposed to an on-screen one. */
    hasPhysicalPad() {
        for (const index of this._pads.keys()) {
            if (!this._virtual || index !== this._virtual.index) return true;
        }
        return false;
    }

    /** The pad behind `index`: the on-screen controller, or the browser's. */
    _padAt(index) {
        if (this._virtual && this._virtual.index === index) return this._virtual;
        const pads = navigator.getGamepads ? navigator.getGamepads() : [];
        return pads[index];
    }

    /**
     * The controller number a new pad takes on the host. A real pad's is its
     * browser index, unless an on-screen controller holds it; the on-screen one
     * takes the lowest free. Nobody is ever renumbered while forwarded: a game
     * would see its player change hands.
     */
    _hostIndexFor(gp) {
        if (this._single) return 0;
        const taken = new Set();
        for (const entry of this._pads.values()) taken.add(entry.hostIndex);
        if (gp !== this._virtual && !taken.has(gp.index)) return gp.index;
        let free = 0;
        while (taken.has(free)) free++;
        return free;
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
        // Laid out as one, and no USB ids or vendor name to tell by.
        if (gp === this._virtual) return CTYPE.XBOX;
        return detectType(gp.id);
    }

    /**
     * Arm the POLL_MS reader while a pad is forwarded, the page is visible and
     * the stream is not paused — disarm it as soon as one of the three fails.
     * Called wherever one of them changes. With nothing to forward, the frame
     * loop alone keeps watching for pads. An on-screen controller alone does not
     * arm it: it says when it changes (pokeVirtual), and on a phone 250 reads a
     * second would be battery spent on nothing.
     */
    _armPoll() {
        const wanted =
            this._running &&
            this.hasPhysicalPad() &&
            !this._paused &&
            document.visibilityState !== 'hidden';
        // Diagnostics: a stretch without reads is not a gap between two.
        if (!wanted) this._lastReadAt = 0;
        if (wanted && this._pollTimer === null) {
            this._pollTimer = setInterval(() => this._poll(), POLL_MS);
        } else if (!wanted && this._pollTimer !== null) {
            clearInterval(this._pollTimer);
            this._pollTimer = null;
        }
    }

    /** Active controllers as a bitmask (one bit per host index). */
    _mask() {
        let m = 0;
        for (const entry of this._pads.values()) m |= 1 << entry.hostIndex;
        return m;
    }

    /**
     * A pad nothing maps: the browser does not know which button is which,
     * and neither do we. Forwarding it anyway would give a pad whose every
     * button may be somewhere else — harder to diagnose than a pad that is not
     * there. The caller is told once, and offers the remap wizard.
     */
    _noteIgnored(gp) {
        if (this._ignored.has(gp.index)) return;
        this._ignored.add(gp.index);
        if (this._onIgnored) this._onIgnored(gp);
    }

    /** How this pad is read (cached per browser index and id). */
    _resolve(gp) {
        const cached = this._resolved.get(gp.index);
        if (cached && cached.id === gp.id) return cached.res;
        const res = resolveMapping(gp, {
            user: this._user,
            db: this._db,
            platform: this._platform,
        });
        if (res.source === 'pending') {
            this._loadDb();
            return res; // not cached: asked again once the database is in
        }
        this._resolved.set(gp.index, { id: gp.id, res });
        return res;
    }

    _loadDb() {
        if (this._dbLoading || this._db !== undefined) return;
        this._dbLoading = true;
        loadGamepadDb().then((db) => {
            this._db = db;
            this._dbLoading = false;
            this._resolved.clear();
        });
    }

    /**
     * Resolve `gp` and, if it is mapped and has a place, announce it to the
     * host. Returns the pad's entry when it is forwarded, else null.
     */
    _consider(gp) {
        const res = this._resolve(gp);
        if (res.source === 'pending') return null;
        let entry = this._pads.get(gp.index);
        if (this._excluded.has(res.key)) {
            if (entry) this._release(gp.index);
            return null;
        }
        if (!res.source) {
            if (entry) this._release(gp.index);
            this._noteIgnored(gp);
            return null;
        }
        this._ignored.delete(gp.index);
        if (entry) {
            entry.bindings = res.bindings;
            return entry;
        }
        if (gp !== this._virtual && this._virtual && this._virtualYields) {
            this.detachVirtual();
            if (this._onVirtualYield) this._onVirtualYield();
        }
        if (this._single) {
            // One pad only. The preferred one takes the place from any other;
            // otherwise first come, first served.
            const holder = Array.from(this._pads.keys())[0];
            if (holder !== undefined) {
                const preferred = this._preferredKey && res.key === this._preferredKey;
                const holderPreferred = this._pads.get(holder).key === this._preferredKey;
                if (!preferred || holderPreferred) return null;
                this._release(holder);
            }
        }
        const hasRumble = !!gp.vibrationActuator;
        const hostIndex = this._hostIndexFor(gp);
        entry = {
            last: null,
            sentAt: 0,
            hasRumble,
            bindings: res.bindings,
            hostIndex,
            key: res.key,
        };
        this._pads.set(gp.index, entry);
        this._armPoll();
        this._send({
            type: 'gamepadconnect',
            index: hostIndex,
            mask: this._mask(),
            ctype: this._controllerType(gp),
            rumble: hasRumble,
        });
        if (isAutoMapped(res.source) && !this._announced.has(gp.index)) {
            this._announced.add(gp.index);
            if (this._onMapped) this._onMapped(gp, res);
        }
        return entry;
    }

    /**
     * Pads the HID passthrough sends to the host as themselves, by key
     * (`usb:vid:pid`): never forwarded as a mapped pad as well, or the game
     * would see the device twice. One already forwarded is taken off at once;
     * one let go again comes back at the next poll.
     */
    setExcluded(keys) {
        this._excluded = new Set(keys || []);
        for (const [index, entry] of Array.from(this._pads)) {
            if (this._excluded.has(entry.key)) this._release(index);
        }
    }

    /** Take a forwarded pad off the host. */
    _release(index) {
        const entry = this._pads.get(index);
        if (!entry) return;
        this._stopRumble(index);
        this._pads.delete(index);
        this._armPoll();
        this._send({ type: 'gamepaddisconnect', index: entry.hostIndex, mask: this._mask() });
    }

    _handleConnect(gp) {
        if (!gp || isTvRemotePad(gp)) return;
        this._consider(gp);
    }

    /**
     * A TV remote's arrows, out of a pad read: each press and release, and
     * the repeats of one held, to onRemoteKey. Nothing else of it is read.
     */
    _readRemote(gp) {
        let held = this._remotes.get(gp.index);
        if (!held) {
            held = new Map();
            this._remotes.set(gp.index, held);
        }
        const now = performance.now();
        for (const b of Object.keys(REMOTE_DIRS)) {
            const i = Number(b);
            const down = !!(gp.buttons[i] && gp.buttons[i].pressed);
            const was = held.get(i);
            if (down && !was) {
                held.set(i, { since: now, lastAt: now });
                if (this._onRemoteKey) this._onRemoteKey(REMOTE_DIRS[b], true, false);
            } else if (down && was) {
                if (
                    now - was.since >= REMOTE_REPEAT_DELAY_MS &&
                    now - was.lastAt >= REMOTE_REPEAT_MS
                ) {
                    was.lastAt = now;
                    if (this._onRemoteKey) this._onRemoteKey(REMOTE_DIRS[b], true, true);
                }
            } else if (!down && was) {
                held.delete(i);
                if (this._onRemoteKey) this._onRemoteKey(REMOTE_DIRS[b], false, false);
            }
        }
    }

    /** Let go of every arrow a remote holds (pause, stop): the host must not keep one down. */
    _releaseRemotes() {
        for (const held of this._remotes.values()) {
            for (const i of held.keys()) {
                if (this._onRemoteKey) this._onRemoteKey(REMOTE_DIRS[i], false, false);
            }
            held.clear();
        }
    }

    /** Select + Start + LB + RB on a forwarded pad: tell once per press. */
    _checkChord(gp) {
        if (!this._onMenuChord) return;
        const all = MENU_CHORD.every((i) => gp.buttons[i] && gp.buttons[i].pressed);
        if (all && !this._chordDown.has(gp.index)) {
            this._chordDown.add(gp.index);
            this._onMenuChord();
        } else if (!all) {
            this._chordDown.delete(gp.index);
        }
    }

    _handleDisconnect(gp) {
        if (!gp) return;
        // Unplugged and plugged back in still the wrong mode deserves the
        // message again; the same index may also be a different pad by then.
        this._ignored.delete(gp.index);
        this._announced.delete(gp.index);
        this._resolved.delete(gp.index);
        this._release(gp.index);
    }

    _loop() {
        if (!this._running) return;
        this._poll();
        this._rafId = requestAnimationFrame(() => this._loop());
    }

    /**
     * One read of every pad — from the frame loop, and every POLL_MS while a
     * pad is forwarded. The two interleave; neither sends what has not changed.
     */
    _poll() {
        const pads = navigator.getGamepads ? navigator.getGamepads() : [];
        for (const gp of pads) {
            if (!gp) continue;
            // A TV remote: its arrows as keys, and never a pad on the host.
            if (isTvRemotePad(gp)) {
                if (!this._paused) this._readRemote(gp);
                continue;
            }
            this._readPad(gp);
        }
        if (this._virtual) this._readPad(this._virtual);
        if (this._probe) this._noteRead();
    }

    /** One read of one pad, sent to the host if it has anything to say. */
    _readPad(gp) {
        // Also seen here, not only on the connect event: a pad plugged in
        // before start() never fires one, and a mapping may have changed.
        const entry = this._consider(gp);
        if (!entry || this._paused) return;
        this._checkChord(gp);

        const src = entry.bindings ? readVirtualPad(gp, entry.bindings) : gp;
        let buttons = 0;
        for (const i in BUTTON_MAP) {
            if (src.buttons[i] && src.buttons[i].pressed) buttons |= BUTTON_MAP[i];
        }
        const lt = src.buttons[6] ? Math.round(src.buttons[6].value * 255) : 0;
        const rt = src.buttons[7] ? Math.round(src.buttons[7].value * 255) : 0;
        // Y axes inverted: Limelight expects up = positive.
        const lx = axisToShort(src.axes[0] || 0);
        const ly = axisToShort(-(src.axes[1] || 0));
        const rx = axisToShort(src.axes[2] || 0);
        const ry = axisToShort(-(src.axes[3] || 0));

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
            if (!isActive(cur) || now - entry.sentAt < REEMIT_MS) return;
        }
        entry.last = cur;
        entry.sentAt = now;
        this._send({ type: 'gamepad', index: entry.hostIndex, mask: this._mask(), ...cur });
        if (this._probe) this._probe.notePadSend();
    }

    /**
     * Diagnostics: the gap since the previous read, counted only while it
     * matters — a pad forwarded and not paused. Any other read starts over, so
     * a quiet stretch never reads as one long gap.
     */
    _noteRead() {
        if (this._pads.size === 0 || this._paused) {
            this._lastReadAt = 0;
            return;
        }
        const now = performance.now();
        if (this._lastReadAt) this._probe.notePadRead(now - this._lastReadAt);
        this._lastReadAt = now;
    }

    /**
     * Forget what was last sent, so the next poll repeats every pad's state
     * whether it changed or not — at rest included.
     *
     * For the tab coming back from the background: rAF stops while it is
     * hidden, so the poll did too, and the host's watchdog has meanwhile
     * centred any pad it stopped hearing from. A stick still held is put back
     * within one poll; a pad at rest costs one idempotent message.
     */
    resendAll() {
        for (const entry of this._pads.values()) {
            entry.last = null;
            entry.sentAt = 0;
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
    rumble(hostIndex, low, high) {
        // The host speaks of its controller numbers; a single-mode pad is
        // controller 0 whatever its browser index.
        let index = hostIndex;
        let found = false;
        for (const [browserIndex, entry] of this._pads) {
            if (entry.hostIndex === hostIndex) {
                index = browserIndex;
                found = true;
            }
        }
        // A controller already gone, whose number is this pad's browser index
        // but not its controller number: that word is not for this pad.
        if (!found && this._pads.has(hostIndex)) return;
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

        const gp = this._padAt(index);
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

        const gp = this._padAt(index);
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
