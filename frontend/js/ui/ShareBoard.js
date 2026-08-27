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
 * MoonlightWeb — the sharing board (owner side of session sharing)
 *
 * One overlay, one row per guest the owner can invite. It opens from two places
 * and looks the same from both: the Share button in a running stream's header,
 * and the Share entry in a host's kebab menu, where nothing is streaming yet.
 *
 * What each row holds:
 *   • a state: OFF, SHARED (link out, PIN unspent), BINDED (a device took it)
 *   • a name the owner can type over
 *   • the two input flags, and the access badge they add up to
 *   • START / STOP, and below it the link, the PIN, a lifetime, a regenerate
 *
 * Nothing folds. A board that hides half of itself hides exactly what an owner
 * opened it to check, so a row that has not been started shows the same shape,
 * greyed, with the fields that do not exist yet left empty.
 *
 * Two things this deliberately does differently from the popins it replaces.
 * The permissions are chosen *before* START mints anything, so a link never
 * exists before the owner has decided what it grants; and they keep moving
 * afterwards, mid-stream included, because the backend carries the change down
 * to the running worker instead of freezing it.
 */
import { BackendClient } from '../api/BackendClient.js';
import { t } from '../i18n/i18n.js';
import { escapeHtml } from '../util/escapeHtml.js';
import { Icons } from './icons.js';

const POLL_MS = 5000;

/** The lifetimes the backend accepts, in the order the slider walks them. */
const TTL_CHOICES = [3600, 4 * 3600, 8 * 3600, 24 * 3600, 48 * 3600, 0];

const ROW_ICON = {
    off: Icons.userPlus,
    shared: Icons.link,
    binded: Icons.monitor,
};

/**
 * "Chrome · Windows" out of a User-Agent string. Rough on purpose: this is a
 * recognition aid for the person who handed the link out ("yes, that's Marie's
 * laptop"), not telemetry, and a wrong guess costs nothing.
 */
function describeDevice(ua) {
    if (!ua) return t('sharing.deviceUnknown');
    const browser = /\bEdg\//.test(ua)
        ? 'Edge'
        : /\bOPR\//.test(ua)
          ? 'Opera'
          : /\bFirefox\//.test(ua)
            ? 'Firefox'
            : /\bChrome\//.test(ua)
              ? 'Chrome'
              : /\bSafari\//.test(ua)
                ? 'Safari'
                : null;
    const os = /Windows/.test(ua)
        ? 'Windows'
        : /Android/.test(ua)
          ? 'Android'
          : /(iPhone|iPad|iOS)/.test(ua)
            ? 'iOS'
            : /Mac OS X/.test(ua)
              ? 'macOS'
              : /Linux/.test(ua)
                ? 'Linux'
                : null;
    const parts = [browser, os].filter(Boolean);
    return parts.length ? parts.join(' · ') : t('sharing.deviceUnknown');
}

/** "21:04" in the viewer's own locale, from unix seconds. */
function clockTime(secs) {
    if (!secs) return '';
    return new Date(Number(secs) * 1000).toLocaleTimeString([], {
        hour: '2-digit',
        minute: '2-digit',
    });
}

export class ShareBoard {
    /**
     * @param {{hostUuid?: string, hostName?: string, appId?: number,
     *          appName?: string, streaming?: boolean}} [context]
     *   Where the board was opened from. With `streaming` true the invitations
     *   bind to the stream already running and no app is picked here. Otherwise
     *   the board is cold: it needs a host, and an app for the guest's arrival
     *   to launch.
     */
    constructor(context = {}) {
        this.ctx = context;
        this.slots = [];
        this.apps = [];
        /** Chosen app for a cold open; -1 until the owner picks or we default. */
        this.appId = Number.isInteger(context.appId) ? context.appId : -1;
        this._busySlots = new Set();
        /** Writes in flight. While any is, the poll must not repaint. */
        this._writes = 0;
        /** Signature of what is currently on screen; the poll diffs against it. */
        this._painted = '';
        /**
         * Lifetimes the owner has dragged to but not committed. They land on
         * the backend at START or when the board closes, never mid-drag.
         * @type {Map<number, number>}
         */
        this._pendingTtl = new Map();
        this._pollTimer = null;
        this.overlay = null;
        this._secrets = new Map(); // slot → {url, pin} | {available:false}
        /** Last refusal worth showing the owner; cleared by the next success. */
        this._error = '';
        /**
         * Called once the board is gone, so whoever opened it can refresh what
         * it was covering — a host card's kebab state, the header's guest count.
         * @type {(() => void) | null}
         */
        this.onClose = null;
    }

