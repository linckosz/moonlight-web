/*
 * MoonlightWeb — Browser-based Moonlight streaming client.
 * Copyright (C) 2026 Bruno Martin.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * GamepadRemapDialog — check a controller, and map it control by control.
 *
 * Two modes over the drawn device (GamepadArt for a pad, DeviceArt for a
 * radio, a flight stick or a wheel), each control labelled with what the
 * game receives from it:
 *   - test:   the pad as MoonlightWeb reads it right now (user mapping,
 *             standard layout, or a guessed one), lit live;
 *   - wizard: one control at a time — "Press A", "Pull LT", "Push the left
 *             stick right" — each answered by the first raw input that moves
 *             (gamepadMapping.detectInput), then saved for this pad in this
 *             browser (gamepadMappingsStore).
 *
 * The wizard speaks to the device in hand (gamepadMapping.padKind): a radio
 * is asked for its throttle, a wheel for its brake pedal, each in the order
 * it is held. The targets stay the Xbox controls the host presents; only the
 * order and the words change. The user can correct the kind in the dialog.
 * A radio sends only the channels its model mixes: the dialog says where the
 * profile reads its switches, and which channel to mix when one stays silent.
 *
 * The dialog reads the pad itself; a stream running underneath is paused by
 * the caller (onOpen / onClose) so mapping "A" does not press A in the game.
 */

import { t } from '../i18n/i18n.js';
import { escapeHtml } from '../util/escapeHtml.js';
import { Toast } from './Toast.js';
import { gamepadArtSvg, GamepadArtView } from './GamepadArt.js';
import { deviceArtSvg } from './DeviceArt.js';
import {
    BUTTON_TARGETS,
    PAD_KINDS,
    resolveMapping,
    readVirtualPad,
    loadGamepadDb,
    padKey,
    padName,
    snapshot,
    detectInput,
    isAtRest,
    describeBinding,
    isAutoMapped,
} from '../stream/gamepadMapping.js';
import { getMapping, setMapping, setKind, removeMapping } from '../util/gamepadMappingsStore.js';

/** Wizard order per kind of device: what it has, in the order it is held. */
export const WIZARD_STEPS = {
    // Face, shoulders, the small buttons, clicks, d-pad, sticks.
    gamepad: [
        'a',
        'b',
        'x',
        'y',
        'leftshoulder',
        'rightshoulder',
        'lefttrigger',
        'righttrigger',
        'back',
        'start',
        'guide',
        'leftstick',
        'rightstick',
        'dpup',
        'dpright',
        'dpdown',
        'dpleft',
        'leftx',
        'lefty',
        'rightx',
        'righty',
    ],
    // Mode 2 sticks (yaw, throttle, roll, pitch), the switches for LT and RT,
    // then buttons — the built-in radio profile's layout.
    rc: [
        'leftx',
        'lefty',
        'rightx',
        'righty',
        'lefttrigger',
        'righttrigger',
        'a',
        'b',
        'x',
        'y',
        'leftshoulder',
        'rightshoulder',
        'back',
        'start',
    ],
    // The grip on the right stick (roll, pitch), the twist and the throttle on
    // the left one, as a Mode 2 radio; the trigger, the hat, the buttons.
    flightstick: [
        'rightx',
        'righty',
        'leftx',
        'lefty',
        'righttrigger',
        'dpup',
        'dpright',
        'dpdown',
        'dpleft',
        'a',
        'b',
        'x',
        'y',
        'leftshoulder',
        'rightshoulder',
        'lefttrigger',
        'back',
        'start',
        'leftstick',
        'rightstick',
    ],
    // Racing on a pad: steering on the left stick, accelerator RT, brake LT,
    // the paddles on the bumpers; then the wheel's buttons and d-pad.
    wheel: [
        'leftx',
        'righttrigger',
        'lefttrigger',
        'rightshoulder',
        'leftshoulder',
        'a',
        'b',
        'x',
        'y',
        'back',
        'start',
        'guide',
        'rightstick',
        'leftstick',
        'dpup',
        'dpright',
        'dpdown',
        'dpleft',
    ],
};

/**
 * Steps a kind says in its own words. Its other buttons are "the button that
 * gives the game A"; a wheel's d-pad is a d-pad, worded as on a pad.
 */
