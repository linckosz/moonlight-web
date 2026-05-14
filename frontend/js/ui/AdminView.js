/**
 * Moonlight-Web — Server Settings
 *
 * Server administration functions (localhost only):
 *   - HTTPS port configuration
 *   - zrok tunnel configuration (remote access)
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

        // zrok state
        this._zrokToken = '';
        this._zrokActive = false;
        this._zrokPublicUrl = '';
        this._zrokReservedName = '';
        this._zrokHasToken = false;

        // Dirty tracking: snapshot of values at load time
        this._cleanState = {};
        this._dirty = false;
    }

    async start() {
        await this._loadState();
        await this._loadZrokState();
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
    }

    async _loadZrokState() {
        try {
            const status = await BackendClient.getZrokStatus();
            this._zrokActive = status.active || false;
            this._zrokPublicUrl = status.public_url || '';
            this._zrokReservedName = status.reserved_name || '';
            this._zrokHasToken = status.token_configured || false;
        } catch (err) {
            console.warn('[Admin] Failed to load zrok status:', err);
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

    _zrokStatusHtml() {
        if (this._zrokActive) {
            return `
                <div class="zrok-status zrok-active">
                    <span class="zrok-dot"></span>
                    Tunnel active
                    <div class="zrok-url">
                        <code>${this.esc(this._zrokPublicUrl)}</code>
                        <button class="zrok-copy-btn" id="btn-zrok-copy" title="Copy URL">Copy</button>
                    </div>
                    <p class="zrok-reserved">Reserved name: <strong>${this.esc(this._zrokReservedName)}</strong></p>
                </div>`;
        } else if (this._zrokHasToken) {
            return `
                <div class="zrok-status zrok-inactive">
                    <span class="zrok-dot"></span>
                    Tunnel inactive — check zrok is installed and token is valid
                    <p class="zrok-reserved">Reserved name: <strong>${this.esc(this._zrokReservedName)}</strong></p>
                </div>`;
        } else {
            return `
                <div class="zrok-status zrok-pending">
                    <span class="zrok-dot"></span>
                    Not configured — enter your zrok token to enable remote access
                </div>`;
        }
    }

    render() {
        this.container.innerHTML = `
            <div class="admin-view" id="view-admin">
                <div class="admin-header">
                    <h2>Server Settings</h2>
                    <button class="view-close-btn" id="btn-admin-close"
                            title="Close (discards unsaved changes)">&times;</button>
                </div>

                <!-- zrok Tunnel -->
                <div class="settings-section">
                    <h3 class="settings-section-title">Remote Access (zrok)</h3>
                    <p class="settings-section-desc">
                        zrok creates a secure tunnel so you can stream from outside
                        your home network. No router configuration needed.
                        <a href="https://zrok.io" target="_blank" rel="noopener">Get a free token</a>.
                    </p>

                    <div class="settings-field">
                        <label class="settings-label" for="admin-zrok-token">
                            zrok Token
                        </label>
                        <input type="password" id="admin-zrok-token" class="settings-input"
                               placeholder="Paste your zrok token..."
                               value="${this.esc(this._zrokToken)}"
                               autocomplete="off" />
                        <button class="btn btn-save" id="btn-zrok-save"
                                style="margin-top:8px;">Save Token</button>
                    </div>

                    ${this._zrokStatusHtml()}
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
        // ── zrok token save ───────────────────────────────────────────────────
        const zrokSaveBtn = this.container.querySelector('#btn-zrok-save');
        const zrokTokenInput = this.container.querySelector('#admin-zrok-token');
        if (zrokSaveBtn && zrokTokenInput) {
            zrokSaveBtn.addEventListener('click', async () => {
                const token = zrokTokenInput.value.trim();
                if (!token) {
                    Toast.warning('Please enter a zrok token');
                    return;
                }

                zrokSaveBtn.disabled = true;
                zrokSaveBtn.textContent = 'Saving...';

                try {
                    const result = await BackendClient.configureZrokToken(token);
                    if (result.status === 'configured') {
                        this._zrokReservedName = result.reserved_name || '';
                        Toast.success('zrok token saved. Tunnel starting...');
                        setTimeout(async () => {
                            await this._loadZrokState();
                            this._refreshZrokSection();
                        }, 2000);
                    }
                } catch (err) {
                    console.error('[Admin] Failed to configure zrok:', err);
                    Toast.error('Failed to configure zrok: ' + err.message);
                } finally {
                    zrokSaveBtn.disabled = false;
                    zrokSaveBtn.textContent = 'Save Token';
                }
            });
        }

        // ── zrok URL copy ─────────────────────────────────────────────────────
        this._bindCopyBtn();

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

    // ── Partial DOM update for zrok section ──────────────────────────────────

    _refreshZrokSection() {
        const statusEl = this.container.querySelector('.zrok-status');
        if (!statusEl) return;

        if (this._zrokActive) {
            statusEl.className = 'zrok-status zrok-active';
            statusEl.innerHTML = `
                <span class="zrok-dot"></span>
                Tunnel active
                <div class="zrok-url">
                    <code>${this.esc(this._zrokPublicUrl)}</code>
                    <button class="zrok-copy-btn" id="btn-zrok-copy" title="Copy URL">Copy</button>
                </div>
                <p class="zrok-reserved">Reserved name: <strong>${this.esc(this._zrokReservedName)}</strong></p>`;
            this._bindCopyBtn();
        } else if (this._zrokHasToken) {
            statusEl.className = 'zrok-status zrok-inactive';
            statusEl.innerHTML = `
                <span class="zrok-dot"></span>
                Tunnel inactive — check zrok is installed and token is valid
                <p class="zrok-reserved">Reserved name: <strong>${this.esc(this._zrokReservedName)}</strong></p>`;
        }
    }

    _bindCopyBtn() {
        const copyBtn = this.container.querySelector('#btn-zrok-copy');
        if (copyBtn) {
            copyBtn.addEventListener('click', () => {
                if (this._zrokPublicUrl) {
                    navigator.clipboard.writeText(this._zrokPublicUrl).then(() => {
                        Toast.success('URL copied');
                    }).catch(() => {
                        Toast.warning('Failed to copy');
                    });
                }
            });
        }
    }

    // --- Helpers ---

    esc(text) {
        const div = document.createElement('div');
        div.textContent = text;
        return div.innerHTML;
    }
}