    /**
     * Build the overlay and fill it. Resolves false — mounting nothing — when
     * the backend has session sharing switched off: every share route then
     * answers 404, which is how the feature disappears.
     */
    async open() {
        try {
            await this._load();
        } catch (err) {
            if (err && err.statusCode === 404) return false;
            console.warn('[ShareBoard] initial status failed:', err);
        }

        // A cold board needs an app to bind invitations to. Fetch the list
        // before painting so the picker is never briefly empty.
        if (!this.ctx.streaming && this.ctx.hostUuid) {
            try {
                const data = await BackendClient.getAppList(this.ctx.hostUuid);
                this.apps = Array.isArray(data?.apps) ? data.apps : [];
                if (this.appId < 0 && this.apps.length) this.appId = this.apps[0].id;
            } catch (err) {
                // No list is survivable: the owner cannot pick, so the board
                // says why rather than offering an OPEN that would 404.
                console.warn('[ShareBoard] app list failed:', err);
            }
        }

        this.overlay = document.createElement('div');
        this.overlay.className = 'share-board-overlay';
        this.overlay.innerHTML = `<div class="share-board" role="dialog" aria-modal="true"
             aria-label="${escapeHtml(t('sharing.boardTitle'))}"></div>`;
        document.body.appendChild(this.overlay);
        this.board = /** @type {HTMLElement} */ (this.overlay.querySelector('.share-board'));

        // Deliberately no backdrop dismiss. This board is a place you work in —
        // typing a name, reading a PIN, dragging a lifetime — and a stray click
        // next to it taking the whole thing away is startling out of proportion
        // to the mistake. The ✕ and Escape are the ways out, and both are aimed.
        this._onKey = (e) => {
            if (e.key === 'Escape') {
                e.stopPropagation();
                this.close();
            }
        };
        document.addEventListener('keydown', this._onKey, true);

        this._paint();
        this._startPolling();
        return true;
    }

    close() {
        this._stopPolling();
        document.removeEventListener('keydown', this._onKey, true);
        if (this.overlay) this.overlay.remove();
        this.overlay = null;
        // Closing is the other place a lifetime is committed. Not awaited: the
        // ✕ has to feel instant, and these are idempotent writes that nothing
        // on screen is waiting for any more.
        this._flushTtl();
        if (this.onClose) this.onClose();
    }

    /**
     * Send the lifetimes the owner dragged to. Called at START (for that row)
     * and when the board closes (for all of them).
     */
    async _flushTtl(only) {
        const pending = [...this._pendingTtl.entries()].filter(
            ([slot]) => only === undefined || slot === only,
        );
        for (const [slot, secs] of pending) {
            this._pendingTtl.delete(slot);
            const row = this.slots.find((s) => s.slot === slot);
            // An idle row's lifetime rides along with START instead: sending it
            // first would be a second write saying the same thing.
            if (!row || row.state === 'off') continue;
            if (Number(row.ttl_secs) === secs) continue;
            try {
                await this._write(() => BackendClient.shareTtl(slot, secs));
            } catch (err) {
                console.warn('[ShareBoard] ttl refused:', err);
            }
        }
    }

    // ── State ───────────────────────────────────────────────────────────────

    async _load() {
        const data = await BackendClient.getShareStatus();
        this.slots = Array.isArray(data.slots) ? data.slots : [];

        // Every live row shows its link and PIN, so every live row needs them.
        // Only ones we have never read: they do not change under us, and asking
        // again on each poll would hand out a credential every five seconds.
        await Promise.all(
            this.slots
                .filter((s) => s.state !== 'off' && !this._secrets.has(s.slot))
                .map((s) => this._fetchSecrets(s.slot)),
        );
        for (const s of this.slots) {
            if (s.state === 'off') this._secrets.delete(s.slot);
        }
    }