const OWN_WORDS = {
    rc: ['leftx', 'lefty', 'rightx', 'righty', 'lefttrigger', 'righttrigger'],
    flightstick: [
        'rightx',
        'righty',
        'leftx',
        'lefty',
        'righttrigger',
        'dpup',
        'dpright',
        'dpdown',
        'dpleft',
    ],
    wheel: ['leftx', 'righttrigger', 'lefttrigger', 'rightshoulder', 'leftshoulder'],
};

/** What the game receives, by target: the labels of an Xbox pad. */
const GAME_NAMES = {
    a: 'A',
    b: 'B',
    x: 'X',
    y: 'Y',
    leftshoulder: 'LB',
    rightshoulder: 'RB',
    lefttrigger: 'LT',
    righttrigger: 'RT',
    back: 'Back',
    start: 'Start',
    guide: 'Guide',
    leftstick: 'L3',
    rightstick: 'R3',
};

/** The prompt for one step, in the device's own words. */
export function stepPrompt(kind, target) {
    if (OWN_WORDS[kind] && OWN_WORDS[kind].includes(target)) {
        return t(`gamepad.remap.steps.${kind}.${target}`);
    }
    if (kind !== 'gamepad' && GAME_NAMES[target]) {
        return t('gamepad.remap.steps.button', { name: GAME_NAMES[target] });
    }
    return t(`gamepad.remap.steps.${target}`);
}

/**
 * Which way a stick step asks to push: +1 for right or down (a standard
 * stick's positive sense), -1 where the device's words say up — a throttle
 * goes up, or forward.
 */
export function stepDir(kind, target) {
    return target === 'lefty' && (kind === 'rc' || kind === 'flightstick') ? -1 : 1;
}

/** Steps where a stick may rest at one end: the throttle's. */
function parkedStep(kind, target) {
    if (kind === 'gamepad') return target === 'lefty' || target === 'righty';
    return target === 'lefty' && kind !== 'wheel';
}

/** Where the radio profile reads each switch and button (BUILTIN_PADS). */
const RC_CHANNELS = {
    lefttrigger: 5,
    righttrigger: 6,
    a: 9,
    b: 10,
    x: 11,
    y: 12,
    leftshoulder: 13,
    rightshoulder: 14,
    back: 15,
    start: 16,
};

/** How long a radio's switch step waits for anything before saying why. */
const UNMIXED_AFTER_MS = 4000;

/**
 * The channel to mix for a radio's step that nothing answers: a radio sends
 * its channels only, so a switch its model leaves unmixed sends nothing, and
 * no wizard or driver can catch it. 0 where there is nothing to mix.
 */
export function unmixedChannel(kind, target) {
    return kind === 'rc' ? RC_CHANNELS[target] || 0 : 0;
}

/** Badge class + label for a resolution source. */
export function sourceBadge(source) {
    const k = source === 'pending' ? 'db' : source || 'none';
    return `<span class="gp-badge gp-badge-${k}">${escapeHtml(t(`gamepad.remap.source.${k}`))}</span>`;
}

/** Connected pads, in browser order. */
export function connectedPads() {
    const list = navigator.getGamepads ? Array.from(navigator.getGamepads()) : [];
    return list.filter((gp) => gp && gp.connected !== false);
}

/** The drawing's part a TV remote's key lights (DeviceArt.remoteArtSvg), or null. */
export function remoteKeyCtl(e) {
    const key = e && e.key;
    if (typeof key !== 'string') return null;
    if (key === 'Enter' && !e.code) return 'ok';
    if (key === 'ChannelUp') return 'chup';
    if (key === 'ChannelDown') return 'chdown';
    if (/^ColorF[0-9]/.test(key)) return 'colour';
    if (/^[0-9]$/.test(key)) return 'digits';
    const arrow = {
        ArrowUp: 'dpup',
        ArrowDown: 'dpdown',
        ArrowLeft: 'dpleft',
        ArrowRight: 'dpright',
    };
    return arrow[key] || null;
}
const REMOTE_PAD_CTLS = ['dpup', 'dpdown', 'dpleft', 'dpright'];
const REMOTE_KEY_CTLS = ['ok', 'chup', 'chdown', 'colour', 'digits', ...REMOTE_PAD_CTLS];

