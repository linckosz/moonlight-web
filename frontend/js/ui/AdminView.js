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
 * MoonlightWeb — Server Settings
 *
 * Server administration functions (localhost only):
 *   - Internet Access (Azure DNS) with DNS propagation check
 *   - HTTPS port configuration
 *   - Transport mode
 *   - Access PIN display with copy-to-clipboard
 *   - Active sessions table with geo-location and revoke
 *
 * All settings stored server-side. Unsaved changes are discarded on close.
 */
import { BackendClient } from '../api/BackendClient.js';
import { Toast } from './Toast.js';
import { t } from '../i18n/i18n.js';
import { escapeHtml } from '../util/escapeHtml.js';

export class AdminView {
    // Backend statusJson "phase" values that mean activation is still running.
    static IN_PROGRESS_PHASES = [
        'starting',
        'detecting_ip',
        'registering_dns',
        'checking_dns',
        'issuing_certificate',
        'configuring_ports',
    ];

    constructor(container, onClose) {
        this.container = container;
        this.onClose = onClose || (() => {});

        // Server settings state
        this._httpsPort = 443;
        this._httpPort = 80;
        // External (router-side) HTTPS port for the public domain URL. Equals the
        // local port for the first instance; a fallback port for a co-existing
        // instance behind the same NAT.
        this._externalHttpsPort = 443;

        // Internet Access state (Azure DNS)
        this._internetEnabled = false;
        this._domain = '';
        this._publicIp = '';
        this._localIp = '';
        this._uniqueId = '';
        this._transportMode = 'auto';
        this._availableTransports = [];
        this._upnpAvailable = false;
        this._pendingRegistration = false;
        this._active = false; // DNS registration actually succeeded (domain live)
        this._lastError = '';

        // Activation loader: live step reported by the backend (statusJson.phase).
        this._activating = false;
        this._phase = '';
        this._phasePollTimer = null;

        // DNS propagation polling
        this._dnsPollTimer = null;
        this._dnsPollAttempts = 0;
        this._maxDnsPollAttempts = 60; // 5 min at 5s interval

        // Sunshine (local streaming server) state — from /api/setup/status.
        this._sunshineInstalled = false;
        this._sunshineCanAutoInstall = false;
        this._sunshineChecked = false; // status fetched at least once

        // Auth / PIN state (default "------" = no valid PIN, 6 digits)
        this._pin = '------';
        this._pinConsumed = false; // true when PIN was used by remote client
        this._activeSessions = 0;
        this._sessions = [];
        this._certAuthEnabled = false;

        // Dirty tracking: snapshot of values at load time
        this._cleanState = {};
        this._dirty = false;

        // Sessions polling
        this._sessionsPollTimer = null;
        // Pagination for sessions table
        this._sessionPage = 0;
        this._sessionsPerPage = 5;
    }

    async start() {
        await this._loadState();
        await this._loadInternetState();
        await this._loadAuthStatus();
        await this._loadSessions();
        await this._loadSunshineState();
        this.render();
        this.bindEvents();
        this._startDnsPollingIfNeeded();
        // Provisioning may have started Internet Access before the page opened;
        // keep the loader live until the backend reaches a terminal phase.
        if (this._activating) this._startPhasePolling();
        this._startSessionsPolling();
    }

    async _loadState() {
        try {
            const admin = await BackendClient.getAdminSettings();
            this._httpsPort = admin.https_port || 443;
            this._httpPort = admin.http_port || 80;
            this._certAuthEnabled = admin.cert_auth_enabled || false;
        } catch (err) {
            console.warn('[Admin] Failed to load server settings:', err);
        }
    }

    async _loadInternetState() {
        try {
            const status = await BackendClient.getInternetStatus();
            this._internetEnabled = status.internet_access_enabled || false;
            this._domain = status.domain || '';
            this._externalHttpsPort = status.external_https_port || this._httpsPort;
            this._publicIp = status.public_ip || '';
            this._localIp = status.local_ip || '';
            this._uniqueId = status.unique_id || '';
            this._transportMode = status.transport_mode || 'auto';
            this._availableTransports = status.available_transports || [];
            this._upnpAvailable = status.upnp_available || false;
            this._pendingRegistration = status.pending_registration || false;
            this._active = status.active || false;
            this._phase = status.phase || '';
            // Show the activation checklist whenever the backend is mid-activation,
            // even when it was triggered by first-run provisioning (not the toggle).
            this._activating = AdminView.IN_PROGRESS_PHASES.includes(this._phase);
            this._lastError = status.last_error || '';
            // Auto-uncheck the toggle when the backend reports an error: an error
            // means the feature is not operational, so the checkbox must not look
            // enabled. start() clears stale errors, so last_error is current.
            if (this._lastError) {
                this._internetEnabled = false;
            }
            // Do NOT overwrite _httpsPort from internet status: the authoritative
            // source is /api/admin/settings (_loadState()). InternetAccessManager
            // may hold a stale port if setPorts() was not called after a backend
            // changeHttpsPort().
        } catch (err) {
            console.warn('[Admin] Failed to load internet status:', err);
        }
    }

    async _loadAuthStatus() {
        try {
            const status = await BackendClient.getAuthStatus();
            if (status.pin) {
                this._pin = status.pin;
            }
            if (status.pin_consumed !== undefined) {
                this._pinConsumed = status.pin_consumed;
            }
            if (status.active_sessions !== undefined) {
                this._activeSessions = status.active_sessions;
            }
        } catch (err) {
            console.warn('[Admin] Failed to load auth status:', err);
        }
    }

    // Sunshine install status (localhost only). Reuses the setup wizard's status
    // endpoint so the admin page can offer a Sunshine install to users who
    // skipped it (or whose install failed) during first-run setup.
    async _loadSunshineState() {
        if (!this._isLocalhost()) return;
        try {
            const status = await BackendClient.getSetupStatus();
            this._sunshineInstalled = !!(status.sunshine && status.sunshine.installed);
            this._sunshineCanAutoInstall = !!(status.sunshine && status.sunshine.can_auto_install);
            this._sunshineChecked = true;
        } catch (err) {
            console.warn('[Admin] Failed to load Sunshine status:', err);
        }
    }

