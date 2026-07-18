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
 * MoonlightWeb — Application entry point
 *
 * Navigation architecture:
 *
 *   Single main view (in #main-content, with a history entry):
 *     - hosts  at  "/"  — host boxes carry their own app grids inline; an app
 *                         is launched directly from its host box.
 *
 *   Overlays (in #main-content, no persistent history entry):
 *     - admin     — guard pushState + URL "/admin"  (survives refresh)
 *     - settings  — guard pushState + URL "/settings"
 *     - streaming — guard pushState, no URL change (fullscreen via StreamView)
 *
 *   Switching overlays (e.g. settings → admin) uses replaceState
 *   so a single Back returns to the hosts view.
 *
 *   History stack possibilities:
 *     [{hosts}]                          — on Hosts
 *     [{hosts}, {overlay-guard, ...}]    — on Hosts + overlay
 *
 *   Back button: overlay guard is consumed first (→ close overlay),
 *   then the hosts view is shown.
 *
 *   Refresh behavior:
 *     /admin     → stays on Admin
 *     /          → stays on Hosts
 *     /settings  → redirect to Hosts
 *     /streaming → redirect to Hosts
 */
import { HostListView } from './ui/HostListView.js';
import { StreamView } from './ui/StreamView.js';
import { SettingsView } from './ui/SettingsView.js';
import { AdminView } from './ui/AdminView.js';
import { LoginView } from './ui/LoginView.js';
import { SetupView } from './ui/SetupView.js';
import { BackendClient } from './api/BackendClient.js';
import { Toast } from './ui/Toast.js';
import { VersionGuard } from './util/VersionGuard.js';
import { IS_MOBILE_OR_TABLET } from './util/BrowserDetect.js';
import { computeAutoBitrate } from './util/AutoBitrate.js';
import * as iosAudioUnlock from './audio/iosAudioUnlock.js';
import { init as i18nInit, applyDOM, t } from './i18n/i18n.js';
import { escapeHtml } from './util/escapeHtml.js';

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
                    <button class="btn btn-reload">${t('common.retry')}</button>
                </div>
            </div>
        `;
        main.querySelector('.btn-reload').addEventListener('click', () => location.reload());
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
                    <button class="btn btn-reload">${t('common.retry')}</button>
                </div>
            </div>
        `;
        main.querySelector('.btn-reload').addEventListener('click', () => location.reload());
    }
});