    /**
     * Everything the board draws from, flattened. The poll compares this and
     * repaints only when it moved: rebuilding identical markup every few
     * seconds destroyed the selection under the owner's cursor, which made the
     * link and the PIN impossible to select by hand — on a board whose whole
     * job is handing those two out.
     */
    _signature() {
        return JSON.stringify([
            this.appId,
            this.slots.map((s) => [
                s.slot,
                s.state,
                s.name,
                s.permissions?.gamepad,
                s.permissions?.keyboardMouse,
                s.streaming,
                s.ttl_secs,
                s.devices?.length,
                s.devices?.[0]?.bound_at,
                s.last_refused?.at,
                this._busySlots.has(s.slot),
                this._secrets.get(s.slot)?.url || this._secrets.get(s.slot)?.available,
            ]),
        ]);
    }

    async refresh() {
        // A write is in flight: its answer is newer than anything this poll can
        // return, and repainting from the older one would put the owner's own
        // click back where it was.
        if (this._writes > 0) return;
        try {
            await this._load();
            if (this._writes > 0) return;
            const signature = this._signature();
            if (signature === this._painted) {
                // Nothing moved. The one thing that still ages on its own is the
                // countdown, and that is a text node, not a rebuild.
                this._paintExpiry();
                return;
            }
            this._paint();
        } catch (err) {
            // A status that cannot be read is not worth tearing the board down
            // for: keep the last known rows and try again on the next tick.
            console.warn('[ShareBoard] status failed:', err);
        }
    }

    /**
     * Age the countdowns and drain the bars in place, leaving every other node
     * alone. This is what makes the yellow bar actually shrink while a share
     * runs, without the rebuild that would take the owner's selection with it.
     */
    _paintExpiry() {
        if (!this.board) return;
        for (const row of this.slots) {
            const detail = this.board.querySelector(`.share-brow-detail[data-slot="${row.slot}"]`);
            if (!detail) continue;

            const label = detail.querySelector('.share-expiry');
            if (label) label.textContent = `(${this._remaining(row)})`;

            const track = /** @type {HTMLElement} */ (detail.querySelector('.share-ttl-track'));
            const slider = /** @type {HTMLInputElement} */ (
                detail.querySelector('.share-ttl-slider')
            );
            if (track && slider) {
                track.style.setProperty('--live', this._livePercent(row, Number(slider.value)));
            }
        }
    }

    /** Run a write with the poll held off for its duration. */
    async _write(fn) {
        this._writes++;
        try {
            return await fn();
        } finally {
            this._writes--;
        }
    }

    _startPolling() {
        this._stopPolling();
        this._pollTimer = setInterval(() => {
            if (document.hidden) return;
            // Never repaint under a field the owner is typing in: it would eat
            // the caret mid-name.
            if (this.board?.querySelector('.share-name-input:focus')) return;
            this.refresh();
        }, POLL_MS);
    }

    _stopPolling() {
        if (this._pollTimer) clearInterval(this._pollTimer);
        this._pollTimer = null;
    }

    // ── Painting ────────────────────────────────────────────────────────────

    _paint() {
        if (!this.board) return;
        // Remembered so the poll can tell "nothing moved" from "redraw needed".
        this._painted = this._signature();

        const subtitle = [this.ctx.hostName, this._appLabel()].filter(Boolean).join(' · ');
        this.board.innerHTML = `
            <div class="share-board-head">
                <span class="share-board-title">${escapeHtml(t('sharing.boardTitle'))}</span>
                <span class="share-board-context">${escapeHtml(subtitle)}</span>
                <button class="share-board-close" type="button"
                        aria-label="${escapeHtml(t('common.close'))}">✕</button>
            </div>
            ${this._appPickerHtml()}
            ${
                this._error
                    ? `<p class="share-board-note is-warning">${escapeHtml(this._error)}</p>`
                    : ''
            }
            <div class="share-board-rows">
                ${this.slots.map((s, i) => this._playerRowHtml(s, i)).join('')}
            </div>
        `;

        this.board
            .querySelector('.share-board-close')
            .addEventListener('click', () => this.close());
        this._wireAppPicker();
        this.slots.forEach((s) => this._wirePlayerRow(s));
    }