let dbCache; // undefined until loadGamepadDb() answers

/** How MoonlightWeb reads `gp` right now (database loaded on demand). */
export function resolvePad(gp) {
    const res = resolveMapping(gp, { user: getMapping, db: dbCache });
    if (res.source === 'pending') {
        loadGamepadDb().then((db) => {
            dbCache = db;
        });
    }
    return res;
}

/** The on-screen controller's value in the pad picker; a padKey has a colon. */
const TOUCH_PAD_ENTRY = 'touch-pad';

export class GamepadRemapDialog {
    /**
     * @param {{ container?: HTMLElement, padKey?: string|null,
     *           mode?: 'test'|'wizard'|'auto', required?: boolean,
     *           onOpen?: () => void, onClose?: () => void,
     *           onSaved?: (key: string) => void,
     *           onPadChange?: (key: string) => void,
     *           touchPad?: { label: string, active: boolean, onPick: () => void }|null }} [opts]
     *   `mode: 'auto'` opens the wizard for a pad nothing maps, the test view
     *   otherwise. `required` (a guest who cannot join unmapped) closes the
     *   dialog as soon as the mapping is saved. `touchPad` puts the on-screen
     *   controller first in the picker; while it is `active`, no real pad is the
     *   selected one, and none is picked just by being there.
     */
    constructor(opts = {}) {
        this._container = opts.container || document.body;
        this._key = opts.padKey || null;
        this._mode = opts.mode || 'auto';
        this._required = opts.required === true;
        this._onOpen = opts.onOpen || null;
        this._onClose = opts.onClose || null;
        this._onSaved = opts.onSaved || null;
        this._onPadChange = opts.onPadChange || null;
        this._touchPad = opts.touchPad || null;
        this._overlay = null;
        this._raf = null;
        this._padsSig = '';
        this._wiz = null;
        this._onKey = (e) => {
            if (e.key !== 'Escape') return;
            e.preventDefault();
            e.stopPropagation();
            this.close();
        };
        /** Keys held now, as remoteKeyCtl names them (a TV remote's view). */
        this._keysHeld = new Set();
        this._onRemoteKey = (e) => {
            const ctl = remoteKeyCtl(e);
            if (!ctl) return;
            if (e.type === 'keydown') this._keysHeld.add(ctl);
            else this._keysHeld.delete(ctl);
        };
    }

    /** A TV remote's keys that come as keys, lit on its drawing (arrows too, from a keyboard-kind remote). */
    _drawRemoteKeys() {
        for (const ctl of REMOTE_KEY_CTLS) {
            if (this._keysHeld.has(ctl)) this._art.setPressed(ctl, true);
            else if (!REMOTE_PAD_CTLS.includes(ctl)) this._art.setPressed(ctl, false);
        }
    }

    open() {
        if (this._overlay) return;
        if (document.pointerLockElement) document.exitPointerLock();
        loadGamepadDb().then((db) => {
            dbCache = db;
        });

        const overlay = document.createElement('div');
        overlay.className = 'gamepad-remap-overlay';
        overlay.innerHTML = `
            <div class="gamepad-remap" role="dialog" aria-modal="true" aria-labelledby="gamepad-remap-title">
                <div class="gamepad-remap-head">
                    <h3 class="gamepad-remap-title" id="gamepad-remap-title">${escapeHtml(t('gamepad.remap.title'))}</h3>
                    <div class="gamepad-remap-pad"></div>
                </div>
                <div class="gamepad-remap-stage">
                    ${gamepadArtSvg()}
                    <div class="gamepad-remap-empty" hidden>${escapeHtml(t('gamepad.remap.empty'))}</div>
                </div>
                <div class="gamepad-remap-status">
                    <span class="gamepad-remap-prompt"></span>
                    <span class="gamepad-remap-raw"></span>
                </div>
                <div class="gamepad-remap-progress" hidden><span></span></div>
                <p class="gamepad-remap-hint" hidden></p>
                <div class="gamepad-remap-actions"></div>
            </div>`;
        this._container.appendChild(overlay);
        this._overlay = overlay;
        this._art = new GamepadArtView(overlay.querySelector('.gamepad-remap-stage'));
        // The pad until the device in hand is known (_mountArt, from _syncPads).
        this._artKind = 'gamepad';
        /** @type {Object<string, HTMLElement>} */
        this._els = {
            pad: overlay.querySelector('.gamepad-remap-pad'),
            stage: overlay.querySelector('.gamepad-remap-stage'),
            empty: overlay.querySelector('.gamepad-remap-empty'),
            prompt: overlay.querySelector('.gamepad-remap-prompt'),
            raw: overlay.querySelector('.gamepad-remap-raw'),
            progress: overlay.querySelector('.gamepad-remap-progress'),
            bar: overlay.querySelector('.gamepad-remap-progress > span'),
            hint: overlay.querySelector('.gamepad-remap-hint'),
            actions: overlay.querySelector('.gamepad-remap-actions'),
        };
        overlay.addEventListener('click', (e) => {
            if (e.target === overlay) this.close();
        });
        document.addEventListener('keydown', this._onKey, true);
        document.addEventListener('keydown', this._onRemoteKey, true);
        document.addEventListener('keyup', this._onRemoteKey, true);
        if (this._onOpen) this._onOpen();

        this._syncPads(true);
        this._loop();
    }

