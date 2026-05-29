/**
 * Moonlight-Web — Server Settings
 *
 * Server administration functions (localhost only):
 *   - Internet Access (Azure DNS) with DNS propagation check
 *   - HTTPS port configuration
 *   - Transport mode
 *
 * All settings stored server-side. Unsaved changes are discarded on close.
 */
import { BackendClient } from '../api/BackendClient.js';
import { Toast } from './Toast.js';

export class AdminView {
    constructor(container, onClose) {
        this.container = container;
        this.onClose = onClose || (() => {});

        // Server settings state
        this._httpsPort = 443;
        this._httpPort = 80;

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
        this._lastError = '';

        // DNS propagation polling
        this._dnsPollTimer = null;
        this._dnsPollAttempts = 0;
        this._maxDnsPollAttempts = 60; // 5 min at 5s interval

        // Dirty tracking: snapshot of values at load time
        this._cleanState = {};
        this._dirty = false;
    }

    async start() {
        await this._loadState();
        await this._loadInternetState();
        this.render();
        this.bindEvents();
        this._startDnsPollingIfNeeded();
    }

    async _loadState() {
        try {
            const admin = await BackendClient.getAdminSettings();
            this._httpsPort = admin.https_port || 443;
            this._httpPort = admin.http_port || 80;
        } catch (err) {
            console.warn('[Admin] Failed to load server settings:', err);
        }
    }

    async _loadInternetState() {
        try {
            const status = await BackendClient.getInternetStatus();
            this._internetEnabled = status.internet_access_enabled || false;
            this._domain = status.domain || '';
            this._publicIp = status.public_ip || '';
            this._localIp = status.local_ip || '';
            this._uniqueId = status.unique_id || '';
            this._transportMode = status.transport_mode || 'auto';
            this._availableTransports = status.available_transports || [];
            this._upnpAvailable = status.upnp_available || false;
            this._pendingRegistration = status.pending_registration || false;
            this._lastError = status.last_error || '';
            this._httpsPort = status.https_port || this._httpsPort;
        } catch (err) {
            console.warn('[Admin] Failed to load internet status:', err);
        }
    }

    destroy() {
        this._stopDnsPolling();
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
                this._lastError = 'DNS propagation timed out after 5 minutes. Check your domain configuration.';
                this.render();
                this.bindEvents();
                return;
            }
            try {
                const status = await BackendClient.getInternetStatus();
                if (!status.pending_registration && status.domain) {
                    // DNS propagated successfully
                    this._stopDnsPolling();
                    this._internetEnabled = true;
                    this._domain = status.domain || this._domain;
                    this._publicIp = status.public_ip || this._publicIp;
                    this._pendingRegistration = false;
                    this._lastError = '';
                    this.render();
                    this.bindEvents();
                    Toast.success('DNS propagated — your site is now available at ' + this._domain);
                } else if (status.last_error && status.last_error !== this._lastError) {
                    this._lastError = status.last_error;
                    this._pendingRegistration = status.pending_registration !== false;
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
            httpsPort: portInput ? parseInt(portInput.value, 10) : this._httpsPort
        };
        this._dirty = false;
        this._updateSaveButton();
    }

    _onFieldChange() {
        const portInput = this.container.querySelector('#admin-https-port');
        if (!portInput) return;

        const currentPort = parseInt(portInput.value, 10);
        this._dirty = (currentPort !== this._cleanState.httpsPort);
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
        if (this._httpsPort !== 443) {
            return prefix + ':' + this._httpsPort;
        }
        return prefix;
    }

    _getLocalIpForDisplay() {
        // Only reveal the server LAN IP when accessing from localhost.
        // Remote LAN clients already know the IP (they typed it in the URL).
        const hostname = window.location.hostname;
        const isLocalhost = hostname === 'localhost' || hostname === '127.0.0.1' || hostname === '::1';
        if (!isLocalhost) return '';

        // Use the backend-discovered LAN IP (from GetAdaptersAddresses / UPnP).
        if (this._localIp) return this._localIp;

        // On localhost the backend should have provided the IP via getLocalIP().
        // If not, show a generic placeholder.
        return '192.168.?.?';
    }

