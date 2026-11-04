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
 */

/**
 * MoonlightWeb — the invited player's page (/p/<token>)
 *
 * This is the whole app for a guest: no host list, no settings, no account.
 * The link alone proves nothing — the 6-digit PIN, sent to them separately, is
 * asked for before anything is shown, including the machine's name. Once it
 * checks out the browser holds a cookie bound to that one invitation, so a
 * reload does not ask again.
 *
 * Screens: pin → join → (stream) → ended | dead.
 */
import { BackendClient } from '../api/BackendClient.js';
import { t } from '../i18n/i18n.js';
import { escapeHtml } from '../util/escapeHtml.js';
import { IS_MOBILE_OR_TABLET, IS_TOUCH_DEVICE, hevcClientDecodes } from '../util/BrowserDetect.js';
import { PlayerArt } from './PlayerArt.js';
import { noticeHtml } from './PrivacyNotice.js';
import {
    GamepadRemapDialog,
    connectedPads,
    resolvePad,
    sourceBadge,
} from './GamepadRemapDialog.js';
import { padKey, padName, isAutoMapped } from '../stream/gamepadMapping.js';

const HEIGHTS = [720, 1080, 1440];

/**
 * The guest's input preferences, kept in this browser. A guest has no account
 * and no server-side settings, but they do come back to the same link — and
 * re-picking "trackpad" on every join gets old fast.
 */
const PREFS_KEY = 'mw_player_prefs';

function loadPrefs() {
    try {
        const raw = JSON.parse(localStorage.getItem(PREFS_KEY) || '{}');
        return {
            // Gaming mode captures the mouse and the whole keyboard; that is a
            // lot to do to a guest without asking, so it is opt-in. `immersive`
            // is what this field was called before the mode was renamed — read
            // it so a returning guest keeps the choice they already made.
            gaming: raw.gaming === true || raw.immersive === true,
            touchScreen: raw.touchScreen === true,
            // The controller picked last time (gamepadMapping.padKey), taken
            // again when it is plugged in.
            padKey: typeof raw.padKey === 'string' ? raw.padKey : null,
            // 'always' once they picked the on-screen controller in their dialog.
            touchPadMode: raw.touchPadMode === 'always' ? 'always' : 'auto',
        };
    } catch (_) {
        return { gaming: false, touchScreen: false, padKey: null, touchPadMode: 'auto' };
    }
}

function savePrefs(prefs) {
    try {
        localStorage.setItem(PREFS_KEY, JSON.stringify(prefs));
    } catch (_) {
        // Private mode with storage disabled: the choice just won't be remembered.
    }
}

/** What a guest last chose between the on-screen controller and a real pad. */
export function guestTouchPadMode() {
    return loadPrefs().touchPadMode;
}

/** @param {'always'|'auto'} mode */
export function rememberGuestTouchPadMode(mode) {
    savePrefs({ ...loadPrefs(), touchPadMode: mode });
}

export class PlayerJoinView {
    /**
     * @param {HTMLElement} container
     * @param {string} token the share token from the URL
     * @param {(info: {height: number, gaming: boolean, touchScreen: boolean, padKey: string|null}, transportIndex?: number, codec?: string) => Promise<void>} onJoin
     */
    constructor(container, token, onJoin) {
        this.container = container;
        this.token = token;
        this.onJoin = onJoin;
        this.info = null;
        this._height = 1080;
        this._prefs = loadPrefs();
        this._padWatch = null;
        this._padSig = '';
        this._joinLabel = '';
        this._enterKey = null;
    }

    async start() {
        this._renderLoading();
        await this.refresh();
    }

    async refresh() {
        try {
            this.info = await BackendClient.playerInfo(this.token);
        } catch (err) {
            // A live link whose PIN another browser already spent: most often
            // this same guest in a private tab, so say where the invitation is.
            if (err?.responseBody?.error === 'bound_elsewhere') {
                this._renderBoundElsewhere();
                return;
            }
            // 404 = the link is dead or was never real. Anything else (offline,
            // 500) is shown the same way: from here they are equally unusable,
            // and the page is not the place to debug a server.
            this._renderDead();
            return;
        }
        if (this.info.needs_pin) {
            this._renderPin();
            return;
        }
        this._renderJoin();
    }