    // Install Sunshine on demand. Reuses /api/setup/apply with only the Sunshine
    // step (no Internet/autostart changes). The OS asks for the account password
    // in a native polkit/authorization dialog; a wrong password surfaces as
    // sunshine_error (HTTP 200), so we show that rather than a generic failure.
    async _installSunshine() {
        const userEl = this.container.querySelector('#admin-sunshine-user');
        const passEl = this.container.querySelector('#admin-sunshine-pass');
        const btn = this.container.querySelector('#btn-install-sunshine');
        const user = (userEl?.value || '').trim();
        const pass = passEl?.value || '';
        if (!user || !pass) {
            Toast.warning(t('admin.sunshineCredsRequired'));
            return;
        }
        if (btn) {
            btn.disabled = true;
            btn.classList.add('btn-loading');
            btn.textContent = t('admin.installingSunshine');
        }
        try {
            const result = await BackendClient.applySetup({
                internet_access_authorized: false,
                autostart: false,
                sunshine: { install: true, username: user, password: pass },
            });
            if (result.sunshine_error) {
                Toast.error(t('admin.sunshineInstallFailed', { message: result.sunshine_error }));
            } else {
                Toast.success(t('admin.sunshineInstalledOk'));
                await this._loadSunshineState();
                this.render();
                this.bindEvents();
                return;
            }
        } catch (err) {
            console.error('[Admin] Failed to install Sunshine:', err);
            Toast.error(t('admin.sunshineInstallFailed', { message: err.message }));
        } finally {
            const b = this.container.querySelector('#btn-install-sunshine');
            if (b) {
                b.disabled = false;
                b.classList.remove('btn-loading');
                b.textContent = t('admin.installSunshine');
            }
        }
    }

    async _loadSessions() {
        try {
            const result = await BackendClient.getAuthSessions();
            const sessions = result.sessions || [];
            // Streaming sessions float to the top so they're immediately visible.
            sessions.sort((a, b) => (b.streaming ? 1 : 0) - (a.streaming ? 1 : 0));
            this._sessions = sessions;
        } catch (err) {
            console.warn('[Admin] Failed to load sessions:', err);
            // Server is dead — no streaming sessions can be alive. Clear the
            // flag on all known sessions so the UI doesn't show stale badges.
            this._sessions.forEach((s) => (s.streaming = false));
        }
    }

    destroy() {
        this._stopDnsPolling();
        this._stopSessionsPolling();
        this._stopPhasePolling();
    }

    // --- Sessions Polling ---

    _startSessionsPolling() {
        this._stopSessionsPolling();
        // Poll every 3 seconds to pick up new sessions (e.g. remote PIN validation)
        // and GeoIP data updates. Quick enough to feel instant on the admin page
        this._sessionsPollTimer = setInterval(async () => {
            // Refresh PIN: autoRegeneratePin() fires after each successful
            // PIN validation, so the displayed PIN may be stale.
            await this._loadAuthStatus();
            await this._loadSessions();
            const sessionsDisplay = this.container.querySelector('#admin-sessions-table');
            // Don't clobber an in-progress rename: skip the re-render while a
            // session name cell has focus.
            const editing = this.container.querySelector('.session-name-edit:focus');
            if (sessionsDisplay && !editing) {
                this._renderSessionsTable();
            }
            // Keep the session count header in the Active Sessions section in sync
            this._activeSessions = this._sessions.length;
            const sessionCount = this.container.querySelector('#admin-session-count');
            if (sessionCount) {
                sessionCount.textContent =
                    this._activeSessions > 0
                        ? t('admin.sessionCount', { count: this._activeSessions })
                        : t('admin.noActiveSessions');
            }
            // Update the PIN display. If the PIN was consumed (auto-regenerated
            // after remote validation), show "--------" to force the admin to
            // explicitly generate a fresh PIN.
            const pinDisplay = this.container.querySelector('#admin-pin-display');
            if (pinDisplay) {
                const displayValue = this._formatPin(this._pinConsumed ? '------' : this._pin);
                if (pinDisplay.textContent.trim() !== displayValue) {
                    pinDisplay.textContent = displayValue;
                    pinDisplay.classList.toggle('pin-consumed', this._pinConsumed);

                    // Update the consumed hint (show/hide)
                    const hintArea = this.container.querySelector('.pin-consumed-hint');
                    if (this._pinConsumed && !hintArea) {
                        // PIN became consumed while view was open — insert hint
                        const hint = document.createElement('p');
                        hint.className = 'settings-hint pin-consumed-hint';
                        hint.textContent = t('admin.pinConsumedHint');
                        // Append to the PIN field so it lands after .pin-display-area,
                        // matching the static render layout.
                        const field = pinDisplay.closest('.settings-field');
                        if (field) field.appendChild(hint);
                    } else if (!this._pinConsumed && hintArea) {
                        hintArea.remove();
                    }

                    if (this._pinConsumed) {
                        console.log('[Admin] PIN consumed by remote user — showing "--------"');
                    }
                }
            }
        }, 5000);
    }

    _stopSessionsPolling() {
        if (this._sessionsPollTimer) {
            clearInterval(this._sessionsPollTimer);
            this._sessionsPollTimer = null;
        }
    }

    // --- DNS Propagation Polling ---

    _startDnsPollingIfNeeded() {
        if (this._internetEnabled && this._pendingRegistration) {
            this._startDnsPolling();
        }
    }

    _startDnsPolling() {
        this._stopDnsPolling();
        this._dnsPollAttempts = 0;
        this._dnsPollTimer = setInterval(async () => {
            this._dnsPollAttempts++;
            if (this._dnsPollAttempts > this._maxDnsPollAttempts) {
                this._stopDnsPolling();
                this._pendingRegistration = false;
                this._lastError = t('admin.dnsTimeout');
                this.render();
                this.bindEvents();
                return;
            }
            try {
                const status = await BackendClient.getInternetStatus();
                if (status.active && status.domain) {
                    // Registration actually succeeded — domain is live
                    this._stopDnsPolling();
                    this._internetEnabled = true;
                    this._active = true;
                    this._domain = status.domain || this._domain;
                    this._externalHttpsPort = status.external_https_port || this._externalHttpsPort;
                    this._publicIp = status.public_ip || this._publicIp;
                    this._pendingRegistration = false;
                    this._lastError = '';
                    this.render();
                    this.bindEvents();
                    Toast.success(t('admin.dnsPropagated', { domain: this._domain }));
                } else if (!status.pending_registration) {
                    // No longer pending but not active → registration failed/gave up
                    this._stopDnsPolling();
                    this._active = false;
                    this._pendingRegistration = false;
                    this._lastError = status.last_error || t('admin.internetNotActive');
                    this._internetEnabled = false; // error → uncheck the toggle
                    this.render();
                    this.bindEvents();
                } else if (status.last_error && status.last_error !== this._lastError) {
                    this._lastError = status.last_error;
                    this._pendingRegistration = status.pending_registration !== false;
                    this._internetEnabled = false; // error → uncheck the toggle
                    this.render();
                    this.bindEvents();
                }
            } catch (err) {
                console.warn('[Admin] DNS poll failed:', err);
            }
        }, 5000);
    }