    render() {
        const transportLabels = {
            'webrtc-media-udp': 'WebRTC MediaTrack (UDP)',
            'webrtc-dc-udp':    'WebRTC DataChannel (UDP)',
            'webrtc-media-tcp': 'WebRTC MediaTrack (TCP)',
            'webrtc-dc-tcp':    'WebRTC DataChannel (TCP)',
            'wss':              'WSS (WebSocket Secure)'
        };
        const transportOptions = [
            { value: 'auto', label: 'Auto (prefer UDP)' },
            ...this._availableTransports.map(t => ({
                value: t,
                label: transportLabels[t] || t
            }))
        ];

        const domainUrl = this._buildDomainUrl();
        const showDomain = this._internetEnabled || !!this._domain;

        this.container.innerHTML = `
            <div class="admin-view" id="view-admin">
                <div class="admin-header">
                    <h2>Server Settings</h2>
                    <button class="view-close-btn" id="btn-admin-close"
                            title="Close (discards unsaved changes)">&times;</button>
                </div>

                <!-- Internet -->
                <div class="settings-section">
                    <h3 class="settings-section-title">Internet</h3>

                    <div class="settings-field">
                        <label class="settings-checkbox-label">
                            <input type="checkbox" id="chk-internet-enable"
                                   ${this._internetEnabled ? 'checked' : ''} />
                            <span class="settings-checkbox-text">Enable Internet Access</span>
                        </label>
                    </div>

                    ${showDomain ? `
                                <div style="padding:6px 0;font-family:monospace;">
                                    ${this._internetEnabled && !this._pendingRegistration && domainUrl
                                        ? `<a href="${this.esc(domainUrl)}" target="_blank" rel="noopener" class="tunnel-url-link">${this.esc(domainUrl)}</a>`
                                        : `<span class="tunnel-url-disabled">${domainUrl ? this.esc(domainUrl) : ''}</span>`
                                    }
                                </div>
                    ` : ''}

                    <!-- Info frame (always visible) -->
                    <div class="internet-info-box">
                        <p><strong class="internet-important-label">Important:</strong><br>
                        By enabling Internet access, you authorize sending this server's
                        <strong>public IP address</strong> to Azure DNS to create an A record
                        pointing to your network.</p>
                        <p>Authorizes <strong>UPnP</strong> port mapping on your router
                        ${this._upnpAvailable ? '(available on this network).' : '(not available on this network).'}
                        You can also forward ports manually.</p>
                        ${this._publicIp ? `<p>Public IP: <code>${this.esc(this._publicIp)}</code></p>` : ''}
                        <p>UPnP: ${this._upnpAvailable
                            ? '<span class="text-success">Available</span>'
                            : '<span class="text-muted">Not available</span>'}
                        </p>
                    </div>

                    <!-- DNS propagation indicator -->
                    ${this._pendingRegistration ? `
                        <div class="dns-propagating">
                            <span class="tunnel-spinner"></span>
                            <span>DNS propagation in progress &mdash; your site will be
                            available shortly. This may take a few minutes...</span>
                        </div>
                    ` : ''}

                    <!-- Error display -->
                    ${this._lastError && !this._pendingRegistration ? `
                        <div class="internet-info-box internet-info-error">
                            <p>${this.esc(this._lastError)}</p>
                        </div>
                    ` : ''}

                    <!-- Port Mapping: only shown when UPnP is NOT available -->
                    ${!this._upnpAvailable ? `
                        <div class="settings-field" style="padding-top:12px;">
                            <label class="settings-label">Port Mapping</label>
                            <p class="settings-hint">
                                UPnP is not available on your router. To allow external access,
                                manually forward ports in your router settings:
                                <br />Source (<strong>Any:${this._httpsPort}</strong>) &rarr;
                                Destination (<strong>${this._getLocalIpForDisplay() ? this.esc(this._getLocalIpForDisplay()) + ':' : ''}${this._httpsPort}</strong>)
                                ${this._getLocalIpForDisplay() ? '' : `<br />Enter the LAN IP address of this server (e.g. 192.168.1.x) as the destination.`}
                                <br /><strong>TCP and UDP</strong>.
                            </p>
                        </div>
                    ` : ''}
                </div>

                <!-- Server Configuration -->
                <div class="settings-section">
                    <h3 class="settings-section-title">Server Configuration</h3>

                    <div class="settings-field">
                        <label class="settings-label" for="select-transport-mode">
                            Transport Mode
                        </label>
                        <select id="select-transport-mode" class="settings-select">
                            ${transportOptions.map(o =>
                                `<option value="${o.value}" ${o.value === this._transportMode ? 'selected' : ''}>${this.esc(o.label)}</option>`
                            ).join('')}
                        </select>
                        <p class="settings-hint">
                            When set to <strong>Auto</strong>, the system tries the best
                            available protocol (UDP &gt; TCP &gt; WSS) based on network
                            conditions and browser support.
                        </p>
                    </div>

                    <div class="settings-field">
                        <label class="settings-label" for="admin-https-port">
                            HTTPS Port
                        </label>
                        <span class="setting-desc">
                            Configure the server HTTPS port. Changes require a brief
                            service interruption while the port is rebound.
                        </span>
                        <div style="display: flex; gap: 8px; align-items: center;">
                            <input type="number" id="admin-https-port" class="settings-input"
                                   placeholder="443"
                                   value="${this.esc(String(this._httpsPort))}"
                                   min="1" max="65535"
                                   style="flex: 1;" />
                            <button class="btn btn-save" id="btn-admin-save" disabled
                                    style="flex-shrink: 0;">
                                Save &amp; Reload
                            </button>
                        </div>
                    </div>
                </div>
            </div>
        `;

        this._markClean();
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
                    Toast.warning('Port must be between 1 and 65535');
                    return;
                }