    close() {
        if (!this._overlay) return;
        if (this._raf !== null) cancelAnimationFrame(this._raf);
        this._raf = null;
        document.removeEventListener('keydown', this._onKey, true);
        document.removeEventListener('keydown', this._onRemoteKey, true);
        document.removeEventListener('keyup', this._onRemoteKey, true);
        this._keysHeld.clear();
        this._overlay.remove();
        this._overlay = null;
        if (this._onClose) this._onClose();
    }

    get isOpen() {
        return !!this._overlay;
    }

    // ── Pads ─────────────────────────────────────────────────────────────

    _touchPadActive() {
        return !!this._touchPad && this._touchPad.active;
    }

    _selectedPad() {
        const pads = connectedPads();
        if (!this._key || this._touchPadActive()) return null;
        return pads.find((gp) => padKey(gp) === this._key) || null;
    }

    /** Rebuild the pad picker when pads come and go. */
    _syncPads(force = false) {
        const pads = connectedPads();
        const sig = pads.map((gp) => `${gp.index}:${gp.id}`).join('|');
        if (!force && sig === this._padsSig) return;
        this._padsSig = sig;

        if (!this._key || !pads.some((gp) => padKey(gp) === this._key)) {
            const first = pads[0];
            const changed = first && padKey(first) !== this._key;
            this._key = first ? padKey(first) : this._key;
            if (changed && this._onPadChange && !this._touchPadActive()) {
                this._onPadChange(this._key);
            }
            this._wiz = null;
        }

        const res =
            pads.length && !this._touchPadActive()
                ? resolvePad(this._selectedPad() || pads[0])
                : null;
        this._mountArt(res ? res.kind : this._artKind);
        // What the device is: deduced, and the user's to correct.
        const remote = !!res && res.kind === 'remote';
        const title = this._overlay && this._overlay.querySelector('.gamepad-remap-title');
        if (title)
            title.textContent = t(remote ? 'gamepad.remap.kind.remote' : 'gamepad.remap.title');
        // A TV remote is no device to correct: it never plays as a pad.
        const kindPicker =
            res && !remote
                ? `<label class="gamepad-remap-kind">
                    <span>${escapeHtml(t('gamepad.remap.kind.label'))}</span>
                    <select class="gamepad-remap-select gamepad-remap-kind-select">
                        ${PAD_KINDS.map(
                            (k) =>
                                `<option value="${k}" ${k === res.kind ? 'selected' : ''}>${escapeHtml(t(`gamepad.remap.kind.${k}`))}</option>`,
                        ).join('')}
                    </select>
                </label>`
                : '';
        if (pads.length > 1 || this._touchPad) {
            const onScreen = this._touchPad
                ? `<option value="${TOUCH_PAD_ENTRY}" ${this._touchPadActive() ? 'selected' : ''}>${escapeHtml(this._touchPad.label)}</option>`
                : '';
            this._els.pad.innerHTML = `
                <select class="gamepad-remap-select" aria-label="${escapeHtml(t('gamepad.remap.pick'))}">
                    ${onScreen}
                    ${pads
                        .map((gp) => {
                            const k = padKey(gp);
                            return `<option value="${escapeHtml(k)}" ${k === this._key && !this._touchPadActive() ? 'selected' : ''}>${escapeHtml(padName(gp))}</option>`;
                        })
                        .join('')}
                </select>
                ${res && !remote ? sourceBadge(res.source) : ''}
                ${kindPicker}`;
            this._els.pad.querySelector('select').addEventListener('change', (e) => {
                const value = /** @type {HTMLSelectElement} */ (e.target).value;
                if (value === TOUCH_PAD_ENTRY) {
                    this._touchPad.active = true;
                    this._touchPad.onPick();
                    this.close();
                    return;
                }
                if (this._touchPad) this._touchPad.active = false;
                this._key = value;
                this._wiz = null;
                if (this._onPadChange) this._onPadChange(this._key);
                this._syncPads(true);
            });
        } else if (pads.length === 1) {
            this._els.pad.innerHTML = `
                <span class="gamepad-remap-name">${escapeHtml(padName(pads[0]))}</span>
                ${remote ? '' : sourceBadge(res.source)}
                ${kindPicker}`;
        } else {
            this._els.pad.innerHTML = '';
        }
        const kindSelect = this._els.pad.querySelector('.gamepad-remap-kind-select');
        if (kindSelect) {
            kindSelect.addEventListener('change', (e) =>
                this._setKind(/** @type {HTMLSelectElement} */ (e.target).value),
            );
        }
        this._enterMode();
    }

