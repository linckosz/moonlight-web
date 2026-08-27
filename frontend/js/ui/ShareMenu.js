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
 * MoonlightWeb — the Share button in a running stream's header.
 *
 * A launcher, nothing more: it shows how many guests are watching and opens the
 * sharing board. Everything the owner can actually do lives in ShareBoard,
 * which the host list's kebab menu opens too — one surface, reached from the
 * two places it makes sense to reach it from.
 *
 * The count is people *streaming*, not links that exist: "Share (2)" means two
 * guests are looking at this screen right now.
 */
import { BackendClient } from '../api/BackendClient.js';
import { t } from '../i18n/i18n.js';
import { escapeHtml } from '../util/escapeHtml.js';
import { ShareBoard } from './ShareBoard.js';

const POLL_MS = 5000;

export class ShareMenu {
    /**
     * @param {HTMLElement} headerEl the .stream-header element
     * @param {HTMLElement} beforeEl insert the button before this node (Stop)
     * @param {{uuid?: string, name?: string}} [host] the host being streamed
     */
    constructor(headerEl, beforeEl, host = {}) {
        this.headerEl = headerEl;
        this.beforeEl = beforeEl;
        this.host = host;
        this.slots = [];
        this._board = null;
        this._pollTimer = null;
    }

    /**
     * Insert the button. Returns false — mounting nothing — when the backend
     * has session sharing switched off: every share route then answers 404,
     * which is exactly how the feature disappears from the UI.
     */
    async mount() {
        try {
            const data = await BackendClient.getShareStatus();
            this.slots = Array.isArray(data.slots) ? data.slots : [];
        } catch (err) {
            if (err && err.statusCode === 404) return false;
            // Any other failure (a hiccup, a slow backend) still gets the
            // button: the poll below will fill in the count.
            console.warn('[ShareMenu] initial status failed:', err);
        }

        this.root = document.createElement('div');
        this.root.className = 'share-menu';
        this.root.innerHTML = `
            <button class="btn share-btn" type="button">${escapeHtml(t('sharing.share'))}</button>
        `;
        this.headerEl.insertBefore(this.root, this.beforeEl);

        this.btn = /** @type {HTMLElement} */ (this.root.querySelector('.share-btn'));
        this.btn.addEventListener('click', (e) => {
            e.stopPropagation();
            e.preventDefault();
            this._openBoard();
        });

        this._paintCount();
        this._startPolling();
        return true;
    }

    destroy() {
        this._stopPolling();
        if (this._board) this._board.close();
        this._board = null;
        if (this.root) this.root.remove();
        this.root = null;
    }

    /**
     * True while at least one player slot is shared or bound — a link is out in
     * the world, whether or not anybody is watching yet. The quality ladder asks
     * before switching legs: a share is a promise to other people.
     */
    hasActiveShare() {
        return this.slots.some((s) => s.state && s.state !== 'off');
    }

    async refresh() {
        try {
            const data = await BackendClient.getShareStatus();
            this.slots = Array.isArray(data.slots) ? data.slots : [];
            this._paintCount();
        } catch (err) {
            // A share status that cannot be read is not worth interrupting a
            // running stream for — keep the last known count.
            console.warn('[ShareMenu] status failed:', err);
        }
    }

    async _openBoard() {
        if (this._board) return;
        const board = new ShareBoard({
            hostUuid: this.host?.uuid,
            hostName: this.host?.name,
            streaming: true, // guests join what is already on screen
        });
        board.onClose = () => {
            this._board = null;
            this.refresh();
        };
        this._board = board;
        if (!(await board.open())) this._board = null;
    }

    _startPolling() {
        this._stopPolling();
        this._pollTimer = setInterval(() => {
            // A backgrounded tab has nothing to repaint; browsers throttle the
            // timer anyway, and a stream in the foreground is the case that
            // matters. The board does its own polling while it is up.
            if (document.hidden || this._board) return;
            this.refresh();
        }, POLL_MS);
    }

    _stopPolling() {
        if (this._pollTimer) clearInterval(this._pollTimer);
        this._pollTimer = null;
    }

    _paintCount() {
        if (!this.btn) return;
        const streaming = this.slots.filter((s) => s.streaming).length;
        this.btn.textContent =
            streaming > 0 ? t('sharing.shareCount', { count: streaming }) : t('sharing.share');
        this.btn.classList.toggle('has-streams', streaming > 0);
    }
}
