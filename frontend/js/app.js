/**
 * Moonlight-Web — Application entry point
 *
 * Navigation architecture:
 *
 *   Main views (with history entries):
 *     - hosts  at  "/"
 *     - apps   at  "/apps"  (with host context in history state)
 *
 *   Overlays (no persistent history entry):
 *     - admin     — guard pushState + URL "/admin"  (survives refresh)
 *     - settings  — guard pushState, no URL change
 *     - streaming — guard pushState, no URL change
 *
 *   History stack possibilities:
 *     [{hosts}]                                         — on Hosts
 *     [{hosts}, {apps, hostUuid, hostDisplayName}]      — on Apps
 *     [{hosts}, {apps, ...}, {overlay-guard, ...}]      — on Apps + overlay
 *
 *   Back button: overlay guard is consumed first (→ close overlay),
 *   then main view transitions happen normally.
 *
 *   Refresh behavior:
 *     /admin     → stays on Admin (overlay on hosts)
 *     /          → stays on Hosts
 *     /apps      → redirect to Hosts
 *     /settings  → redirect to Hosts
 *     /streaming → redirect to Hosts
 */
import { HostListView } from './ui/HostListView.js';
import { AppListView } from './ui/AppListView.js';
import { StreamView } from './ui/StreamView.js';
import { SettingsView } from './ui/SettingsView.js';
import { AdminView } from './ui/AdminView.js';
import { BackendClient } from './api/BackendClient.js';
import { Toast } from './ui/Toast.js';