    /**
     * Draw the device in hand — the pad, a radio, a flight stick, a wheel —
     * when its kind changes: another device picked, or the kind corrected.
     */
    _mountArt(kind) {
        if (!this._overlay || kind === this._artKind) return;
        const stage = this._els.stage;
        const old = stage.querySelector('svg.gp-art');
        if (old) old.remove();
        stage.insertAdjacentHTML('afterbegin', deviceArtSvg(kind));
        this._art = new GamepadArtView(stage);
        this._artKind = kind;
        // The steps a wizard under way has done, marked on the new drawing.
        if (this._wiz) {
            for (const target of Object.keys(this._wiz.bindings)) this._art.setMapped(target, true);
        }
    }

    /**
     * The user says what the device is. Saved at once — alone, when the
     * layout comes from elsewhere — and a wizard under way starts over in the
     * device's own words and order.
     */
    _setKind(kind) {
        const gp = this._selectedPad();
        if (!gp || !PAD_KINDS.includes(kind)) return;
        setKind(this._key, padName(gp), kind);
        const wizard = !!this._wiz;
        this._wiz = null;
        if (wizard) this._startWizard(gp);
        this._syncPads(true);
    }

    /** Pick the view for the selected pad, and draw its buttons. */
    _enterMode() {
        const gp = this._selectedPad();
        if (!gp) {
            this._view = 'empty';
        } else if (this._wiz) {
            this._view = 'wizard';
        } else {
            const res = resolvePad(gp);
            const unmapped = res.source === null;
            const remote = res.kind === 'remote';
            if (!remote && (this._mode === 'wizard' || (this._mode === 'auto' && unmapped))) {
                this._startWizard(gp);
                return;
            }
            this._view = 'test';
        }
        this._renderStatic();
    }

    // ── Wizard ───────────────────────────────────────────────────────────

    _startWizard(gp) {
        const kind = resolvePad(gp).kind;
        this._wiz = {
            kind,
            steps: WIZARD_STEPS[kind] || WIZARD_STEPS.gamepad,
            idx: 0,
            bindings: {},
            base: snapshot(gp),
            // Whatever is held when the wizard starts must be let go first.
            waitRelease: false,
            caught: null,
        };
        this._view = 'wizard';
        this._art.clearMarks();
        this._renderStatic();
    }

    _wizardFrame(gp) {
        const w = this._wiz;
        if (w.idx >= w.steps.length) return;
        const target = w.steps[w.idx];
        if (w.waitRelease) {
            if (isAtRest(w.base, gp)) {
                w.waitRelease = false;
                this._renderStatic();
            }
            return;
        }
        // Each step's own clock, for a radio's switch that stays silent.
        const now = performance.now();
        if (w.stepIdx !== w.idx) {
            w.stepIdx = w.idx;
            w.stepAt = now;
            w.silent = false;
        }
        const exclude = Object.values(w.bindings);
        const b = detectInput(w.base, gp, target, exclude, stepDir(w.kind, target));
        if (!b) {
            if (!w.silent && unmixedChannel(w.kind, target) && now - w.stepAt > UNMIXED_AFTER_MS) {
                w.silent = true;
                this._renderStatic();
            }
            return;
        }
        w.bindings[target] = b;
        w.caught = b;
        this._art.setMapped(target, true);
        w.idx++;
        w.waitRelease = true;
        this._renderStatic();
    }

