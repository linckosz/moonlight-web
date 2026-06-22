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
 * Moonlight-Web — Application entry point
 *
 * Navigation architecture:
 *
 *   Main views (in #main-content, with history entries):
 *     - hosts  at  "/"
 *     - apps   at  "/apps"  (with host context in history state)
 *
 *   Overlays (in #main-content, no persistent history entry):
 *     - admin     — guard pushState + URL "/admin"  (survives refresh)
 *     - settings  — guard pushState + URL "/settings"
 *     - streaming — guard pushState, no URL change (fullscreen via StreamView)
 *
 *   Switching overlays (e.g. settings → admin) uses replaceState
 *   so a single Back returns to the underlying main view.
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
 *     /admin     → stays on Admin
 *     /          → stays on Hosts
 *     /apps      → redirect to Hosts
 *     /settings  → redirect to Hosts
 *     /streaming → redirect to Hosts
 */
import { HostListView } from './ui/HostListView.js';
import { AppListView } from './ui/AppListView.js';
import { App } from './models/App.js';
import { StreamView } from './ui/StreamView.js';
import { SettingsView } from './ui/SettingsView.js';
import { AdminView } from './ui/AdminView.js';
import { LoginView } from './ui/LoginView.js';
import { BackendClient } from './api/BackendClient.js';
import { Toast } from './ui/Toast.js';
import { VersionGuard } from './util/VersionGuard.js';
import { init as i18nInit, applyDOM, t } from './i18n/i18n.js';

// ── Global error handler ──────────────────────────────────────────────────────
window.addEventListener('error', (evt) => {
    console.error('[MW] Uncaught error:', evt.error || evt.message, evt);
    const main = document.getElementById('main-content');
    if (main && main.children.length === 0) {
        main.innerHTML = `
            <div class="hosts-view">
                <div class="hosts-header"><h2>${t('appError.title')}</h2></div>
                <div class="hosts-error">
                    <p>${t('appError.failedToLoad')}</p>
                    <p class="hint">${(evt.error && evt.error.message) || evt.message || t('appError.unknownError')}</p>
                    <button class="btn" onclick="location.reload()">${t('common.retry')}</button>
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
                <div class="hosts-header"><h2>${t('appError.initTitle')}</h2></div>
                <div class="hosts-error">
                    <p>${msg}</p>
                    <button class="btn" onclick="location.reload()">${t('common.retry')}</button>
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
    loginView: null,

    // ── HEVC fallback guard ─────────────────────────────────────────────────
    // Counts consecutive HEVC→H.264 fallback attempts to prevent infinite
    // re-launch loops. Reset to 0 each time the user initiates a new stream
    // (not a fallback). If H.264 also fails, stop falling back.
    _fallbackAttemptCount: 0,

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

        // Reload the app if a newer build is deployed while it stays open.
        VersionGuard.start();

        // ── Hide admin/settings buttons upfront for non-localhost ─────────
        // They will be revealed by _initNavButtons() if authenticated.
        this._hideNavButtonsConditionally();

        // ── Auth check: show login if remote and not authenticated ─────────
        const authOk = await this._checkAuth();
        if (!authOk) return;  // LoginView handles rendering, stop here

        this._initNavButtons();
        this._initRouter();

        // Background async housekeeping — never blocks the initial render.
        this._initAsync();
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

            // ── Overlay guard was popped — close overlay, restore main view ──
            if (this._nav.overlay) {
                if (this._nav.overlay === 'streaming') {
                    if (this.streamView) {
                        if (!this.streamView._quitting) {
                            this.streamView.quit();
                        }
                        this.streamView = null;
                    }
                    this._nav.overlay = null;
                } else {
                    this._closeOverlay();
                }

                // Restore the main view revealed by popping the guard.
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
            this.appListView = new AppListView(main, host, this._takePrewarmedApps(host.uuid));
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

        // Fix URL if an overlay left it at /admin or /settings.
        if (window.location.pathname === '/admin' || window.location.pathname === '/settings') {
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
            this.appListView = new AppListView(main, host, this._takePrewarmedApps(host.uuid));
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
     * Open an overlay (admin, settings) in #main-content, replacing the
     * current main view.  Pushes (or replaces) a guard state so pressing
     * Back closes the overlay and restores the underlying main view.
     *
     * When switching overlays (e.g. settings → admin), the old guard is
     * replaced via replaceState so a single Back returns to the main view.
     *
     * @param {string} type - 'admin' | 'settings'
     */
    _openOverlay(type) {
        // ── Streaming has absolute priority ────────────────────────────────
        // Never let admin/settings open on top of (or under) an active or
        // launching stream — it corrupts the overlay/history state and the
        // view would pop up when the stream ends.
        if (this._nav.overlay === 'streaming' || this.streamView ||
            this.state === 'launching' || this.state === 'streaming') {
            console.warn('[MW] Overlay "' + type + '" blocked: streaming in progress');
            return;
        }

        const switching = !!this._nav.overlay;

        if (switching) {
            // Switching overlays: destroy the old one, replace guard state
            this._destroyOverlayViews();
        } else {
            // Opening on top of a main view: save main view state for restoration
            if (this.hostListView && typeof this.hostListView.stop === 'function') {
                this.hostListView.stop();
            }
            this._destroyMainViews();
        }

        this._nav.overlay = type;

        const main = document.getElementById('main-content');
        if (!main) return;
        main.innerHTML = '';

        const guardState = {
            view: this._GUARD_PREFIX + type,
            mainView: this._nav.mainView,
            hostUuid: this._nav.mainState.hostUuid,
            hostDisplayName: this._nav.mainState.hostDisplayName
        };

        const url = '/' + type;
        if (switching) {
            history.replaceState(guardState, '', url);
        } else {
            history.pushState(guardState, '', url);
        }

        if (type === 'admin') {
            this.transition('admin');
            this.adminView = new AdminView(main, () => history.back());
            this.adminView.start();
            this._updateNavHighlight('admin');
        } else {
            this.transition('settings');
            this.settingsView = new SettingsView(main, () => history.back());
            this.settingsView.start();
            this._updateNavHighlight('settings');
        }
    },

    /**
     * Close the current overlay (admin/settings) and destroy its view.
     * Does NOT restore the main view — callers (popstate handler, button
     * handlers) are responsible for restoring the main view afterwards.
     */
    _closeOverlay() {
        if (!this._nav.overlay) return;

        if (this._nav.overlay === 'streaming') {
            console.warn('[MW] _closeOverlay called with streaming overlay — use popstate or Stop button');
            return;
        }

        this._nav.overlay = null;
        this._destroyOverlayViews();

        // Clear main content (caller will re-render the appropriate view)
        const main = document.getElementById('main-content');
        if (main) main.innerHTML = '';

        this._updateNavHighlight(null);
    },

    /** Destroy admin/settings overlay views. */
    _destroyOverlayViews() {
        if (this.adminView) {
            this.adminView.destroy();
            this.adminView = null;
        }
        if (this.settingsView) {
            this.settingsView.destroy();
            this.settingsView = null;
        }
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
    // Auth Check
    // =========================================================================

    /**
     * Check if the remote visitor is authenticated.
     * Shows LoginView if not authenticated and not on localhost.
     * Returns true if the app should continue initializing, false otherwise.
     */
    async _checkAuth() {
        this.state = 'auth_check';
        console.log('[MW] Checking authentication...');

        const main = document.getElementById('main-content');
        if (!main) return true;

        try {
            const status = await BackendClient.getAuthStatus();

            // Localhost or already authenticated — proceed
            if (status.is_localhost || status.authenticated) {
                return true;
            }

            // Not authenticated and not localhost — show login
            this.loginView = new LoginView(main, () => {
                // On successful login, re-initialize the app
                console.log('[MW] Authentication successful, re-initializing...');
                this.loginView = null;
                this._initNavButtons();
                this._initRouter();
                this._initAsync();
            });

            main.innerHTML = '';
            this.transition('login');
            this.loginView.start();
            return false;
        } catch (err) {
            console.warn('[MW] Auth check failed:', err);
            // If we can't reach the server, show error in main content
            main.innerHTML = `
                <div class="hosts-view">
                    <div class="hosts-header"><h2>${t('appError.connectionTitle')}</h2></div>
                    <div class="hosts-error">
                        <p>${t('appError.unableToConnect')}</p>
                        <p class="hint">${err.message || t('appError.unknownError')}</p>
                        <button class="btn" onclick="location.reload()">${t('common.retry')}</button>
                    </div>
                </div>
            `;
            return false;
        }
    },


    // =========================================================================
    // Nav Buttons
    // =========================================================================

    /**
     * Hide admin/settings buttons on non-localhost before auth.
     * Called from init() before _checkAuth(). The settings button
     * is revealed by _initNavButtons() once authenticated.
     */
    _hideNavButtonsConditionally() {
        if (window.location.hostname === 'localhost' ||
            window.location.hostname === '127.0.0.1' ||
            window.location.hostname === '[::1]') {
            return;  // localhost: keep both visible
        }
        // Remote: hide admin always, settings until authenticated
        const btnAdmin = document.getElementById('btn-admin');
        if (btnAdmin) btnAdmin.style.display = 'none';
        const btnSettings = document.getElementById('btn-settings');
        if (btnSettings) btnSettings.style.display = 'none';
    },

    _initNavButtons() {
        const isLocal = window.location.hostname === 'localhost' ||
                        window.location.hostname === '127.0.0.1' ||
                        window.location.hostname === '[::1]';

        const btnAdmin = document.getElementById('btn-admin');
        if (btnAdmin) {
            if (!isLocal) {
                btnAdmin.style.display = 'none';
            } else {
                btnAdmin.style.display = '';
            }
            btnAdmin.addEventListener('click', () => {
                if (this._nav.overlay === 'admin') {
                    history.back();
                } else if (this._nav.overlay) {
                    this._openOverlay('admin');
                } else {
                    this._openOverlay('admin');
                }
            });
        }

        const btnSettings = document.getElementById('btn-settings');
        if (btnSettings) {
            // On remote: show settings only after authentication
            if (!isLocal) {
                btnSettings.style.display = '';
            }
            btnSettings.addEventListener('click', () => {
                if (this._nav.overlay === 'settings') {
                    history.back();
                } else if (this._nav.overlay) {
                    this._openOverlay('settings');
                } else {
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
    // Remote connection detection
    // =========================================================================

    /**
     * Returns true if the current connection is from outside the local network.
     * UPnP warnings and certain UI elements are only relevant for remote access.
     */
    _isRemoteConnection() {
        const hostname = window.location.hostname;
        // Localhost / loopback
        if (hostname === 'localhost' || hostname === '127.0.0.1' || hostname === '[::1]') return false;
        // Private IP ranges (LAN)
        if (/^10\./.test(hostname)) return false;
        if (/^192\.168\./.test(hostname)) return false;
        if (/^172\.(1[6-9]|2[0-9]|3[0-1])\./.test(hostname)) return false;
        // Everything else (domain, public IP) is remote
        return true;
    },

    // =========================================================================
    // Streaming (fullscreen overlay)
    // =========================================================================

    async launchApp(host, app, codecOverride) {
        console.log(`[MW] Launching: ${app.name} (id=${app.id}) on ${host.displayName}` +
            (codecOverride ? ` (forced codec: ${codecOverride})` : ''));
        this.transition('launching');
        Toast.info(t('launch.launching', { name: app.name }));

        // Reset fallback counter on user-initiated launch (not a fallback re-launch)
        if (!codecOverride) {
            this._fallbackAttemptCount = 0;
            // Reset transport-chain fallback state for a fresh launch.
            this._transportIndex = 0;
            this._firstTransportRetried = false;
        }

        // Store host/app for HEVC fallback re-launch
        this._lastStreamHost = host;
        this._lastStreamApp = app;

        // UPnP is enabled by default — the backend's upnpAvailable field
        // in the launch response reports the actual availability to the UI.
        const upnpEnabled = true;
        const isRemote = this._isRemoteConnection();

        // Read per-browser streaming settings from localStorage
        // (defaults come from server on first visit via SettingsView)
        let streamingSettings = {};
        const stored = localStorage.getItem('mw-streaming-settings');
        if (stored) {
            try {
                streamingSettings = JSON.parse(stored);
            } catch (e) {
                console.warn('[MW] Failed to parse streaming settings from localStorage:', e);
            }
        }

        // Override codec if provided (HEVC fallback to H.264)
        if (codecOverride) {
            streamingSettings.video_codec = codecOverride;
        }

        // Power Saving (mobile): force the native video transport, UDP first.
        // webrtc-media-udp is tried before any TCP/DataChannel fallback, so the
        // <video> pipeline (no canvas) and UDP have priority. The other settings
        // (H.264, no HDR/enhancement, VSync, 720p/60) are already in the stored
        // object because SettingsView applied them when the mode was enabled.
        if (streamingSettings.power_save) {
            streamingSettings.transport_mode = 'webrtc-media-udp';
        }

        try {
            const result = await BackendClient.launchApp(host.uuid, app.id, streamingSettings);
            // Log only safe fields — never log the full response as it may contain
            // internal IP info (e.g. sessionUrl exposes Sunshine's LAN address).
            console.log('[MW] Launch result:', {
                status: result.status,
                videoCodec: result.videoCodec,
                gamingMode: result.gamingMode,
                transport: result.transport,
                transport_mode: result.transport_mode,
                upnpAvailable: result.upnpAvailable,
                signalingUrl: result.signalingUrl
            });

            if (result.status === 'streaming') {
                // Dismiss "Launching..." toast so only the current status is visible
                await Toast.dismissAll();
                Toast.success(t('launch.started', { name: app.name }));

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

                // Remember context so a failed transport can be relaunched with
                // the next entry in the priority chain (see _onTransportFailed).
                this._lastStreamingSettings = streamingSettings;
                this._startStreamView(result, host, streamingSettings);
            }
        } catch (err) {
            console.error('[MW] Launch failed:', err);
            Toast.error(err.message || t('launch.failed'));
            this.transition('app_list');
        }
    },

    /**
     * Create the StreamView for a successful launch response and wire its
     * callbacks. Shared by the initial launch and transport-chain relaunches.
     * Stores the backend-reported transport fallback chain + index.
     */
    _startStreamView(result, host, streamingSettings) {
        const isRemote = this._isRemoteConnection();
        const upnpEnabled = true;
        const internalTransport = result.transport || (result.signalingUrl ? 'webrtc' : 'wss');
        const transportMode = result.transport_mode || internalTransport;
        // Respect the "show performance stats" setting (default: true)
        const showPerfStats = streamingSettings.show_performance_stats !== false;
        // Touch/trackpad sensitivity (default 2.2)
        const touchSensitivity = typeof streamingSettings.touch_sensitivity === 'number'
            ? streamingSettings.touch_sensitivity : 2.2;
        // VSync (default on): when off, the canvas allows tearing (lower latency)
        const vsync = streamingSettings.vsync_enabled !== false;
        // Video worker mode: 'auto' (heuristic — desktop only), 'on' or 'off'.
        const videoWorker = streamingSettings.video_worker;
        // Video enhancement (WebGPU upscale/sharpen). Forced OFF on MediaTrack:
        // <video> cannot be processed by WebGPU, so a MediaTrack last-resort
        // attempt always streams without enhancement.
        let videoEnhancement = streamingSettings.video_enhancement === 'on' ? 'on' : 'off';
        if (internalTransport === 'webrtc-media') videoEnhancement = 'off';
        const videoEnhancementAlgo = streamingSettings.video_enhancement_algo || 'auto';

        // Track the fallback chain reported by the backend.
        this._transportChain = Array.isArray(result.transport_chain) ? result.transport_chain : [];
        this._transportIndex = typeof result.transport_index === 'number' ? result.transport_index : 0;

        this.streamView = new StreamView(
            document.getElementById('app'),
            result.signalingUrl || result.wsUrl,
            host,
            result.videoCodec,
            result.gamingMode !== false,
            upnpEnabled,
            result.upnpAvailable !== false,
            internalTransport,
            transportMode,
            isRemote,
            showPerfStats,
            touchSensitivity,
            vsync,
            videoWorker,
            videoEnhancement,
            videoEnhancementAlgo,
            result.yuv444 === true
        );

        // ── Callbacks ──────────────────────────────────────────────────────
        // onQuit: normal end (Stop button / mid-stream disconnect).
        this.streamView.onQuit = () => this._onStreamingQuit();
        // onQuitStart: Stop pressed — prefetch the apps list while the exit
        // animation plays so the view is ready the instant we land on it.
        this.streamView.onQuitStart = () => this._prewarmAppList();
        // onConnectionFailed: transport never connected → try the next entry
        // in the priority chain (or give up if the chain is exhausted).
        this.streamView.onConnectionFailed = (reason) => this._onTransportFailed(reason);
    },

    /** Human-readable label for a transport mode (warning toasts / logs). */
    _transportLabel(mode) {
        const keys = {
            'webrtc-dc-udp':    'transport.webrtcDcUdp',
            'webrtc-dc-tcp':    'transport.webrtcDcTcp',
            'webrtc-media-udp': 'transport.webrtcMediaUdp',
            'webrtc-media-tcp': 'transport.webrtcMediaTcp',
            'wss':              'transport.wss'
        };
        return keys[mode] ? t(keys[mode]) : mode;
    },

    /**
     * A transport failed to establish a connection. If there is a next entry in
     * the priority chain, relaunch with it (warning toast). Otherwise show a
     * final error and return to the apps view.
     */
    _onTransportFailed(reason) {
        // Ignore if streaming was already torn down (e.g. user pressed Back).
        if (this._nav.overlay !== 'streaming') return;

        const chain = this._transportChain || [];
        const cur = this._transportIndex || 0;

        // Always retry the FIRST transport once before moving down the chain — a
        // single transient ICE failure on the preferred transport shouldn't
        // immediately downgrade. The retry is silent (no warning toast).
        if (cur === 0 && !this._firstTransportRetried) {
            this._firstTransportRetried = true;
            console.warn(`[MW] Transport ${chain[0]} failed (${reason}) — retrying once (silent)`);
            this._relaunchTransport(0);
            return;
        }

        const next = cur + 1;
        if (next < chain.length) {
            console.warn(`[MW] Transport ${chain[cur]} failed (${reason}) — trying ${chain[next]}`);
            Toast.warning(t('transport.connectFailed', {
                from: this._transportLabel(chain[cur]),
                to: this._transportLabel(chain[next])
            }));
            this._relaunchTransport(next);
        } else {
            console.error(`[MW] All transports failed (last: ${chain[cur] || '?'}, reason: ${reason})`);
            Toast.error(t('transport.allFailed'));
            this._hideRelaunchLoader();
            // quit() fires onQuit → _onStreamingQuit, which handles navigation
            // back to the apps view. silent: suppress the "Stream end" toast.
            if (this.streamView) this.streamView.quit({ silent: true });
        }
    },

    /** Full-screen loader shown during a transport relaunch so the apps view is
     *  never revealed between tearing down the old StreamView and rendering the
     *  new one. Reuses the StreamView startup-loader visuals. */
    _showRelaunchLoader() {
        if (document.getElementById('stream-relaunch-loader')) return;
        const el = document.createElement('div');
        el.id = 'stream-relaunch-loader';
        el.innerHTML =
            '<div class="startup-loader" aria-hidden="true"><div class="startup-loader-ring"></div></div>' +
            '<div class="startup-step active"><span class="startup-step-label">' + t('stream.connecting') + '</span></div>';
        document.getElementById('app').appendChild(el);
    },

    _hideRelaunchLoader() {
        const el = document.getElementById('stream-relaunch-loader');
        if (el) el.remove();
    },

    /**
     * Tear down the current StreamView and relaunch the same app with the next
     * transport index. The backend recomputes the (identical) chain and attempts
     * the requested index.
     */
    async _relaunchTransport(nextIndex) {
        const host = this._lastStreamHost;
        const app = this._lastStreamApp;
        if (!host || !app) {
            if (this.streamView) this.streamView.quit({ silent: true });
            return;
        }

        // Show the bridging loader BEFORE teardown so the apps view underneath
        // is never revealed (stay on the stream initialization screen).
        this._showRelaunchLoader();

        // Quietly tear down the failed StreamView (no navigation, no toast).
        const sv = this.streamView;
        this.streamView = null;
        if (sv) {
            sv.onQuit = null;
            sv.onConnectionFailed = null;
            try { await sv.quit({ silent: true }); } catch (e) { /* ignore */ }
        }

        this._transportIndex = nextIndex;

        // Relaunch targeting the next chain index. Same settings otherwise; the
        // backend recomputes the same chain and picks chain[nextIndex].
        const settings = { ...this._lastStreamingSettings, transport_index: nextIndex };
        try {
            const result = await BackendClient.launchApp(host.uuid, app.id, settings);
            if (result.status === 'streaming') {
                this._startStreamView(result, host, this._lastStreamingSettings);
                // New StreamView rendered its own #stream-view on top — drop the
                // bridging loader.
                this._hideRelaunchLoader();
            } else {
                this._onTransportFailed('launch status ' + result.status);
            }
        } catch (err) {
            // HTTP error on this transport (e.g. backend 502 chain exhausted) →
            // advance the chain / final error.
            console.warn('[MW] Relaunch failed for index', nextIndex, err);
            this._onTransportFailed('launch error');
        }
    },

    /**
     * Called when streaming ends (Stop button, disconnect, or HEVC fallback).
     * Popstate handler may have already cleaned up (if Back was pressed).
     * This method only acts if the overlay is still marked as 'streaming'.
     *
     * If the stream ended because HEVC is not supported (_codecFallbackRequested),
     * automatically re-launch with H.264 forced. The fallback counter prevents
     * infinite loops if H.264 also fails.
     */
    _onStreamingQuit() {
        // Drop any lingering transport-relaunch loader.
        this._hideRelaunchLoader();
        // Guard: if popstate already handled cleanup, skip.
        // This prevents double-navigation when:
        //   a) User presses Back → popstate fires quit() + navigates
        //   b) streamView.quit() completes and calls onQuit
        // Without this guard, both popstate and onQuit would navigate.
        if (this._nav.overlay !== 'streaming') {
            this.streamView = null;
            return;
        }

        // Capture fallback state BEFORE nulling streamView
        const fallbackRequested = this.streamView && this.streamView._codecFallbackRequested;
        const fallbackHost = this._lastStreamHost;
        const fallbackApp = this._lastStreamApp;

        this._nav.overlay = null;
        this.streamView = null;

        // ── HEVC → H.264 fallback ─────────────────────────────────────────
        // When the browser cannot decode HEVC (Windows Chrome), the stream
        // quits with _codecFallbackRequested=true. Re-launch with H.264 forced.
        if (fallbackRequested && fallbackHost && fallbackApp) {
            this._fallbackAttemptCount++;
            if (this._fallbackAttemptCount > 1) {
                // H.264 fallback failed too — give up to prevent infinite loop
                console.error('[MW] HEVC→H.264 fallback also failed, giving up');
                this.transition('app_list');
                if (history.state && history.state.view === this._GUARD_PREFIX + 'streaming') {
                    history.back();
                } else {
                    this._renderMainView('apps', {
                        hostUuid: fallbackHost.uuid,
                        hostDisplayName: fallbackHost.displayName
                    });
                    history.replaceState({ view: 'apps', hostUuid: fallbackHost.uuid,
                        hostDisplayName: fallbackHost.displayName }, '', '/apps');
                }
                Toast.error(t('launch.hevcH264Unsupported'));
                return;
            }

            console.warn('[MW] HEVC not supported by browser, re-launching with H.264 ' +
                '(attempt ' + this._fallbackAttemptCount + ')');

            // Pop the streaming guard from history
            if (history.state && history.state.view === this._GUARD_PREFIX + 'streaming') {
                history.back();
            }

            // Re-launch with H.264 forced. This pushes a new streaming guard
            // on top of the revealed main view.
            this.transition('app_list'); // Reset state for re-launch
            this.launchApp(fallbackHost, fallbackApp, 'h264');
            return;
        }

        // Reset fallback counter on successful non-fallback quit
        this._fallbackAttemptCount = 0;

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

    /**
     * Prefetch the apps list (and warm box-art images) for the host we'll
     * return to, so the apps view is fully built by the time the stream-exit
     * animation finishes. Result is cached and consumed once on next render.
     */
    async _prewarmAppList() {
        const state = this._nav.mainState;
        if (!state || !state.hostUuid) return;
        const hostUuid = state.hostUuid;
        try {
            const data = await BackendClient.getAppList(hostUuid);
            if (!data || data.status !== 'ok') return;
            const apps = (data.apps || []).map(a => new App(a, hostUuid));
            this._appListCache = { hostUuid, apps };
            // Warm box-art images so the grid paints instantly (no lazy pop-in).
            apps.forEach(a => {
                if (a.boxArtUrl) { const img = new Image(); img.src = a.boxArtUrl; }
            });
        } catch (e) {
            // Prefetch is best-effort — AppListView falls back to normal load().
        }
    },

    /** Pull the prefetched apps for `hostUuid` if any, consuming the cache. */
    _takePrewarmedApps(hostUuid) {
        const c = this._appListCache;
        if (c && c.hostUuid === hostUuid) {
            this._appListCache = null;
            return c.apps;
        }
        return null;
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
        if (this.loginView) {
            this.loginView.destroy();
            this.loginView = null;
        }
    },

    // ── Legacy helpers kept for backward compat ──────────────────────────────

    transition(newState) {
        console.log(`[MW] State: ${this.state} -> ${newState}`);
        this.state = newState;
    }
};

// Boot
document.addEventListener('DOMContentLoaded', async () => {
    // Load translations before the first render, then translate the static
    // markup (header buttons / footer) that exists in index.html.
    await i18nInit();
    applyDOM(document);
    MoonlightApp.init();
});