    _stopDnsPolling() {
        if (this._dnsPollTimer) {
            clearInterval(this._dnsPollTimer);
            this._dnsPollTimer = null;
        }
        this._dnsPollAttempts = 0;
    }

    // --- Dirty tracking ---

    _markClean() {
        const portInput = this.container.querySelector('#admin-https-port');
        this._cleanState = {
            httpsPort: portInput ? parseInt(portInput.value, 10) : this._httpsPort,
        };
        this._dirty = false;
        this._updateSaveButton();
    }

    _onFieldChange() {
        const portInput = this.container.querySelector('#admin-https-port');
        if (!portInput) return;

        const currentPort = parseInt(portInput.value, 10);
        this._dirty = currentPort !== this._cleanState.httpsPort;
        this._updateSaveButton();
    }

    _updateSaveButton() {
        const saveBtn = this.container.querySelector('#btn-admin-save');
        if (saveBtn) {
            saveBtn.disabled = !this._dirty;
        }
    }

    // --- Rendering ---

    _buildDomainUrl() {
        if (!this._domain) return '';
        const prefix = 'https://' + this._domain;
        // Use the external (router-side) port: a second instance behind the same
        // NAT is reachable from the internet on a fallback port, not 443.
        const port = this._externalHttpsPort || this._httpsPort;
        if (port !== 443) {
            return prefix + ':' + port;
        }
        return prefix;
    }

    _isLocalhost() {
        const hostname = window.location.hostname;
        return hostname === 'localhost' || hostname === '127.0.0.1' || hostname === '::1';
    }

    _buildLocalUrl() {
        const ip = this._getLocalIpForDisplay();
        if (!ip) return '';
        const port = this._httpsPort !== 443 ? ':' + this._httpsPort : '';
        return 'https://' + ip + port;
    }

    _getLocalIpForDisplay() {
        return this._localIp || '';
    }

