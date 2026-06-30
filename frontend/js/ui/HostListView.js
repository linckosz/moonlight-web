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
 * MoonlightWeb — Host list view component
 *
 * Single-page model: each host box carries its own state inline.
 *   - online + paired  → app grid (loaded per host, click to launch)
 *   - online + locked  → centered "Pair" action
 *   - offline + MAC    → centered "Wake on LAN" action
 *   - offline          → centered offline message
 * The online/offline badge sits next to the host name; "Remove" lives in a
 * per-host "…" menu.
 */
import { BackendClient } from '../api/BackendClient.js';
import { Host } from '../models/Host.js';
import { App } from '../models/App.js';
import { PairDialog } from './PairDialog.js';
import { Toast } from './Toast.js';
import { t } from '../i18n/i18n.js';
import { Icons } from './icons.js';

export class HostListView {
    constructor(container) {
        this.container = container;
        this.hosts = [];
        this.pollTimer = null;
        this.eventsBound = false;
        this.onLaunchApp = null; // called with (host, app) when an app card is clicked
        // Per-host app cache: uuid -> { apps: App[] } | { error: string } | { loading: true }
        this.appsByHost = {};
        this._active = false;
        this._destroyed = false;
        this.renderShell();

        // Auto-scan when page gains focus
        this._visibilityHandler = () => {
            if (!document.hidden && !this._destroyed) this._autoScan();
        };
        document.addEventListener('visibilitychange', this._visibilityHandler);

        // Close any open kebab menu when clicking outside of it.
        this._docClickHandler = (e) => {
            if (this._destroyed) return;
            if (!e.target.closest('.host-card-menu')) this._closeAllMenus();
        };
        document.addEventListener('click', this._docClickHandler);

        // Button delegation — bound once, cleaned up in destroy()
        this._clickHandler = (e) => {
            if (this._destroyed) return;

            // ── Kebab menu toggle ──────────────────────────────────────────
            const menuBtn = e.target.closest('.btn-host-menu');
            if (menuBtn) {
                e.stopPropagation();
                const card = menuBtn.closest('.host-card');
                const menu = card && card.querySelector('.host-menu');
                if (!menu) return;
                const wasHidden = menu.hasAttribute('hidden');
                this._closeAllMenus();
                if (wasHidden) {
                    menu.removeAttribute('hidden');
                    menuBtn.setAttribute('aria-expanded', 'true');
                }
                return;
            }

            const pairBtn = e.target.closest('.btn-pair');
            if (pairBtn) {
                const uuid = pairBtn.dataset.uuid;
                const host = this.hosts.find((h) => h.uuid === uuid);
                if (host) {
                    this.stop();
                    const dialog = new PairDialog(host);
                    dialog.onComplete = () => {
                        this.start(); // start() already calls refresh()
                    };
                    dialog.onCancel = () => this.start();
                    dialog.show();
                }
                return;
            }

            const wolBtn = e.target.closest('.btn-wol');
            if (wolBtn) {
                const uuid = wolBtn.dataset.uuid;
                const host = this.hosts.find((h) => h.uuid === uuid);
                wolBtn.disabled = true;
                BackendClient.wakeHost(uuid)
                    .then(() => {
                        Toast.show(
                            t('hosts.wolSent', { name: host ? host.displayName : uuid }),
                            'success',
                        );
                    })
                    .catch((err) => {
                        console.error('[MW] Wake-on-LAN failed:', err);
                        Toast.show(err.message, 'error');
                    })
                    .finally(() => {
                        wolBtn.disabled = false;
                    });
                return;
            }

            const removeBtn = e.target.closest('.btn-remove');
            if (removeBtn) {
                removeBtn.disabled = true;
                removeBtn.textContent = t('common.removing');
                const uuid = removeBtn.dataset.uuid;
                BackendClient.removeHost(uuid)
                    .then(() => {
                        this.hosts = this.hosts.filter((h) => h.uuid !== uuid);
                        delete this.appsByHost[uuid];
                        this.renderList();
                    })
                    .catch((err) => {
                        console.error('[MW] Remove host failed:', err);
                        this.refresh();
                    });
                return;
            }

            // ── App card launch ───────────────────────────────────────────
            const appCard = e.target.closest('.app-card');
            if (appCard) {
                if (appCard.classList.contains('app-card--launching')) return;
                const hostCard = appCard.closest('.host-card');
                const uuid = hostCard && hostCard.dataset.uuid;
                const host = this.hosts.find((h) => h.uuid === uuid);
                const entry = this.appsByHost[uuid];
                const appId = parseInt(appCard.dataset.appId, 10);
                const app = entry && entry.apps && entry.apps.find((a) => a.id === appId);
                if (host && app && this.onLaunchApp) {
                    this.setLaunching(appCard);
                    this.stop(); // pause polling so a refresh can't wipe the launching card
                    this.onLaunchApp(host, app);
                }
            }
        };
        this.container.addEventListener('click', this._clickHandler);

        // App-card keyboard activation (Enter/Space) — cards are role="button".
        this._keydownHandler = (e) => {
            if (this._destroyed) return;
            if (e.key !== 'Enter' && e.key !== ' ') return;
            const appCard = e.target.closest && e.target.closest('.app-card');
            if (!appCard) return;
            e.preventDefault();
            appCard.click();
        };
        this.container.addEventListener('keydown', this._keydownHandler);
    }