    _appLabel() {
        if (this.ctx.streaming) return this.ctx.appName || '';
        const app = this.apps.find((a) => a.id === this.appId);
        return app ? app.name : '';
    }

    /**
     * Only on a cold board. With a stream running the app is not a choice —
     * a guest joins what the owner is already playing.
     */
    _appPickerHtml() {
        if (this.ctx.streaming || !this.ctx.hostUuid) return '';
        if (!this.apps.length) {
            return `<p class="share-board-note is-warning">${escapeHtml(t('sharing.noApps'))}</p>`;
        }
        const options = this.apps
            .map(
                (a) =>
                    `<option value="${a.id}" ${a.id === this.appId ? 'selected' : ''}>` +
                    `${escapeHtml(a.name)}</option>`,
            )
            .join('');
        return `
            <div class="share-board-app">
                <label class="share-field-label" for="share-app-select">${escapeHtml(
                    t('sharing.appLabel'),
                )}</label>
                <select class="share-app-select" id="share-app-select">${options}</select>
                <p class="share-board-note">${escapeHtml(t('sharing.appHint'))}</p>
            </div>
        `;
    }

    _nameFieldHtml(value, key) {
        return `
            <span class="share-name-field">
                <input class="share-name-input" type="text" data-name-for="${key}"
                       value="${escapeHtml(value)}" maxlength="32"
                       aria-label="${escapeHtml(t('sharing.nameLabel'))}">
                <span class="share-name-pencil" aria-hidden="true">${Icons.pencil}</span>
            </span>
        `;
    }

    _playerRowHtml(row, index) {
        const slot = row.slot;
        const busy = this._busySlots.has(slot);
        const state = row.state || 'off';
        const live = state !== 'off';
        const name = row.name || t('sharing.playerN', { n: index + 1 });
        const perms = row.permissions || { gamepad: false, keyboardMouse: false };
        const level = row.access_level || 'viewer';

        // Header and detail are one block, not two stacked ones: the status rail
        // runs the full height of the player it belongs to, so where one guest
        // ends and the next begins is never a question.
        return `
          <div class="share-player" data-state="${escapeHtml(state)}" data-slot="${slot}"
               ${row.streaming ? 'data-streaming="1"' : ''}>
            <div class="share-brow" data-state="${escapeHtml(state)}" data-slot="${slot}"
                 ${row.streaming ? 'data-streaming="1"' : ''}>
                <span class="status-icon ${state === 'off' ? 'offline' : 'ready'}">
                    ${ROW_ICON[state] || ''}
                </span>
                <span class="share-brow-state">${escapeHtml(t(`sharing.state.${state}`))}</span>
                <span class="share-brow-id">
                    ${this._nameFieldHtml(name, String(slot))}
                    <span class="share-brow-hint">${escapeHtml(this._hintFor(row))}</span>
                </span>
                <span class="share-brow-perms">
                    <label class="share-check">
                        <input type="checkbox" class="share-perm-gamepad"
                               ${perms.gamepad ? 'checked' : ''} ${busy ? 'disabled' : ''}>
                        <span>${escapeHtml(t('sharing.gamepad'))}</span>
                    </label>
                    <label class="share-check">
                        <input type="checkbox" class="share-perm-km"
                               ${perms.keyboardMouse ? 'checked' : ''} ${busy ? 'disabled' : ''}>
                        <span>${escapeHtml(t('sharing.keyboardMouse'))}</span>
                    </label>
                </span>
                <span class="share-access-badge" data-level="${escapeHtml(level)}">${escapeHtml(
                    t(`sharing.access.${level}`),
                )}</span>
                <span class="share-brow-action">
                    <button class="btn ${live ? 'btn-secondary' : 'btn-primary'} share-toggle-btn"
                            type="button" ${busy ? 'disabled' : ''}>
                        ${escapeHtml(busy ? t('sharing.working') : live ? t('sharing.stop') : t('sharing.start'))}
                    </button>
                </span>
            </div>
            ${this._detailHtml(row)}
          </div>
        `;
    }

