/**
 * Moonlight-Web — App list grid view
 */
import { BackendClient } from '../api/BackendClient.js';
import { App } from '../models/App.js';

export class AppListView {
    constructor(container, host) {
        this.container = container;
        this.host = host;
        this.apps = [];
        this.loading = true;
        this.error = null;
        this._loadInProgress = false;
        this._destroyed = false;
        this.render();
        this.load();
    }

    async load() {
        if (this._loadInProgress) return;
        this._loadInProgress = true;
        try {
            const data = await BackendClient.getAppList(this.host.uuid);
            if (data.status === 'ok') {
                this.apps = (data.apps || []).map(a => new App(a, this.host.uuid));
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
                    <button class="btn btn-secondary" id="btn-back-hosts">&larr; Back to Hosts</button>
                    <div class="apps-host-info">
                        <div class="apps-host-name">${this.esc(this.host.displayName)}</div>
                        <div class="apps-host-addr">${this.esc(this.host.displayAddress)}</div>
                    </div>
                    <div class="apps-spacer"></div>
                </div>
                ${this.apps.length === 0
                    ? `<div class="apps-empty">
                         <span class="empty-icon">\u{1F4E6}</span>
                         <p>No applications found</p>
                         <p class="hint">Add applications in Sunshine on the host</p>
                       </div>`
                    : `<div class="apps-grid">
                         ${this.apps.map(a => this.renderCard(a)).join('')}
                       </div>`
                }
            </div>
        `;

        this.bindEvents();
    }

    renderCard(app) {
        return `
            <div class="app-card" data-app-id="${app.id}">
                <div class="app-card-image">
                    ${app.boxArtUrl
                        ? `<img src="${this.esc(app.boxArtUrl)}"
                               alt="${this.esc(app.displayName)}"
                               loading="lazy"
                               onerror="this.parentElement.innerHTML='<span class=\\'app-icon\\'>\u{1F3AE}</span>'">`
                        : `<span class="app-icon">\u{1F3AE}</span>`
                    }
                </div>
                <div class="app-card-name">${this.esc(app.displayName)}</div>
                <div class="app-card-actions">
                    <button class="btn btn-launch" data-app-id="${app.id}">Launch</button>
                </div>
            </div>
        `;
    }

    renderLoading() {
        this.container.innerHTML = `
            <div class="apps-view">
                <div class="apps-header">
                    <button class="btn btn-secondary" id="btn-back-hosts">&larr; Back to Hosts</button>
                </div>
                <div class="apps-loading">
                    <p>Loading applications...</p>
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
                    <p>Failed to load applications</p>
                    <p class="hint">The host is unreachable or took too long to respond</p>
                    <button class="btn" id="btn-retry-apps">Retry</button>
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

        // Launch button delegation
        this.container.querySelectorAll('.btn-launch').forEach(btn => {
            btn.addEventListener('click', (e) => {
                const appId = parseInt(e.target.dataset.appId);
                const app = this.apps.find(a => a.id === appId);
                if (app) {
                    this.onLaunch && this.onLaunch(app);
                }
            });
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