    render() {
        this._sessionPage = 0;
        const transportLabels = {
            'webrtc-media-udp': 'WebRTC MediaTrack (UDP)',
            'webrtc-dc-udp': 'WebRTC DataChannel (UDP)',
            'webrtc-media-tcp': 'WebRTC MediaTrack (TCP)',
            'webrtc-dc-tcp': 'WebRTC DataChannel (TCP)',
            wss: 'WSS (WebSocket Secure)',
        };
        const transportOptions = [
            { value: 'auto', label: t('admin.transportAuto') },
            ...this._availableTransports.map((t) => ({
                value: t,
                label: transportLabels[t] || t,
            })),
        ];

        const domainUrl = this._buildDomainUrl();
        const showDomain = this._internetEnabled || !!this._domain;

        this.container.innerHTML = `
            <div class="admin-view" id="view-admin">
                <div class="admin-header">
                    <h2>${t('admin.title')}</h2>
                    <button class="view-close-btn" id="btn-admin-close"
                            title="${this.esc(t('admin.closeDiscard'))}">&times;</button>
                </div>

                <!-- PIN (localhost only) -->
                ${
                    this._isLocalhost()
                        ? `
                    <div class="settings-section">
                        <h3 class="settings-section-title">${t('admin.accessPin')}</h3>
                        <div class="settings-field u-pt-0">
                            <p class="setting-desc">
                                ${t('admin.accessPinDesc')}
                            </p>
                            <div class="pin-display-area">
                                <span class="pin-display ${this._pinConsumed ? 'pin-consumed' : ''}"
                                      id="admin-pin-display">
                                    ${this.esc(this._formatPin(this._pinConsumed ? '------' : this._pin))}
                                </span>
                                <button class="btn btn-secondary" id="btn-regenerate-pin">
                                    ${t('common.generate')}
                                </button>
                                <button class="btn btn-secondary" id="btn-copy-pin"
                                        title="${this.esc(t('admin.copyPinTitle'))}">
                                    ${t('common.copy')}
                                </button>
                                <button class="btn btn-secondary" id="btn-clear-pin">
                                    ${t('common.clear')}
                                </button>
                            </div>
                            ${
                                this._pinConsumed
                                    ? `
                                <p class="settings-hint pin-consumed-hint">
                                    ${t('admin.pinConsumedHint')}
                                </p>
                            `
                                    : ''
                            }
                        </div>
                    </div>
                `
                        : ''
                }

                <!-- Active Sessions (localhost only) -->
                ${
                    this._isLocalhost()
                        ? `
                    <div class="settings-section">
                        <h3 class="settings-section-title">${t('admin.activeSessions')}</h3>
                        <div class="settings-field u-pt-0">
                            <p class="setting-desc">
                                ${t('admin.activeSessionsDesc')}
                            </p>
                            <p class="settings-hint" id="admin-session-count">
                                ${
                                    this._activeSessions > 0
                                        ? t('admin.sessionCount', { count: this._activeSessions })
                                        : t('admin.noActiveSessions')
                                }
                            </p>
                            <div id="admin-sessions-table">
                                ${this._buildSessionsAreaHtml()}
                            </div>
                        </div>
                    </div>
                `
                        : ''
                }

                <!-- Certificate Authentication (localhost only) -->
                ${
                    this._isLocalhost()
                        ? `
                    <div class="settings-section">
                        <h3 class="settings-section-title">${t('admin.certAuth')}</h3>
                        <div class="settings-field u-pt-0">
                            <label class="settings-checkbox-label">
                                <input type="checkbox" id="chk-cert-auth"
                                       ${this._certAuthEnabled ? 'checked' : ''} />
                                <span class="settings-checkbox-text">${t('admin.enableCertAuth')}</span>
                            </label>
                            <p class="setting-desc">
                                ${t('admin.certAuthDesc')}
                            </p>
                            <div id="cert-download-area" style="${this._certAuthEnabled ? '' : 'display:none;'}">
                                <button class="btn btn-secondary u-mt-2" id="btn-download-cert">
                                    ${t('admin.downloadCert')}
                                </button>
                                <p class="settings-hint">
                                    ${t('admin.downloadCertHint')}
                                </p>
                                <div class="u-mt-2">
                                    <button class="btn btn-danger btn-small" id="btn-regenerate-cert">
                                        ${t('admin.regenerateCert')}
                                    </button>
                                    <span class="settings-hint u-ml-2">
                                        ${t('admin.regenerateCertHint')}
                                    </span>
                                </div>
                            </div>
                        </div>
                    </div>
                `
                        : ''
                }

                <!-- Internet -->
                <div class="settings-section">
                    <h3 class="settings-section-title">${t('admin.internet')}${
                        this._internetEnabled && !this._active && !this._lastError
                            ? `<span class="cyber-loader" title="${this.esc(t('admin.internetComingUp'))}"
                                     aria-label="${this.esc(t('admin.internetComingUp'))}"></span>`
                            : ''
                    }</h3>

                    <div class="settings-field">
                        <label class="settings-checkbox-label">
                            <input type="checkbox" id="chk-internet-enable"
                                   ${this._internetEnabled ? 'checked' : ''} />
                            <span class="settings-checkbox-text">${t('admin.enableInternet')}</span>
                        </label>
                    </div>

                    ${
                        this._activating
                            ? `<div id="internet-activation" class="internet-activation">${this._renderActivationSteps()}</div>`
                            : ''
                    }

                    ${
                        showDomain
                            ? `
                                <div class="admin-url-row">
                                    ${
                                        this._internetEnabled &&
                                        this._active &&
                                        !this._pendingRegistration &&
                                        domainUrl
                                            ? `<a href="${this.esc(domainUrl)}" target="_blank" rel="noopener" class="tunnel-url-link">${this.esc(domainUrl)}</a>`
                                            : `<span class="tunnel-url-disabled">${domainUrl ? this.esc(domainUrl) : ''}</span>`
                                    }
                                </div>
                    `
                            : ''
                    }

                    <!-- Info frame (always visible) -->
                    <div class="internet-info-box">
                        <p><strong class="internet-important-label">${t('admin.importantLabel')}</strong><br>
                        ${t('admin.internetInfo1')}</p>
                        <p>${this._upnpAvailable ? t('admin.upnpAvailableNote') : t('admin.upnpUnavailableNote')}</p>
                        ${this._publicIp ? `<p>${t('admin.publicIp')} <code>${this.esc(this._publicIp)}</code></p>` : ''}
                        <p>${t('admin.upnpLabel')} ${
                            this._upnpAvailable
                                ? `<span class="text-success">${t('admin.available')}</span>`
                                : `<span class="text-muted">${t('admin.notAvailable')}</span>`
                        }
                        </p>
                    </div>

                    <!-- DNS propagation indicator -->
                    ${
                        this._pendingRegistration
                            ? `
                        <div class="dns-propagating">
                            <span class="tunnel-spinner"></span>
                            <span>${t('admin.dnsPropagating')}</span>
                        </div>
                    `
                            : ''
                    }

                    <!-- Error display -->
                    ${
                        this._lastError && !this._pendingRegistration
                            ? `
                        <div class="internet-info-box internet-info-error">
                            <p>${this.esc(this._lastError)}</p>
                        </div>
                    `
                            : ''
                    }

                    <!-- Enabled but registration not live, with no specific error
                         (e.g. remote view where the reason is hidden) -->
                    ${
                        this._internetEnabled &&
                        !this._active &&
                        !this._pendingRegistration &&
                        !this._lastError
                            ? `
                        <div class="internet-info-box internet-info-error">
                            <p>${t('admin.internetNotActive')}</p>
                        </div>
                    `
                            : ''
                    }

                    <!-- Port Mapping: only shown when UPnP is NOT available -->
                    ${
                        !this._upnpAvailable
                            ? `
                        <div class="settings-field u-pt-3">
                            <label class="settings-label">${t('admin.portMapping')}</label>
                            <p class="settings-hint">
                                ${t('admin.portMappingHint')}
                                <br />${t('admin.portMappingSource')} (<strong>Any:${this._httpsPort}</strong>) &rarr;
                                ${t('admin.portMappingDest')} (<strong>${this._getLocalIpForDisplay() ? this.esc(this._getLocalIpForDisplay()) + ':' : ''}${this._httpsPort}</strong>)
                                ${this._getLocalIpForDisplay() ? '' : `<br />${t('admin.portMappingEnterIp')}`}
                                <br /><strong>${t('admin.portMappingProtocols')}</strong>.
                            </p>
                        </div>
                    `
                            : ''
                    }
                </div>

                <!-- Local Access -->
                ${
                    this._localIp
                        ? `
                <div class="settings-section">
                    <h3 class="settings-section-title">${t('admin.localAccess')}</h3>
                    <div class="settings-field u-pt-0">
                        <div class="admin-url-row">
                            <a href="${this.esc(this._buildLocalUrl())}" target="_blank" rel="noopener"
                               class="tunnel-url-link">${this.esc(this._buildLocalUrl())}</a>
                        </div>
                        <p class="settings-hint">
                            ${t('admin.localAccessHint')}
                        </p>
                    </div>
                </div>
                `
                        : ''
                }

                <!-- Server Configuration -->
                <div class="settings-section">
                    <h3 class="settings-section-title">${t('admin.serverConfig')}</h3>

                    <div class="settings-field">
                        <label class="settings-label" for="select-transport-mode">
                            ${t('admin.transportMode')}
                        </label>
                        <select id="select-transport-mode" class="settings-select">
                            ${transportOptions
                                .map(
                                    (o) =>
                                        `<option value="${o.value}" ${o.value === this._transportMode ? 'selected' : ''}>${this.esc(o.label)}</option>`,
                                )
                                .join('')}
                        </select>
                        <p class="settings-hint">
                            ${t('admin.transportModeHint')}
                        </p>
                    </div>

                    <div class="settings-field">
                        <label class="settings-label" for="admin-https-port">
                            ${t('admin.httpsPort')}
                        </label>
                        <span class="setting-desc">
                            ${t('admin.httpsPortDesc')}
                        </span>
                        <div class="u-row">
                            <input type="number" id="admin-https-port" class="settings-input u-grow"
                                   placeholder="443"
                                   value="${this.esc(String(this._httpsPort))}"
                                   min="1" max="65535" />
                            <button class="btn btn-save u-shrink-0" id="btn-admin-save" disabled>
                                ${t('admin.saveReload')}
                            </button>
                        </div>
                    </div>
                </div>

                ${this._renderSunshineSection()}
            </div>
        `;

        this._markClean();
    }