    // --- Public API ---

    start() {
        this._active = true;
        this._autoScan();
        this.scheduleNextPoll(0); // immediate first refresh
    }

    async _autoScan() {
        if (!this._active) return;
        try {
            await BackendClient.scanHosts();
        } catch (err) {
            // Silent — scan is best-effort, errors are expected
        }
    }

    stop() {
        this._active = false;
        if (this.pollTimer) {
            clearTimeout(this.pollTimer);
            this.pollTimer = null;
        }
    }

    destroy() {
        this._active = false;
        this._destroyed = true;
        this.stop();
        if (this._clickHandler) {
            this.container.removeEventListener('click', this._clickHandler);
            this._clickHandler = null;
        }
        if (this._keydownHandler) {
            this.container.removeEventListener('keydown', this._keydownHandler);
            this._keydownHandler = null;
        }
        if (this._docClickHandler) {
            document.removeEventListener('click', this._docClickHandler);
            this._docClickHandler = null;
        }
        if (this._visibilityHandler) {
            document.removeEventListener('visibilitychange', this._visibilityHandler);
            this._visibilityHandler = null;
        }
    }

    scheduleNextPoll(delay = 3000) {
        if (!this._active || this._destroyed) return;
        this.pollTimer = setTimeout(async () => {
            if (!this._active || this._destroyed) return;
            await this.refresh();
            // Don't re-schedule if stop() was called during refresh
            if (this._active && !this._destroyed) this.scheduleNextPoll();
        }, delay);
    }

    async refresh() {
        if (!this._active) return;
        try {
            const data = await BackendClient.getHosts();
            // Re-check after await: stop()/destroy() may have run while the
            // request was in flight (e.g. a stream was launched) — a late
            // render here would overwrite the current view.
            if (!this._active || this._destroyed) return;
            const serverHosts = data.hosts || [];
            const before = this._fingerprint();
            // Merge — add/update, never remove
            for (const h of serverHosts) {
                const host = new Host(h);
                const idx = this.hosts.findIndex((ex) => ex.uuid === host.uuid);
                if (idx >= 0) this.hosts[idx] = host;
                else this.hosts.push(host);
            }
            const after = this._fingerprint();
            if (before !== after) this.renderList();
        } catch (err) {
            console.error('[MW] Failed to refresh hosts:', err);
            // Show error only on first load; keep existing data on refresh
            if (this.hosts.length === 0) this.renderError(err.message);
        }
    }

    // Stable fingerprint of host list — skips re-render when nothing changed
    _fingerprint() {
        return JSON.stringify(
            this.hosts.map((h) => [
                h.uuid,
                h.state,
                h.pairState,
                h.name,
                h.activeAddress,
                h.port,
                h.gpuModel,
                h.displayModes,
            ]),
        );
    }

    _cardFingerprint(host) {
        return [
            host.uuid,
            host.state,
            host.pairState,
            host.name,
            host.activeAddress,
            host.port,
            host.gpuModel,
            host.macAddress,
            JSON.stringify(host.displayModes),
        ].join('|');
    }

    // --- Rendering ---

    // Render the outer shell once (header + empty list container)
    renderShell() {
        this.container.innerHTML = `
            <div class="hosts-view" id="view-hosts">
                <div class="hosts-header">
                    <h2>${t('hosts.title')}</h2>
                    <div class="hosts-actions">
                        <button class="btn btn-neutral" id="btn-manual">${t('hosts.addManually')}</button>
                    </div>
                </div>
                <div class="hosts-list" id="hosts-list"></div>
            </div>
        `;
        this.bindEvents();
    }