    _wizardBack() {
        const w = this._wiz;
        if (!w || w.idx === 0) return;
        w.idx--;
        const target = w.steps[w.idx];
        delete w.bindings[target];
        this._art.setMapped(target, false);
        w.caught = null;
        w.waitRelease = false;
        this._renderStatic();
    }

    _wizardSkip() {
        const w = this._wiz;
        if (!w || w.idx >= w.steps.length) return;
        delete w.bindings[w.steps[w.idx]];
        w.idx++;
        w.caught = null;
        w.waitRelease = false;
        this._renderStatic();
    }

    /**
     * Straight to the sticks, keeping what is bound so far: a device taken
     * for a pad that has none of the seventeen buttons before them to give.
     * The other kinds start with their sticks.
     */
    _wizardSkipToSticks() {
        const w = this._wiz;
        const sticks = w ? w.steps.indexOf('leftx') : -1;
        if (!w || w.idx >= sticks) return;
        for (let i = w.idx; i < sticks; i++) delete w.bindings[w.steps[i]];
        w.idx = sticks;
        w.caught = null;
        w.waitRelease = false;
        this._renderStatic();
    }

    _save() {
        const gp = this._selectedPad();
        const w = this._wiz;
        if (!gp || !w) return;
        setMapping(this._key, padName(gp), w.bindings, w.kind);
        this._wiz = null;
        this._art.clearMarks();
        Toast.success(t('gamepad.remap.saved', { name: padName(gp) }));
        if (this._onSaved) this._onSaved(this._key);
        if (this._required) {
            this.close();
            return;
        }
        this._mode = 'test';
        this._syncPads(true);
    }

    _reset() {
        if (!this._key) return;
        removeMapping(this._key);
        this._wiz = null;
        this._mode = 'auto';
        if (this._onSaved) this._onSaved(this._key);
        this._syncPads(true);
    }

    // ── Rendering ────────────────────────────────────────────────────────