    // Sunshine install/status section (localhost only). Lets a user who skipped
    // Sunshine during setup — or whose install failed — install it from here.
    _renderSunshineSection() {
        if (!this._isLocalhost() || !this._sunshineChecked) return '';

        let body;
        if (this._sunshineInstalled) {
            body = `<p class="setting-desc setup-ok">
                        <span class="setup-ok-check">✓</span> ${t('admin.sunshineInstalled')}
                    </p>`;
        } else if (this._sunshineCanAutoInstall) {
            body = `
                <p class="setting-desc">${t('admin.sunshineNotInstalled')}</p>
                <div class="settings-field u-pt-0">
                    <label class="settings-label" for="admin-sunshine-user">${t('admin.sunshineUsername')}</label>
                    <input type="text" id="admin-sunshine-user" class="settings-input"
                           autocomplete="off" value="admin" />
                </div>
                <div class="settings-field u-pt-0">
                    <label class="settings-label" for="admin-sunshine-pass">${t('admin.sunshinePassword')}</label>
                    <input type="password" id="admin-sunshine-pass" class="settings-input"
                           autocomplete="off" />
                </div>
                <button class="btn btn-neutral u-mt-2" id="btn-install-sunshine">
                    ${t('admin.installSunshine')}
                </button>
                <p class="settings-hint">${t('admin.sunshineInstallHint')}</p>`;
        } else {
            body = `<p class="setting-desc">${t('admin.sunshineManual')}</p>`;
        }

        return `
            <div class="settings-section">
                <h3 class="settings-section-title">${t('admin.sunshine')}</h3>
                <div class="settings-field u-pt-0">${body}</div>
            </div>`;
    }

    _buildSessionsTableHtml(sessions) {
        const list = sessions || this._sessions;
        const rows = list
            .map((s) => {
                let location = this.esc(s.location || '');
                let ip = this.esc(s.ip || '');
                if (s.location === 'Local') {
                    location = t('admin.localNetwork');
                    ip = t('admin.localNetwork');
                } else if (s.city) {
                    location = this.esc(s.city) + (s.country ? ', ' + this.esc(s.country) : '');
                }
                const streamingBadge = s.streaming
                    ? `<span class="session-streaming-badge" title="${this.esc(t('admin.streamingTitle'))}">${t('admin.streaming')}</span>`
                    : '';
                return `
            <tr data-token="${this.esc(s.token)}" class="${s.streaming ? 'session-row-streaming' : ''}">
                <td><span class="session-name-edit" contenteditable="plaintext-only"
                          spellcheck="false"
                          data-token="${this.esc(s.token)}"
                          title="${this.esc(t('admin.editNameTitle'))}">${this.esc(s.machine_name || t('common.unknown'))}</span>${streamingBadge}</td>
                <td>${this._formatDate(s.created_at)}</td>
                <td>${ip}</td>
                <td>${location}</td>
                <td>
                    <button class="btn btn-danger btn-small btn-session-revoke"
                            data-token="${this.esc(s.token)}"
                            title="${this.esc(t('admin.revokeTitle'))}">
                        ${t('admin.revoke')}
                    </button>
                </td>
            </tr>`;
            })
            .join('');

        return `
            <table class="sessions-table">
                <thead>
                    <tr>
                        <th>${t('admin.colMachine')}</th>
                        <th>${t('admin.colAuthorized')}</th>
                        <th>${t('admin.colIp')}</th>
                        <th>${t('admin.colLocation')}</th>
                        <th>${t('admin.colAction')}</th>
                    </tr>
                </thead>
                <tbody>
                    ${rows}
                </tbody>
            </table>
        `;
    }

    _getPaginatedSessions() {
        const start = this._sessionPage * this._sessionsPerPage;
        return this._sessions.slice(start, start + this._sessionsPerPage);
    }

    _buildPaginationHtml() {
        const totalPages = Math.max(1, Math.ceil(this._sessions.length / this._sessionsPerPage));
        const currentPage = this._sessionPage + 1;
        return `
            <div class="sessions-pagination">
                <button class="btn btn-small btn-pagination" id="btn-page-prev"
                        ${this._sessionPage === 0 ? 'disabled' : ''}>
                    ${t('admin.prevPage')}
                </button>
                <span class="pagination-info">${t('admin.pageInfo', { current: currentPage, total: totalPages })}</span>
                <button class="btn btn-small btn-pagination" id="btn-page-next"
                        ${this._sessionPage >= totalPages - 1 ? 'disabled' : ''}>
                    ${t('admin.nextPage')}
                </button>
            </div>
        `;
    }

    _buildSessionsAreaHtml() {
        if (this._sessions.length === 0) {
            return `<p class="settings-hint">${t('admin.noActiveSessions')}</p>`;
        }
        const pageSessions = this._getPaginatedSessions();
        return this._buildSessionsTableHtml(pageSessions) + this._buildPaginationHtml();
    }

    _renderSessionsTable() {
        const container = this.container.querySelector('#admin-sessions-table');
        if (!container) return;

        // Clamp page if needed after data refresh
        const totalPages = Math.max(1, Math.ceil(this._sessions.length / this._sessionsPerPage));
        if (this._sessionPage >= totalPages) {
            this._sessionPage = totalPages - 1;
        }

        container.innerHTML = this._buildSessionsAreaHtml();

        // Re-bind revoke buttons
        container.querySelectorAll('.btn-session-revoke').forEach((btn) => {
            btn.addEventListener('click', () => this._revokeSession(btn.dataset.token));
        });

        // Re-bind editable session names
        this._bindSessionNameEdits(container);

        // Re-bind pagination buttons
        const prevBtn = container.querySelector('#btn-page-prev');
        const nextBtn = container.querySelector('#btn-page-next');
        if (prevBtn) {
            prevBtn.addEventListener('click', () => {
                if (this._sessionPage > 0) {
                    this._sessionPage--;
                    this._renderSessionsTable();
                }
            });
        }
        if (nextBtn) {
            nextBtn.addEventListener('click', () => {
                const maxPage = Math.ceil(this._sessions.length / this._sessionsPerPage) - 1;
                if (this._sessionPage < maxPage) {
                    this._sessionPage++;
                    this._renderSessionsTable();
                }
            });
        }
    }

