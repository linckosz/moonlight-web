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
 * MoonlightWeb — Share menu (owner side of session sharing)
 *
 * Sits in the stream header, left of Stop. One row per player slot, styled
 * like the status lines of the hosts page:
 *
 *   off       (red)   → click mints a link + PIN and opens the popin
 *   shared    (green) → click revokes the link
 *   streaming (cyan)  → click disconnects the player and revokes the link
 *
 * The popin is the only place a share is configured: the input permissions are
 * chosen there and freeze when it closes, because from that moment the link is
 * out in the world and a worker may already be carrying the policy. Changing
 * them means turning the player off and sharing again — which is also what
 * invalidates the old link.
 */
import { BackendClient } from '../api/BackendClient.js';
import { t } from '../i18n/i18n.js';
import { escapeHtml } from '../util/escapeHtml.js';
import { Icons } from './icons.js';

const POLL_MS = 5000;

/* Each row reads like a host card on the hosts page: a round status glyph, the
   name, and an uppercase badge. The status classes are the ones already
   defined for hosts (.status-icon / .status-badge) so the two lists share one
   visual language; 'live' is the only addition, for a player mid-stream. */
const ROW_ICON = {
    off: Icons.userPlus,
    shared: Icons.link,
    streaming: Icons.play,
    busy: Icons.unavailable,
};
const ROW_CLASS = {
    off: 'offline',
    shared: 'ready',
    streaming: 'live',
    busy: 'unavailable',
};

export class ShareMenu {
    /**
     * @param {HTMLElement} headerEl the .stream-header element
     * @param {HTMLElement} beforeEl insert the button before this node (Stop)
     */
    constructor(headerEl, beforeEl) {
        this.headerEl = headerEl;
        this.beforeEl = beforeEl;
        this.root = null;
        this.slots = [];
        this._open = false;
        this._pollTimer = null;
        this._popin = null;
        this._busySlots = new Set();
    }

    /**
     * Insert the button and load the rows. Returns false — mounting nothing —
     * when the backend has session sharing switched off: every share route then
     * answers 404, which is exactly how the feature disappears from the UI.
     */
    async mount() {
        try {
            const data = await BackendClient.getShareStatus();
            this.slots = Array.isArray(data.slots) ? data.slots : [];
        } catch (err) {
            if (err && err.statusCode === 404) return false;
            // Any other failure (a hiccup, a slow backend) still gets the menu:
            // the poll below will fill it in.
            console.warn('[ShareMenu] initial status failed:', err);
        }

        this.root = document.createElement('div');
        this.root.className = 'share-menu';
        this.root.innerHTML = `
            <button class="btn share-btn" type="button" aria-haspopup="true" aria-expanded="false">
                ${escapeHtml(t('sharing.share'))}
            </button>
            <div class="share-dropdown" hidden></div>
        `;
        this.headerEl.insertBefore(this.root, this.beforeEl);

        this.btn = /** @type {HTMLElement} */ (this.root.querySelector('.share-btn'));
        this.dropdown = /** @type {HTMLElement} */ (this.root.querySelector('.share-dropdown'));

        this.btn.addEventListener('click', (e) => {
            e.stopPropagation();
            this._toggle();
        });
        // Any click outside closes the dropdown — but never while the popin is
        // up, or dismissing it would also drop the link the user is reading.
        this._onDocClick = () => {
            if (!this._popin) this._close();
        };
        document.addEventListener('click', this._onDocClick);

        this._paint();
        this._startPolling();
        return true;
    }

    destroy() {
        this._stopPolling();
        document.removeEventListener('click', this._onDocClick);
        if (this._popin) this._popin.remove();
        this._popin = null;
        if (this.root) this.root.remove();
        this.root = null;
    }

    // ── State ───────────────────────────────────────────────────────────────

    async refresh() {
        try {
            const data = await BackendClient.getShareStatus();
            this.slots = Array.isArray(data.slots) ? data.slots : [];
            this._paint();
        } catch (err) {
            // A share status that cannot be read is not worth interrupting a
            // running stream for — keep the last known rows.
            console.warn('[ShareMenu] status failed:', err);
        }
    }

    _startPolling() {
        this._stopPolling();
        this._pollTimer = setInterval(() => {
            // A backgrounded tab has nothing to repaint; browsers throttle the
            // timer anyway, and a stream in the foreground is the case that
            // matters.
            if (document.hidden) return;
            this.refresh();
        }, POLL_MS);
    }

