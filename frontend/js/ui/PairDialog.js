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
 * MoonlightWeb — Pairing dialog
 *
 * Flow:
 *   1. GET /api/hosts/:id/pair → backend does stage 1, returns PIN
 *   2. Dialog displays PIN, auto-polls POST /api/hosts/:id/pair (stages 2-5)
 *   3. User enters PIN in Sunshine → next POST poll succeeds → paired
 */
import { BackendClient } from '../api/BackendClient.js';
import { t } from '../i18n/i18n.js';
import { escapeHtml } from '../util/escapeHtml.js';

export class PairDialog {
    constructor(host) {
        this.host = host;
        this.overlay = null;
        this.pin = null;
        this.pollTimer = null;
        this.onComplete = null;
        this.onCancel = null;
    }

    show() {
        this.render();
        document.body.appendChild(this.overlay);
        this.bindEvents();
        this.startPairing();
    }

    close() {
        this.stopPolling();
        if (this.overlay) {
            this.overlay.remove();
            this.overlay = null;
        }
    }

    async startPairing() {
        const statusEl = this.overlay.querySelector('.pairing-status-text');
        const pinEl = this.overlay.querySelector('.pairing-pin-display');

        try {
            const result = await BackendClient.getPairState(this.host.uuid);

            if (result.status === 'initiated' && result.pin) {
                this.pin = result.pin;
                pinEl.textContent = this.pin;
                pinEl.classList.add('visible');
                statusEl.textContent = t('pairing.enterPin');
                statusEl.className = 'pairing-status-text pairing-info';

                // Auto-poll for completion
                this.pollPairing();
            } else if (result.status === 'error') {
                statusEl.textContent = result.message;
                statusEl.className = 'pairing-status-text pairing-error';
            } else {
                statusEl.textContent = t('pairing.unexpectedResponse');
                statusEl.className = 'pairing-status-text pairing-error';
            }
        } catch (err) {
            statusEl.textContent = t('pairing.contactFailed', { message: err.message });
            statusEl.className = 'pairing-status-text pairing-error';
        }
    }

    pollPairing() {
        // Stage 1 blocks up to 60s — schedule next poll only after previous completes
        const tick = async () => {
            if (!this.overlay) return; // dialog closed
            await this.tryComplete();
            if (!this.overlay) return;
            this.pollTimer = setTimeout(tick, 5000);
        };
        this.pollTimer = setTimeout(tick, 5000);
    }

    stopPolling() {
        if (this.pollTimer) {
            clearTimeout(this.pollTimer);
            this.pollTimer = null;
        }
    }

    async tryComplete() {
        const statusEl = this.overlay.querySelector('.pairing-status-text');

        try {
            const result = await BackendClient.confirmPairing(this.host.uuid);

            if (result.status === 'paired') {
                this.stopPolling();
                statusEl.textContent = t('pairing.paired');
                statusEl.className = 'pairing-status-text pairing-success';
                if (this.onComplete) this.onComplete();
                setTimeout(() => this.close(), 1500);
            } else if (result.status === 'awaiting_pin') {
                // Still waiting — keep polling, keep the PIN displayed
            } else if (result.status === 'error') {
                this.stopPolling();
                statusEl.textContent = result.message;
                statusEl.className = 'pairing-status-text pairing-error';
            }
        } catch (err) {
            // Network error — keep polling
        }
    }

    render() {
        this.overlay = document.createElement('div');
        this.overlay.className = 'pairing-overlay';
        this.overlay.innerHTML = `
            <div class="pairing-dialog">
                <h3>${this.esc(t('pairing.title', { name: this.host.displayName }))}</h3>
                <p class="pairing-instruction">
                    ${t('pairing.instruction')}
                </p>
                <div class="pairing-status">
                    <div class="pairing-pin-display">----</div>
                    <p class="pairing-status-text pairing-info">${t('pairing.contacting')}</p>
                </div>
                <div class="pairing-actions">
                    <button class="btn btn-secondary btn-pair-cancel">${t('common.cancel')}</button>
                </div>
            </div>
        `;
    }

    bindEvents() {
        this.overlay.querySelector('.btn-pair-cancel').addEventListener('click', () => {
            this.close();
            if (this.onCancel) this.onCancel();
        });

        this.overlay.addEventListener('click', (e) => {
            if (e.target === this.overlay) {
                this.close();
                if (this.onCancel) this.onCancel();
            }
        });
    }

    esc(text) {
        return escapeHtml(text);
    }
}