    /** The one line under a row's name: what an owner needs to know at a glance. */
    _hintFor(row) {
        if (row.state === 'off') return t('sharing.hint.off');
        if (row.state === 'shared') return t('sharing.hint.shared');
        const device = row.devices && row.devices.length ? row.devices[0] : null;
        const who = describeDevice(device?.user_agent);
        return row.streaming
            ? t('sharing.hint.streamingSince', { device: who, time: clockTime(device?.bound_at) })
            : t('sharing.hint.bindedSince', { device: who, time: clockTime(device?.bound_at) });
    }

    /**
     * The row's own half, always on screen. A board that folds hides exactly
     * what an owner opened it to check, so nothing folds: a row that has not
     * been started shows the same shape, greyed, with the fields that do not
     * exist yet left empty.
     *
     * The lifetime is not one of those. It is a choice made *before* START,
     * like the two input boxes, so it stays live on an idle row.
     */
    _detailHtml(row) {
        const slot = row.slot;
        const live = row.state !== 'off';
        const creds = this._secrets.get(slot);
        const lost = live && creds && creds.available === false;
        const spent = row.state === 'binded';
        const off = live ? '' : 'disabled';

        const ttl = this._chosenTtl(row);
        const index = Math.max(0, TTL_CHOICES.indexOf(ttl));
        const ticks = TTL_CHOICES.map(
            (secs) =>
                `<span class="share-ttl-tick ${secs === ttl ? 'is-on' : ''}"
                       data-ttl="${secs}">${escapeHtml(this._ttlLabel(secs))}</span>`,
        ).join('');

        // Built, then hidden by CSS (.share-warning). The badge already names
        // the level and the board is meant to stay scannable, so the sentence is
        // noise today — but whether people actually understand what "desktop"
        // hands over is a question user feedback answers, not this file. Kept
        // one CSS line away from coming back.
        const level = row.access_level || 'viewer';
        const warning =
            level === 'desktop' || level === 'full'
                ? `<p class="share-warning" data-level="${escapeHtml(level)}">${escapeHtml(
                      t(`sharing.warning.${level}`),
                  )}</p>`
                : '';

        return `
            <div class="share-brow-detail" data-slot="${slot}" data-live="${live ? '1' : '0'}">
                <div class="share-detail-line ${live ? '' : 'is-idle'}">
                    <label class="share-field-label">${escapeHtml(t('sharing.linkLabel'))}</label>
                    <div class="share-copy-row">
                        <input class="share-link-input" type="text" readonly ${off}
                               value="${escapeHtml(lost ? '' : creds?.url || '')}">
                        <button class="btn btn-secondary share-copy-btn" type="button" ${off}>${escapeHtml(
                            t('common.copy'),
                        )}</button>
                        <button class="btn btn-secondary share-regen-btn" type="button" ${off}
                                title="${escapeHtml(t('sharing.regenerateHint'))}">
                            ${Icons.refresh}<span>${escapeHtml(t('sharing.regenerate'))}</span>
                        </button>
                    </div>
                </div>

                <div class="share-detail-line share-detail-split">
                    <div class="share-detail-pin ${live ? '' : 'is-idle'}">
                        <label class="share-field-label">${escapeHtml(t('sharing.pinLabel'))}</label>
                        ${
                            spent
                                ? `<p class="share-pin-spent">${escapeHtml(t('sharing.pinSpent'))}</p>`
                                : `<div class="share-copy-row">
                                       <input class="share-pin" type="text" readonly ${off}
                                              value="${escapeHtml(lost ? '' : creds?.pin || '')}"
                                              aria-label="${escapeHtml(t('sharing.pinLabel'))}">
                                       <button class="btn btn-secondary share-copy-btn"
                                               type="button" ${off}>${escapeHtml(t('common.copy'))}</button>
                                   </div>`
                        }
                    </div>
                    <div class="share-detail-ttl">
                        <label class="share-field-label">${escapeHtml(
                            t('sharing.expiresLabel'),
                        )} <span class="share-expiry">(${escapeHtml(this._remaining(row))})</span></label>
                        <div class="share-ttl-body">
                            <span class="share-ttl-track"
                                  style="--fill:${this._fillPercent(
                                      index,
                                  )};--live:${this._livePercent(row, index)}">
                                <input class="settings-slider share-ttl-slider" type="range" min="0"
                                       max="${TTL_CHOICES.length - 1}" step="1" value="${index}"
                                       aria-label="${escapeHtml(t('sharing.expiresLabel'))}">
                            </span>
                            <div class="share-ttl-scale">${ticks}</div>
                        </div>
                    </div>
                </div>

                ${lost ? `<p class="share-pin-hint">${escapeHtml(t('sharing.secretsLost'))}</p>` : ''}
                ${warning}
                ${this._refusedHtml(row)}
            </div>
        `;
    }