const MoonlightApp = {
    // ── View instances persisted across overlays ─────────────────────────────
    state: 'loading',
    hostListView: null,
    streamView: null,
    settingsView: null,
    adminView: null,
    loginView: null,
    setupView: null,

    // ── HEVC fallback guard ─────────────────────────────────────────────────
    // Counts consecutive HEVC→H.264 fallback attempts to prevent infinite
    // re-launch loops. Reset to 0 each time the user initiates a new stream
    // (not a fallback). If H.264 also fails, stop falling back.
    _fallbackAttemptCount: 0,

    // ── Navigation state (never destroyed by overlays) ──────────────────────
    _nav: {
        /** Always 'hosts' — the single main view */
        mainView: 'hosts',
        /** Reserved for future per-view state; currently unused */
        mainState: {},
        /** null | 'admin' | 'settings' | 'streaming' — current overlay */
        overlay: null,
    },

    async init() {
        console.log('[MW] Initializing MoonlightWeb...');

        // Mark iOS in CSS (UA sniff) — used to restore tick visibility on
        // the settings checkbox, where Safari's native tick is invisible.
        if (
            /iPhone|iPod|iPad/i.test(navigator.userAgent) ||
            (['MacIntel', 'Mac68K'].indexOf(navigator.platform) !== -1 && 'ontouchend' in document)
        ) {
            document.body.classList.add('ios');
        }

        // Block browser zoom on the app UI (allowed only on the stream surfaces).
        this._initZoomGuard();

        // iOS: pre-buffer the audio-session unlock element and authorize it on
        // the first user interaction, so the launch-click unlock reliably starts
        // it (a freshly-created <audio> can have its first .play() rejected).
        iosAudioUnlock.prime();

        // Reload the app if a newer build is deployed while it stays open.
        VersionGuard.start();

        // ── Hide admin/settings buttons upfront for non-localhost ─────────
        // They will be revealed by _initNavButtons() if authenticated.
        this._hideNavButtonsConditionally();

        // ── Host key (?mwk=...) — the host machine's own entry points embed it
        // when they open the public domain: redeem it for a localhost-equivalent
        // session BEFORE the auth check, then strip it from the address bar.
        await this._redeemHostKey();

        // ── Canonical origin: once Internet Access is live, a page loaded (or
        // refreshed) on https://localhost moves to the public domain, so the
        // host machine uses the same URL as everyone else.
        if (await this._maybeRedirectToDomain()) return; // navigating away

        // ── Auth check: show login if remote and not authenticated ─────────
        const authOk = await this._checkAuth();
        if (!authOk) return; // LoginView handles rendering, stop here

        // ── First-run setup wizard (localhost only) ────────────────────────
        // macOS/Linux have no native installer, so the app hosts the setup
        // wizard. Windows reports setup_completed=true (its Inno Setup installer
        // owns provisioning), so the gate is a no-op there.
        const setupShown = await this._maybeShowSetup();
        if (setupShown) return; // SetupView handles rendering; continues on finish

        this._initNavButtons();
        this._initRouter();

        // Single-tab dedup: keep a control-channel WebSocket open so a second
        // app launch (Desktop shortcut) surfaces the admin page in THIS tab
        // instead of opening a duplicate.
        this._initControlChannel();

        // Background async housekeeping — never blocks the initial render.
        this._initAsync();
    },

    // =========================================================================
    // Control channel (single-tab dedup)
    // =========================================================================

    /**
     * Open a persistent WebSocket to the backend (proxied at /ws/control).
     * When the user clicks the Desktop shortcut while the app is already open,
     * the freshly-launched process asks the running backend to surface admin;
     * the backend broadcasts {"type":"focus-admin"} here, and this tab opens the
     * admin overlay + brings itself to the foreground — no duplicate tab.
     *
     * Host-machine only: admin is host-local, and only the host's own tab should
     * react to a local relaunch. Reconnects with capped exponential backoff so a
     * backend restart (or transient drop) re-establishes the channel.
     */
    _initControlChannel() {
        if (!this._isHostLocal()) return;
        if (this._controlWs) return;

        const scheme = window.location.protocol === 'https:' ? 'wss' : 'ws';
        const url = `${scheme}://${window.location.host}/ws/control`;
        let backoff = 1000;

        const connect = () => {
            let ws;
            try {
                ws = new WebSocket(url);
            } catch (e) {
                console.warn('[MW] control channel: connect failed', e);
                this._controlReconnectTimer = setTimeout(connect, backoff);
                backoff = Math.min(backoff * 2, 30000);
                return;
            }
            this._controlWs = ws;
            ws.onopen = () => {
                backoff = 1000;
            };
            ws.onmessage = (ev) => {
                let msg;
                try {
                    msg = JSON.parse(ev.data);
                } catch {
                    return;
                }
                if (msg && msg.type === 'focus-admin') this._onFocusAdminRequest();
            };
            ws.onclose = () => {
                this._controlWs = null;
                this._controlReconnectTimer = setTimeout(connect, backoff);
                backoff = Math.min(backoff * 2, 30000);
            };
            ws.onerror = () => {
                try {
                    ws.close();
                } catch {
                    /* onclose handles reconnect */
                }
            };
        };
        connect();

        window.addEventListener('beforeunload', () => {
            if (this._controlReconnectTimer) clearTimeout(this._controlReconnectTimer);
            if (this._controlWs) {
                try {
                    this._controlWs.close();
                } catch {
                    /* tab is unloading anyway */
                }
            }
        });
    },

    /** Surface the admin overlay in response to a local relaunch. */
    _onFocusAdminRequest() {
        console.log('[MW] focus-admin requested by a new launch');
        try {
            window.focus();
        } catch {
            /* focus is best-effort; browsers may block cross-tab focus */
        }
        // Already on admin, or a stream is running (overlay open is blocked and
        // must not be disrupted) — nothing to navigate.
        if (this._nav.overlay === 'admin') return;
        this._openOverlay('admin');
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

                // Restore the (only) main view revealed by popping the guard.
                this._setMainView('hosts', {});
                return;
            }

            // ── Normal main-view navigation ──────────────────────────────────
            this._navigateToMainView('hosts', state);
        });

        // ── Initial route ──────────────────────────────────────────────────
        const path = window.location.pathname;
        let mainView, mainState, initialOverlay;

        if (path === '/admin' && this._isHostLocal()) {
            // Admin survives refresh — shown as overlay on hosts. Host machine
            // only: a remote session landing on /admin is sent to the hosts view.
            mainView = 'hosts';
            mainState = { view: 'hosts' };
            initialOverlay = 'admin';
        } else {
            // All other paths → hosts (including apps, settings, streaming)
            mainView = 'hosts';
            mainState = { view: 'hosts' };
        }

        history.replaceState(mainState, '', initialOverlay === 'admin' ? '/admin' : '/');

        this._nav.mainView = mainView;
        this._nav.mainState = mainState;
        this._renderMainView();

        if (initialOverlay) {
            this._openOverlay(initialOverlay);
        }
    },

    // =========================================================================
    // Main View Management
    // =========================================================================

    /**
     * Navigate to the main (hosts) view, cleaning up any overlay first.
     * Called from the popstate handler and showHostList.
     */
    _navigateToMainView() {
        // Close any open overlay before switching main view
        this._closeOverlay();
        this._setMainView('hosts', {});
    },

    /**
     * Set the main view (hosts) and render it into #main-content.
     * Destroys the previous main view and all overlays.
     * Also fixes the URL if an overlay left it at /admin or /settings.
     */
    _setMainView(view, state) {
        this._closeOverlay();
        this._destroyMainViews();

        this._nav.mainView = 'hosts';
        this._nav.mainState = {};

        const main = document.getElementById('main-content');
        if (!main) return;

        this._renderHosts(main);

        // Fix URL if an overlay left it at /admin or /settings.
        if (window.location.pathname === '/admin' || window.location.pathname === '/settings') {
            history.replaceState({ view: 'hosts' }, '', '/');
        }
    },

    /**
     * Render the hosts view without pushing history or altering _nav.
     * Used by _initRouter and the popstate → _setMainView chain.
     */
    _renderMainView() {
        this._destroyMainViews();
        const main = document.getElementById('main-content');
        if (!main) return;
        this._renderHosts(main);
    },

    /** Build and start the HostListView, wiring app launches. */
    _renderHosts(main) {
        this.transition('host_list');
        this._updateNavHighlight('hosts');

        // Streaming needs a trusted TLS origin: the signaling WebSocket is wss://,
        // which a plain-HTTP page can't open against the self-signed localhost
        // cert (the browser never prompts for a WS cert — it just fails). The
        // loopback entry points open over http:// to dodge the cert warning /
        // Firefox blank page, so when we're on http:// show a gate that switches
        // to HTTPS instead of an unusable host list.
        if (window.location.protocol === 'http:') {
            this._renderInsecureGate(main);
            return;
        }

        this.hostListView = new HostListView(main);
        this.hostListView.onLaunchApp = (host, app) => this.launchApp(host, app);
        this.hostListView.start();

        // On https://localhost, tell the user how OTHER PCs reach this server.
        this._maybeShowRemoteAccessBanner();
    },

    /**
     * Requirement: opened over http:// → hide the hosts and show a button that
     * reopens the app on the secure HTTPS origin (needed for streaming).
     */
    async _renderInsecureGate(main) {
        main.innerHTML = `
            <div class="insecure-gate" id="view-hosts">
                <div class="insecure-gate-icon" role="img" aria-hidden="true">\u{1F512}</div>
                <h2>${t('secure.title')}</h2>
                <p>${t('secure.explain')}</p>
                <button class="btn btn-neutral" id="btn-open-secure">${t('secure.openSecure')}</button>
            </div>`;

        // Build the HTTPS URL for this host (needs the HTTPS port).
        let httpsPort = 443;
        try {
            const admin = await BackendClient.getAdminSettings();
            httpsPort = admin.https_port || 443;
        } catch (err) {
            console.warn('[MW] Could not read HTTPS port for secure gate:', err);
        }
        const port = httpsPort !== 443 ? ':' + httpsPort : '';
        const url = 'https://' + window.location.hostname + port + '/';
        const btn = main.querySelector('#btn-open-secure');
        if (btn) btn.addEventListener('click', () => (window.location.href = url));
    },

    /**
     * Best-effort: resolve the URL other devices use to reach this server.
     *   - Internet Access active → the public domain URL (with external port).
     *   - Otherwise → the LAN IP URL (with local HTTPS port).
     * Returns { url, domain, localIp, httpsPort, extPort, upnpAvailable,
     * internetActive } or null on failure (url may be '' when nothing is known).
     * Host-machine only in practice — callers gate on _isHostLocal().
     */
    async _computeRemoteAccessUrls() {
        let httpsPort = 443;
        let extPort = 443;
        let domain = '';
        let localIp = '';
        let upnpAvailable = false;
        let internetActive = false;
        try {
            const [admin, status] = await Promise.all([
                BackendClient.getAdminSettings(),
                BackendClient.getInternetStatus(),
            ]);
            httpsPort = admin.https_port || 443;
            domain = status.domain || '';
            localIp = status.local_ip || '';
            extPort = status.external_https_port || httpsPort;
            upnpAvailable = !!status.upnp_available;
            internetActive = !!status.active && !!status.internet_access_enabled;
        } catch (err) {
            console.warn('[MW] Could not read remote-access info:', err);
            return null;
        }

        let url = '';
        if (internetActive && domain) {
            url = 'https://' + domain + (extPort !== 443 ? ':' + extPort : '');
        } else if (localIp) {
            url = 'https://' + localIp + (httpsPort !== 443 ? ':' + httpsPort : '');
        }
        return { url, domain, localIp, httpsPort, extPort, upnpAvailable, internetActive };
    },

    /**
     * On the host machine, show an informative banner at the top of the hosts
     * page telling the user how other devices reach this server:
     *   - Internet Access active → the full public domain URL only (the local
     *     HTTPS port equals the router-forwarded port, so one URL fits all).
     *   - Otherwise → the LAN IP URL, plus (when UPnP is available) a discreet
     *     "enable internet access" link that opens the admin page scrolled to
     *     the INTERNET section.
     * Best-effort and host-machine-only; silently skipped otherwise.
     */
    async _maybeShowRemoteAccessBanner() {
        if (!this._isHostLocal()) return;

        const info = await this._computeRemoteAccessUrls();
        if (!info || !info.url) return;

        const view = document.getElementById('view-hosts');
        if (!view || view.querySelector('.remote-access-banner')) return;

        const linkHtml = (u) =>
            `<a href="${encodeURI(u)}" target="_blank" rel="noopener">${escapeHtml(u)}</a>`;

        let bodyHtml = `${t('hosts.remoteAccess')} ${linkHtml(info.url)}`;
        // LAN URL (Internet Access off) + UPnP available → offer the discreet
        // shortcut to open this server to the Internet.
        if (!(info.internetActive && info.domain) && info.upnpAvailable) {
            bodyHtml +=
                `<br><span class="banner-internet-hint">${t('hosts.internetAvailableHint')}` +
                ` <a href="#" id="banner-enable-internet" class="consent-highlight">` +
                `${t('hosts.enableInternetLink')}</a></span>`;
        }

        const banner = document.createElement('div');
        banner.className = 'remote-access-banner';
        banner.innerHTML = `<span class="remote-access-icon" aria-hidden="true">\u{1F310}</span>
            <span>${bodyHtml}</span>`;
        view.insertBefore(banner, view.firstChild);

        const enableLink = banner.querySelector('#banner-enable-internet');
        if (enableLink) {
            enableLink.addEventListener('click', (e) => {
                e.preventDefault();
                this._openOverlay('admin', { scrollTo: 'internet' });
            });
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
     * @param {object} [options] - forwarded to the overlay view (e.g.
     *                             { scrollTo: 'internet' } for AdminView)
     */
    _openOverlay(type, options) {
        // Admin is host-machine-only (AdminView re-checks server-side data too).
        if (type === 'admin' && !this._isHostLocal()) {
            console.warn('[MW] Admin overlay blocked: not on the host machine');
            return;
        }

        // ── Streaming has absolute priority ────────────────────────────────
        // Never let admin/settings open on top of (or under) an active or
        // launching stream — it corrupts the overlay/history state and the
        // view would pop up when the stream ends.
        if (
            this._nav.overlay === 'streaming' ||
            this.streamView ||
            this.state === 'launching' ||
            this.state === 'streaming'
        ) {
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
            hostDisplayName: this._nav.mainState.hostDisplayName,
        };

        const url = '/' + type;
        if (switching) {
            history.replaceState(guardState, '', url);
        } else {
            history.pushState(guardState, '', url);
        }

        if (type === 'admin') {
            this.transition('admin');
            this.adminView = new AdminView(main, () => history.back(), options);
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
            console.warn(
                '[MW] _closeOverlay called with streaming overlay — use popstate or Stop button',
            );
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
            // Reflect the real backend version in the header. The HTML ships an
            // empty, hidden placeholder so we never flash a stale/wrong number:
            // the tag stays invisible until the real version arrives here.
            if (health && health.version) {
                const versionEl = document.querySelector('.app-header .version');
                if (versionEl) {
                    versionEl.textContent = 'v' + health.version;
                    versionEl.hidden = false;
                }
            }
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
    /**
     * Redeem a host key passed as ?mwk=... in the URL (added by the host
     * machine's Desktop shortcut / tray / startup open when they point at the
     * public domain). On success the backend sets a session cookie flagged as
     * "host", making is_localhost true even though the peer address is the
     * router. The key is stripped from the address bar either way.
     */
    async _redeemHostKey() {
        const params = new URLSearchParams(window.location.search);
        const key = params.get('mwk');
        if (!key) return;
        try {
            await BackendClient.redeemHostKey(key);
            console.log('[MW] Host key redeemed — host-machine session granted');
        } catch (err) {
            console.warn('[MW] Host key redemption failed:', err);
        }
        params.delete('mwk');
        const query = params.toString();
        history.replaceState(
            history.state,
            '',
            window.location.pathname + (query ? '?' + query : '') + window.location.hash,
        );
    },

    /**
     * When Internet Access is live, the host machine's canonical origin is the
     * public domain (valid certificate, the exact URL every other device uses).
     * A page loaded or refreshed on https://localhost moves there, carrying the
     * host key so the localhost-equivalent (admin) access survives the origin
     * change. Routers without NAT hairpin cannot reach the domain from inside
     * the LAN — the backend tests this from the host itself (hairpin_reachable),
     * because the untrusted-cert localhost page cannot probe the domain (Chrome
     * blocks its cross-origin subresources). Returns true when a navigation was
     * started (caller must stop init).
     */
    async _maybeRedirectToDomain() {
        if (window.location.protocol !== 'https:') return false;
        const hostname = window.location.hostname;
        const isLoopback =
            hostname === 'localhost' || hostname === '127.0.0.1' || hostname === '[::1]';
        if (!isLoopback) return false;

        try {
            const [status, admin] = await Promise.all([
                BackendClient.getInternetStatus(),
                BackendClient.getAdminSettings(),
            ]);
            if (!status.active || !status.internet_access_enabled || !status.domain) return false;
            if (!status.hairpin_reachable) {
                console.log('[MW] Domain unreachable from LAN (no NAT hairpin) — staying local');
                return false;
            }
            const key = admin.local_key || '';
            if (!key) return false;

            const extPort = status.external_https_port || admin.https_port || 443;
            const origin = 'https://' + status.domain + (extPort !== 443 ? ':' + extPort : '');
            const path = window.location.pathname || '/';
            console.log('[MW] Internet Access live — moving to the public domain:', origin);
            window.location.replace(origin + path + '?mwk=' + encodeURIComponent(key));
            return true;
        } catch (err) {
            console.warn('[MW] Domain redirect check failed:', err);
            return false;
        }
    },

    /**
     * True when this browser runs on the host machine: loopback origin, or the
     * backend confirmed it (host-key session over the public domain).
     */
    _isHostLocal() {
        if (this._isHostMachine) return true;
        const hostname = window.location.hostname;
        return hostname === 'localhost' || hostname === '127.0.0.1' || hostname === '[::1]';
    },

    async _checkAuth() {
        this.state = 'auth_check';
        console.log('[MW] Checking authentication...');

        const main = document.getElementById('main-content');
        if (!main) return true;

        try {
            const status = await BackendClient.getAuthStatus();
            this._isHostMachine = !!status.is_localhost;

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
                        <button class="btn btn-reload">${t('common.retry')}</button>
                    </div>
                </div>
            `;
            main.querySelector('.btn-reload').addEventListener('click', () => location.reload());
            return false;
        }
    },

    /**
     * On localhost, check whether setup is pending and, if so, render the
     * SetupView. Shown on first run, but also on later launches whenever an
     * essential step is missing (Sunshine not installed / not paired — e.g.
     * stale settings from a previous install), with the completed steps
     * pre-checked. "Skip for now" dismisses it persistently for this browser.
     * Returns true when the wizard was shown (init() should stop; the wizard
     * reloads the page when finished). Best-effort: any error just lets normal
     * init continue.
     */
    async _maybeShowSetup() {
        if (!this._isHostLocal()) return false;

        try {
            const status = await BackendClient.getSetupStatus();
            // Windows provisioning is owned by the Inno Setup installer.
            if (!status || status.os === 'Windows') return false;
            const firstRun = status.setup_completed === false;
            // Only Sunshine's *installed* state gates a re-show: `paired` is
            // discovered asynchronously (mDNS host list), so at cold start a
            // fully set-up machine often reports paired=false for a moment — which
            // used to re-pop the wizard on every launch. If Sunshine is installed
            // but momentarily unpaired, the hosts page handles pairing itself.
            const sunshineInstalled = !!(status.sunshine && status.sunshine.installed);
            const dismissed = localStorage.getItem('mw_setup_dismissed') === '1';
            if (!firstRun && (sunshineInstalled || dismissed)) return false;
        } catch (err) {
            console.warn('[MW] Setup status check failed:', err);
            return false;
        }

        const main = document.getElementById('main-content');
        if (!main) return false;
        main.innerHTML = '';
        this.transition('setup');
        this.setupView = new SetupView(main, () => {
            this.setupView = null;
        });
        this.setupView.start();
        return true;
    },

    // =========================================================================
    // Global zoom guard
    // =========================================================================

    /**
     * Prevent browser zoom on the application UI. Zoom gestures are allowed
     * only on the stream surfaces (<canvas>/<video> and the transparent input
     * layer covering them), where StreamViewTouch owns the pinch handling.
     *
     * The viewport meta (user-scalable=no) is ignored by iOS Safari and the
     * body touch-action CSS doesn't cover Ctrl+wheel on desktop, so both
     * paths need explicit JS guards.
     */
    _initZoomGuard() {
        const onStreamSurface = (target) =>
            target instanceof Element &&
            !!target.closest('#stream-canvas, #stream-video, #stream-input-layer');

        // iOS Safari pinch — non-standard gesture events, fired even with
        // user-scalable=no. preventDefault() is the only reliable block.
        for (const type of ['gesturestart', 'gesturechange', 'gestureend']) {
            document.addEventListener(
                type,
                (e) => {
                    if (!onStreamSurface(e.target)) e.preventDefault();
                },
                { passive: false, capture: true },
            );
        }

        // Safari fallback: an in-progress pinch is also reported on touchmove
        // via the proprietary `scale` property.
        document.addEventListener(
            'touchmove',
            (e) => {
                if (e.scale !== undefined && e.scale !== 1 && !onStreamSurface(e.target)) {
                    e.preventDefault();
                }
            },
            { passive: false, capture: true },
        );

        // Desktop: Ctrl+wheel (mouse) and trackpad pinch (delivered as a
        // ctrlKey wheel event) trigger page zoom unless prevented.
        window.addEventListener(
            'wheel',
            (e) => {
                if (e.ctrlKey && !onStreamSurface(e.target)) e.preventDefault();
            },
            { passive: false, capture: true },
        );
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
        // Always hide the Admin button upfront, regardless of whether we're on
        // localhost — the button will be shown later by _initNavButtons() only
        // when authenticated (and on localhost).
        const btnAdmin = document.getElementById('btn-admin');
        if (btnAdmin) btnAdmin.style.display = 'none';
        const isLocal =
            window.location.hostname === 'localhost' ||
            window.location.hostname === '127.0.0.1' ||
            window.location.hostname === '[::1]';
        const btnSettings = document.getElementById('btn-settings');
        if (btnSettings) {
            if (!isLocal) btnSettings.style.display = 'none';
        }
        return isLocal;
    },

    _initNavButtons() {
        // Backend-driven: also true when the page was opened on the host machine
        // through the public domain (host-key session).
        const isLocal = this._isHostLocal();

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
            // Authenticated (or host machine) — always available.
            btnSettings.style.display = '';
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
        if (hostname === 'localhost' || hostname === '127.0.0.1' || hostname === '[::1]')
            return false;
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

    async launchApp(host, app, codecOverride, hdrOverride, opts) {
        // A launch is already in flight or a stream is active — ignore. A second
        // launch here would trigger a backend take-over of the session the user
        // just started. (HostListView blocks card clicks too; this covers every
        // other entry point.)
        if (this.state === 'launching' || this.state === 'streaming') {
            console.warn(`[MW] Launch of "${app.name}" ignored: state=${this.state}`);
            return;
        }

        // Streaming your own PC: browser + MoonlightWeb + Sunshine all on ONE
        // machine (host flagged local AND this browser is on the host). Fun
        // "Inception" loop, but pointless — warn once and let the user proceed
        // ("Stream anyway") or back out. Skipped on codec/HDR fallback relaunches
        // and when already confirmed. The dialog re-invokes launchApp from its own
        // click so the iOS audio-unlock gesture below stays valid.
        if (
            !codecOverride &&
            !(opts && opts.skipSelfStreamWarn) &&
            host &&
            host.isLocalHost &&
            this._isHostLocal()
        ) {
            this._showSelfStreamWarning(host, app);
            return;
        }

        // iOS: create + unlock the AudioContext and start the silent element NOW,
        // while we still hold the launch-click user activation. The audio pipeline
        // is created only after the network round-trip below, by which point no
        // gesture is left — a context created then never acquires the loudspeaker
        // route and the session stays "ambient" (Silent-switch / ringer volume)
        // until the user taps the stream. AudioPipeline adopts this context.
        // No-op off iOS.
        iosAudioUnlock.prepareForLaunch(48000);

        console.log(
            `[MW] Launching: ${app.name} (id=${app.id}) on ${host.displayName}` +
                (codecOverride ? ` (forced codec: ${codecOverride})` : '') +
                (hdrOverride !== undefined ? ` (forced HDR: ${hdrOverride})` : ''),
        );
        this.transition('launching');
        Toast.info(t('launch.launching', { name: app.name }));

        // Reset fallback counter on user-initiated launch (not a fallback re-launch)
        if (!codecOverride) {
            this._fallbackAttemptCount = 0;
            // Reset transport-chain fallback state for a fresh launch.
            this._transportIndex = 0;
            this._firstTransportRetried = false;
            // Reset the congestion-degradation ladder. The overrides are
            // session-only (never persisted): a fresh launch always starts from
            // the user's own settings.
            this._degradeLevel = 0;
            this._degradeOverrides = {};
            this._lastDegradeTime = 0;
            this._enhancementAutoForced = false;
        }

        // Store host/app for HEVC fallback re-launch
        this._lastStreamHost = host;
        this._lastStreamApp = app;

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

        // Session-only congestion-degradation overrides (bitrate/fps/transport).
        // They survive codec/HDR fallback relaunches within the same session and
        // are reset on a fresh user-initiated launch above. localStorage (the UI
        // settings) is never touched.
        Object.assign(streamingSettings, this._degradeOverrides || {});

        // Override codec if provided (codec fallback chain)
        if (codecOverride) {
            streamingSettings.video_codec = codecOverride;
            // The H.264 fallback exists because the browser couldn't decode the
            // negotiated stream (e.g. HEVC Main10 HDR, unsupported on Android
            // Chrome). Drop HDR and 4:4:4 too: H.264 HDR is carried as High 4:4:4
            // Predictive 10-bit (profile 244, avc1.f4002a), which is even less
            // widely decodable and re-triggers the decode-error spiral. Fall all
            // the way back to plain 8-bit 4:2:0 H.264 for maximum compatibility.
            if (codecOverride === 'h264') {
                streamingSettings.hdr_enabled = false;
                streamingSettings.chroma_444_enabled = false;
            }
        }
        // Explicit HDR override from the fallback chain (e.g. AV1 HDR → HEVC SDR
        // drops HDR). undefined means "leave the stored preference untouched".
        if (hdrOverride !== undefined) {
            streamingSettings.hdr_enabled = hdrOverride === true;
        }

        // Mobile: request lower-bandwidth audio (10ms Opus frames, half the
        // packet rate) to ease transmission on constrained networks. Negligible
        // quality loss on phone speakers.
        if (IS_MOBILE_OR_TABLET) {
            streamingSettings.low_audio = true;
        }

        // Power Saving (mobile): force the native video transport, UDP first.
        // webrtc-media-udp is tried before any TCP/DataChannel fallback, so the
        // <video> pipeline (no canvas) and UDP have priority. The other settings
        // (H.264, no HDR/enhancement, no tearing, 720p/60) are already in the stored
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
                signalingUrl: result.signalingUrl,
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
                    hostDisplayName: this._nav.mainState.hostDisplayName,
                };
                history.pushState(guardState, '');

                // Remember context so a failed transport can be relaunched with
                // the next entry in the priority chain (see _onTransportFailed).
                this._lastStreamingSettings = streamingSettings;
                // A codec/HDR fallback re-launch (codecOverride set) is not a
                // fresh user launch — don't re-show the shortcuts/help slide.
                this._startStreamView(result, host, streamingSettings, !!codecOverride);
                // New StreamView rendered on top — drop any bridging loader left
                // by a codec/HDR fallback re-launch.
                this._hideRelaunchLoader();
            }
        } catch (err) {
            console.error('[MW] Launch failed:', err);
            // Launch failed before the audio pipeline could take over the iOS
            // session hold — release it so we don't keep a silent element looping.
            iosAudioUnlock.release();
            this._hideRelaunchLoader();
            // Revert the app card stuck on "Launching..." back to its idle state
            // and resume the host poll (paused when the launch began).
            if (this.hostListView) {
                this.hostListView.clearLaunching();
                this.hostListView.start();
            }
            // Sunshine couldn't start video capture (503). The dominant macOS
            // cause is a missing Screen Recording permission, which can't be
            // granted programmatically — show an explicit dialog telling the user
            // exactly what to enable, with a button to open the settings pane.
            if (err && err.responseBody && err.responseBody.code === 'video_capture_failed') {
                this._showVideoCaptureHelp(host, app, err.message);
                this.transition('app_list');
                return;
            }
            // Distinguish a client-side timeout (backend hung/crashed) from a
            // regular failure so the user gets a clear, actionable message.
            const msg =
                err && (err.aborted || err.message === 'server_timeout')
                    ? t('launch.timeout')
                    : err.message || t('launch.failed');
            Toast.error(msg);
            this.transition('app_list');
        }
    },

    /**
     * Playful confirmation shown when the user is about to stream the very PC
     * they're sitting at (the "Inception" loop). Offers the LAN/domain URL to use
     * from another device, a "Stream anyway" escape hatch, and a Cancel. Cancel
     * (or Escape / backdrop click) reverts the launching card and resumes polling;
     * "Stream anyway" re-invokes launchApp with the warning suppressed — done from
     * the button's own click so the iOS audio-unlock user gesture stays valid.
     */
    async _showSelfStreamWarning(host, app) {
        // Guard against a double-open (e.g. a replayed click).
        if (document.querySelector('.self-stream-overlay')) return;

        // Fetch the "reach me from elsewhere" URL up front (best-effort).
        const info = await this._computeRemoteAccessUrls();
        if (document.querySelector('.self-stream-overlay')) return;
        const url = info && info.url ? info.url : '';

        const overlay = document.createElement('div');
        overlay.className = 'pairing-overlay self-stream-overlay';
        // Plain (non-clickable) text so the user can select and copy the URL to
        // paste into a browser on another device.
        const urlHtml = url
            ? `<p class="self-stream-url">${escapeHtml(t('selfStream.useUrl'))}
                   <span class="self-stream-url-value">${escapeHtml(url)}</span></p>`
            : '';
        overlay.innerHTML = `
            <div class="pairing-dialog">
                <h3>${escapeHtml(t('selfStream.title'))}</h3>
                <p class="pairing-instruction">${escapeHtml(t('selfStream.body'))}</p>
                ${urlHtml}
                <div class="pairing-actions">
                    <button class="btn btn-secondary self-stream-cancel">${escapeHtml(
                        t('common.cancel'),
                    )}</button>
                    <button class="btn self-stream-go">${escapeHtml(
                        t('selfStream.streamAnyway'),
                    )}</button>
                </div>
            </div>
        `;

        const onKey = (e) => {
            if (e.key === 'Escape') cancel();
        };
        const cleanup = () => {
            document.removeEventListener('keydown', onKey);
            overlay.remove();
        };
        const cancel = () => {
            cleanup();
            // Revert the app card stuck on "Launching..." and resume the host poll
            // (both paused by HostListView before it called onLaunchApp).
            if (this.hostListView) {
                this.hostListView.clearLaunching();
                this.hostListView.start();
            }
        };
        const proceed = () => {
            cleanup();
            this.launchApp(host, app, undefined, undefined, { skipSelfStreamWarn: true });
        };

        overlay.querySelector('.self-stream-cancel').addEventListener('click', cancel);
        overlay.querySelector('.self-stream-go').addEventListener('click', proceed);
        overlay.addEventListener('click', (e) => {
            if (e.target === overlay) cancel();
        });
        document.addEventListener('keydown', onKey);
        document.body.appendChild(overlay);
    },

    /**
     * Explicit modal shown when Sunshine can't start video capture. Leads with
     * the macOS Screen Recording permission (the common cause) and offers two
     * host-side actions — shown only when the browser is on the host itself
     * (localhost), the only place the buttons can act:
     *   1. Open the Screen Recording settings pane so the user can grant it.
     *   2. Restart Sunshine + retry the launch. macOS only applies a freshly
     *      granted TCC permission to a *relaunched* process, so a Sunshine that
     *      was already running keeps failing capture even after the user ticks
     *      the box — the reason the dialog "reappears even though I authorized
     *      it". Restarting Sunshine is what actually clears the error.
     * Buttons are stacked (column) so the long "Open Screen Recording settings"
     * label fits inside the narrow dialog instead of being clipped.
     */
    _showVideoCaptureHelp(host, app, detail) {
        const onHost = this._isHostLocal();
        const name = (host && (host.displayName || host.name)) || '';

        const overlay = document.createElement('div');
        overlay.className = 'pairing-overlay';
        const hostBtns = onHost
            ? `<button class="btn btn-open btn-open-screenrec">${escapeHtml(
                  t('permission.screenRecording.openSettings'),
              )}</button>
               <button class="btn btn-save btn-restart-sunshine">${escapeHtml(
                   t('permission.screenRecording.restartRetry'),
               )}</button>`
            : '';
        overlay.innerHTML = `
            <div class="pairing-dialog">
                <h3>${escapeHtml(t('permission.screenRecording.title'))}</h3>
                <p class="pairing-instruction">${escapeHtml(
                    t('permission.screenRecording.body', { name }),
                )}</p>
                <div class="pairing-actions pairing-actions-stack">
                    ${hostBtns}
                    <button class="btn btn-secondary btn-dismiss">${escapeHtml(
                        t('common.close'),
                    )}</button>
                </div>
            </div>
        `;
        const close = () => overlay.remove();
        overlay.addEventListener('click', (e) => {
            if (e.target === overlay) close();
        });
        overlay.querySelector('.btn-dismiss').addEventListener('click', close);
        const ob = overlay.querySelector('.btn-open-screenrec');
        if (ob) {
            ob.addEventListener('click', async () => {
                try {
                    await BackendClient.openScreenRecordingSettings();
                    Toast.info(t('permission.screenRecording.opened'));
                } catch (e) {
                    console.warn('[MW] open screen recording settings failed:', e);
                    Toast.error(e.message || t('permission.screenRecording.openFailed'));
                }
            });
        }
        const rb = overlay.querySelector('.btn-restart-sunshine');
        if (rb) {
            rb.addEventListener('click', async () => {
                rb.disabled = true;
                rb.classList.add('btn-loading');
                try {
                    // Stop then start so the new process reads the just-granted
                    // Screen Recording permission (TCC only applies it on
                    // relaunch). Give the fresh process a moment to open its
                    // GameStream port before retrying the launch.
                    await BackendClient.stopSunshine();
                    await new Promise((r) => setTimeout(r, 800));
                    await BackendClient.startSunshine();
                    await new Promise((r) => setTimeout(r, 3000));
                    Toast.info(t('permission.screenRecording.restarted'));
                    close();
                    if (app) this.launchApp(host, app);
                } catch (e) {
                    console.warn('[MW] restart Sunshine failed:', e);
                    Toast.error(e.message || t('permission.screenRecording.restartFailed'));
                    rb.disabled = false;
                    rb.classList.remove('btn-loading');
                }
            });
        }
        document.body.appendChild(overlay);
    },

    /**
     * Create the StreamView for a successful launch response and wire its
     * callbacks. Shared by the initial launch and transport-chain relaunches.
     * Stores the backend-reported transport fallback chain + index.
     */
    _startStreamView(result, host, streamingSettings, suppressShortcuts = false) {
        const isRemote = this._isRemoteConnection();
        const upnpEnabled = true;
        const internalTransport = result.transport || (result.signalingUrl ? 'webrtc' : 'wss');
        const transportMode = result.transport_mode || internalTransport;
        // Respect the "show performance stats" setting (default: true)
        const showPerfStats = streamingSettings.show_performance_stats !== false;
        // Touch/trackpad sensitivity (default 2.2)
        const touchSensitivity =
            typeof streamingSettings.touch_sensitivity === 'number'
                ? streamingSettings.touch_sensitivity
                : 2.2;
        // Mobile only: direct touch-screen input (absolute) instead of trackpad.
        const touchScreen = streamingSettings.touch_screen === true;
        // Allow tearing (default off): disables VSync pacing and presents frames
        // on decode via a desynchronized canvas (Chromium desktop only — the
        // flag is ignored elsewhere). Migrates the legacy inverted vsync key.
        const tearing =
            streamingSettings.tearing_enabled === true ||
            (streamingSettings.tearing_enabled === undefined &&
                streamingSettings.vsync_enabled === false);
        // Video worker mode: 'auto' (heuristic — desktop only), 'on' or 'off'.
        const videoWorker = streamingSettings.video_worker;
        // Video enhancement (WebGPU upscale/sharpen). Forced OFF on MediaTrack:
        // <video> cannot be processed by WebGPU, so a MediaTrack last-resort
        // attempt always streams without enhancement.
        let videoEnhancement = streamingSettings.video_enhancement === 'on' ? 'on' : 'off';
        if (internalTransport === 'webrtc-media') videoEnhancement = 'off';
        const videoEnhancementAlgo = streamingSettings.video_enhancement_algo || 'auto';
        // HDR: requires a WebGPU-capable browser and Sunshine negotiating HEVC
        // Main10 / AV1 10-bit. The decoder colorSpace is set accordingly.
        const hdrEnabled = streamingSettings.hdr_enabled === true;
        // Audio time-stretch (WSOLA) — server kill switch (env MW_AUDIO_TIME_STRETCH).
        // Read fresh from the launch result; defaults to on when unspecified.
        const audioTimeStretch = result.audio_time_stretch !== false;

        // Track the fallback chain reported by the backend.
        this._transportChain = Array.isArray(result.transport_chain) ? result.transport_chain : [];
        this._transportIndex =
            typeof result.transport_index === 'number' ? result.transport_index : 0;

        this.streamView = new StreamView(
            document.getElementById('app'),
            this._streamWsUrl(result.signalingUrl || result.wsUrl),
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
            tearing,
            videoWorker,
            videoEnhancement,
            videoEnhancementAlgo,
            result.yuv444 === true,
            hdrEnabled,
            touchScreen,
            audioTimeStretch,
        );

        // The keyboard/gesture help slide shows once per user-initiated launch.
        // Suppress it on transport relaunches (congestion degradation / fallback
        // chain / codec fallback) so it doesn't re-pop on every degrade step.
        this.streamView._suppressShortcutsSlide = suppressShortcuts;

        // ── Callbacks ──────────────────────────────────────────────────────
        // onQuit: normal end (Stop button / mid-stream disconnect).
        this.streamView.onQuit = () => this._onStreamingQuit();
        // onConnectionFailed: transport never connected → try the next entry
        // in the priority chain (or give up if the chain is exhausted).
        this.streamView.onConnectionFailed = (reason) => this._onTransportFailed(reason);
        // onCongestion: sustained congestion detected mid-stream → relaunch with
        // a degraded session-only profile (bitrate −30% steps, media transport,
        // 60 fps cap). See _onStreamCongested().
        this.streamView.onCongestion = () => this._onStreamCongested();
    },

    /**
     * Anchor the signaling WebSocket to the exact origin the page was loaded
     * from, keeping only the backend-provided path (/ws or /ws/stream).
     *
     * The WS is proxied through the same HTTPS port as the page (HttpServer
     * upgrades /ws), but the backend builds the URL from its *internal* HTTPS
     * port (443). When this instance is reached through a non-standard external
     * port — e.g. the UPnP multi-instance fallback maps public :44729 → internal
     * :443 — the backend emits wss://domain/ws (i.e. :443), which the router
     * forwards to a *different* instance that owns external 443. The result is an
     * immediate 1006 "connection failed" and every transport falls through.
     * window.location.host already carries the correct host:port (the REST calls
     * use it and succeed), so reuse it verbatim.
     */
    _streamWsUrl(rawUrl) {
        let path = '/ws';
        try {
            if (rawUrl) path = new URL(rawUrl, window.location.href).pathname || '/ws';
        } catch (e) {
            console.warn('[MW] Could not parse signaling URL, defaulting to /ws:', rawUrl, e);
        }
        const scheme = window.location.protocol === 'https:' ? 'wss' : 'ws';
        return `${scheme}://${window.location.host}${path}`;
    },

    /** Human-readable label for a transport mode (warning toasts / logs). */
    _transportLabel(mode) {
        const keys = {
            'webrtc-dc-udp': 'transport.webrtcDcUdp',
            'webrtc-dc-tcp': 'transport.webrtcDcTcp',
            'webrtc-media-udp': 'transport.webrtcMediaUdp',
            'webrtc-media-tcp': 'transport.webrtcMediaTcp',
            wss: 'transport.wss',
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
        // Ignore if the user pressed Stop (quit animation / quit in progress) —
        // relaunching here would resurrect the session being stopped.
        if (this.streamView && (this.streamView._manualQuitting || this.streamView._quitting)) {
            console.warn(`[MW] Transport failure ignored (stopping): ${reason}`);
            return;
        }

        const chain = this._transportChain || [];
        const cur = this._transportIndex || 0;

        // WebCodecs cannot decode anything on this machine/browser: retrying the
        // same transport (or any other WebCodecs-based one) is pointless.
        const decoderFail = reason === 'decoder_unsupported';

        // Always retry the FIRST transport once before moving down the chain — a
        // single transient ICE failure on the preferred transport shouldn't
        // immediately downgrade. The retry is silent (no warning toast).
        if (cur === 0 && !this._firstTransportRetried && !decoderFail) {
            this._firstTransportRetried = true;
            console.warn(`[MW] Transport ${chain[0]} failed (${reason}) — retrying once (silent)`);
            this._relaunchTransport(0);
            return;
        }

        let next = cur + 1;
        // On a decode-capability failure, skip every other WebCodecs-based
        // transport (webrtc-dc-*, wss) and jump straight to a native-media one
        // (<video> RTP decodes via the browser's regular media stack).
        if (decoderFail) {
            while (next < chain.length && !chain[next].startsWith('webrtc-media')) next++;
        }
        if (next < chain.length) {
            console.warn(`[MW] Transport ${chain[cur]} failed (${reason}) — trying ${chain[next]}`);
            Toast.warning(
                t('transport.connectFailed', {
                    from: this._transportLabel(chain[cur]),
                    to: this._transportLabel(chain[next]),
                }),
            );
            this._relaunchTransport(next);
        } else {
            console.error(
                `[MW] All transports failed (last: ${chain[cur] || '?'}, reason: ${reason})`,
            );
            Toast.error(t('transport.allFailed'));
            this._hideRelaunchLoader();
            // quit() fires onQuit → _onStreamingQuit, which handles navigation
            // back to the apps view. silent: suppress the "Stream end" toast.
            if (this.streamView) this.streamView.quit({ silent: true });
        }
    },

    /** Full-screen loader shown during a transport relaunch so the apps view is
     *  never revealed between tearing down the old StreamView and rendering the
     *  new one. Reuses the StreamView startup-loader visuals. When a frozen
     *  last frame of the outgoing stream is provided, it is shown (dimmed)
     *  behind the steps so the transition reads as a brief pause, not a cut. */
    _showRelaunchLoader(freezeFrame) {
        if (document.getElementById('stream-relaunch-loader')) return;
        const el = document.createElement('div');
        el.id = 'stream-relaunch-loader';
        // Mirror the StreamView startup overlay (3 steps, step 1 active) so a
        // transport retry never flickers from 3 steps down to a single line and
        // back — common on macOS where the first ICE attempt often retries once.
        el.innerHTML =
            '<div class="startup-loader" aria-hidden="true"><div class="startup-loader-ring"></div></div>' +
            '<div class="startup-step active" data-step="1"><span class="startup-step-dot"></span>' +
            '<span class="startup-step-label">' +
            t('stream.connecting') +
            '</span></div>' +
            '<div class="startup-step" data-step="2"><span class="startup-step-dot"></span>' +
            '<span class="startup-step-label">' +
            t('stream.startingVideo') +
            '</span></div>' +
            '<div class="startup-step" data-step="3"><span class="startup-step-dot"></span>' +
            '<span class="startup-step-label">' +
            t('stream.streamReady') +
            '</span></div>';
        if (freezeFrame) {
            freezeFrame.className = 'relaunch-freeze-frame';
            el.classList.add('has-freeze-frame');
            el.insertBefore(freezeFrame, el.firstChild);
        }
        document.getElementById('app').appendChild(el);
    },

    _hideRelaunchLoader() {
        const el = document.getElementById('stream-relaunch-loader');
        if (el) el.remove();
    },

    /** Persist hdr_enabled=false in localStorage so the Settings HDR checkbox
     *  reflects an automatic HDR fallback. Best-effort (ignores parse errors). */
    _persistHdrDisabled() {
        try {
            const stored = localStorage.getItem('mw-streaming-settings');
            const settings = stored ? JSON.parse(stored) : {};
            settings.hdr_enabled = false;
            localStorage.setItem('mw-streaming-settings', JSON.stringify(settings));
        } catch (e) {
            console.warn('[MW] Failed to persist HDR fallback to localStorage:', e);
        }
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
        // is never revealed (stay on the stream initialization screen). When the
        // old stream already presented frames (congestion degradation, not a
        // failed connect), bridge with its frozen last image instead of a black
        // loader so the interruption stays discreet.
        const sv = this.streamView;
        const freezeFrame = sv ? sv.captureLastFrame() : null;
        this._showRelaunchLoader(freezeFrame);

        // Quietly tear down the failed StreamView (no navigation, no toast).
        this.streamView = null;
        if (sv) {
            sv.onQuit = null;
            sv.onConnectionFailed = null;
            sv.onCongestion = null;
            try {
                await sv.quit({ silent: true });
            } catch (e) {
                /* ignore */
            }
        }

        this._transportIndex = nextIndex;

        // Relaunch targeting the next chain index. Same settings otherwise (plus
        // any session-only degradation overrides); the backend recomputes the
        // chain and picks chain[nextIndex].
        const settings = {
            ...this._lastStreamingSettings,
            ...(this._degradeOverrides || {}),
            transport_index: nextIndex,
        };
        try {
            const result = await BackendClient.launchApp(host.uuid, app.id, settings);
            if (result.status === 'streaming') {
                // Transport relaunch (fallback chain / congestion degradation) —
                // suppress the shortcuts slide so it doesn't re-pop each step.
                // Pass the merged settings so the degradation overrides (reduced
                // height, auto-SGSR enhancement) reach the new StreamView.
                this._startStreamView(result, host, settings, true);
                if (freezeFrame && this.streamView) {
                    // Hold the frozen frame until the new stream presents its
                    // first frame (bounded — the failure paths hide the loader
                    // themselves, this timer only covers a silent stall).
                    const failsafe = setTimeout(() => this._hideRelaunchLoader(), 15000);
                    this.streamView.onFirstFrame = () => {
                        clearTimeout(failsafe);
                        this._hideRelaunchLoader();
                    };
                } else {
                    // New StreamView rendered its own #stream-view on top — drop
                    // the bridging loader.
                    this._hideRelaunchLoader();
                }
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

    /** Effective stream height (px) for the degradation ladder. stream_height 0
     *  means "Same as Host": probe the live stream's real frame height, and
     *  assume the 1080p reference when that is unknown too. */
    _effectiveStreamHeight(effective) {
        if (effective.stream_height > 0) return effective.stream_height;
        const live = this.streamView ? this.streamView.currentFrameHeight() : 0;
        return live > 0 ? live : 1080;
    },

    /**
     * Sustained congestion reported by StreamView: relaunch the CURRENT stream
     * with a degraded, session-only profile. The user's settings in the UI are
     * never touched — a fresh launch starts from them again.
     *
     * Escalation ladder — one knob per congestion event, in order (state-based,
     * so a knob that is already at its floor is skipped naturally):
     *   1. bitrate −30%
     *   2. resolution one rung down (2160→1440→1080→720), bitrate aligned to
     *      the new height; SGSR upscaling is auto-enabled when the user streams
     *      without video enhancement so the drop stays visually discreet
     *   3. switch to the native media transport (RTP: no SCTP queueing/cwnd)
     *   4. cap at 60 fps (+ bitrate −30%)
     *   5+ bitrate −30% … floor at 2 Mbps, then remaining resolution rungs
     * Once every knob is at its floor, keep restarting the transport — a fresh
     * SCTP/ICE state is exactly what a manual stop/start fixes.
     */
    _onStreamCongested() {
        if (this._nav.overlay !== 'streaming') return;
        const now = performance.now();
        if (this._lastDegradeTime && now - this._lastDegradeTime < 25000) return;
        this._lastDegradeTime = now;

        this._degradeOverrides = this._degradeOverrides || {};
        const o = this._degradeOverrides;
        const effective = { ...this._lastStreamingSettings, ...o };
        const curBitrate = effective.stream_bitrate > 0 ? effective.stream_bitrate : 20000;
        const curFps = effective.stream_fps > 0 ? effective.stream_fps : 60;
        const curHeight = this._effectiveStreamHeight(effective);
        const chain = this._transportChain || [];
        const curTransport = chain[this._transportIndex || 0] || '';
        const onMedia = curTransport.startsWith('webrtc-media');

        this._degradeLevel = (this._degradeLevel || 0) + 1;

        let relaunchIndex = this._transportIndex || 0;
        let toastKey;
        const toastParams = {};

        const cutBitrate = () => {
            o.stream_bitrate = Math.max(2000, Math.round(curBitrate * 0.7));
            toastKey = 'stream.degradeBitrate';
            toastParams.mbps = (o.stream_bitrate / 1000).toFixed(1);
        };
        const stepDownResolution = () => {
            const next = [2160, 1440, 1080, 720].find((r) => r < curHeight);
            o.stream_height = next;
            // Resolution alone barely shrinks the encoded bandwidth unless the
            // bitrate follows: align it with the recommendation for the new
            // height (never raise it, keep the 2 Mbps floor).
            const autoKbps =
                computeAutoBitrate(
                    next,
                    curFps,
                    effective.stream_aspect,
                    effective.chroma_444_enabled === true,
                    effective.hdr_enabled === true,
                ) * 1000;
            o.stream_bitrate = Math.max(2000, Math.min(curBitrate, autoKbps));
            toastKey = 'stream.degradeResolution';
            toastParams.res = String(next);
            // SGSR auto-enable: upscale the reduced stream back to the display
            // size so the drop stays visually discreet. Session-only override —
            // the user's Settings checkbox is never touched. Not applicable on
            // the media transport (<video> element, no canvas for WebGPU).
            if (effective.video_enhancement !== 'on' && !onMedia && !o.transport_mode) {
                o.video_enhancement = 'on';
                o.video_enhancement_algo = 'sgsr';
                this._enhancementAutoForced = true;
                toastKey = 'stream.degradeResolutionSgsr';
            }
        };

        if (o.stream_bitrate === undefined && curBitrate > 2000) {
            cutBitrate();
        } else if (o.stream_height === undefined && curHeight > 720) {
            stepDownResolution();
        } else if (!onMedia && !o.transport_mode) {
            o.transport_mode = 'webrtc-media-udp';
            relaunchIndex = 0; // forced mode is first in the recomputed chain
            toastKey = 'stream.degradeMedia';
        } else if (o.stream_fps === undefined && curFps > 60) {
            o.stream_fps = 60;
            if (curBitrate > 2000) {
                cutBitrate();
                toastParams.mbps = (o.stream_bitrate / 1000).toFixed(1);
            } else {
                toastParams.mbps = (curBitrate / 1000).toFixed(1);
            }
            toastKey = 'stream.degradeBitrateFps';
        } else if (curBitrate > 2000) {
            cutBitrate();
        } else if (curHeight > 720) {
            stepDownResolution();
        } else {
            // Every knob at its floor: plain transport restart (fresh
            // SCTP/ICE state, same effect as a manual stop/start).
            toastKey = 'stream.degradeRestart';
        }

        console.warn(
            '[MW] Sustained congestion — degrade step ' + this._degradeLevel + ':',
            JSON.stringify(o),
        );
        Toast.warning(t(toastKey, toastParams));
        this._relaunchTransport(relaunchIndex);
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
            iosAudioUnlock.release(); // real teardown — drop the iOS session hold
            return;
        }

        // Capture fallback state BEFORE nulling streamView
        const fallbackRequested = this.streamView && this.streamView._codecFallbackRequested;
        const fallbackTarget = this.streamView && this.streamView._codecFallback;
        const fallbackHost = this._lastStreamHost;
        const fallbackApp = this._lastStreamApp;

        this._nav.overlay = null;
        this.streamView = null;

        // ── Codec fallback chain ──────────────────────────────────────────
        // When the browser cannot decode the negotiated config, the stream quits
        // with _codecFallbackRequested=true and a _codecFallback {codec, hdr}
        // target. Chain: HEVC HDR → AV1 HDR → HEVC SDR → H.264 SDR. Re-launch
        // with the next step (loader stays up so Apps is never revealed).
        if (fallbackRequested && fallbackTarget && fallbackHost && fallbackApp) {
            this._fallbackAttemptCount++;
            // The chain has at most 3 hops; allow up to 4 attempts as a safety net.
            if (this._fallbackAttemptCount > 4) {
                console.error('[MW] Codec fallback chain exhausted, giving up');
                iosAudioUnlock.release(); // giving up — drop the iOS session hold
                this._hideRelaunchLoader();
                this.transition('host_list');
                if (history.state && history.state.view === this._GUARD_PREFIX + 'streaming') {
                    history.back();
                } else {
                    this._renderMainView();
                    history.replaceState({ view: 'hosts' }, '', '/');
                }
                Toast.error(t('launch.hevcH264Unsupported'));
                return;
            }

            console.warn(
                `[MW] Codec fallback → ${fallbackTarget.codec}` +
                    `${fallbackTarget.hdr ? ' HDR' : ' SDR'} (attempt ${this._fallbackAttemptCount})`,
            );

            // When the fallback drops HDR, persist the unchecked preference so the
            // Settings HDR checkbox reflects reality, and inform the user.
            if (fallbackTarget.hdr === false) {
                this._persistHdrDisabled();
                Toast.warning(t('launch.hdrFallback', { to: fallbackTarget.codec.toUpperCase() }));
            }

            // Keep the full-screen loader up across teardown + relaunch so the
            // Apps view is never flashed between attempts.
            this._showRelaunchLoader();

            // Pop the streaming guard from history (without revealing Apps: the
            // loader covers it).
            if (history.state && history.state.view === this._GUARD_PREFIX + 'streaming') {
                history.back();
            }

            this.transition('host_list'); // Reset state for re-launch
            this.launchApp(fallbackHost, fallbackApp, fallbackTarget.codec, fallbackTarget.hdr);
            return;
        }

        // Reset fallback counter on successful non-fallback quit
        this._fallbackAttemptCount = 0;

        // Real streaming exit — release the iOS playback-session hold.
        iosAudioUnlock.release();

        this.transition('host_list');

        // Pop the streaming guard from history to reveal the hosts view below.
        if (history.state && history.state.view === this._GUARD_PREFIX + 'streaming') {
            // Guard still present — pop it via history.back(). This fires
            // popstate which navigates to the hosts view.
            history.back();
        } else {
            // Guard already consumed (e.g. user pressed Back before pressing
            // Stop). Render the hosts view directly.
            this._renderMainView();
            history.replaceState({ view: 'hosts' }, '', '/');
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
        if (this.loginView) {
            this.loginView.destroy();
            this.loginView = null;
        }
    },

    // ── Legacy helpers kept for backward compat ──────────────────────────────

    transition(newState) {
        console.log(`[MW] State: ${this.state} -> ${newState}`);
        this.state = newState;
    },
};

// Boot
document.addEventListener('DOMContentLoaded', async () => {
    // Load translations before the first render, then translate the static
    // markup (header buttons / footer) that exists in index.html.
    await i18nInit();
    applyDOM(document);
    MoonlightApp.init();
});
