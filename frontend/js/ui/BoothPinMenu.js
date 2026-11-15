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
 * A booth's stream PIN, in the owner's stream header (booth_stream_pin, issue
 * #68). Where Share would be: the PIN another device of the booth enters to
 * join this stream, and how many are watching it (as Share counts them, #47).
 * A click opens a small panel: the PIN large, "New PIN", and one row per guest
 * who joined with it — their level and Remove.
 *
 * The PIN is asked of the host every few seconds, with a proof by this
 * browser's key each time: it changes after every join and every departure,
 * and the header must never show one that no longer opens anything.
 */

import { BackendClient } from '../api/BackendClient.js';
import { t } from '../i18n/i18n.js';
import { escapeHtml } from '../util/escapeHtml.js';
import { Icons } from './icons.js';
import { Toast } from './Toast.js';
import {
    announceChanges,
    formatPin,
    ownerSidePeople,
    presenceChanges,
    presenceTitle,
} from './SharePresence.js';

const POLL_MS = 5000;

/** The levels a guest can be moved between, as the board names them. */
export const BOOTH_LEVELS = ['viewer', 'gamer', 'desktop', 'full'];

/**
 * The permissions a level names (the backend's boothPermissionsFor).
 * @param {string} level
 */
export function permissionsFor(level) {
    return {
        gamepad: level === 'gamer' || level === 'full',
        keyboardMouse: level === 'desktop' || level === 'full',
    };
}

export class BoothPinMenu {
    /**
     * @param {HTMLElement} headerEl  the stream header
     * @param {HTMLElement} beforeEl  what the chip goes in front of (Stop)
     * @param {{uuid: string, name?: string}} host
     */
    constructor(headerEl, beforeEl, host) {
        this.headerEl = headerEl;
        this.beforeEl = beforeEl;
        this.host = host;
        /** @type {HTMLElement|null} */
        this.root = null;
        /** @type {HTMLElement|null} */
        this.btn = null;
        /** @type {HTMLElement|null} */
        this.panel = null;
        this.pin = '';
        /** @type {number|null} */
        this.generation = null;
        /** @type {Array<{slot: number, name?: string, streaming?: boolean, access_level?: string}>} */
        this.guests = [];
        /** @type {Map<string, string>|null} */
        this._people = null;
        this._timer = null;
        this._onOutside = (e) => {
            if (this.panel && !this.root?.contains(/** @type {Node} */ (e.target)))
                this.closePanel();
        };
    }

    async mount() {
        this.root = document.createElement('div');
        this.root.className = 'booth-pin-menu';
        this.root.innerHTML = `
            <button class="booth-pin" type="button">
                <span class="booth-pin-label">${escapeHtml(t('booth.pin'))}</span>
                <span class="booth-pin-digits"></span>
                <span class="booth-pin-count">${Icons.users}<span class="booth-pin-n">0</span></span>
            </button>
        `;
        this.headerEl.insertBefore(this.root, this.beforeEl);
        this.btn = /** @type {HTMLElement} */ (this.root.querySelector('.booth-pin'));
        this.btn.addEventListener('click', (e) => {
            e.stopPropagation();
            e.preventDefault();
            if (this.panel) this.closePanel();
            else this.openPanel();
        });
        await this.refresh();
        this._timer = setInterval(() => {
            if (!document.hidden) this.refresh();
        }, POLL_MS);
        return true;
    }

    /**
     * True while a guest holds a seat on this stream: the quality ladder asks
     * before switching legs, as it does for the board's invitations.
     */
    hasActiveShare() {
        return this.guests.length > 0;
    }

    destroy() {
        if (this._timer) clearInterval(this._timer);
        this._timer = null;
        this.closePanel();
        if (this.root) this.root.remove();
        this.root = null;
        this.btn = null;
    }

    async refresh() {
        try {
            const answer = await BackendClient.boothPin(this.host.uuid);
            if (answer && typeof answer.pin === 'string') {
                const generation = Number(answer.generation) || 0;
                if (
                    this.generation !== null &&
                    generation > this.generation &&
                    answer.changed_by_failures === true
                )
                    Toast.warning(t('booth.pinChangedFailures'));
                this.pin = answer.pin;
                this.generation = generation;
            }
        } catch (err) {
            // A hiccup keeps the last PIN rather than blanking the header.
            console.warn('[BoothPin] PIN read failed:', err);
        }
        try {
            const data = await BackendClient.getShareStatus(this.host.uuid);
            this.guests = (Array.isArray(data.slots) ? data.slots : []).filter((s) => s && s.booth);
            const people = ownerSidePeople(this.guests);
            if (this._people && this.root) announceChanges(presenceChanges(this._people, people));
            this._people = people;
        } catch (err) {
            console.warn('[BoothPin] status failed:', err);
        }
        this._paint();
        if (this.panel) this._renderPanel();
    }

    _paint() {
        if (!this.btn) return;
        const digits = /** @type {HTMLElement} */ (this.btn.querySelector('.booth-pin-digits'));
        digits.textContent = formatPin(this.pin) || '··· ···';
        const names = this._people ? [...this._people.values()] : [];
        const n = /** @type {HTMLElement} */ (this.btn.querySelector('.booth-pin-n'));
        // The owner and the guests watching, as a guest's own count reads it.
        n.textContent = String(names.length + 1);
        this.btn.title = presenceTitle([t('sharing.presenceHost'), ...names]);
    }

    openPanel() {
        if (this.panel || !this.root) return;
        this.panel = document.createElement('div');
        this.panel.className = 'booth-panel';
        this.root.appendChild(this.panel);
        this._renderPanel();
        document.addEventListener('click', this._onOutside, true);
    }

    closePanel() {
        document.removeEventListener('click', this._onOutside, true);
        if (this.panel) this.panel.remove();
        this.panel = null;
    }

    _renderPanel() {
        if (!this.panel) return;
        const rows = this.guests
            .map((g) => {
                const level = BOOTH_LEVELS.includes(g.access_level || '')
                    ? g.access_level
                    : 'viewer';
                const options = BOOTH_LEVELS.map(
                    (l) =>
                        `<option value="${l}"${l === level ? ' selected' : ''}>${escapeHtml(
                            t(`sharing.access.${l}`),
                        )}</option>`,
                ).join('');
                return `
                <div class="booth-panel-row" data-slot="${g.slot}">
                    <span class="booth-panel-name">${escapeHtml(g.name || '')}</span>
                    <select class="booth-panel-level" aria-label="${escapeHtml(
                        t('booth.level'),
                    )}">${options}</select>
                    <button class="btn btn-secondary booth-panel-remove" type="button">${escapeHtml(
                        t('booth.remove'),
                    )}</button>
                </div>`;
            })
            .join('');
        this.panel.innerHTML = `
            <div class="booth-panel-head">
                <div>
                    <h4>${escapeHtml(t('booth.panelTitle'))}</h4>
                    <div class="booth-panel-pin">${escapeHtml(formatPin(this.pin) || '··· ···')}</div>
                </div>
                <button class="btn btn-secondary booth-panel-new" type="button">${Icons.refresh} ${escapeHtml(
                    t('booth.newPin'),
                )}</button>
            </div>
            ${rows || `<div class="booth-panel-empty">${escapeHtml(t('booth.noGuests'))}</div>`}
            <div class="booth-panel-note">${escapeHtml(t('booth.panelNote'))}</div>
        `;
        this.panel.querySelector('.booth-panel-new')?.addEventListener('click', async (e) => {
            e.stopPropagation();
            try {
                const answer = await BackendClient.boothPin(this.host.uuid, 'new');
                if (answer && typeof answer.pin === 'string') {
                    this.pin = answer.pin;
                    this.generation = Number(answer.generation) || 0;
                }
            } catch (err) {
                console.warn('[BoothPin] new PIN failed:', err);
            }
            this._paint();
            this._renderPanel();
        });
        for (const row of this.panel.querySelectorAll('.booth-panel-row')) {
            const slot = Number(/** @type {HTMLElement} */ (row).dataset.slot);
            row.querySelector('.booth-panel-level')?.addEventListener('change', async (e) => {
                const level = /** @type {HTMLSelectElement} */ (e.target).value;
                try {
                    await BackendClient.sharePermissions(slot, permissionsFor(level));
                } catch (err) {
                    console.warn('[BoothPin] level change failed:', err);
                }
                this.refresh();
            });
            row.querySelector('.booth-panel-remove')?.addEventListener('click', async (e) => {
                e.stopPropagation();
                try {
                    await BackendClient.shareDeactivate(slot);
                } catch (err) {
                    console.warn('[BoothPin] remove failed:', err);
                }
                this.refresh();
            });
        }
    }
}