    // ── Screens ─────────────────────────────────────────────────────────────

    _shell(inner, artKey) {
        this._stopPadWatch();
        this._stopEnterKey();
        this.container.innerHTML = `
            <div class="player-page">
                ${artKey ? `<div class="player-art">${PlayerArt[artKey]}</div>` : ''}
                <div class="player-card">${inner}</div>
                <button class="player-privacy-btn" type="button" id="player-privacy-btn"
                        aria-expanded="false">${escapeHtml(t('stats.cookiesButton'))}</button>
            </div>
        `;
        const btn = this.container.querySelector('#player-privacy-btn');
        if (btn) btn.addEventListener('click', () => this._togglePrivacy(btn));
    }

    /**
     * What this page keeps, and what the machine behind it counts.
     *
     * A guest has no account and no Settings page, so this corner button is
     * their only way to find out — and their session IS one of the ones
     * counted. They cannot change the answer: it belongs to the machine's
     * owner. Telling them anyway is the point; a disclosure only the person
     * who benefits from it can read is not one.
     *
     * Nothing here gates the page. It opens on demand and closes again, and no
     * button on it has to be pressed before joining.
     */
    _togglePrivacy(btn) {
        const open = this.container.querySelector('.player-privacy-panel');
        if (open) {
            open.remove();
            btn.setAttribute('aria-expanded', 'false');
            return;
        }

        // The join page may not have loaded yet (or the link is dead): the
        // browser-storage half is always true, the census half needs the info.
        const reporting = !!(this.info && this.info.stats_reporting);
        const panel = document.createElement('div');
        panel.className = 'player-privacy-panel';
        panel.setAttribute('role', 'region');
        panel.setAttribute('aria-label', t('stats.playerTitle'));
        panel.innerHTML = `
            <strong class="player-privacy-title">${escapeHtml(t('stats.playerTitle'))}</strong>
            <p>${escapeHtml(t('stats.playerCookies'))}</p>
            ${
                this.info
                    ? `<p>${escapeHtml(t(reporting ? 'stats.playerStatsOn' : 'stats.playerStatsOff'))}</p>
                       ${reporting ? noticeHtml() : ''}`
                    : ''
            }
            <button class="btn btn-neutral player-privacy-close" type="button">
                ${escapeHtml(t('common.close'))}
            </button>`;
        btn.parentElement.appendChild(panel);
        btn.setAttribute('aria-expanded', 'true');
        panel
            .querySelector('.player-privacy-close')
            .addEventListener('click', () => this._togglePrivacy(btn));
    }

    _renderLoading() {
        this._shell(`<p class="player-muted">${escapeHtml(t('player.loading'))}</p>`);
    }

    _renderDead() {
        this._shell(
            `
            <h1>${escapeHtml(t('player.deadTitle'))}</h1>
            <p>${escapeHtml(t('player.deadBody'))}</p>
            <p class="player-muted">${escapeHtml(t('player.deadHint'))}</p>
        `,
            'void',
        );
    }

    /** The invitation is alive, but tied to the browser that entered its PIN. */
    _renderBoundElsewhere() {
        this._shell(
            `
            <h1>${escapeHtml(t('player.boundTitle'))}</h1>
            <p>${escapeHtml(t('player.boundBody'))}</p>
            <p class="player-muted">${escapeHtml(t('player.boundHint'))}</p>
        `,
            'void',
        );
    }

    /** The owner stopped the session mid-stream (or before we could join). */
    renderEnded() {
        this._shell(
            `
            <h1>${escapeHtml(t('player.endedTitle'))}</h1>
            <p>${escapeHtml(t('player.endedBody'))}</p>
            <p class="player-muted">${escapeHtml(t('player.endedHint'))}</p>
        `,
            'unplugged',
        );
    }

