/*
 * Moonlight-Web — browser-based Sunshine/GameStream client.
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
 * Moonlight-Web — App list grid view
 */
import { BackendClient } from '../api/BackendClient.js';
import { App } from '../models/App.js';
import { t } from '../i18n/i18n.js';

export class AppListView {
    constructor(container, host, preloadedApps = null) {
        this.container = container;
        this.host = host;
        this.apps = [];
        this.loading = true;
        this.error = null;
        this._loadInProgress = false;
        this._destroyed = false;
        // When the list was prefetched (e.g. during the stream-exit animation),
        // render the grid straight away — no loading flash, images already warm.
        if (Array.isArray(preloadedApps)) {
            this.apps = preloadedApps;
            this.loading = false;
            this.render();
        } else {
            this.render();
            this.load();
        }
    }

    async load() {
        if (this._loadInProgress) return;
        this._loadInProgress = true;
        try {
            const data = await BackendClient.getAppList(this.host.uuid);
            if (data.status === 'ok') {
                this.apps = (data.apps || []).map((a) => new App(a, this.host.uuid));
                this.error = null;
            } else {
                this.error = data.message || 'Failed to load apps';
            }
        } catch (err) {
            console.error('[MW] Failed to load app list:', err);
            this.error = err.message;
        }
        this._loadInProgress = false;
        this.loading = false;
        // Late response after navigation/stream start: never re-render —
        // it would overwrite whatever view now owns the container.
        if (this._destroyed) return;
        this.render();
    }

    destroy() {
        // Block any in-flight load() from rendering after teardown
        this._destroyed = true;
    }

    // --- Rendering ---

    render() {
        if (this.loading) {
            this.renderLoading();
            return;
        }

        if (this.error) {
            this.renderError();
            return;
        }

        this.container.innerHTML = `
            <div class="apps-view" id="view-apps">
                <div class="apps-header">
                    <button class="btn btn-secondary" id="btn-back-hosts">${t('apps.backToHosts')}</button>
                    <div class="apps-host-info">
                        <div class="apps-host-name">${this.esc(this.host.displayName)}</div>
                        <div class="apps-host-addr">${this.esc(this.host.displayAddress)}</div>
                    </div>
                    <div class="apps-spacer"></div>
                </div>
                ${
                    this.apps.length === 0
                        ? `<div class="apps-empty">
                         <span class="empty-icon">\u{1F4E6}</span>
                         <p>${t('apps.empty')}</p>
                         <p class="hint">${t('apps.emptyHint')}</p>
                       </div>`
                        : `<div class="apps-grid">
                         ${this.apps.map((a) => this.renderCard(a)).join('')}
                       </div>`
                }
            </div>
        `;

        this.bindEvents();
    }

    renderCard(app) {
        // The whole card is the launch action (no separate button). role/tabindex
        // keep it keyboard-accessible now that the <button> is gone.
        return `
            <div class="app-card" data-app-id="${app.id}"
                 role="button" tabindex="0"
                 aria-label="${this.esc(t('apps.launchAria', { name: app.displayName }))}">
                <div class="app-card-image">
                    ${
                        app.boxArtUrl
                            ? `<img src="${this.esc(app.boxArtUrl)}"
                               alt="${this.esc(app.displayName)}"
                               loading="lazy"
                               onerror="this.outerHTML='<span class=\\'app-icon\\'>\u{1F3AE}</span>'">`
                            : `<span class="app-icon">\u{1F3AE}</span>`
                    }
                </div>
                <div class="app-card-name">${this.esc(app.displayName)}</div>
            </div>
        `;
    }

    renderLoading() {
        this.container.innerHTML = `
            <div class="apps-view">
                <div class="apps-header">
                    <button class="btn btn-secondary" id="btn-back-hosts">${t('apps.backToHosts')}</button>
                </div>
                <div class="apps-loading">
                    <p>${t('apps.loading')}</p>
                </div>
            </div>
        `;
        this.bindBackButton();
    }

    renderError() {
        this.container.innerHTML = `
            <div class="apps-view">
                <div class="apps-header">
                    <button class="btn btn-secondary" id="btn-back-hosts">&larr; Back to Hosts</button>
                </div>
                <div class="apps-error">
                    <p>${t('apps.loadFailed')}</p>
                    <p class="hint">${t('apps.loadFailedHint')}</p>
                    <button class="btn" id="btn-retry-apps">${t('common.retry')}</button>
                </div>
            </div>
        `;
        this.bindEvents();
    }

    // --- Events ---

    bindEvents() {
        this.bindBackButton();

        const retryBtn = this.container.querySelector('#btn-retry-apps');
        if (retryBtn) {
            retryBtn.addEventListener('click', () => {
                this.loading = true;
                this.error = null;
                this.render();
                this.load();
            });
        }

        // The whole card is the launch action (click/tap anywhere, or Enter/
        // Space when focused). No separate Launch/Play button.
        this.container.querySelectorAll('.app-card').forEach((card) => {
            const launch = () => {
                // Ignore re-taps while a launch is already in flight on this card.
                if (card.classList.contains('app-card--launching')) return;
                const appId = parseInt(card.dataset.appId);
                const app = this.apps.find((a) => a.id === appId);
                if (app) {
                    this.setLaunching(card);
                    this.onLaunch && this.onLaunch(app);
                }
            };
            card.addEventListener('click', launch);
            card.addEventListener('keydown', (e) => {
                if (e.key === 'Enter' || e.key === ' ') {
                    e.preventDefault();
                    launch();
                }
            });
        });
    }

    // Switch a card into the "launching" state: a discreet Cyberpunk scan/glow
    // animation driven entirely by CSS, plus a status label over the name.
    setLaunching(card) {
        card.classList.add('app-card--launching');
        card.setAttribute('aria-busy', 'true');
        const nameEl = card.querySelector('.app-card-name');
        if (nameEl && !nameEl.dataset.label) {
            nameEl.dataset.label = nameEl.textContent;
            nameEl.textContent = t('apps.launching');
        }
    }

    // Revert any card stuck in the "launching" state (e.g. backend crash/timeout)
    // back to its idle appearance and restore the original app name.
    clearLaunching() {
        if (!this.container) return;
        this.container.querySelectorAll('.app-card--launching').forEach((card) => {
            card.classList.remove('app-card--launching');
            card.removeAttribute('aria-busy');
            const nameEl = card.querySelector('.app-card-name');
            if (nameEl && nameEl.dataset.label) {
                nameEl.textContent = nameEl.dataset.label;
                delete nameEl.dataset.label;
            }
        });
    }

    bindBackButton() {
        const backBtn = this.container.querySelector('#btn-back-hosts');
        if (backBtn) {
            backBtn.addEventListener('click', () => {
                this.destroy();
                this.onBack && this.onBack();
            });
        }
    }

    // --- Helpers ---

    esc(text) {
        const div = document.createElement('div');
        div.textContent = text;
        return div.innerHTML;
    }
}
