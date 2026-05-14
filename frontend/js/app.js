/**
 * Moonlight-Web — Application entry point
 *
 * State machine:
 *   LOADING -> HOST_LIST -> APP_LIST -> STREAMING (Phase 5)
 *                        \-> PAIRING -/
 */
import { HostListView } from './ui/HostListView.js';
import { AppListView } from './ui/AppListView.js';
import { StreamView } from './ui/StreamView.js';
import { SettingsView } from './ui/SettingsView.js';
import { AdminView } from './ui/AdminView.js';
import { BackendClient } from './api/BackendClient.js';
import { Toast } from './ui/Toast.js';

// ── Global error handler ──────────────────────────────────────────────────────
// Catches runtime errors and module-load failures that would otherwise silently
// prevent the UI from rendering.  Shows a visible message in the main area.
window.addEventListener('error', (evt) => {
    console.error('[MW] Uncaught error:', evt.error || evt.message, evt);
    const main = document.getElementById('main-content');
    if (main && main.children.length === 0) {
        main.innerHTML = `
            <div class="hosts-view">
                <div class="hosts-header"><h2>Error</h2></div>
                <div class="hosts-error">
                    <p>Failed to load application</p>
                    <p class="hint">${(evt.error && evt.error.message) || evt.message || 'Unknown error'}</p>
                    <button class="btn" onclick="location.reload()">Retry</button>
                </div>
            </div>
        `;
    }
});

window.addEventListener('unhandledrejection', (evt) => {
    console.error('[MW] Unhandled Promise rejection:', evt.reason);
    const main = document.getElementById('main-content');
    if (main && main.children.length === 0) {
        const msg = (evt.reason && evt.reason.message) || String(evt.reason || 'Unknown error');
        main.innerHTML = `
            <div class="hosts-view">
                <div class="hosts-header"><h2>Initialization Error</h2></div>
                <div class="hosts-error">
                    <p>${msg}</p>
                    <button class="btn" onclick="location.reload()">Retry</button>
                </div>
            </div>
        `;
    }
});