                saveBtn.disabled = true;
                saveBtn.classList.add('btn-loading');
                saveBtn.textContent = 'Saving...';

                try {
                    const result = await BackendClient.saveAdminSettings({ https_port: newPort });
                    if (result.status === 'saved') {
                        Toast.success('Port changed to ' + result.https_port);

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
                        Toast.success('Port saved to settings. Restart required to apply.');
                    }
                } catch (err) {
                    console.error('[Admin] Failed to save settings:', err);
                    Toast.error('Failed to save: ' + err.message);
                } finally {
                    saveBtn.classList.remove('btn-loading');
                    saveBtn.textContent = 'Save & Reload';
                    this._updateSaveButton();
                }
            });
        }

        // Close button
        const closeBtn = this.container.querySelector('#btn-admin-close');
        if (closeBtn) {
            closeBtn.addEventListener('click', () => {
                if (this._dirty) {
                    Toast.info('Settings changes discarded');
                }
                this.onClose();
            });
        }
    }

    // Internet Access enable / disable

    async _enableInternet() {
        try {
            const result = await BackendClient.enableInternet({
                internet_access_enabled: true,
                auto_ip_detection: true,
                transport_mode: this._transportMode
            });
            if (result.status === 'enabled') {
                Toast.success('Internet Access enabled — domain: ' + (result.domain || '...'));
                await this._loadInternetState();
                this.render();
                this.bindEvents();
                if (this._pendingRegistration) {
                    this._startDnsPolling();
                }
            } else {
                Toast.error('Failed to enable Internet Access');
                const chk = this.container.querySelector('#chk-internet-enable');
                if (chk) chk.checked = false;
            }
        } catch (err) {
            console.error('[Admin] Failed to enable Internet Access:', err);
            Toast.error('Failed to enable: ' + err.message);
            const chk = this.container.querySelector('#chk-internet-enable');
            if (chk) chk.checked = false;
        }
    }

    async _disableInternet() {
        try {
            await BackendClient.disableInternet();
            this._internetEnabled = false;
            this._stopDnsPolling();
            Toast.success('Internet Access disabled');
            await this._loadInternetState();
            this.render();
            this.bindEvents();
        } catch (err) {
            console.error('[Admin] Failed to disable Internet Access:', err);
            Toast.error('Failed to disable: ' + err.message);
            const chk = this.container.querySelector('#chk-internet-enable');
            if (chk) chk.checked = true;
        }
    }

    async _saveInternetPrefs() {
        const transportSelect = this.container.querySelector('#select-transport-mode');
        const newMode = transportSelect ? transportSelect.value : this._transportMode;

        const prefs = {
            internet_access_enabled: this._internetEnabled,
            transport_mode: newMode
        };

        try {
            await BackendClient.enableInternet(prefs);
            this._transportMode = newMode;
            Toast.success('Transport mode saved: ' + newMode);
        } catch (err) {
            console.warn('[Admin] Failed to save transport prefs:', err);
            Toast.error('Failed to save transport mode');
        }
    }

    // --- Helpers ---

    esc(text) {
        const div = document.createElement('div');
        div.textContent = text;
        return div.innerHTML;
    }
}