    /**
     * Someone else opened this link with the right PIN and was turned away.
     * The owner had no way of seeing this before; now it is on the row.
     */
    _refusedHtml(row) {
        if (!row.last_refused) return '';
        return `<p class="share-refused">${escapeHtml(
            t('sharing.refused', {
                device: describeDevice(row.last_refused.user_agent),
                time: clockTime(row.last_refused.at),
            }),
        )}</p>`;
    }

    _ttlLabel(secs) {
        if (secs === 0) return t('sharing.ttlUnlimited');
        return secs % 3600 === 0 ? t('sharing.ttlHours', { h: secs / 3600 }) : `${secs}s`;
    }

    /**
     * The lifetime this row is currently set to: what the owner has dragged the
     * slider to since the board opened, or what the server holds if they have
     * not touched it.
     */
    _chosenTtl(row) {
        return this._pendingTtl.has(row.slot)
            ? this._pendingTtl.get(row.slot)
            : Number(row.ttl_secs ?? 0);
    }

    /** Where the thumb sits: the chosen lifetime, as a percentage of the track. */
    _fillPercent(index) {
        return `${(index / (TTL_CHOICES.length - 1)) * 100}%`;
    }

    /**
     * Where the solid bar ends. The bar starts at the left and its length *is*
     * the time left, so it drains towards zero while the gap between it and the
     * thumb (the time already spent) opens up. Both live inside the chosen
     * lifetime; the track beyond the thumb is untouched scale.
     */
    _livePercent(row, index) {
        const fill = (index / (TTL_CHOICES.length - 1)) * 100;
        const chosen = this._chosenTtl(row);
        if (!chosen) return `${fill}%`; // unlimited: nothing is draining
        const ratio = Math.max(0, Math.min(1, this._remainingSecs(row) / chosen));
        return `${ratio * fill}%`;
    }

    /**
     * Seconds left on this row, Infinity when unlimited.
     *
     * Counted from now and capped by the lifetime currently dragged to, so
     * moving to 8 h reads 8 h when there was more than that left and leaves
     * 3 h alone when there was less. Mirrors what the backend will do when this
     * is committed (ShareManager::setTtl).
     */
    _remainingSecs(row) {
        const chosen = this._chosenTtl(row);
        // Nothing started yet: there is no deadline to count down to, only the
        // length the next invitation will be minted with.
        if (row.state === 'off') return chosen || Infinity;
        const live = row.expires_at
            ? Math.max(0, Number(row.expires_at) - Date.now() / 1000)
            : Infinity;
        return chosen ? Math.min(chosen, live) : live;
    }

    _remaining(row) {
        const secs = this._remainingSecs(row);
        if (!Number.isFinite(secs)) return t('sharing.expiresNever');
        if (row.state === 'off') return this._ttlLabel(this._chosenTtl(row));

        const whole = Math.round(secs);
        const h = Math.floor(whole / 3600);
        const m = Math.floor((whole % 3600) / 60);
        return h > 0 ? t('sharing.expiresInH', { h, m }) : t('sharing.expiresInM', { m });
    }