    _formatDate(timestamp) {
        if (!timestamp) return '-';
        const d = new Date(timestamp * 1000);
        const pad = (n) => String(n).padStart(2, '0');
        return `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())} ${pad(d.getHours())}:${pad(d.getMinutes())}`;
    }

    bindEvents() {
        // Internet Access checkbox toggle
        const internetChk = this.container.querySelector('#chk-internet-enable');
        if (internetChk) {
            internetChk.addEventListener('change', async () => {
                if (internetChk.checked) {
                    await this._enableInternet();
                } else {
                    await this._disableInternet();
                }
            });
        }

        // Transport mode change
        const transportSelect = this.container.querySelector('#select-transport-mode');
        if (transportSelect) {
            transportSelect.addEventListener('change', () => {
                this._saveInternetPrefs();
            });
        }

        // Port field dirty tracking
        const portInput = this.container.querySelector('#admin-https-port');
        if (portInput) {
            portInput.addEventListener('input', () => this._onFieldChange());
            portInput.addEventListener('change', () => this._onFieldChange());
            portInput.addEventListener('keydown', (e) => {
                if (e.key === 'Enter') {
                    e.preventDefault();
                    const saveBtn = this.container.querySelector('#btn-admin-save');
                    if (saveBtn && !saveBtn.disabled) saveBtn.click();
                }
            });
        }

        // Save Settings button
        const saveBtn = this.container.querySelector('#btn-admin-save');
        if (saveBtn) {
            saveBtn.addEventListener('click', async () => {
                const newPort = parseInt(portInput.value, 10);
                if (isNaN(newPort) || newPort < 1 || newPort > 65535) {
                    Toast.warning(t('admin.portRange'));
                    return;
                }

                saveBtn.disabled = true;
                saveBtn.classList.add('btn-loading');
                saveBtn.textContent = t('admin.saving');

                try {
                    const result = await BackendClient.saveAdminSettings({ https_port: newPort });
                    if (result.status === 'saved') {
                        Toast.success(t('admin.portChanged', { port: result.https_port }));

                        if (result.port_changed) {
                            const newUrl = new URL(window.location.href);
                            newUrl.port = String(result.https_port);
                            setTimeout(() => {
                                window.location.href = newUrl.toString();
                            }, 300);
                            return;
                        }

                        await this._loadState();
                        portInput.value = String(this._httpsPort);
                        this._markClean();
                    } else if (result.status === 'partial') {
                        Toast.success(t('admin.portSavedRestart'));
                    }
                } catch (err) {
                    console.error('[Admin] Failed to save settings:', err);
                    Toast.error(t('admin.saveFailed', { message: err.message }));
                } finally {
                    saveBtn.classList.remove('btn-loading');
                    saveBtn.textContent = t('admin.saveReload');
                    this._updateSaveButton();
                }
            });
        }

        // Copy PIN button (only copies a real PIN, not "--------")
        const copyBtn = this.container.querySelector('#btn-copy-pin');
        if (copyBtn) {
            copyBtn.addEventListener('click', async () => {
                if (this._pin === '--------' || !this._pin) {
                    Toast.warning(t('admin.noPinToCopy'));
                    return;
                }
                try {
                    await navigator.clipboard.writeText(this._pin);
                    Toast.success(t('admin.pinCopied'));
                    copyBtn.textContent = t('common.copied');
                    setTimeout(() => {
                        copyBtn.textContent = t('common.copy');
                    }, 2000);
                } catch (err) {
                    console.warn('[Admin] Clipboard write failed:', err);
                    // Fallback: select the PIN text
                    const pinDisplay = this.container.querySelector('#admin-pin-display');
                    if (pinDisplay) {
                        const range = document.createRange();
                        range.selectNodeContents(pinDisplay);
                        const sel = window.getSelection();
                        sel.removeAllRanges();
                        sel.addRange(range);
                        Toast.info(t('admin.pinSelected'));
                    }
                }
            });
        }

        // Generate PIN button (does NOT revoke existing sessions)
        const regenBtn = this.container.querySelector('#btn-regenerate-pin');
        if (regenBtn) {
            regenBtn.addEventListener('click', async () => {
                regenBtn.disabled = true;
                regenBtn.textContent = t('admin.generating');
                try {
                    const result = await BackendClient.generatePin();
                    this._pin = result.pin;
                    this._pinConsumed = false; // fresh PIN
                    this.render();
                    this.bindEvents();
                    Toast.success(t('admin.newPin', { pin: result.pin }));
                } catch (err) {
                    console.error('[Admin] Failed to generate PIN:', err);
                    Toast.error(t('admin.generateFailed', { message: err.message }));
                    regenBtn.disabled = false;
                    regenBtn.textContent = t('common.generate');
                }
            });
        }

        // Clear PIN button
        const clearBtn = this.container.querySelector('#btn-clear-pin');
        if (clearBtn) {
            clearBtn.addEventListener('click', async () => {
                clearBtn.disabled = true;
                clearBtn.textContent = t('admin.clearing');
                try {
                    const result = await BackendClient.clearPin();
                    this._pin = result.pin || '------';
                    this._pinConsumed = false;
                    this.render();
                    this.bindEvents();
                    Toast.info(t('admin.pinCleared'));
                } catch (err) {
                    console.error('[Admin] Failed to clear PIN:', err);
                    Toast.error(t('admin.clearFailed', { message: err.message }));
                    clearBtn.disabled = false;
                    clearBtn.textContent = t('common.clear');
                }
            });
        }

        // ── Certificate Authentication ─────────────────────────────────────

        // Certificate auth checkbox toggle
        const certChk = this.container.querySelector('#chk-cert-auth');
        if (certChk) {
            certChk.addEventListener('change', async () => {
                const enabled = certChk.checked;
                try {
                    await BackendClient.saveAdminSettings({ cert_auth_enabled: enabled });
                    this._certAuthEnabled = enabled;
                    const area = this.container.querySelector('#cert-download-area');
                    if (area) {
                        area.style.display = enabled ? '' : 'none';
                    }
                    Toast.success(
                        enabled ? t('admin.certAuthEnabled') : t('admin.certAuthDisabled'),
                    );
                } catch (err) {
                    console.error('[Admin] Failed to save cert auth setting:', err);
                    Toast.error(t('admin.saveFailed', { message: err.message }));
                    certChk.checked = !enabled; // revert
                }
            });
        }

        // Download certificate button
        const downloadBtn = this.container.querySelector('#btn-download-cert');
        if (downloadBtn) {
            downloadBtn.addEventListener('click', async () => {
                downloadBtn.disabled = true;
                downloadBtn.textContent = t('admin.downloading');
                try {
                    const content = await BackendClient.downloadCertificate();
                    // Trigger file download via a temporary anchor
                    const blob = new Blob([content], { type: 'text/plain;charset=utf-8' });
                    const url = URL.createObjectURL(blob);
                    const a = document.createElement('a');
                    a.href = url;
                    a.download = 'moonlightweb-certificate.txt';
                    document.body.appendChild(a);
                    a.click();
                    document.body.removeChild(a);
                    URL.revokeObjectURL(url);
                    Toast.success(t('admin.certDownloaded'));
                } catch (err) {
                    console.error('[Admin] Failed to download certificate:', err);
                    Toast.error(t('admin.downloadFailed', { message: err.message }));
                } finally {
                    downloadBtn.disabled = false;
                    downloadBtn.textContent = t('admin.downloadCert');
                }
            });
        }

        // Regenerate certificate button
        const regenCertBtn = this.container.querySelector('#btn-regenerate-cert');
        if (regenCertBtn) {
            regenCertBtn.addEventListener('click', async () => {
                if (!confirm(t('admin.confirmRegenCert'))) {
                    return;
                }
                regenCertBtn.disabled = true;
                regenCertBtn.textContent = t('admin.regenerating');
                try {
                    await BackendClient.regenerateCertificate();
                    Toast.success(t('admin.certRegenerated'));
                    if (this._certAuthEnabled) {
                        // Re-enable the checkbox to trigger UI update
                        const chk = this.container.querySelector('#chk-cert-auth');
                        if (chk) chk.checked = true;
                    }
                } catch (err) {
                    console.error('[Admin] Failed to regenerate certificate:', err);
                    Toast.error(t('admin.regenerateFailed', { message: err.message }));
                } finally {
                    regenCertBtn.disabled = false;
                    regenCertBtn.textContent = t('admin.regenerateCert');
                }
            });
        }

        // Pagination buttons (must be bound immediately after render, not only in _renderSessionsTable)
        const prevBtn = this.container.querySelector('#btn-page-prev');
        const nextBtn = this.container.querySelector('#btn-page-next');
        if (prevBtn) {
            prevBtn.addEventListener('click', () => {
                if (this._sessionPage > 0) {
                    this._sessionPage--;
                    this._renderSessionsTable();
                }
            });
        }
        if (nextBtn) {
            nextBtn.addEventListener('click', () => {
                const maxPage = Math.ceil(this._sessions.length / this._sessionsPerPage) - 1;
                if (this._sessionPage < maxPage) {
                    this._sessionPage++;
                    this._renderSessionsTable();
                }
            });
        }

        // Revoke session buttons
        this.container.querySelectorAll('.btn-session-revoke').forEach((btn) => {
            btn.addEventListener('click', () => this._revokeSession(btn.dataset.token));
        });

        // Editable session names
        this._bindSessionNameEdits(this.container);

        // Install Sunshine button (localhost only)
        const installSunBtn = this.container.querySelector('#btn-install-sunshine');
        if (installSunBtn) {
            installSunBtn.addEventListener('click', () => this._installSunshine());
        }

        // Close button
        const closeBtn = this.container.querySelector('#btn-admin-close');
        if (closeBtn) {
            closeBtn.addEventListener('click', () => {
                if (this._dirty) {
                    Toast.info(t('admin.changesDiscarded'));
                }
                this.onClose();
            });
        }
    }