    // Patch only the host cards — never touches the header
    renderList() {
        const list = this.container.querySelector('#hosts-list');
        if (!list) return;

        // Empty state
        if (this.hosts.length === 0) {
            list.innerHTML = `
                <div class="hosts-empty">
                    <span class="empty-icon">\u{1F5B4}</span>
                    <p>${t('hosts.empty')}</p>
                    <p class="hint">${t('hosts.emptyHint')}</p>
                </div>`;
            return;
        }

        // Remove empty placeholder if present
        const empty = list.querySelector('.hosts-empty');
        if (empty) empty.remove();

        const currentUuids = new Set(this.hosts.map((h) => h.uuid));

        // Remove cards for hosts no longer in the list
        const existingCards = list.querySelectorAll('.host-card');
        for (const card of existingCards) {
            if (!currentUuids.has(card.dataset.uuid)) card.remove();
        }

        // Insert or update cards in order
        for (let i = 0; i < this.hosts.length; i++) {
            const host = this.hosts[i];
            const fp = this._cardFingerprint(host);
            let card = list.querySelector(`.host-card[data-uuid="${host.uuid}"]`);

            if (!card) {
                // New host — insert at correct position
                const tmp = document.createElement('div');
                tmp.innerHTML = this.renderCard(host);
                card = tmp.firstElementChild;
                card.dataset.fingerprint = fp;
                // Insert at the right position
                const allCards = list.querySelectorAll('.host-card');
                if (i < allCards.length) {
                    list.insertBefore(card, allCards[i]);
                } else {
                    list.appendChild(card);
                }
            } else if (card.dataset.fingerprint !== fp) {
                // Data changed — replace just this card
                const tmp = document.createElement('div');
                tmp.innerHTML = this.renderCard(host);
                const newCard = tmp.firstElementChild;
                newCard.dataset.fingerprint = fp;
                card.replaceWith(newCard);
            }
        }

        // Fill in app grids for available hosts (cache hit = instant).
        for (const host of this.hosts) {
            if (host.isAvailable) this._ensureAppsLoaded(host);
        }
    }

    renderCard(host) {
        const cls = host.statusClass;
        return `
            <div class="host-card ${cls}" data-uuid="${host.uuid}">
                <div class="host-card-head">
                    <div class="host-card-icon">
                        <span class="status-icon ${cls}">${host.statusIcon}</span>
                    </div>
                    <div class="host-card-info">
                        <div class="host-name-row">
                            <span class="host-name">${this.esc(host.displayName)}</span>
                            <span class="status-badge ${cls}">${host.statusLabel}</span>
                        </div>
                        <div class="host-address">${this.esc(host.displayAddress)}</div>
                        ${
                            host.displayGpu
                                ? `<div class="host-gpu">${this.esc(host.displayGpu)}</div>`
                                : ''
                        }
                        ${
                            host.resolutionText
                                ? `<div class="host-resolution">${this.esc(host.resolutionText)}</div>`
                                : ''
                        }
                    </div>
                    <div class="host-card-menu">
                        <button class="btn-icon btn-host-menu" data-uuid="${host.uuid}"
                                aria-haspopup="true" aria-expanded="false"
                                aria-label="${this.esc(t('hosts.menuAria'))}">${Icons.menu}</button>
                        <div class="host-menu" hidden>
                            <button class="host-menu-item btn-remove" data-uuid="${host.uuid}">${t('common.remove')}</button>
                        </div>
                    </div>
                </div>
                <div class="host-card-body">
                    ${this.renderBody(host)}
                </div>
            </div>
        `;
    }

    // Body content driven by the host state.
    renderBody(host) {
        if (host.isAvailable) {
            // App grid filled asynchronously by _ensureAppsLoaded().
            return `<div class="host-apps" data-uuid="${host.uuid}"></div>`;
        }
        if (host.isLocked) {
            return `<div class="host-body-center">
                        <button class="btn btn-secondary btn-pair" data-uuid="${host.uuid}">${t('common.pair')}</button>
                    </div>`;
        }
        if (host.canWake) {
            return `<div class="host-body-center">
                        <button class="btn btn-secondary btn-wol" data-uuid="${host.uuid}"
                                title="${this.esc(t('hosts.wakeTitle'))}">${Icons.power}${t('hosts.wakeBtn')}</button>
                    </div>`;
        }
        // Offline with no usable MAC — nothing to do but show status.
        return `<div class="host-body-center host-offline-msg">${t('hosts.offlineMessage')}</div>`;
    }

    // --- Per-host app loading ---