    _renderPin() {
        this._shell(`
            <h1>${escapeHtml(t('player.pinTitle'))}</h1>
            <p>${escapeHtml(t('player.pinBody'))}</p>
            <form class="player-pin-form" autocomplete="off">
                <input class="player-pin-input" type="text" inputmode="numeric"
                       pattern="[0-9]*" maxlength="6" autofocus
                       aria-label="${escapeHtml(t('player.pinTitle'))}">
                <button class="btn btn-primary" type="submit">${escapeHtml(t('player.pinSubmit'))}</button>
            </form>
            <p class="player-error" hidden></p>
        `);

        const form = /** @type {HTMLFormElement} */ (
            this.container.querySelector('.player-pin-form')
        );
        const input = /** @type {HTMLInputElement} */ (
            this.container.querySelector('.player-pin-input')
        );
        const err = /** @type {HTMLElement} */ (this.container.querySelector('.player-error'));

        input.addEventListener('input', () => {
            input.value = input.value.replace(/\D/g, '').slice(0, 6);
            // All six typed on a phone: put the keyboard away, so the button
            // is in plain sight instead of under it.
            if (input.value.length === 6 && window.matchMedia?.('(pointer: coarse)').matches) {
                input.blur();
            }
        });

        form.addEventListener('submit', async (e) => {
            e.preventDefault();
            if (input.value.length !== 6) return;
            err.hidden = true;
            input.disabled = true;

            try {
                await BackendClient.playerPin(this.token, input.value);
                await this.refresh();
            } catch (ex) {
                const body = ex && ex.responseBody;
                const lockout = body && body.lockout_seconds;
                // The code was right, but this invitation already answers to
                // another machine. Not a wrong PIN, and telling them so is what
                // stops them retyping it ten times — those attempts would not
                // count anyway, and the message would stay just as wrong.
                err.textContent =
                    body && body.error === 'already_bound'
                        ? t('player.alreadyBound')
                        : lockout
                          ? t('player.pinLocked', { seconds: lockout })
                          : t('player.pinWrong');
                err.hidden = false;
                input.disabled = false;
                input.value = '';
                input.focus();
            }
        });
    }

    /**
     * How the guest wants to drive the stream, remembered between visits. Each
     * row is only offered where it means something: pointer capture needs a
     * pointer, and the trackpad/touch model needs a touchscreen. A touch laptop
     * legitimately gets both.
     */
    _inputToggles() {
        // Both toggles only shape how the mouse and keyboard reach the host, so
        // they mean nothing to a viewer (no input) or a gamepad-only guest. Same
        // on mobile: the touch model drives the pointer, which they can't use.
        // "desktop" is keyboard and mouse without a gamepad — every bit as
        // entitled to these rows as "full", and it used to lose them both.
        const level = this.info.access_level;
        if (level !== 'full' && level !== 'desktop') return '';

        // The buttons carry the answer, not the question: "Desktop | Gaming"
        // reads as two unrelated words until something above them says they are
        // both mouse modes. `hints` is optional and only worth passing where
        // hovering exists — a guest has no settings page to go read.
        const row = (key, labelKey, offKey, onKey, on, hints) => {
            const hint = (which) => (hints ? ` title="${escapeHtml(t(hints[which]))}"` : '');
            const labelId = `player-toggle-${key}-label`;
            return `
            <div class="player-toggle" data-pref="${key}">
                <span class="player-field-label" id="${labelId}">${escapeHtml(t(labelKey))}</span>
                <div class="player-toggle-choice" role="group"
                     aria-labelledby="${labelId}">
                    <button class="btn player-toggle-btn ${on ? '' : 'is-selected'}"
                            type="button" data-value="off"${hint('off')}>${escapeHtml(t(offKey))}</button>
                    <button class="btn player-toggle-btn ${on ? 'is-selected' : ''}"
                            type="button" data-value="on"${hint('on')}>${escapeHtml(t(onKey))}</button>
                </div>
            </div>
        `;
        };

        return `
            <div class="player-toggles">
                ${
                    IS_MOBILE_OR_TABLET
                        ? ''
                        : row(
                              'gaming',
                              'player.mouseMode',
                              'player.desktop',
                              'player.gaming',
                              this._prefs.gaming,
                              {
                                  off: 'player.desktopHint',
                                  on: 'player.gamingHint',
                              },
                          )
                }
                ${
                    IS_TOUCH_DEVICE
                        ? row(
                              'touchScreen',
                              'player.touchMode',
                              'player.trackpad',
                              'player.touch',
                              this._prefs.touchScreen,
                          )
                        : ''
                }
            </div>
        `;
    }