    async _revokeSession(token) {
        if (!token) return;

        const btn = this.container.querySelector(
            `.btn-session-revoke[data-token="${CSS.escape(token)}"]`,
        );
        if (btn) {
            btn.disabled = true;
            btn.textContent = t('admin.revoking');
        }

        try {
            await BackendClient.revokeSession(token);
            Toast.success(t('admin.sessionRevoked'));

            // Remove from local list and re-render
            this._sessions = this._sessions.filter((s) => s.token !== token);
            this._activeSessions = this._sessions.length;
            this._renderSessionsTable();

            // Update session count in Active Sessions section
            const sessionCount = this.container.querySelector('#admin-session-count');
            if (sessionCount) {
                sessionCount.textContent =
                    this._activeSessions > 0
                        ? t('admin.sessionCount', { count: this._activeSessions })
                        : t('admin.noActiveSessions');
            }
        } catch (err) {
            console.error('[Admin] Failed to revoke session:', err);
            Toast.error(t('admin.revokeFailed', { message: err.message }));
            if (btn) {
                btn.disabled = false;
                btn.textContent = t('admin.revoke');
            }
        }
    }

    // --- Editable session names ---

    _bindSessionNameEdits(root) {
        root.querySelectorAll('.session-name-edit').forEach((el) => {
            el.dataset.original = el.textContent.trim();
            el.addEventListener('focus', () => {
                el.dataset.original = el.textContent.trim();
            });
            el.addEventListener('keydown', (e) => {
                if (e.key === 'Enter') {
                    e.preventDefault();
                    el.blur();
                } else if (e.key === 'Escape') {
                    e.preventDefault();
                    el.textContent = el.dataset.original || '';
                    el.blur();
                }
            });
            el.addEventListener('blur', () => this._commitSessionName(el));
        });
    }

