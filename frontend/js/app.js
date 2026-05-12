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

const MoonlightApp = {
    state: 'loading',
    hostListView: null,
    appListView: null,
    streamView: null,
    settingsView: null,
    adminView: null,

    async init() {
        console.log('[MW] Initializing Moonlight-Web...');

        try {
            const health = await BackendClient.get('/api/health');
            console.log('[MW] Server:', health);
        } catch (err) {
            console.warn('[MW] Server health check failed:', err);
        }

        // Navigation buttons in header
        this._initNavButtons();

        // Check if DuckDNS is active and show public-access banner
        this.checkDdnsBanner();

        this.transition('host_list');
        this.showHostList();
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

    async checkDdnsBanner() {
        // Respect permanent dismissal
        if (localStorage.getItem('ddns_banner_dismissed')) return;

        try {
            const consent = await BackendClient.get('/api/ddns/consent');
            if (consent.active && consent.subdomain) {
                this.showDdnsBanner(consent.subdomain, consent.port || 48443);
            }
        } catch (err) {
            console.warn('[MW] Failed to check DuckDNS status:', err);
        }
    },

    showDdnsBanner(subdomain, port) {
        const app = document.getElementById('app');
        if (!app) return;

        // Avoid duplicates
        if (app.querySelector('.ddns-banner')) return;

        const banner = document.createElement('div');
        banner.className = 'ddns-banner';

        const publicUrl = `https://${subdomain}.duckdns.org:${port}`;

        banner.innerHTML = `
            <span class="ddns-banner-text">
                Public access: <a href="${publicUrl}" target="_blank" rel="noopener">${publicUrl}</a>
            </span>
            <button class="ddns-banner-close" title="Dismiss">&times;</button>
        `;

        banner.querySelector('.ddns-banner-close').addEventListener('click', () => {
            banner.remove();
            localStorage.setItem('ddns_banner_dismissed', 'true');
        });

        // Insert at the top of #app, above the header
        const header = app.querySelector('.app-header');
        if (header) {
            app.insertBefore(banner, header);
        } else {
            app.prepend(banner);
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
        this.settingsView = new SettingsView(main);
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
                    result.wsUrl,
                    host,
                    result.videoCodec
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
