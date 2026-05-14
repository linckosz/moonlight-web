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

        // IMPORTANT: render the HostListView SYNCHRONOUSLY before any async
        // operations.  If the health check fetch hangs or throws, the user
        // would otherwise see an empty <main> (blank page).
        this._initNavButtons();
        this.showHostList();

        // Background async housekeeping — never blocks the initial render.
        this._initAsync();
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
        this.transition('host_list');
        this._clearCurrentView();
        const main = document.getElementById('main-content');
        if (!main) {
            console.error('[MW] #main-content not found in DOM');
            return;
        }
        this.hostListView = new HostListView(main);
        this.hostListView.onLaunch = (host) => this.showAppList(host);
        this.hostListView.start();
        this._updateNavHighlight('hosts');
    },

    showAdmin() {
        this.transition('admin');
        this._clearCurrentView();
        const main = document.getElementById('main-content');
        this.adminView = new AdminView(main, () => this.showHostList());
        this.adminView.start();
        this._updateNavHighlight('admin');
    },

    showSettings() {
        this.transition('settings');
        this._clearCurrentView();
        const main = document.getElementById('main-content');
        this.settingsView = new SettingsView(main, () => this.showHostList());
        this.settingsView.start();
        this._updateNavHighlight('settings');
    },

    showAppList(host) {
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