    _stopPolling() {
        if (this._pollTimer) clearInterval(this._pollTimer);
        this._pollTimer = null;
    }

    _paint() {
        if (!this.root) return;

        // The count is players actually streaming — "Share (2)" means two
        // people are watching right now, not two links exist.
        const streaming = this.slots.filter((s) => s.state === 'streaming').length;
        this.btn.textContent =
            streaming > 0 ? t('sharing.shareCount', { count: streaming }) : t('sharing.share');
        this.btn.classList.toggle('has-streams', streaming > 0);

        const rows = this.slots
            .map((s, i) => {
                const busy = this._busySlots.has(s.slot);
                const state = busy ? 'busy' : s.state;
                // Slots are 2..4 on the wire (0 and 1 belong to the owner), but
                // a list that starts at "Player 2" reads like something is
                // missing — the guests are numbered from one.
                const label = t('sharing.playerN', { n: i + 1 });
                const cls = ROW_CLASS[state] || 'unavailable';
                const badge = busy ? t('sharing.working') : t(`sharing.state.${state}`);
                const hint = busy ? '' : t(`sharing.hint.${state}`);
                return `
                    <button class="share-row" type="button"
                            data-slot="${s.slot}" data-state="${escapeHtml(state)}"
                            ${busy ? 'disabled' : ''}>
                        <span class="status-icon ${cls}">${ROW_ICON[state] || ''}</span>
                        <span class="share-row-info">
                            <span class="share-row-name">${escapeHtml(label)}</span>
                            <span class="share-row-hint">${escapeHtml(hint)}</span>
                        </span>
                        <span class="status-badge ${cls}">${escapeHtml(badge)}</span>
                    </button>
                `;
            })
            .join('');

        this.dropdown.innerHTML = `
            <div class="share-dropdown-title">${escapeHtml(t('sharing.dropdownTitle'))}</div>
            ${rows}
        `;

        this.dropdown.querySelectorAll('.share-row').forEach((row) => {
            row.addEventListener('click', (e) => {
                // stopPropagation alone keeps the document handler from closing
                // the menu; preventDefault keeps the browser from synthesising
                // anything the stream's input layer could pick up underneath.
                e.stopPropagation();
                e.preventDefault();
                const slot = Number(/** @type {HTMLElement} */ (row).dataset.slot);
                this._onRowClick(slot);
            });
        });
    }

    _toggle() {
        this._open = !this._open;
        this.dropdown.hidden = !this._open;
        this.btn.setAttribute('aria-expanded', String(this._open));
        if (this._open) this.refresh();
    }

    _close() {
        if (!this._open) return;
        this._open = false;
        this.dropdown.hidden = true;
        this.btn.setAttribute('aria-expanded', 'false');
    }

    // ── Actions ─────────────────────────────────────────────────────────────

    async _onRowClick(slot) {
        const row = this.slots.find((s) => s.slot === slot);
        if (!row || this._busySlots.has(slot)) return;

        if (row.state === 'off') {
            await this._activate(slot);
            return;
        }

        // Live row: revoke. Disconnecting a player is a real action on someone
        // else's screen, so the row shows a spinner until the backend confirms.
        this._busySlots.add(slot);
        this._paint();
        try {
            await BackendClient.shareDeactivate(slot);
        } catch (err) {
            console.warn('[ShareMenu] deactivate failed:', err);
        } finally {
            this._busySlots.delete(slot);
            await this.refresh();
        }
    }

    async _activate(slot) {
        this._busySlots.add(slot);
        this._paint();
        try {
            const data = await BackendClient.shareActivate(slot);
            this._openPopin(slot, data);
        } catch (err) {
            console.warn('[ShareMenu] activate failed:', err);
        } finally {
            this._busySlots.delete(slot);
            await this.refresh();
        }
    }

    // ── Popin ───────────────────────────────────────────────────────────────