    // Ensure the app grid for an available host is loaded and painted into its
    // card. Uses a dataset flag so a re-render (host poll) never repaints an
    // already-filled grid and clobbers a launching card.
    _ensureAppsLoaded(host) {
        const uuid = host.uuid;
        const cont = this.container.querySelector(`.host-card[data-uuid="${uuid}"] .host-apps`);
        if (!cont || cont.dataset.loaded === '1') return;

        const entry = this.appsByHost[uuid];
        if (entry && (entry.apps || entry.error)) {
            this._paintApps(cont, entry);
            cont.dataset.loaded = '1';
            return;
        }
        if (entry && entry.loading) return; // request in flight, placeholder shown

        cont.innerHTML = `<div class="host-apps-loading">${t('apps.loading')}</div>`;
        this.appsByHost[uuid] = { loading: true };
        BackendClient.getAppList(uuid)
            .then((data) => {
                if (data && data.status === 'ok') {
                    this.appsByHost[uuid] = {
                        apps: (data.apps || []).map((a) => new App(a, uuid)),
                    };
                } else {
                    this.appsByHost[uuid] = { error: (data && data.message) || 'load_failed' };
                }
            })
            .catch((err) => {
                console.error('[MW] Failed to load app list:', err);
                this.appsByHost[uuid] = { error: err.message };
            })
            .finally(() => {
                if (this._destroyed) return;
                const c2 = this.container.querySelector(
                    `.host-card[data-uuid="${uuid}"] .host-apps`,
                );
                if (c2) {
                    this._paintApps(c2, this.appsByHost[uuid]);
                    c2.dataset.loaded = '1';
                }
            });
    }

    _paintApps(cont, entry) {
        if (!entry || entry.error) {
            cont.innerHTML = `<div class="host-apps-msg host-apps-error">${t('apps.loadFailed')}</div>`;
            return;
        }
        const apps = entry.apps || [];
        if (apps.length === 0) {
            cont.innerHTML = `<div class="host-apps-msg">${t('apps.empty')}</div>`;
            return;
        }
        cont.innerHTML = `<div class="apps-grid">${apps.map((a) => this.renderApp(a)).join('')}</div>`;
    }

    renderApp(app) {
        // The whole card is the launch action. role/tabindex keep it
        // keyboard-accessible (Enter/Space handled by the keydown delegate).
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

    _closeAllMenus() {
        this.container.querySelectorAll('.host-menu:not([hidden])').forEach((m) => {
            m.setAttribute('hidden', '');
        });
        this.container
            .querySelectorAll('.btn-host-menu[aria-expanded="true"]')
            .forEach((b) => b.setAttribute('aria-expanded', 'false'));
    }

    renderError(message) {
        this.container.innerHTML = `
            <div class="hosts-view">
                <div class="hosts-header">
                    <h2>${t('hosts.title')}</h2>
                    <button class="btn" onclick="location.reload()">${t('common.retry')}</button>
                </div>
                <div class="hosts-error">
                    <p>${t('hosts.connectionLost')}</p>
                    <p class="hint">${this.esc(message)}</p>
                </div>
            </div>
        `;
    }

    // --- Events ---

    bindEvents() {
        const manualBtn = this.container.querySelector('#btn-manual');
        if (manualBtn) {
            manualBtn.addEventListener('click', async () => {
                const addr = prompt(t('hosts.promptManual'));
                if (!addr || !addr.trim()) return;
                manualBtn.disabled = true;
                manualBtn.classList.add('btn-loading');
                try {
                    const data = await BackendClient.addManualHost(addr.trim());
                    if (data.hosts && data.hosts.length > 0) {
                        const newHost = new Host(data.hosts[0]);
                        const idx = this.hosts.findIndex((h) => h.uuid === newHost.uuid);
                        if (idx >= 0) {
                            this.hosts[idx] = newHost;
                            this.renderList();
                            Toast.show(
                                t('hosts.alreadyInList', { name: newHost.displayName }),
                                'warning',
                            );
                        } else {
                            this.hosts.push(newHost);
                            this.renderList();
                            Toast.show(
                                t('hosts.addedSuccess', { name: newHost.displayName }),
                                'success',
                            );
                        }
                    } else {
                        Toast.show(t('hosts.noHostReturned'), 'error');
                    }
                } catch (err) {
                    console.error('[MW] Add host failed:', err);
                    Toast.show(err.message, 'error');
                } finally {
                    manualBtn.disabled = false;
                    manualBtn.classList.remove('btn-loading');
                }
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
