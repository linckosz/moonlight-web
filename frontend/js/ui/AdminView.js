/**
 * Moonlight-Web — Admin Control Panel
 *
 * Server administration functions (localhost only):
 *   - DuckDNS dynamic DNS configuration (token + GDPR consent)
 *   - HTTPS port configuration
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
        this._httpPort = 48000;

        // DuckDNS state
        this._ddnsConsent = false;
        this._ddnsToken = '';
        this._ddnsActive = false;
        this._ddnsSubdomain = '';
        this._tokenConfigured = false;

        // Dirty tracking: snapshot of values at load time
        this._cleanState = {};
        this._dirty = false;
    }

    async start() {
        await this._loadState();
        this.render();
        this.bindEvents();
    }

    async _loadState() {
        try {
            const admin = await BackendClient.getAdminSettings();
            this._httpsPort = admin.https_port || 443;
            this._httpPort = admin.http_port || 48000;
        } catch (err) {
            console.warn('[Admin] Failed to load server settings:', err);
        }

        try {
            const ddns = await BackendClient.getDdnsConsent();
            this._ddnsConsent = ddns.consent_granted || false;
            this._ddnsActive = ddns.active || false;
            this._ddnsSubdomain = ddns.subdomain || '';
            this._tokenConfigured = ddns.token_configured || false;
            this._ddnsToken = ''; // never expose token to UI for security
        } catch (err) {
            console.warn('[Admin] Failed to load DuckDNS state:', err);
        }
    }

    destroy() {
        // Cleanup if needed
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

    render() {
        this.container.innerHTML = `
            <div class="admin-view" id="view-admin">
                <div class="admin-header">
                    <h2>Admin Control Panel</h2>
                    <button class="view-close-btn" id="btn-admin-close"
                            title="Close (discards unsaved changes)">&times;</button>
                </div>

                <!-- Server Settings -->
                <div class="settings-section">
                    <h3 class="settings-section-title">Server Configuration</h3>
                    <p class="settings-section-desc">
                        Configure the server HTTPS port. Changes require a brief
                        service interruption while the port is rebound.
                    </p>

                    <div class="settings-field">
                        <label class="settings-label" for="admin-https-port">
                            HTTPS Port
                        </label>
                        <input type="number" id="admin-https-port" class="settings-input"
                               placeholder="443"
                               value="${this.esc(String(this._httpsPort))}"
                               min="1" max="65535" />
                        <p class="settings-hint">
                            Current HTTP redirect port: <strong>${this._httpPort}</strong>.
                            Changing this value takes effect immediately.
                        </p>
                    </div>

                    <div class="settings-actions">
                        <button class="btn btn-save" id="btn-admin-save" disabled>
                            Save Changes
                        </button>
                    </div>
                </div>

                <!-- DuckDNS -->
                <div class="settings-section">
                    <h3 class="settings-section-title">DuckDNS Dynamic DNS</h3>
                    <p class="settings-section-desc">
                        DuckDNS provides a free dynamic DNS service so your Moonlight-Web
                        server can be reached via a public hostname (e.g.
                        <code>moonlightweb-xxxx.duckdns.org</code>).
                    </p>

                    <div class="settings-field">
                        <label class="settings-label" for="admin-ddns-token">
                            DuckDNS Token
                        </label>
                        <input type="text" id="admin-ddns-token" class="settings-input"
                               placeholder="Enter your DuckDNS token"
                               value="${this.esc(this._ddnsToken)}"
                               ${this._tokenConfigured ? 'readonly' : ''} />
                        <p class="settings-hint">
                            Get your token from
                            <a href="https://www.duckdns.org" target="_blank" rel="noopener">
                                duckdns.org
                            </a>.
                        </p>
                    </div>

                    <div class="settings-field">
                        <label class="settings-checkbox-label">
                            <input type="checkbox" id="admin-ddns-consent"
                                   ${this._ddnsConsent ? 'checked' : ''} />
                            <span class="settings-checkbox-text">
                                I authorize Moonlight-Web to create a DuckDNS subdomain
                                and periodically update its public IP address.
                            </span>
                        </label>
                        <p class="settings-note">
                            <em>GDPR notice:</em> This setting is stored server-side only
                            (local settings.json). No data is transmitted to third parties
                            beyond DuckDNS for the sole purpose of dynamic DNS updates.
                            Moonlight-Web does not collect or store any personal data.
                        </p>
                        <p class="settings-note">
                            By enabling this feature, your server's public IP address will be
                            shared with DuckDNS (duckdns.org) and updated every 5 minutes.
                            No personal data is collected or stored. Your DuckDNS token is
                            stored locally on this server only &mdash; never transmitted to third
                            parties.
                        </p>
                    </div>

                    <div class="settings-actions">
                        <button class="btn btn-save" id="btn-admin-save-token"
                                ${this._tokenConfigured ? 'disabled' : ''}>
                            ${this._tokenConfigured ? 'Token Saved' : 'Save Token'}
                        </button>
                    </div>

                    ${this._ddnsActive && this._ddnsSubdomain
                        ? `<div class="settings-status settings-status-ok">
                               Public URL: <a href="https://${this.esc(this._ddnsSubdomain)}.duckdns.org"
                                  target="_blank" rel="noopener">
                                  ${this.esc(this._ddnsSubdomain)}.duckdns.org</a>
                           </div>`
                        : this._ddnsConsent && !this._tokenConfigured
                            ? `<div class="settings-status settings-status-pending">
                                   Consent given. Enter your DuckDNS token above to activate.
                               </div>`
                            : ''
                    }
                </div>
            </div>
        `;

        // Snapshot clean state after render
        this._markClean();
    }

    bindEvents() {
        // ── Port field dirty tracking ──────────────────────────────────────────
        const portInput = this.container.querySelector('#admin-https-port');
        if (portInput) {
            portInput.addEventListener('input', () => this._onFieldChange());
            portInput.addEventListener('change', () => this._onFieldChange());
        }

        // ── Save Settings button ──────────────────────────────────────────────
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
                            // Redirect browser to the new HTTPS port.
                            // The backend changed the port asynchronously.
                            const newUrl = new URL(window.location.href);
                            newUrl.port = String(result.https_port);
                            // Brief delay to let the toast appear before reload
                            setTimeout(() => {
                                window.location.href = newUrl.toString();
                            }, 300);
                            return;
                        }

                        // Reload state to reflect actual bound port
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
                    saveBtn.textContent = 'Save Changes';
                    this._updateSaveButton();
                }
            });
        }

        // ── Close button ──────────────────────────────────────────────────────
        const closeBtn = this.container.querySelector('#btn-admin-close');
        if (closeBtn) {
            closeBtn.addEventListener('click', () => {
                if (this._dirty) {
                    Toast.info('Settings changes discarded');
                }
                this.onClose();
            });
        }

        // ── Consent checkbox ──────────────────────────────────────────────────
        const consentCb = this.container.querySelector('#admin-ddns-consent');
        if (consentCb) {
            consentCb.addEventListener('change', async () => {
                const granted = consentCb.checked;
                try {
                    const result = await BackendClient.setDdnsConsent(granted);
                    if (result.status === 'accepted' || result.status === 'declined') {
                        this._ddnsConsent = granted;
                        Toast.success(
                            granted
                                ? 'DuckDNS consent granted'
                                : 'DuckDNS consent revoked'
                        );
                        // Re-render status section
                        if (this._tokenConfigured || !granted) {
                            await this._loadState();
                            this._updateDdnsStatus();
                        }

                        // If conset revoked, re-enable token input
                        const tokenInput = this.container.querySelector('#admin-ddns-token');
                        if (tokenInput && !granted) {
                            tokenInput.removeAttribute('readonly');
                            this._tokenConfigured = false;
                            this._updateDdnsStatus();
                        }
                    }
                } catch (err) {
                    console.error('[Admin] Failed to set consent:', err);
                    Toast.error('Failed to save consent: ' + err.message);
                    consentCb.checked = !granted;
                }
            });
        }

        // ── Save token button ─────────────────────────────────────────────────
        const saveTokenBtn = this.container.querySelector('#btn-admin-save-token');
        const tokenInput = this.container.querySelector('#admin-ddns-token');
        if (saveTokenBtn && tokenInput) {
            saveTokenBtn.addEventListener('click', async () => {
                const token = tokenInput.value.trim();
                if (!token) {
                    Toast.warning('Please enter a DuckDNS token');
                    return;
                }

                saveTokenBtn.disabled = true;
                saveTokenBtn.classList.add('btn-loading');
                saveTokenBtn.textContent = 'Saving...';

                try {
                    const result = await BackendClient.configureDdnsToken(token);
                    if (result.status === 'configured') {
                        this._tokenConfigured = true;
                        saveTokenBtn.textContent = 'Token Saved';
                        saveTokenBtn.classList.remove('btn-loading');
                        await this._loadState();
                        this._updateDdnsStatus();
                        Toast.success('DuckDNS token saved');
                    }
                } catch (err) {
                    console.error('[Admin] Failed to save token:', err);
                    Toast.error('Failed to save token: ' + err.message);
                    saveTokenBtn.disabled = false;
                    saveTokenBtn.classList.remove('btn-loading');
                    saveTokenBtn.textContent = 'Save Token';
                }
            });
        }
    }

    // --- DuckDNS status partial update ---

    _updateDdnsStatus() {
        const section = this.container.querySelector('.settings-section:last-child');
        if (!section) return;

        let statusEl = section.querySelector('.settings-status');
        const actionsEl = section.querySelector('.settings-actions:last-of-type');

        // Remove old status
        if (statusEl) statusEl.remove();

        const newStatus = this._ddnsActive && this._ddnsSubdomain
            ? `<div class="settings-status settings-status-ok">
                   Public URL: <a href="https://${this.esc(this._ddnsSubdomain)}.duckdns.org"
                      target="_blank" rel="noopener">
                      ${this.esc(this._ddnsSubdomain)}.duckdns.org</a>
               </div>`
            : this._ddnsConsent && !this._tokenConfigured
                ? `<div class="settings-status settings-status-pending">
                       Consent given. Enter your DuckDNS token above to activate.
                   </div>`
                : '';

        if (newStatus && actionsEl) {
            actionsEl.insertAdjacentHTML('afterend', newStatus);
        }

        // Update save token button state
        const saveTokenBtn = section.querySelector('#btn-admin-save-token');
        if (saveTokenBtn) {
            saveTokenBtn.disabled = this._tokenConfigured;
            saveTokenBtn.textContent = this._tokenConfigured ? 'Token Saved' : 'Save Token';
        }

        if (this._tokenConfigured) {
            const tokenInput = this.container.querySelector('#admin-ddns-token');
            if (tokenInput) tokenInput.setAttribute('readonly', '');
        }
    }

    // --- Helpers ---

    esc(text) {
        const div = document.createElement('div');
        div.textContent = text;
        return div.innerHTML;
    }
}