    // ── Wiring ──────────────────────────────────────────────────────────────

    _wireAppPicker() {
        const select = /** @type {HTMLSelectElement} */ (
            this.board.querySelector('.share-app-select')
        );
        if (!select) return;
        select.addEventListener('change', () => {
            // Only affects rows opened from now on: an invitation already out
            // there is bound to the app it was minted with, and silently moving
            // it would change what a guest lands in without telling them.
            this.appId = Number(select.value);
            const context = /** @type {HTMLElement} */ (
                this.board.querySelector('.share-board-context')
            );
            if (context) {
                context.textContent = [this.ctx.hostName, this._appLabel()]
                    .filter(Boolean)
                    .join(' · ');
            }
        });
    }

    _wirePlayerRow(row) {
        const slot = row.slot;
        const el = /** @type {HTMLElement} */ (
            this.board.querySelector(`.share-brow[data-slot="${slot}"]`)
        );
        if (!el) return;

        const nameInput = /** @type {HTMLInputElement} */ (el.querySelector('.share-name-input'));
        this._wireNameInput(nameInput, (value) => BackendClient.shareRename(slot, value));

        const gamepad = /** @type {HTMLInputElement} */ (el.querySelector('.share-perm-gamepad'));
        const km = /** @type {HTMLInputElement} */ (el.querySelector('.share-perm-km'));
        const pushPerms = async () => {
            const perms = { gamepad: gamepad.checked, keyboardMouse: km.checked };
            // Repaint the badge from the boxes immediately: the owner is
            // watching the label, and a round trip before it moves reads as a
            // click that did not register.
            const badge = /** @type {HTMLElement} */ (el.querySelector('.share-access-badge'));
            const level =
                perms.gamepad && perms.keyboardMouse
                    ? 'full'
                    : perms.keyboardMouse
                      ? 'desktop'
                      : perms.gamepad
                        ? 'gamer'
                        : 'viewer';
            badge.dataset.level = level;
            badge.textContent = t(`sharing.access.${level}`);

            const local = this.slots.find((s) => s.slot === slot);
            if (local) {
                local.permissions = perms;
                local.access_level = level;
            }
            // Sent whatever the row's state. On an idle row the backend records
            // it as what START will mint; not sending it was what let the next
            // poll repaint the tick away a second after the click.
            try {
                await this._write(() => BackendClient.sharePermissions(slot, perms));
            } catch (err) {
                console.warn('[ShareBoard] permissions refused:', err);
                await this.refresh();
            }
        };
        gamepad.addEventListener('change', pushPerms);
        km.addEventListener('change', pushPerms);

        el.querySelector('.share-toggle-btn').addEventListener('click', () => {
            if (row.state === 'off') this._openRow(slot);
            else this._closeRow(slot);
        });

        this._wireDetail(row);
    }

    /** Commit a name on blur and on Enter, never on every keystroke. */
    _wireNameInput(input, commit) {
        if (!input) return;
        let last = input.value;
        const send = async () => {
            const value = input.value.trim();
            if (value === last) return;
            last = value;
            try {
                await this._write(() => commit(value));
            } catch (err) {
                console.warn('[ShareBoard] rename failed:', err);
                await this.refresh();
            }
        };
        input.addEventListener('blur', send);
        input.addEventListener('keydown', (e) => {
            if (e.key === 'Enter') {
                e.preventDefault();
                input.blur();
            } else if (e.key === 'Escape') {
                e.stopPropagation(); // the board's own Escape must not fire
                input.value = last;
                input.blur();
            }
        });
    }

