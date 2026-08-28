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
import { IS_MOBILE_OR_TABLET, IS_TOUCH_DEVICE } from '../util/BrowserDetect.js';
import { PlayerArt } from './PlayerArt.js';
import { noticeHtml } from './PrivacyNotice.js';

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
            // Immersive captures the mouse and the whole keyboard; that is a lot
            // to do to a guest without asking, so it is opt-in.
            immersive: raw.immersive === true,
            touchScreen: raw.touchScreen === true,
        };
    } catch (_) {
        return { immersive: false, touchScreen: false };
    }
}

function savePrefs(prefs) {
    try {
        localStorage.setItem(PREFS_KEY, JSON.stringify(prefs));
    } catch (_) {
        // Private mode with storage disabled: the choice just won't be remembered.
    }
}

export class PlayerJoinView {
    /**
     * @param {HTMLElement} container
     * @param {string} token the share token from the URL
     * @param {(info: {height: number, immersive: boolean, touchScreen: boolean}) => Promise<void>} onJoin
     */
    constructor(container, token, onJoin) {
        this.container = container;
        this.token = token;
        this.onJoin = onJoin;
        this.info = null;
        this._height = 1080;
        this._prefs = loadPrefs();
    }

    async start() {
        this._renderLoading();
        await this.refresh();
    }

    async refresh() {
        try {
            this.info = await BackendClient.playerInfo(this.token);
        } catch (err) {
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
        if (this.info.access_level !== 'full') return '';

        // The two choices name themselves — "Trackpad | Touch" needs no label
        // saying "Touch" above it. The group keeps one for screen readers.
        // `hints` is optional and only worth passing where hovering exists:
        // "Seamless" and "Immersive" say nothing about the mouse escaping the
        // frame, and a guest has no settings page to go read.
        const row = (key, labelKey, offKey, onKey, on, hints) => {
            const hint = (which) => (hints ? ` title="${escapeHtml(t(hints[which]))}"` : '');
            return `
            <div class="player-toggle" data-pref="${key}">
                <div class="player-toggle-choice" role="group"
                     aria-label="${escapeHtml(t(labelKey))}">
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
                              'immersive',
                              'player.desktopMode',
                              'player.seamless',
                              'player.immersive',
                              this._prefs.immersive,
                              {
                                  off: 'player.seamlessHint',
                                  on: 'player.immersiveHint',
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
                <div class="player-quality" role="group" aria-label="${escapeHtml(t('player.quality'))}">
                    ${HEIGHTS.map(
                        (h) => `
                        <button class="btn player-quality-btn ${h === this._height ? 'is-selected' : ''}"
                                type="button" data-height="${h}">${h}p</button>
                    `,
                    ).join('')}
                </div>
                ${this._inputToggles()}
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
        joinBtn.addEventListener('click', async () => {
            const err = /** @type {HTMLElement} */ (
                this.container.querySelector('.player-join-error')
            );
            /** @type {HTMLButtonElement} */ (joinBtn).disabled = true;
            joinBtn.textContent = t('player.joining');
            err.hidden = true;
            try {
                await this.onJoin({
                    height: this._height,
                    // No aspect: a guest cannot know the host's format, and its
                    // own monitor is irrelevant — the backend hands it the ratio
                    // the owner's session already settled on for that host.
                    immersive: this._prefs.immersive,
                    touchScreen: this._prefs.touchScreen,
                });
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
            }
        });
    }
}
