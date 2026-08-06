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
import { PlayerArt } from './PlayerArt.js';

const HEIGHTS = [720, 1080, 1440];

export class PlayerJoinView {
    /**
     * @param {HTMLElement} container
     * @param {string} token the share token from the URL
     * @param {(info: {height: number}) => Promise<void>} onJoin
     */
    constructor(container, token, onJoin) {
        this.container = container;
        this.token = token;
        this.onJoin = onJoin;
        this.info = null;
        this._height = 1080;
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
            </div>
        `;
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
            'phone',
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
                err.textContent = lockout
                    ? t('player.pinLocked', { seconds: lockout })
                    : t('player.pinWrong');
                err.hidden = false;
                input.disabled = false;
                input.value = '';
                input.focus();
            }
        });
    }

    _renderJoin() {
        const machine = this.info.machine_name || t('player.thisPc');
        const busy = this.info.state === 'streaming';
        const noSession = this.info.owner_streaming === false;

        this._shell(`
            <h1>${escapeHtml(t('player.joinTitle', { machine }))}</h1>
            <p class="player-access">${escapeHtml(t(`player.access.${this.info.access_level}`))}</p>

            ${
                busy
                    ? `<p class="player-error">${escapeHtml(t('player.busy'))}</p>`
                    : noSession
                      ? `<p class="player-error">${escapeHtml(t('player.noSession'))}</p>`
                      : `
                <div class="player-quality" role="group" aria-label="${escapeHtml(t('player.quality'))}">
                    ${HEIGHTS.map(
                        (h) => `
                        <button class="btn player-quality-btn ${h === this._height ? 'is-selected' : ''}"
                                type="button" data-height="${h}">${h}p</button>
                    `,
                    ).join('')}
                </div>
                <button class="btn btn-primary player-join-btn" type="button">
                    ${escapeHtml(t('player.joinButton', { machine }))}
                </button>
            `
            }
            <p class="player-error player-join-error" hidden></p>
            ${
                // Only worth offering while the guest is waiting on someone
                // else: the other screen to let go, or the host to start a
                // game. With a Join button on screen it was a second button
                // that did nothing they wanted.
                busy || noSession
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
                await this.onJoin({ height: this._height });
            } catch (ex) {
                const code = ex && ex.responseBody && ex.responseBody.error;
                if (code === 'session_ended') {
                    this.renderEnded();
                    return;
                }
                err.textContent =
                    code === 'stream_in_progress' ? t('player.busy') : t('player.joinFailed');
                err.hidden = false;
                /** @type {HTMLButtonElement} */ (joinBtn).disabled = false;
                joinBtn.textContent = t('player.joinButton', {
                    machine: this.info.machine_name || t('player.thisPc'),
                });
            }
        });
    }
}