const MoonlightApp = {
    state: 'loading',
    hostListView: null,
    appListView: null,
    streamView: null,
    settingsView: null,
    adminView: null,

    async init() {
        console.log('[MW] Initializing Moonlight-Web...');

        this._initNavButtons();
        this._initRouter();

        // Background async housekeeping — never blocks the initial render.
        this._initAsync();
    },

    /**
     * History API router: reads window.location.pathname for initial route
     * and listens to popstate for browser back/forward.
     */
    _initRouter() {
        window.addEventListener('popstate', (e) => {
            const state = e.state || {};
            const view = state.view || 'hosts';
            this._navigateByView(view, state);
        });

        // Determine initial view from URL path (no pushState — URL already matches)
        const path = window.location.pathname;
        let initialView = 'hosts';
        if (path === '/admin') initialView = 'admin';
        else if (path === '/settings') initialView = 'settings';
        else if (path === '/apps') initialView = 'apps';
        else if (path === '/streaming') initialView = 'streaming';

        const initialState = { view: initialView };
        history.replaceState(initialState, '', path);
        this._navigateByView(initialView, initialState);
    },

    /**
     * Non-blocking async setup: health check, etc.
     * Runs after the UI is visible so a slow/failed fetch doesn't hide it.
     */
    async _initAsync() {
        try {
            const health = await BackendClient.get('/api/health');
            console.log('[MW] Server:', health);
        } catch (err) {
            console.warn('[MW] Server health check failed:', err);
        }
    },

    _initNavButtons() {
        // Admin button (localhost only, gear icon)
        const btnAdmin = document.getElementById('btn-admin');
        if (btnAdmin) {
            // Hide admin button if not localhost
            if (window.location.hostname !== 'localhost' &&
                window.location.hostname !== '127.0.0.1' &&
                window.location.hostname !== '[::1]') {
                btnAdmin.style.display = 'none';
            }

            btnAdmin.addEventListener('click', () => {
                if (this.state === 'admin') {
                    this.showHostList();
                } else {
                    this.showAdmin();
                }
            });
        }

        // Settings button (streaming params)
        const btnSettings = document.getElementById('btn-settings');
        if (btnSettings) {
            btnSettings.addEventListener('click', () => {
                if (this.state === 'settings') {
                    this.showHostList();
                } else {
                    this.showSettings();
                }
            });
        }
    },

    _updateNavHighlight(active) {
        const btnAdmin = document.getElementById('btn-admin');
        if (btnAdmin) {
            btnAdmin.classList.toggle('nav-active', active === 'admin');
        }
        const btnSettings = document.getElementById('btn-settings');
        if (btnSettings) {
            btnSettings.classList.toggle('nav-active', active === 'settings');
        }
    },

    /**
     * Render a view by name without pushing history state.
     * Used internally by _initRouter() and the popstate handler.
     * URL/bookmark management is handled by the caller (pushState in show*()
     * methods, or the browser's own history for popstate).
     * @param {string} view - 'hosts', 'admin', 'settings', 'apps', or 'streaming'
     * @param {Object} [state] - History state for view restoration (e.g. { hostUuid, hostDisplayName })
     */
    _navigateByView(view, state) {
        this._clearCurrentView();
        const main = document.getElementById('main-content');
        if (!main) return;

        switch (view) {
            case 'admin':
                this.transition('admin');
                this.adminView = new AdminView(main, () => history.back());
                this.adminView.start();
                this._updateNavHighlight('admin');
                break;
            case 'settings':
                this.transition('settings');
                this.settingsView = new SettingsView(main, () => history.back());
                this.settingsView.start();
                this._updateNavHighlight('settings');
                break;
            case 'apps': {
                const hostState = state || {};
                if (hostState.hostUuid) {
                    this.transition('app_list');
                    this._clearCurrentView();
                    // Reconstruct a minimal host object from stored state
                    const host = {
                        uuid: hostState.hostUuid,
                        displayName: hostState.hostDisplayName || 'Host'
                    };
                    this.appListView = new AppListView(main, host);
                    this.appListView.onBack = () => {
                        this._clearCurrentView();
                        this.transition('host_list');
                        this.showHostList();
                    };
                    this.appListView.onLaunch = (app) => this.launchApp(host, app);
                    this._updateNavHighlight('hosts');
                } else {
                    // No host context — redirect to host list
                    this.showHostList();
                }
                break;
            }
            case 'streaming':
                // Streaming state is ephemeral — cannot restore from URL alone
                this.showHostList();
                break;
            default:
                this.transition('host_list');
                this.hostListView = new HostListView(main);
                this.hostListView.onLaunch = (host) => this.showAppList(host);
                this.hostListView.start();
                this._updateNavHighlight('hosts');
                break;
        }
    },

    // --- Navigation ---

    _clearCurrentView() {
        if (this.hostListView) {
            this.hostListView.destroy();
            this.hostListView = null;
        }
        if (this.appListView) {
            this.appListView.destroy();
            this.appListView = null;
        }
        if (this.streamView) {
            this.streamView.destroy();
            this.streamView = null;
        }
        if (this.settingsView) {
            this.settingsView.destroy();
            this.settingsView = null;
        }
        if (this.adminView) {
            this.adminView.destroy();
            this.adminView = null;
        }
    },

    showHostList() {
        history.pushState({ view: 'hosts' }, '', '/');
        this._navigateByView('hosts');
    },

    showAdmin() {
        history.pushState({ view: 'admin' }, '', '/admin');
        this._navigateByView('admin');
    },

    showSettings() {
        history.pushState({ view: 'settings' }, '', '/settings');
        this._navigateByView('settings');
    },

    showAppList(host) {
        history.pushState({ view: 'apps', hostUuid: host.uuid, hostDisplayName: host.displayName }, '', '/apps');
        this.transition('app_list');
        this._clearCurrentView();
        const main = document.getElementById('main-content');
        this.appListView = new AppListView(main, host);
        this.appListView.onBack = () => {
            this._clearCurrentView();
            this.transition('host_list');
            this.showHostList();
        };
        this.appListView.onLaunch = (app) => this.launchApp(host, app);
    },

    transition(newState) {
        console.log(`[MW] State: ${this.state} -> ${newState}`);
        this.state = newState;
    },

    async launchApp(host, app) {
        console.log(`[MW] Launching: ${app.name} (id=${app.id}) on ${host.displayName}`);
        this.transition('launching');
        Toast.info(`Launching ${app.name}...`);

        try {
            const result = await BackendClient.launchApp(host.uuid, app.id);
            console.log('[MW] Launch result:', result);

            if (result.status === 'streaming') {
                Toast.success(`${app.name} started`);
                history.pushState({ view: 'streaming' }, '', '/streaming');
                this.transition('streaming');

                this.streamView = new StreamView(
                    document.getElementById('app'),
                    result.signalingUrl || result.wsUrl,  // WebRTC signaling URL (fallback to WS for legacy)
                    host,
                    result.videoCodec,
                    result.gamingMode !== false
                );
            }
        } catch (err) {
            console.error('[MW] Launch failed:', err);
            Toast.error(err.message || 'Launch failed');
            this.transition('app_list');
        }
    }
};

// Boot
document.addEventListener('DOMContentLoaded', () => MoonlightApp.init());