    _wireDetail(row) {
        const detail = /** @type {HTMLElement} */ (
            this.board.querySelector(`.share-brow-detail[data-slot="${row.slot}"]`)
        );
        if (!detail) return;

        detail.querySelectorAll('.share-copy-row').forEach((copyRow) => {
            const input = /** @type {HTMLInputElement} */ (copyRow.querySelector('input'));
            const btn = /** @type {HTMLElement} */ (copyRow.querySelector('.share-copy-btn'));
            if (!input || !btn) return;
            input.addEventListener('focus', () => input.select());
            btn.addEventListener('click', async () => {
                try {
                    await navigator.clipboard.writeText(input.value);
                } catch (_) {
                    // The clipboard API needs a secure context and a permission
                    // the browser may withhold — select it so Ctrl+C works.
                    input.select();
                }
                btn.textContent = t('common.copied');
                setTimeout(() => {
                    btn.textContent = t('common.copy');
                }, 1500);
            });
        });

        const slider = /** @type {HTMLInputElement} */ (detail.querySelector('.share-ttl-slider'));
        if (slider) {
            const track = /** @type {HTMLElement} */ (detail.querySelector('.share-ttl-track'));
            const expiry = /** @type {HTMLElement} */ (detail.querySelector('.share-expiry'));
            // Dragging commits nothing. The lifetime lands on the backend when
            // the row is started, or when the board closes — moving the thumb
            // through 1 h on the way to 48 h must not cut an invitation short
            // in passing.
            const preview = () => {
                const index = Number(slider.value);
                const secs = TTL_CHOICES[index];
                this._pendingTtl.set(row.slot, secs);
                detail.querySelectorAll('.share-ttl-tick').forEach((tick, i) => {
                    tick.classList.toggle('is-on', TTL_CHOICES[i] === secs);
                });
                if (track) {
                    track.style.setProperty('--fill', this._fillPercent(index));
                    track.style.setProperty('--live', this._livePercent(row, index));
                }
                if (expiry) expiry.textContent = `(${this._remaining(row)})`;
            };
            slider.addEventListener('input', preview);
            slider.addEventListener('change', preview);
        }

        const regen = /** @type {HTMLButtonElement} */ (detail.querySelector('.share-regen-btn'));
        if (regen && !regen.disabled) {
            regen.addEventListener('click', async () => {
                regen.disabled = true;
                // Minting is destructive by design: the backend revokes the old
                // activation (link, PIN, bound device and any live stream on it)
                // before handing back a new pair.
                await this._openRow(row.slot);
            });
        }
    }

    // ── Actions ─────────────────────────────────────────────────────────────

    async _fetchSecrets(slot) {
        try {
            const creds = await BackendClient.shareCredentials(slot);
            this._secrets.set(slot, creds);
        } catch (err) {
            // Falls through to the "cannot show them again" copy. An owner who
            // still has the link in their chat window is not blocked by this.
            console.warn('[ShareBoard] credentials failed:', err);
            this._secrets.set(slot, { available: false });
        }
    }

    /** START (or regenerate): mint the link and PIN for this row. */
    async _openRow(slot) {
        this._busySlots.add(slot);
        this._paint();
        try {
            const local = this.slots.find((s) => s.slot === slot);
            // START is where a dragged lifetime becomes real, so it travels
            // with the activation rather than as a second call after it.
            const chosen = local ? this._chosenTtl(local) : TTL_CHOICES[2];
            this._pendingTtl.delete(slot);
            const target = { ttl_secs: chosen };
            if (!this.ctx.streaming) {
                target.host_uuid = this.ctx.hostUuid;
                target.app_id = this.appId;
            }
            const data = await this._write(() => BackendClient.shareActivate(slot, target));
            this._secrets.set(slot, { available: true, url: data.url, pin: data.pin });
            this._error = '';
        } catch (err) {
            // The refusals worth reading are the owner's to fix: no host picked,
            // an app that has gone away. Say so on the board, since a row that
            // simply stayed off would leave them clicking it again.
            console.warn('[ShareBoard] start failed:', err);
            this._error = err?.responseBody?.error || t('sharing.openFailed');
        } finally {
            this._busySlots.delete(slot);
            await this.refresh();
        }
    }

    /** STOP: revoke the link, the PIN, the bound device and any live stream. */
    async _closeRow(slot) {
        this._busySlots.add(slot);
        this._paint();
        try {
            await this._write(() => BackendClient.shareDeactivate(slot));
            this._secrets.delete(slot);
        } catch (err) {
            console.warn('[ShareBoard] stop failed:', err);
        } finally {
            this._busySlots.delete(slot);
            await this.refresh();
        }
    }
}