    _wireInputToggles() {
        this.container.querySelectorAll('.player-toggle').forEach((row) => {
            const key = /** @type {HTMLElement} */ (row).dataset.pref;
            row.querySelectorAll('.player-toggle-btn').forEach((b) => {
                b.addEventListener('click', () => {
                    this._prefs[key] = /** @type {HTMLElement} */ (b).dataset.value === 'on';
                    savePrefs(this._prefs);
                    row.querySelectorAll('.player-toggle-btn').forEach((o) =>
                        o.classList.remove('is-selected'),
                    );
                    b.classList.add('is-selected');
                });
            });
        });
    }

    _renderJoin() {
        const machine = this.info.machine_name || t('player.thisPc');
        const busy = this.info.state === 'streaming';
        // Nothing up on the bound host is not a dead end: the guest's arrival is
        // what starts the app the owner picked when they opened the row. Say so,
        // because pressing Join then wakes a machine nobody is sitting at.
        const app = this.info.app_name || '';
        const willLaunch = this.info.cold_start === true;
        // A native host's guests watch one picture, at the height its owner
        // picked: nothing to choose here, only to know.
        const fixedHeight = [720, 1080, 1440].includes(Number(this.info.feed_height))
            ? Number(this.info.feed_height)
            : 0;
        if (fixedHeight) this._height = fixedHeight;

        this._shell(`
            <h1>${escapeHtml(t('player.joinTitle', { machine }))}</h1>
            <p class="player-access">${escapeHtml(t(`player.access.${this.info.access_level}`))}</p>

            ${
                busy
                    ? `<p class="player-error">${escapeHtml(t('player.busy'))}</p>`
                    : `
                ${
                    willLaunch
                        ? `<p class="player-hint">${escapeHtml(
                              app ? t('player.willLaunchApp', { app }) : t('player.willLaunch'),
                          )}</p>`
                        : ''
                }
                ${
                    fixedHeight
                        ? `<p class="player-hint player-quality-fixed">${escapeHtml(
                              t('player.qualityFixed', { height: fixedHeight }),
                          )}</p>`
                        : `
                <div class="player-quality">
                    <span class="player-field-label" id="player-quality-label"
                          >${escapeHtml(t('player.quality'))}</span>
                    <div class="player-quality-choice" role="group"
                         aria-labelledby="player-quality-label">
                        ${HEIGHTS.map(
                            (h) => `
                            <button class="btn player-quality-btn ${h === this._height ? 'is-selected' : ''}"
                                    type="button" data-height="${h}">${h}p</button>
                        `,
                        ).join('')}
                    </div>
                </div>`
                }
                ${this._inputToggles()}
                ${this._gamepadAllowed() ? '<div class="player-gamepad"></div>' : ''}
                <button class="btn btn-open player-join-btn" type="button">
                    ${escapeHtml(willLaunch ? t('player.launchButton') : t('player.joinButton'))}
                </button>
            `
            }
            <p class="player-error player-join-error" hidden></p>
            ${
                // Only worth offering while the guest is waiting on someone
                // else to let go of the screen. With a Join button on screen it
                // was a second button that did nothing they wanted.
                busy
                    ? `<button class="btn btn-secondary player-refresh" type="button">${escapeHtml(t('player.checkAgain'))}</button>`
                    : ''
            }
        `);

        this.container.querySelectorAll('.player-quality-btn').forEach((b) => {
            b.addEventListener('click', () => {
                this._height = Number(/** @type {HTMLElement} */ (b).dataset.height);
                this.container
                    .querySelectorAll('.player-quality-btn')
                    .forEach((o) => o.classList.remove('is-selected'));
                b.classList.add('is-selected');
            });
        });

        this._wireInputToggles();

        const refreshBtn = this.container.querySelector('.player-refresh');
        if (refreshBtn) refreshBtn.addEventListener('click', () => this.refresh());

        const joinBtn = this.container.querySelector('.player-join-btn');
        if (!joinBtn) return;
        this._joinLabel = willLaunch ? t('player.launchButton') : t('player.joinButton');
        this._startPadWatch();
        joinBtn.addEventListener('click', async () => {
            // An unknown controller is mapped first: the guest would otherwise
            // walk in with a pad the game never hears.
            if (this._padNeedsMap()) {
                this._openPadDialog(true);
                return;
            }
            const err = /** @type {HTMLElement} */ (
                this.container.querySelector('.player-join-error')
            );
            this._stopPadWatch();
            /** @type {HTMLButtonElement} */ (joinBtn).disabled = true;
            joinBtn.textContent = t('player.joining');
            err.hidden = true;
            try {
                // The stream the guests share is HEVC unless one of them cannot
                // decode it. A browser that cannot says so now: the stream goes
                // H.264 for everyone once, instead of this guest failing its
                // first picture and coming back.
                const codec =
                    this.info.shared_feed === true && !(await hevcClientDecodes())
                        ? 'h264'
                        : undefined;
                await this.onJoin(
                    {
                        height: this._height,
                        // No aspect: a guest cannot know the host's format, and
                        // its own monitor is irrelevant — the backend hands it the
                        // ratio the owner's session already settled on for that
                        // host.
                        gaming: this._prefs.gaming,
                        touchScreen: this._prefs.touchScreen,
                        padKey: this._gamepadAllowed() ? this._selectedPadKey() : null,
                    },
                    0,
                    codec,
                );
            } catch (ex) {
                const code = ex && ex.responseBody && ex.responseBody.error;
                if (code === 'session_ended') {
                    this.renderEnded();
                    return;
                }
                // An invitation opened cold names the app the guest's arrival
                // launches. Between the link being sent and this click, that app
                // can have been removed from the host — say so, rather than
                // leaving them with a generic failure they cannot act on.
                err.textContent =
                    code === 'stream_in_progress'
                        ? t('player.busy')
                        : code === 'app_unavailable'
                          ? t('player.appGone')
                          : t('player.joinFailed');
                err.hidden = false;
                /** @type {HTMLButtonElement} */ (joinBtn).disabled = false;
                joinBtn.textContent = t('player.joinButton');
                this._startPadWatch();
            }
        });
        this._startEnterKey(/** @type {HTMLButtonElement} */ (joinBtn));
    }