// ── Global error handler ──────────────────────────────────────────────────────
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
    // ── View instances persisted across overlays ─────────────────────────────
    state: 'loading',
    hostListView: null,
    appListView: null,
    streamView: null,
    settingsView: null,
    adminView: null,

    // ── Navigation state (never destroyed by overlays) ──────────────────────
    _nav: {
        /** 'hosts' | 'apps' — the underlying main view */
        mainView: 'hosts',
        /** Host context for apps view: { hostUuid, hostDisplayName } */
        mainState: {},
        /** null | 'admin' | 'settings' | 'streaming' — current overlay */
        overlay: null,
    },

    async init() {
        console.log('[MW] Initializing Moonlight-Web...');

        this._createOverlayRoot();
        this._initNavButtons();
        this._initRouter();

        // Background async housekeeping — never blocks the initial render.
        this._initAsync();
    },

    /**
     * Create a fixed overlay container for admin/settings views.
     */
    _createOverlayRoot() {
        const root = document.createElement('div');
        root.id = 'overlay-root';
        document.getElementById('app').appendChild(root);
    },

    // =========================================================================
    // History API Router
    // =========================================================================

    /** Prefix for overlay guard states — used to distinguish from main views */
    _GUARD_PREFIX: 'overlay::',

    /**
     * History API router:
     *   - popstate: if overlay is open → close it and consume the guard entry.
     *               Otherwise, navigate to the main view from the state.
     *   - Initial route: reads pathname. Only "/" and "/admin" survive refresh.
     */
    _initRouter() {
        window.addEventListener('popstate', (e) => {
            const state = e.state || {};

            // ── Overlay guard was popped — close overlay, navigate to revealed state ──
            if (this._nav.overlay) {
                if (this._nav.overlay === 'streaming') {
                    // Streaming: quit may be async — fire it, don't await.
                    // Mark handled so _onStreamingQuit doesn't re-navigate.
                    if (this.streamView) {
                        if (!this.streamView._quitting) {
                            this.streamView.quit();
                        }
                        this.streamView = null;
                    }
                    this._nav.overlay = null;
                } else {
                    // Admin / settings: close synchronously
                    this._closeOverlay();
                }

                // Navigate to the main view revealed by popping the guard.
                if (state.hostUuid) {
                    this._setMainView('apps', {
                        hostUuid: state.hostUuid,
                        hostDisplayName: state.hostDisplayName || 'Host'
                    });
                } else {
                    this._setMainView('hosts', {});
                }
                return;
            }

            // ── Normal main-view navigation ──────────────────────────────────
            const viewName = state.view || 'hosts';
            this._navigateToMainView(viewName, state);
        });

        // ── Initial route ──────────────────────────────────────────────────
        const path = window.location.pathname;
        let mainView, mainState, initialOverlay;

        if (path === '/admin') {
            // Admin survives refresh — shown as overlay on hosts
            mainView = 'hosts';
            mainState = { view: 'hosts' };
            initialOverlay = 'admin';
        } else {
            // All other paths → hosts (including apps, settings, streaming)
            mainView = 'hosts';
            mainState = { view: 'hosts' };
        }

        history.replaceState(mainState, '', path === '/admin' ? '/admin' : '/');

        this._nav.mainView = mainView;
        this._nav.mainState = mainState;
        this._renderMainView(mainView, mainState);

        if (initialOverlay) {
            this._openOverlay(initialOverlay);
        }
    },

    // =========================================================================
    // Main View Management
    // =========================================================================

    /**
     * Navigate to a main view (hosts or apps), cleaning up any overlay first.
     * Called from popstate handler and showHostList/showAppList.
     */
    _navigateToMainView(view, state) {
        // Close any open overlay before switching main view
        this._closeOverlay();

        if (view === 'apps' && state && state.hostUuid) {
            this._setMainView('apps', {
                hostUuid: state.hostUuid,
                hostDisplayName: state.hostDisplayName || 'Host'
            });
        } else {
            this._setMainView('hosts', {});
        }
    },

    /**
     * Set the main view (hosts or apps) and render it into #main-content.
     * Destroys the previous main view and all overlays.
     * Also fixes the URL if admin overlay left it at /admin.
     */
    _setMainView(view, state) {
        this._closeOverlay();
        this._destroyMainViews();

        this._nav.mainView = view;
        this._nav.mainState = state;

        const main = document.getElementById('main-content');
        if (!main) return;

        if (view === 'apps') {
            this.transition('app_list');
            const host = {
                uuid: state.hostUuid,
                displayName: state.hostDisplayName || 'Host'
            };
            this.appListView = new AppListView(main, host);
            this.appListView.onBack = () => {
                // Pop the apps entry — reveals hosts below, popstate navigates there.
                history.back();
            };
            this.appListView.onLaunch = (app) => this.launchApp(host, app);
            this._updateNavHighlight('hosts');
        } else {
            this.transition('host_list');
            this.hostListView = new HostListView(main);
            this.hostListView.onLaunch = (host) => this.showAppList(host);
            this.hostListView.start();
            this._updateNavHighlight('hosts');
        }

        // Ensure URL is correct after navigation — admin overlay may leave
        // URL at /admin when closing via refresh→close or popstate.
        if (window.location.pathname === '/admin') {
            if (this._nav.mainView === 'apps' && this._nav.mainState.hostUuid) {
                history.replaceState({
                    view: 'apps',
                    hostUuid: this._nav.mainState.hostUuid,
                    hostDisplayName: this._nav.mainState.hostDisplayName
                }, '', '/apps');
            } else {
                history.replaceState({ view: 'hosts' }, '', '/');
            }
        }
    },

    /**
     * Render a main view by name without pushing history or altering _nav.
     * Used by _initRouter and the popstate → _setMainView chain.
     */
    _renderMainView(view, state) {
        this._destroyMainViews();
        const main = document.getElementById('main-content');
        if (!main) return;

        if (view === 'apps' && state && state.hostUuid) {
            this.transition('app_list');
            const host = {
                uuid: state.hostUuid,
                displayName: state.hostDisplayName || 'Host'
            };
            this.appListView = new AppListView(main, host);
            this.appListView.onBack = () => {
                // Pop the apps entry — reveals hosts below, popstate navigates there.
                history.back();
            };
            this.appListView.onLaunch = (app) => this.launchApp(host, app);
            this._updateNavHighlight('hosts');
        } else {
            this.transition('host_list');
            this.hostListView = new HostListView(main);
            this.hostListView.onLaunch = (host) => this.showAppList(host);
            this.hostListView.start();
            this._updateNavHighlight('hosts');
        }
    },

    // =========================================================================
    // Overlay Management
    // =========================================================================

    /**
     * Open an overlay (admin, settings) over the current main view.
     * Pushes a guard state so pressing Back closes the overlay without
     * leaving a history entry.
     * @param {string} type - 'admin' | 'settings'
     */
    _openOverlay(type) {
        if (this._nav.overlay) {
            console.warn('[MW] Overlay already open:', this._nav.overlay);
            return;
        }
        this._nav.overlay = type;

        // Pause any main view timers (e.g. HostListView polling)
        if (this.hostListView && typeof this.hostListView.stop === 'function') {
            this.hostListView.stop();
        }

        const root = document.getElementById('overlay-root');
        if (!root) return;

        // Push a guard state so back closes the overlay
        const guardState = {
            view: this._GUARD_PREFIX + type,
            mainView: this._nav.mainView,
            hostUuid: this._nav.mainState.hostUuid,
            hostDisplayName: this._nav.mainState.hostDisplayName
        };

        if (type === 'admin') {
            history.pushState(guardState, '', '/admin');
        } else {
            history.pushState(guardState, '');
        }

        root.style.display = 'block';
        root.innerHTML = '';

        if (type === 'admin') {
            this.transition('admin');
            this.adminView = new AdminView(root, () => {
                // Close the overlay, then pop the guard from history.
                // history.back() fires popstate which navigates to the
                // revealed main view.  _setMainView will fix any /admin URL.
                this._closeOverlay();
                history.back();
            });
            this.adminView.start();
            this._updateNavHighlight('admin');
        } else if (type === 'settings') {
            this.transition('settings');
            this.settingsView = new SettingsView(root, () => {
                this._closeOverlay();
                // Pop the guard so Back doesn't see a stale overlay state.
                history.back();
            });
            this.settingsView.start();
            this._updateNavHighlight('settings');
        }
    },

    /**
     * Close the current overlay (admin/settings) and reveal the underlying
     * main view.  Does NOT handle streaming — streaming has its own async
     * lifecycle managed by popstate handler and _onStreamingQuit.
     * If called while streaming is active, it is a no-op (streaming must
     * be handled via popstate or _onStreamingQuit).
     */
    _closeOverlay() {
        if (!this._nav.overlay) return;

        // Streaming has its own lifecycle — never close via this path.
        if (this._nav.overlay === 'streaming') {
            console.warn('[MW] _closeOverlay called with streaming overlay — use popstate or Stop button');
            return;
        }

        this._nav.overlay = null;

        // Destroy overlay views
        if (this.adminView) {
            this.adminView.destroy();
            this.adminView = null;
        }
        if (this.settingsView) {
            this.settingsView.destroy();
            this.settingsView = null;
        }

        // Clear overlay DOM
        const root = document.getElementById('overlay-root');
        if (root) {
            root.innerHTML = '';
            root.style.display = 'none';
        }

        // Resume main view timers
        if (this.hostListView && typeof this.hostListView.start === 'function') {
            this.hostListView.start();
        }

        this._updateNavHighlight(null);
    },

    // =========================================================================
    // Non-blocking async setup
    // =========================================================================

    async _initAsync() {
        try {
            const health = await BackendClient.get('/api/health');
            console.log('[MW] Server:', health);
        } catch (err) {
            console.warn('[MW] Server health check failed:', err);
        }
    },

    // =========================================================================
    // Nav Buttons
    // =========================================================================

    _initNavButtons() {
        const btnAdmin = document.getElementById('btn-admin');
        if (btnAdmin) {
            if (window.location.hostname !== 'localhost' &&
                window.location.hostname !== '127.0.0.1' &&
                window.location.hostname !== '[::1]') {
                btnAdmin.style.display = 'none';
            }
            btnAdmin.addEventListener('click', () => {
                if (this._nav.overlay === 'admin') {
                    this._closeOverlay();
                } else {
                    // Close any current overlay before opening admin
                    if (this._nav.overlay) this._closeOverlay();
                    this._openOverlay('admin');
                }
            });
        }

        const btnSettings = document.getElementById('btn-settings');
        if (btnSettings) {
            btnSettings.addEventListener('click', () => {
                if (this._nav.overlay === 'settings') {
                    this._closeOverlay();
                } else {
                    // Close any current overlay before opening settings
                    if (this._nav.overlay) this._closeOverlay();
                    this._openOverlay('settings');
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

    // =========================================================================
    // Navigation — Main Views
    // =========================================================================

    showHostList() {
        this._closeOverlay();
        history.pushState({ view: 'hosts' }, '', '/');
        this._setMainView('hosts', {});
    },

    showAppList(host) {
        this._closeOverlay();
        const state = {
            view: 'apps',
            hostUuid: host.uuid,
            hostDisplayName: host.displayName
        };
        history.pushState(state, '', '/apps');
        this._setMainView('apps', state);
    },

    // =========================================================================
    // Streaming (fullscreen overlay)
    // =========================================================================

    async launchApp(host, app) {
        console.log(`[MW] Launching: ${app.name} (id=${app.id}) on ${host.displayName}`);
        this.transition('launching');
        Toast.info(`Launching ${app.name}...`);

        let upnpEnabled = true;
        try {
            const settings = await BackendClient.getStreamingSettings();
            upnpEnabled = settings.upnp_enabled !== false;
        } catch (err) {
            console.warn('[MW] Failed to load streaming settings for UPnP check:', err);
        }

        try {
            const result = await BackendClient.launchApp(host.uuid, app.id);
            console.log('[MW] Launch result:', result);

            if (result.status === 'streaming') {
                Toast.success(`${app.name} started`);

                // ── Streaming overlay guard ────────────────────────────────
                // Push a guard state so Back from streaming goes to Apps.
                // The guard is consumed by popstate, leaving Apps in history.
                this._nav.overlay = 'streaming';
                this.transition('streaming');

                // Pause main view timers (apps view, which has no polling,
                // but hosts might resume polling when we return)
                if (this.hostListView && typeof this.hostListView.stop === 'function') {
                    this.hostListView.stop();
                }

                const guardState = {
                    view: this._GUARD_PREFIX + 'streaming',
                    mainView: this._nav.mainView,
                    hostUuid: this._nav.mainState.hostUuid,
                    hostDisplayName: this._nav.mainState.hostDisplayName
                };
                history.pushState(guardState, '');

                this.streamView = new StreamView(
                    document.getElementById('app'),
                    result.signalingUrl || result.wsUrl,
                    host,
                    result.videoCodec,
                    result.gamingMode !== false,
                    upnpEnabled,
                    result.upnpAvailable !== false,
                    result.transport || (result.signalingUrl ? 'webrtc' : 'wss')
                );

                // ── Callback when streaming quits (Stop button / disconnect) ─
                this.streamView.onQuit = () => this._onStreamingQuit();
            }
        } catch (err) {
            console.error('[MW] Launch failed:', err);
            Toast.error(err.message || 'Launch failed');
            this.transition('app_list');
        }
    },

    /**
     * Called when streaming ends (Stop button, disconnect, or quit).
     * Popstate handler may have already cleaned up (if Back was pressed).
     * This method only acts if the overlay is still marked as 'streaming'.
     */
    _onStreamingQuit() {
        // Guard: if popstate already handled cleanup, skip.
        // This prevents double-navigation when:
        //   a) User presses Back → popstate fires quit() + navigates
        //   b) streamView.quit() completes and calls onQuit
        // Without this guard, both popstate and onQuit would navigate.
        if (this._nav.overlay !== 'streaming') {
            this.streamView = null;
            return;
        }
        this._nav.overlay = null;
        this.streamView = null;
        this.transition('app_list');

        // Pop the streaming guard from history to reveal apps/hosts state below.
        if (history.state && history.state.view === this._GUARD_PREFIX + 'streaming') {
            // Guard still present — pop it via history.back(). This fires
            // popstate which navigates to the revealed main view.
            history.back();
        } else {
            // Guard already consumed (e.g. user pressed Back before pressing
            // Stop). Navigate directly to the saved main view.
            if (this._nav.mainView === 'apps' && this._nav.mainState.hostUuid) {
                this._renderMainView('apps', this._nav.mainState);
                history.replaceState({
                    view: 'apps',
                    hostUuid: this._nav.mainState.hostUuid,
                    hostDisplayName: this._nav.mainState.hostDisplayName
                }, '', '/apps');
            } else {
                this._renderMainView('hosts', { view: 'hosts' });
                history.replaceState({ view: 'hosts' }, '', '/');
            }
        }
    },

    // =========================================================================
    // Cleanup helpers
    // =========================================================================

    _destroyMainViews() {
        if (this.hostListView) {
            this.hostListView.destroy();
            this.hostListView = null;
        }
        if (this.appListView) {
            this.appListView.destroy();
            this.appListView = null;
        }
    },

    // ── Legacy helpers kept for backward compat ──────────────────────────────

    transition(newState) {
        console.log(`[MW] State: ${this.state} -> ${newState}`);
        this.state = newState;
    }
};

// Boot
document.addEventListener('DOMContentLoaded', () => MoonlightApp.init());
