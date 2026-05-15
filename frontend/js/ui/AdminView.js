/**
 * Moonlight-Web — Server Settings
 *
 * Server administration functions (localhost only):
 *   - HTTPS port configuration
 *   - nport.io tunnel configuration (remote access via nport CLI)
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

        // Tunnel state (nport)
        this._tunnelActive = false;
        this._tunnelUrl = '';
        this._tunnelSubdomain = '';
        this._tunnelAvailable = false;
        this._tunnelError = '';
        this._tunnelState = 'idle';  // idle | checking | starting | active | error | unavailable
        this._tunnelSeq = 0;         // Incremented on each enable/disable, guards poll races

        // Dirty tracking: snapshot of values at load time
        this._cleanState = {};
        this._dirty = false;
    }

    async start() {
        await this._loadState();
        await this._loadTunnelState();
        this.render();
        this.bindEvents();
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

    async _loadTunnelState() {
        try {
            const status = await BackendClient.getTunnelStatus();
            this._tunnelActive = status.active || false;
            this._tunnelUrl = status.public_url || '';
            this._tunnelSubdomain = status.subdomain || '';
            this._tunnelAvailable = status.available || false;
            this._tunnelError = status.error || '';
            this._updateTunnelState();
        } catch (err) {
            console.warn('[Admin] Failed to load tunnel status:', err);
            this._tunnelState = 'error';
            this._tunnelError = 'Failed to check tunnel status';
        }
    }

    _updateTunnelState() {
        if (!this._tunnelAvailable) {
            this._tunnelState = 'unavailable';
        } else if (this._tunnelError && !this._tunnelActive) {
            this._tunnelState = 'error';
        } else if (this._tunnelActive) {
            this._tunnelState = 'active';
        } else {
            this._tunnelState = 'idle';
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

    _tunnelInfoText() {
        switch (this._tunnelState) {
            case 'starting':
                return '<span class="tunnel-spinner"></span> Starting tunnel...';
            case 'active':
                return 'Tunnel is active — you can stream from anywhere.';
            case 'error':
                return this.esc(this._tunnelError || 'Failed to start tunnel.');
            case 'unavailable':
                return 'Unavailable — nport binary not found. Run prepare_node_nport.ps1 to enable remote access.';
            case 'idle':
            default:
                return '';
        }
    }

    _tunnelInfoClass() {
        switch (this._tunnelState) {
            case 'active':  return 'tunnel-info-success';
            case 'error':   return 'tunnel-info-error';
            case 'starting': return 'tunnel-info-pending';
            case 'unavailable': return 'tunnel-info-warning';
            default:        return 'tunnel-info-neutral';
        }
    }

    _tunnelUrlHtml() {
        if (!this._tunnelSubdomain) return '';
        const domain = `moonlightweb-${this._tunnelSubdomain}.nport.link`;
        const displayUrl = `https://${domain}`;
        if (this._tunnelState === 'active' && this._tunnelUrl) {
            return `<a class="tunnel-url-link" href="${this.esc(displayUrl)}" target="_blank" rel="noopener">${this.esc(displayUrl)}</a>`;
        }
        return `<span class="tunnel-url-disabled">${this.esc(displayUrl)}</span>`;
    }

    render() {
        this.container.innerHTML = `
            <div class="admin-view" id="view-admin">
                <div class="admin-header">
                    <h2>Server Settings</h2>
                    <button class="view-close-btn" id="btn-admin-close"
                            title="Close (discards unsaved changes)">&times;</button>
                </div>

                <!-- nport.io Tunnel (Internet Access) -->
                <div class="settings-section">

                    <div class="tunnel-checkbox-row">
                        <label class="tunnel-checkbox-label">
                            <input type="checkbox" id="chk-tunnel-enable"
                                   ${this._tunnelState === 'unavailable' ? 'disabled' : ''}
                                   ${this._tunnelState === 'active' ? 'checked' : ''} />
                            <span class="tunnel-checkbox-text">Internet Access</span>
                        </label>
                    </div>

                    <p class="settings-section-desc">
                        Creates a secure nport tunnel you can stream from outside
                        your home network.
                        <a href="https://www.npmjs.com/package/nport" target="_blank" rel="noopener">Learn more</a>.
                    </p>

                    <div class="tunnel-domain-row">
                        <span class="tunnel-domain-label">Your secured public domain:</span>
                        ${this._tunnelUrlHtml()}
                    </div>

                    <p class="tunnel-info ${this._tunnelInfoClass()}"
                       style="${this._tunnelState === 'idle' ? 'display:none' : ''}">
                        <span class="tunnel-info-text">${this._tunnelInfoText()}</span>
                        <button class="tunnel-info-close" title="Dismiss">&times;</button>
                    </p>
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
                            Save &amp; Reload
                        </button>
                    </div>
                </div>
            </div>
        `;

        this._markClean();
    }

    bindEvents() {
        // ── Tunnel checkbox toggle ──────────────────────────────────────────
        const tunnelChk = this.container.querySelector('#chk-tunnel-enable');
        if (tunnelChk) {
            tunnelChk.addEventListener('change', async () => {
                if (tunnelChk.checked) {
                    await this._enableTunnel();
                } else {
                    await this._disableTunnel();
                }
            });
        }

        // ── Port field dirty tracking ──────────────────────────────────────────
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
    }

    // ── Tunnel enable / disable ────────────────────────────────────────────────

    async _enableTunnel() {
        const seq = ++this._tunnelSeq;
        this._tunnelState = 'starting';
        this._refreshTunnelSection();

        try {
            const result = await BackendClient.configureTunnel({});
            if (seq !== this._tunnelSeq) return;  // Cancelled by disable
            if (result.status === 'configured') {
                // Poll until tunnel becomes active
                await this._pollTunnelActive(seq);
            } else {
                if (seq !== this._tunnelSeq) return;
                this._tunnelState = 'error';
                this._tunnelError = 'Backend returned unexpected status';
                this._refreshTunnelSection();
            }
        } catch (err) {
            if (seq !== this._tunnelSeq) return;
            console.error('[Admin] Failed to enable tunnel:', err);
            this._tunnelState = 'error';
            this._tunnelError = err.message || 'Failed to enable tunnel';
            this._refreshTunnelSection();
        }
    }

    async _disableTunnel() {
        ++this._tunnelSeq;  // Cancel any active poll
        try {
            await BackendClient.disableTunnel();
            this._tunnelActive = false;
            this._tunnelUrl = '';
            this._tunnelState = 'idle';
            this._tunnelError = '';
        } catch (err) {
            console.error('[Admin] Failed to disable tunnel:', err);
            // Keep state as-is but log the error
        }
        this._refreshTunnelSection();
    }

    async _pollTunnelActive(seq, maxRetries = 15, interval = 1500) {
        for (let i = 0; i < maxRetries; i++) {
            if (seq !== this._tunnelSeq) return;  // Cancelled
            await new Promise(r => setTimeout(r, interval));
            if (seq !== this._tunnelSeq) return;  // Cancelled during sleep
            try {
                const status = await BackendClient.getTunnelStatus();
                if (seq !== this._tunnelSeq) return;
                if (status.active) {
                    this._tunnelActive = true;
                    this._tunnelUrl = status.public_url || '';
                    this._tunnelSubdomain = status.subdomain || '';
                    this._tunnelError = '';
                    this._tunnelState = 'active';
                    this._refreshTunnelSection();
                    return;
                }
                // Check for errors during polling
                if (status.error) {
                    this._tunnelError = status.error;
                }
            } catch (err) {
                console.warn('[Admin] Tunnel poll error:', err);
            }
        }
        if (seq !== this._tunnelSeq) return;
        // Timed out — check one final time for error
        try {
            const status = await BackendClient.getTunnelStatus();
            if (status.error) {
                this._tunnelError = status.error;
            }
        } catch (_) {}
        if (seq !== this._tunnelSeq) return;
        this._tunnelState = 'error';
        if (!this._tunnelError) {
            this._tunnelError = 'Tunnel did not become active — check nport';
        }
        this._refreshTunnelSection();
    }

    // ── Partial DOM update for tunnel section ──────────────────────────────────

    _refreshTunnelSection() {
        // Update checkbox
        const chk = this.container.querySelector('#chk-tunnel-enable');
        if (chk) {
            chk.disabled = (this._tunnelState === 'unavailable');
            chk.checked = (this._tunnelState === 'active' || this._tunnelState === 'starting');
        }

        // Update domain URL
        const domainRow = this.container.querySelector('.tunnel-domain-row');
        if (domainRow) {
            domainRow.innerHTML = `
                <span class="tunnel-domain-label">Your secured public domain:</span>
                ${this._tunnelUrlHtml()}`;
        }

        // Update info text
        const infoEl = this.container.querySelector('.tunnel-info');
        if (infoEl) {
            const text = this._tunnelInfoText();
            infoEl.className = `tunnel-info ${this._tunnelInfoClass()}`;
            const textSpan = infoEl.querySelector('.tunnel-info-text');
            if (textSpan) {
                textSpan.innerHTML = text;
            }
            // Show if there is text, hide otherwise (idle state)
            infoEl.style.display = text ? '' : 'none';
            // Bind close button (×) to dismiss the message
            const closeBtn = infoEl.querySelector('.tunnel-info-close');
            if (closeBtn) {
                closeBtn.onclick = (e) => {
                    e.stopPropagation();
                    infoEl.style.display = 'none';
                };
            }
        }
    }

    // --- Helpers ---

    esc(text) {
        const div = document.createElement('div');
        div.textContent = text;
        return div.innerHTML;
    }
}