    /**
     * Enter on the join screen presses Join. The PIN was just typed on the
     * keyboard, and reaching for the mouse for the one button on the page is
     * a step nobody needs. A focused control (a quality button, the controller
     * list, the privacy corner) keeps Enter for itself, and so does an open
     * dialog; the button itself already answers Enter when it has the focus.
     */
    _startEnterKey(joinBtn) {
        this._stopEnterKey();
        const onKey = (e) => {
            if (!joinBtn.isConnected) {
                // The stream replaced the page: this screen is over.
                this._stopEnterKey();
                return;
            }
            if (e.key !== 'Enter' || e.repeat || e.isComposing || e.defaultPrevented) return;
            if (e.altKey || e.ctrlKey || e.metaKey || e.shiftKey) return;
            if (joinBtn.disabled) return;
            const active = document.activeElement;
            if (
                active &&
                active !== document.body &&
                active.closest('button, a[href], input, select, textarea, [contenteditable]')
            ) {
                return;
            }
            if (document.querySelector('[aria-modal="true"]')) return;
            e.preventDefault();
            joinBtn.click();
        };
        this._enterKey = onKey;
        document.addEventListener('keydown', onKey);
    }

    _stopEnterKey() {
        if (!this._enterKey) return;
        document.removeEventListener('keydown', this._enterKey);
        this._enterKey = null;
    }

    // ── Controller (invitations that allow a gamepad) ─────────────────────
    //
    // One controller per invitation: the guest picks it here when several are
    // plugged in, and only that one reaches the host. A controller nothing
    // maps must be mapped before joining — the mapping is kept in this browser
    // for every later visit, so this is asked once per controller.

    _gamepadAllowed() {
        return !!(this.info && this.info.permissions && this.info.permissions.gamepad === true);
    }

    /** The pad to use: the one picked (if plugged in), else the first. */
    _selectedPadKey() {
        const keys = connectedPads().map((gp) => padKey(gp));
        if (this._prefs.padKey && keys.includes(this._prefs.padKey)) return this._prefs.padKey;
        return keys[0] || null;
    }

