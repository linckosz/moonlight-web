/**
 * Moonlight-Web — Application entry point
 *
 * State machine:
 *   LOADING -> HOST_LIST -> APP_LIST -> STREAMING (Phase 5)
 *                        \-> PAIRING -/
 */
import { HostListView } from './ui/HostListView.js';
import { AppListView } from './ui/AppListView.js';
import { BackendClient } from './api/BackendClient.js';
import { Toast } from './ui/Toast.js';

const MoonlightApp = {
    state: 'loading',
    hostListView: null,
    appListView: null,

    async init() {
        console.log('[MW] Initializing Moonlight-Web...');

        try {
            const health = await BackendClient.get('/api/health');
            console.log('[MW] Server:', health);
        } catch (err) {
            console.warn('[MW] Server health check failed:', err);
        }

        this.transition('host_list');
        this.showHostList();
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
    },

    showHostList() {
        this._clearCurrentView();
        const main = document.getElementById('main-content');
        this.hostListView = new HostListView(main);
        this.hostListView.onLaunch = (host) => this.showAppList(host);
        this.hostListView.start();
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
                // Phase 5b: open stream view + WebSocket
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