    _openPopin(slot, data) {
        const index = this.slots.findIndex((s) => s.slot === slot);
        const name = t('sharing.playerN', { n: (index < 0 ? 0 : index) + 1 });
        const perms = data.permissions || { gamepad: false, keyboardMouse: false };

        const overlay = document.createElement('div');
        overlay.className = 'share-popin-overlay';
        overlay.innerHTML = `
            <div class="share-popin" role="dialog" aria-modal="true">
                <h3>${escapeHtml(t('sharing.popinTitle', { player: name }))}</h3>

                <label class="share-field-label">${escapeHtml(t('sharing.linkLabel'))}</label>
                <div class="share-copy-row">
                    <input class="share-link-input" type="text" readonly value="${escapeHtml(data.url || '')}">
                    <button class="btn btn-secondary share-copy-btn" type="button">${escapeHtml(t('common.copy'))}</button>
                </div>

                <label class="share-field-label">${escapeHtml(t('sharing.pinLabel'))}</label>
                <div class="share-pin">${escapeHtml(data.pin || '')}</div>
                <p class="share-pin-hint">${escapeHtml(t('sharing.pinHint'))}</p>

                <label class="share-field-label">${escapeHtml(t('sharing.inputsLabel'))}</label>
                <div class="share-perms">
                    <label class="share-check">
                        <input type="checkbox" class="share-perm-gamepad" ${perms.gamepad ? 'checked' : ''}>
                        <span>${escapeHtml(t('sharing.gamepad'))}</span>
                    </label>
                    <label class="share-check">
                        <input type="checkbox" class="share-perm-km" ${perms.keyboardMouse ? 'checked' : ''}>
                        <span>${escapeHtml(t('sharing.keyboardMouse'))}</span>
                    </label>
                </div>

                <div class="share-access" data-level=""></div>
                <p class="share-expiry">${escapeHtml(t('sharing.expiry'))}</p>

                <div class="share-popin-actions">
                    <button class="btn btn-primary share-done-btn" type="button">${escapeHtml(t('sharing.done'))}</button>
                </div>
            </div>
        `;
        document.body.appendChild(overlay);
        this._popin = overlay;

        const gamepadCb = /** @type {HTMLInputElement} */ (
            overlay.querySelector('.share-perm-gamepad')
        );
        const kmCb = /** @type {HTMLInputElement} */ (overlay.querySelector('.share-perm-km'));
        const accessEl = /** @type {HTMLElement} */ (overlay.querySelector('.share-access'));

        const paintAccess = () => {
            const level = kmCb.checked ? 'full' : gamepadCb.checked ? 'gamer' : 'viewer';
            accessEl.dataset.level = level;
            accessEl.innerHTML = `
                <div class="share-access-level">${escapeHtml(t(`sharing.access.${level}`))}</div>
                <div class="share-access-warning">${escapeHtml(t(`sharing.warning.${level}`))}</div>
            `;
        };
        paintAccess();

        const push = async () => {
            paintAccess();
            try {
                await BackendClient.sharePermissions(slot, {
                    gamepad: gamepadCb.checked,
                    keyboardMouse: kmCb.checked,
                });
            } catch (err) {
                // The only way this fails is a race with a player joining, which
                // locks the policy. Snap the boxes back to the truth.
                console.warn('[ShareMenu] permissions refused:', err);
                await this.refresh();
                const fresh = this.slots.find((s) => s.slot === slot);
                if (fresh && fresh.permissions) {
                    gamepadCb.checked = !!fresh.permissions.gamepad;
                    kmCb.checked = !!fresh.permissions.keyboardMouse;
                    gamepadCb.disabled = true;
                    kmCb.disabled = true;
                    paintAccess();
                }
            }
        };
        gamepadCb.addEventListener('change', push);
        kmCb.addEventListener('change', push);

        const copyBtn = /** @type {HTMLElement} */ (overlay.querySelector('.share-copy-btn'));
        const linkInput = /** @type {HTMLInputElement} */ (
            overlay.querySelector('.share-link-input')
        );
        copyBtn.addEventListener('click', async () => {
            try {
                await navigator.clipboard.writeText(linkInput.value);
            } catch (_) {
                // Clipboard API needs a secure context and a permission the
                // browser may withhold — select the text so Ctrl+C still works.
                linkInput.select();
            }
            copyBtn.textContent = t('common.copied');
            setTimeout(() => {
                copyBtn.textContent = t('common.copy');
            }, 1500);
        });

        const close = async () => {
            if (!this._popin) return;
            this._popin.remove();
            this._popin = null;
            // Closing is what freezes the choice: the link is out there now.
            try {
                await BackendClient.shareLock(slot);
            } catch (err) {
                console.warn('[ShareMenu] lock failed:', err);
            }
            await this.refresh();
        };
        overlay.querySelector('.share-done-btn').addEventListener('click', close);
        overlay.addEventListener('click', (e) => {
            if (e.target === overlay) close();
        });
    }
}