    _selectedPad() {
        const key = this._selectedPadKey();
        return connectedPads().find((gp) => padKey(gp) === key) || null;
    }

    _padNeedsMap() {
        if (!this._gamepadAllowed()) return false;
        const gp = this._selectedPad();
        return !!gp && resolvePad(gp).source === null;
    }

    /**
     * Keep the field in step with what is plugged in. Chrome only reveals a
     * pad after a button press, so this polls rather than waiting for events.
     */
    _startPadWatch() {
        if (!this._gamepadAllowed() || this._padWatch) return;
        this._padSig = '';
        const tick = () => this._renderPadField();
        this._padWatch = { timer: setInterval(tick, 400), tick };
        window.addEventListener('gamepadconnected', tick);
        window.addEventListener('gamepaddisconnected', tick);
        tick();
    }

    _stopPadWatch() {
        if (!this._padWatch) return;
        clearInterval(this._padWatch.timer);
        window.removeEventListener('gamepadconnected', this._padWatch.tick);
        window.removeEventListener('gamepaddisconnected', this._padWatch.tick);
        this._padWatch = null;
    }

    _renderPadField() {
        const field = /** @type {HTMLElement} */ (this.container.querySelector('.player-gamepad'));
        const joinBtn = /** @type {HTMLButtonElement} */ (
            this.container.querySelector('.player-join-btn')
        );
        if (!field) return;
        const pads = connectedPads();
        const key = this._selectedPadKey();
        const gp = this._selectedPad();
        const res = gp ? resolvePad(gp) : null;
        const sig = pads.map((p) => p.id).join('|') + '#' + key + '#' + (res ? res.source : '');
        if (sig === this._padSig) return;
        this._padSig = sig;

        let body;
        if (!pads.length) {
            body = `<p class="player-gamepad-empty">${escapeHtml(t('player.gamepadNone'))}</p>`;
        } else {
            const control =
                pads.length > 1
                    ? `<select class="gamepad-remap-select player-gamepad-select" aria-labelledby="player-gamepad-label">
                        ${pads
                            .map((p) => {
                                const k = padKey(p);
                                return `<option value="${escapeHtml(k)}" ${k === key ? 'selected' : ''}>${escapeHtml(padName(p))}</option>`;
                            })
                            .join('')}
                       </select>`
                    : `<span class="gamepad-remap-name">${escapeHtml(padName(pads[0]))}</span>`;
            const guessed = res && isAutoMapped(res.source);
            body = `
                <div class="player-gamepad-row">${control}${res ? sourceBadge(res.source) : ''}</div>
                ${res && res.source === null ? `<p class="player-hint">${escapeHtml(t('player.gamepadNeedsMap'))}</p>` : ''}
                ${
                    res && res.source
                        ? `<button type="button" class="player-gamepad-check">${escapeHtml(
                              t(guessed ? 'player.gamepadCheck' : 'gamepad.remap.remap'),
                          )}</button>`
                        : ''
                }`;
        }
        field.innerHTML = `
            <span class="player-field-label" id="player-gamepad-label">${escapeHtml(t('player.gamepad'))}</span>
            ${body}`;

        const select = /** @type {HTMLSelectElement} */ (field.querySelector('select'));
        if (select) {
            select.addEventListener('change', () => {
                this._prefs.padKey = select.value;
                savePrefs(this._prefs);
                this._renderPadField();
            });
        }
        const check = field.querySelector('.player-gamepad-check');
        if (check) check.addEventListener('click', () => this._openPadDialog(false));

        if (joinBtn && !joinBtn.disabled) {
            joinBtn.textContent =
                res && res.source === null ? t('player.gamepadConfigure') : this._joinLabel;
        }
    }

    _openPadDialog(required) {
        const dialog = new GamepadRemapDialog({
            padKey: this._selectedPadKey(),
            mode: 'auto',
            required,
            onPadChange: (k) => {
                this._prefs.padKey = k;
                savePrefs(this._prefs);
            },
            onClose: () => {
                this._padSig = '';
                this._renderPadField();
            },
        });
        dialog.open();
    }
}