    /** Prompt, hint and buttons: redrawn on each change of step or view. */
    _renderStatic() {
        const els = this._els;
        const gp = this._selectedPad();
        els.stage.classList.toggle('is-empty', this._view === 'empty');
        els.empty.hidden = this._view !== 'empty';
        els.progress.hidden = this._view !== 'wizard';
        els.raw.textContent = '';
        els.raw.classList.remove('is-caught');
        this._art.setTarget(null);

        const btn = (cls, key, variant = 'btn-secondary') =>
            `<button type="button" class="btn ${variant} ${cls}">${escapeHtml(t(key))}</button>`;
        let hint = '';
        let actions = '';

        if (this._view === 'empty') {
            els.prompt.textContent = t('gamepad.remap.empty');
            actions = `<span class="gp-spacer"></span>${btn('gp-close', 'gamepad.remap.close')}`;
        } else if (this._view === 'test' && resolvePad(gp).kind === 'remote') {
            // A TV remote: what its keys do, nothing to remap.
            els.prompt.textContent = t('gamepad.remap.remotePrompt');
            hint = t('gamepad.remap.remoteHint');
            actions = `<span class="gp-spacer"></span>${btn('gp-close', 'gamepad.remap.close', 'btn-save')}`;
        } else if (this._view === 'test') {
            const res = resolvePad(gp);
            els.prompt.textContent = t(
                res.source ? 'gamepad.remap.testPrompt' : 'gamepad.remap.unknown',
            );
            if (isAutoMapped(res.source)) hint = t('gamepad.remap.guessed');
            // A radio laid out by its profile: where its switches must be mixed.
            if (res.kind === 'rc' && res.source === 'builtin') {
                hint = `${hint} ${t('gamepad.remap.rcMixHint')}`;
            }
            // The user's own layout, or only the kind they picked: either
            // way, there is something to go back from.
            const own = this._key ? getMapping(this._key) : null;
            actions =
                (own ? btn('gp-reset', 'gamepad.remap.reset') : '') +
                `<span class="gp-spacer"></span>` +
                btn('gp-close', 'gamepad.remap.close') +
                btn(
                    'gp-remap',
                    res.source ? 'gamepad.remap.remap' : 'gamepad.remap.start',
                    'btn-save',
                );
        } else {
            const w = this._wiz;
            const done = w.idx >= w.steps.length;
            els.bar.style.width = `${Math.round((w.idx / w.steps.length) * 100)}%`;
            if (done) {
                els.prompt.textContent = t('gamepad.remap.done');
            } else if (w.waitRelease) {
                els.prompt.textContent = t('gamepad.remap.release');
            } else {
                const target = w.steps[w.idx];
                els.prompt.innerHTML = `<span class="gamepad-remap-step">${w.idx + 1}/${w.steps.length}</span> · ${escapeHtml(stepPrompt(w.kind, target))}`;
                this._art.setTarget(target);
                if (target === 'guide') hint = t('gamepad.remap.guideHint');
                // A throttle can only move away from where it rests.
                if (parkedStep(w.kind, target)) hint = t('gamepad.remap.parkedHint');
                // A radio's switch that sends nothing: its model does not mix it.
                if (w.silent && w.stepIdx === w.idx) {
                    hint = t('gamepad.remap.rcUnmixedHint', {
                        ch: unmixedChannel(w.kind, target),
                    });
                }
            }
            if (w.caught) {
                els.raw.textContent = describeBinding(w.caught);
                els.raw.classList.add('is-caught');
            }
            // Only a pad's wizard has buttons to skip before its sticks.
            const beforeSticks = w.kind === 'gamepad' && w.idx < w.steps.indexOf('leftx');
            actions =
                btn('gp-back', 'gamepad.remap.back') +
                (done ? '' : btn('gp-skip', 'gamepad.remap.skip')) +
                (beforeSticks ? btn('gp-sticks', 'gamepad.remap.skipToSticks') : '') +
                `<span class="gp-spacer"></span>` +
                btn('gp-close', 'gamepad.remap.cancel') +
                (done ? btn('gp-save', 'gamepad.remap.save', 'btn-save') : '');
        }

        els.hint.hidden = !hint;
        els.hint.textContent = hint;
        els.actions.innerHTML = actions;
        const on = (cls, fn) => {
            const b = els.actions.querySelector(`.${cls}`);
            if (b) b.addEventListener('click', fn);
        };
        on('gp-close', () => this.close());
        on('gp-remap', () => gp && this._startWizard(gp));
        on('gp-reset', () => this._reset());
        on('gp-back', () => this._wizardBack());
        on('gp-skip', () => this._wizardSkip());
        on('gp-sticks', () => this._wizardSkipToSticks());
        on('gp-save', () => this._save());
        const back = els.actions.querySelector('.gp-back');
        if (back)
            /** @type {HTMLButtonElement} */ (back).disabled = !this._wiz || this._wiz.idx === 0;
    }

    _loop() {
        if (!this._overlay) return;
        this._syncPads();
        const gp = this._selectedPad();
        if (gp && this._view === 'wizard' && this._wiz) {
            this._wizardFrame(gp);
            this._art.render(readVirtualPad(gp, this._wiz.bindings), BUTTON_TARGETS);
        } else if (gp && this._view === 'test') {
            const res = resolvePad(gp);
            if (res.source !== this._lastSource) {
                // The database answered, or the mapping changed elsewhere.
                this._lastSource = res.source;
                this._syncPads(true);
            }
            if (res.kind === 'remote') {
                this._art.render(gp, BUTTON_TARGETS);
                this._drawRemoteKeys();
            } else if (res.source === 'pending') {
                this._art.clear();
            } else if (res.bindings) {
                this._art.render(readVirtualPad(gp, res.bindings), BUTTON_TARGETS);
            } else if (res.source) {
                this._art.render(gp, BUTTON_TARGETS);
            } else {
                this._art.clear();
            }
        } else if (gp && this._view === 'empty') {
            this._syncPads(true);
        } else {
            this._art.clear();
        }
        this._raf = requestAnimationFrame(() => this._loop());
    }
}