    async _commitSessionName(el) {
        const token = el.dataset.token;
        const original = (el.dataset.original || '').trim();
        let name = el.textContent.trim();

        // Empty or unchanged: revert to the stored value, no request.
        if (!name) {
            el.textContent = original;
            return;
        }
        if (name === original) {
            el.textContent = name;
            return;
        }
        if (name.length > 64) name = name.slice(0, 64);

        try {
            const result = await BackendClient.renameSession(token, name);
            const finalName = result.machine_name || name;
            el.textContent = finalName;
            el.dataset.original = finalName;
            const s = this._sessions.find((s) => s.token === token);
            if (s) s.machine_name = finalName;
            Toast.success(t('admin.sessionRenamed'));
        } catch (err) {
            console.error('[Admin] Failed to rename session:', err);
            Toast.error(t('admin.renameFailed', { message: err.message }));
            el.textContent = original; // revert on failure
        }
    }

    // Internet Access enable / disable

    async _enableInternet() {
        // Show the activation loader immediately; the enable request blocks on the
        // backend while it runs through its steps, so we poll status.phase in
        // parallel to surface live progress in the loader.
        this._activating = true;
        this._phase = 'starting';
        this._internetEnabled = true;
        this._lastError = '';
        this.render();
        this.bindEvents();
        this._startPhasePolling();
        try {
            const result = await BackendClient.enableInternet({
                internet_access_enabled: true,
                auto_ip_detection: true,
                transport_mode: this._transportMode,
            });
            this._stopPhasePolling();
            this._activating = false;
            if (result.status === 'enabled') {
                Toast.success(t('admin.internetEnabled', { domain: result.domain || '...' }));
                await this._loadInternetState();
                this.render();
                this.bindEvents();
                if (this._pendingRegistration) {
                    this._startDnsPolling();
                }
            } else {
                Toast.error(t('admin.internetEnableFailed'));
                this._internetEnabled = false;
                await this._loadInternetState();
                this.render();
                this.bindEvents();
            }
        } catch (err) {
            this._stopPhasePolling();
            this._activating = false;
            console.error('[Admin] Failed to enable Internet Access:', err);
            Toast.error(t('admin.enableFailed', { message: err.message }));
            this._internetEnabled = false;
            this.render();
            this.bindEvents();
        }
    }

    // Poll status.phase while the (blocking) enable request is in flight so the
    // activation loader reflects the backend's current step.
    _startPhasePolling() {
        this._stopPhasePolling();
        this._phasePollTimer = setInterval(async () => {
            try {
                const status = await BackendClient.getInternetStatus();
                const phase = status.phase || '';
                if (phase && phase !== this._phase) {
                    this._phase = phase;
                    const loader = this.container.querySelector('#internet-activation');
                    if (loader) loader.innerHTML = this._renderActivationSteps();
                }
                // Terminal phase (success, give-up or waiting): close the loader and
                // refresh the full view. Needed when activation was triggered by
                // provisioning (no awaited enable call to finalize). An empty phase
                // is treated as "not reported yet", never as terminal.
                if (phase === 'active' || phase === 'error' || phase === 'pending') {
                    this._stopPhasePolling();
                    this._activating = false;
                    await this._loadInternetState();
                    this.render();
                    this.bindEvents();
                    if (this._pendingRegistration) this._startDnsPolling();
                }
            } catch (_err) {
                // Transient during activation (backend busy) — ignore and retry.
            }
        }, 700);
    }

    _stopPhasePolling() {
        if (this._phasePollTimer) {
            clearInterval(this._phasePollTimer);
            this._phasePollTimer = null;
        }
    }

    async _disableInternet() {
        try {
            await BackendClient.disableInternet();
            this._internetEnabled = false;
            this._stopDnsPolling();
            Toast.success(t('admin.internetDisabled'));
            await this._loadInternetState();
            this.render();
            this.bindEvents();
        } catch (err) {
            console.error('[Admin] Failed to disable Internet Access:', err);
            Toast.error(t('admin.disableFailed', { message: err.message }));
            const chk = this.container.querySelector('#chk-internet-enable');
            if (chk) chk.checked = true;
        }
    }

    async _saveInternetPrefs() {
        const transportSelect = this.container.querySelector('#select-transport-mode');
        const newMode = transportSelect ? transportSelect.value : this._transportMode;

        const prefs = {
            internet_access_enabled: this._internetEnabled,
            transport_mode: newMode,
        };

        try {
            await BackendClient.enableInternet(prefs);
            this._transportMode = newMode;
            Toast.success(t('admin.transportSaved', { mode: newMode }));
        } catch (err) {
            console.warn('[Admin] Failed to save transport prefs:', err);
            Toast.error(t('admin.transportSaveFailed'));
        }
    }

    // --- Helpers ---

    // Build the activation step checklist for the loader. Marks steps before the
    // current phase as done, the current one as spinning, the rest as pending.
    _renderActivationSteps() {
        const phases = [
            ['detecting_ip', t('admin.phaseDetectingIp')],
            ['registering_dns', t('admin.phaseRegisteringDns')],
            ['checking_dns', t('admin.phaseCheckingDns')],
            ['issuing_certificate', t('admin.phaseIssuingCert')],
            ['configuring_ports', t('admin.phaseConfiguringPorts')],
        ];
        const order = phases.map((p) => p[0]);
        const done = this._phase === 'active';
        const cur = order.indexOf(this._phase);
        const items = phases
            .map(([, label], i) => {
                let cls = 'step-pending';
                let marker = '<span class="step-dot">○</span>';
                if (done || (cur >= 0 && i < cur)) {
                    cls = 'step-done';
                    marker = '<span class="step-check">✓</span>';
                } else if (i === cur) {
                    cls = 'step-active';
                    marker = '<span class="tunnel-spinner"></span>';
                }
                return `<li class="${cls}">${marker}<span class="step-label">${this.esc(label)}</span></li>`;
            })
            .join('');
        return `
            <div class="internet-activation-title">
                <span class="tunnel-spinner"></span>${this.esc(t('admin.activating'))}
            </div>
            <ul class="activation-steps">${items}</ul>`;
    }

    // Format a 6-char PIN as two groups of 3 for readability ("123 456",
    // "--- ---"). Any other length is returned unchanged.
    _formatPin(value) {
        const s = String(value || '');
        return s.length === 6 ? s.slice(0, 3) + ' ' + s.slice(3) : s;
    }

    esc(text) {
        return escapeHtml(text);
    }
}
