/**
 * Moonlight-Web — Login View
 *
 * Authentication for remote visitors. Supports two methods:
 *   1. PIN-based (default): user enters the PIN shown on the admin page
 *   2. Certificate-based (alternative): user uploads a certificate file
 *
 * When certificate authentication is enabled on the server, the UI shows
 * a toggle between PIN and certificate modes.
 */
import { BackendClient } from '../api/BackendClient.js';
import { Toast } from './Toast.js';

export class LoginView {
    constructor(container, onAuthenticated) {
        this.container = container;
        this.onAuthenticated = onAuthenticated || (() => {});

        // Internal state
        this._loading = true;
        this._error = '';
        this._remaining = 3;
        this._lockoutRemaining = 0;
        this._lockoutTimer = null;
        this._submitting = false;
        this._machineName = '';
        this._clientIp = '';             // Detected local IP via WebRTC
        this._certAuthAvailable = false;  // Server has cert auth enabled
        this._certMode = false;           // User chose cert upload mode
        this._selectedFileName = '';
    }

    async start() {
        this._loading = true;
        this._error = '';
        this._lockoutRemaining = 0;
        this._certMode = false;
        this.render();
        this.bindEvents();
        this._focusInput();

        // Build default machine name from client OS + browser
        this._suggestMachineName();

        // Check auth status — maybe already authenticated
        try {
            const status = await BackendClient.getAuthStatus();
            if (status.authenticated) {
                this.onAuthenticated();
                return;
            }
            this._loading = false;
            this._certAuthAvailable = status.cert_auth_enabled || false;
            this._remaining = status.remaining !== undefined ? status.remaining : 3;
            if (status.lockout_seconds > 0) {
                this._startLockoutTimer(status.lockout_seconds);
            }
            this.render();
            this.bindEvents();
            this._focusInput();
        } catch (err) {
            console.warn('[Login] Failed to check auth status:', err);
            this._loading = false;
            this._error = 'Unable to connect to server';
            this.render();
            this.bindEvents();
        }
    }

    destroy() {
        this._stopLockoutTimer();
    }

    /**
     * Build a suggested machine name from client OS + browser.
     * e.g. "Windows Chrome", "macOS Safari", "Android Chrome"
     */
    _suggestMachineName() {
        const ua = navigator.userAgent;
        let os = 'Unknown';
        if (ua.includes('Windows')) os = 'Windows';
        else if (ua.includes('Android')) os = 'Android';
        else if (ua.includes('iPhone') || ua.includes('iPad')) os = 'iOS';
        else if (ua.includes('Mac OS') || ua.includes('Macintosh')) os = 'macOS';
        else if (ua.includes('Linux') && !ua.includes('Android')) os = 'Linux';
        else if (ua.includes('CrOS')) os = 'ChromeOS';

        let browser = 'Browser';
        if (ua.includes('Firefox')) browser = 'Firefox';
        else if (ua.includes('Edg')) browser = 'Edge';
        else if (ua.includes('Chrome')) browser = 'Chrome';
        else if (ua.includes('Safari') && !ua.includes('Chrome')) browser = 'Safari';
        else if (ua.includes('OPR') || ua.includes('Opera')) browser = 'Opera';

        this._machineName = `${os} ${browser}`;
    }

    /**
     * Detect the client's local IP address via WebRTC.
     * Returns the first non-loopback IPv4 address, or empty string on failure.
     */
    async _detectLocalIP() {
        try {
            const pc = new RTCPeerConnection({ iceServers: [] });
            pc.createDataChannel('');
            const offer = await pc.createOffer();
            await pc.setLocalDescription(offer);

            const ip = await new Promise((resolve) => {
                const timeout = setTimeout(() => resolve(''), 2000);
                pc.onicecandidate = (e) => {
                    if (!e.candidate) {
                        clearTimeout(timeout);
                        resolve('');
                        return;
                    }
                    // Look for IPv4 host candidates (private/LAN IPs)
                    const match = e.candidate.candidate.match(/(\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3})/);
                    if (match && !match[1].startsWith('127.')) {
                        console.log('[Login] Detected local IP:', match[1]);
                        clearTimeout(timeout);
                        resolve(match[1]);
                    }
                };
            });

            pc.close();
            return ip;
        } catch (_) {
            return '';
        }
    }

    render() {
        this.container.innerHTML = `
            <div class="login-view" id="view-login">
                <div class="login-box">
                    <div class="login-logo">
                        <svg width="48" height="48" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round">
                            <path d="M12 2L2 7l10 5 10-5-10-5z"/>
                            <path d="M2 17l10 5 10-5"/>
                            <path d="M2 12l10 5 10-5"/>
                        </svg>
                    </div>
                    <h1 class="login-title">Moonlight-Web</h1>

                    <div id="login-form-area">
                        ${this._loading ? `
                            <div class="login-loading">
                                <div class="login-spinner"></div>
                                <p>Connecting to server...</p>
                            </div>
                        ` : this._certMode ? this._renderCertForm() : this._renderPinForm()}
                    </div>

                    ${!this._loading && this._certAuthAvailable ? `
                        <div id="login-method-toggle" style="margin-top:12px;text-align:center;">
                            <button id="btn-toggle-auth-method" class="btn btn-link"
                                    style="background:none;border:none;color:var(--accent);cursor:pointer;font-size:0.85em;text-decoration:underline;">
                                ${this._certMode
                                    ? 'Use PIN instead'
                                    : 'Use certificate instead'}
                            </button>
                        </div>
                    ` : ''}

                    ${!this._loading && !this._certMode ? `
                        <p class="login-hint">Access the admin page from localhost to view the PIN</p>
                    ` : ''}
                </div>
            </div>
        `;
    }

    _renderPinForm() {
        return `
            <div class="login-form">
                <p class="login-subtitle">Enter the PIN shown on the server admin page</p>

                <!-- Machine name -->
                <div class="login-field">
                    <label class="login-label" for="login-machine-input">Name this session</label>
                    <input type="text" id="login-machine-input" class="login-input"
                           placeholder="e.g. BrunoXT Chrome"
                           value="${this.esc(this._machineName)}"
                           maxlength="64"
                           autocomplete="off" />
                </div>

                <!-- PIN input -->
                <div class="login-field">
                    <label class="login-label" for="login-pin-input">PIN</label>
                    <div class="login-pin-input-wrap">
                        <input type="text" id="login-pin-input" class="login-pin-input"
                               inputmode="numeric" pattern="[0-9]*"
                               maxlength="20" placeholder="Enter the PIN"
                               autocomplete="off"
                               ${this._lockoutRemaining > 0 ? 'disabled' : ''} />
                    </div>
                </div>

                ${this._error ? `<p id="login-error" class="login-error">${this.esc(this._error)}</p>` : '<p id="login-error" class="login-error"></p>'}
                <p id="login-remaining" class="login-remaining">${this._remaining > 0 && this._remaining <= 3 ? `${this._remaining} attempt(s) remaining before lockout` : ''}</p>
                ${this._lockoutRemaining > 0 ? `
                    <div id="login-lockout" class="login-lockout">
                        Try again in <span id="login-lockout-countdown">${this._lockoutRemaining}</span> seconds
                    </div>
                ` : '<div id="login-lockout" class="login-lockout" style="display:none;"></div>'}
                <button id="btn-login-unlock" class="btn btn-neutral login-submit"
                        disabled
                        ${this._lockoutRemaining > 0 || this._submitting ? 'disabled' : ''}>
                    ${this._submitting ? 'Verifying...' : 'Unlock'}
                </button>
            </div>
        `;
    }

    _renderCertForm() {
        return `
            <div class="login-form">
                <p class="login-subtitle">Upload your certificate file to authenticate</p>

                <!-- Machine name -->
                <div class="login-field">
                    <label class="login-label" for="login-machine-input">Name this session</label>
                    <input type="text" id="login-machine-input" class="login-input"
                           placeholder="e.g. BrunoXT Chrome"
                           value="${this.esc(this._machineName)}"
                           maxlength="64"
                           autocomplete="off" />
                </div>

                <!-- File upload -->
                <div class="login-field">
                    <label class="login-label" for="login-cert-input">Certificate file</label>
                    <div class="login-cert-upload-wrap">
                        <input type="file" id="login-cert-input" class="login-cert-input"
                               accept=".txt,.cert,.pem,.key,text/plain"
                               ${this._submitting ? 'disabled' : ''} />
                    </div>
                    ${this._selectedFileName ? `
                        <p class="login-file-selected" style="font-size:0.85em;margin-top:4px;color:var(--text-muted);">
                            Selected: ${this.esc(this._selectedFileName)}
                        </p>
                    ` : ''}
                </div>

                ${this._error ? `<p id="login-error" class="login-error">${this.esc(this._error)}</p>` : '<p id="login-error" class="login-error"></p>'}
                <button id="btn-login-cert-auth" class="btn btn-neutral login-submit"
                        disabled
                        ${this._submitting ? 'disabled' : ''}>
                    ${this._submitting ? 'Verifying...' : 'Authenticate with Certificate'}
                </button>
            </div>
        `;
    }

    bindEvents() {
        const formArea = this.container.querySelector('#login-form-area');
        if (!formArea) return;

        if (this._certMode) {
            this._bindCertEvents();
        } else {
            this._bindPinEvents();
        }

        // Auth method toggle button
        const toggleBtn = this.container.querySelector('#btn-toggle-auth-method');
        if (toggleBtn) {
            toggleBtn.addEventListener('click', (e) => {
                e.preventDefault();
                this._certMode = !this._certMode;
                this._error = '';
                this._selectedFileName = '';
                this.render();
                this.bindEvents();
                this._focusInput();
            });
        }
    }

    _bindPinEvents() {
        const input = this.container.querySelector('#login-pin-input');
        const machineInput = this.container.querySelector('#login-machine-input');
        const btn = this.container.querySelector('#btn-login-unlock');

        if (!input || !btn) return;

        input.addEventListener('input', () => {
            input.value = input.value.replace(/\D/g, '');
            btn.disabled = input.value.length === 0
                || this._lockoutRemaining > 0
                || this._submitting;
        });

        input.addEventListener('keydown', (e) => {
            if (e.key === 'Enter' && input.value.length > 0 && !btn.disabled) {
                e.preventDefault();
                this._submitPin(input, machineInput, btn);
            }
        });

        btn.addEventListener('click', () => {
            if (input.value.length > 0) {
                this._submitPin(input, machineInput, btn);
            }
        });
    }

    _bindCertEvents() {
        const certInput = this.container.querySelector('#login-cert-input');
        const machineInput = this.container.querySelector('#login-machine-input');
        const btn = this.container.querySelector('#btn-login-cert-auth');

        if (!certInput || !btn) return;

        certInput.addEventListener('change', () => {
            const file = certInput.files && certInput.files[0];
            this._selectedFileName = file ? file.name : '';
            btn.disabled = !file || this._submitting;

            // Show selected file name
            const fileLabel = this.container.querySelector('.login-file-selected');
            if (fileLabel && file) {
                fileLabel.textContent = `Selected: ${file.name}`;
            } else if (fileLabel) {
                fileLabel.textContent = '';
            }
        });

        btn.addEventListener('click', () => {
            const file = certInput.files && certInput.files[0];
            if (file) {
                this._submitCertificate(file, machineInput, btn, certInput);
            }
        });

        // Submit on Enter (if file selected)
        certInput.addEventListener('keydown', (e) => {
            if (e.key === 'Enter' && certInput.files && certInput.files[0] && !btn.disabled) {
                e.preventDefault();
                this._submitCertificate(certInput.files[0], machineInput, btn, certInput);
            }
        });
    }

    async _submitPin(input, machineInput, btn) {
        if (this._submitting) return;
        this._submitting = true;
        btn.disabled = true;
        btn.textContent = 'Verifying...';

        const pin = input.value;
        const machineName = machineInput ? machineInput.value.trim() : '';

        // Detect local IP now (user gesture avoids browser WebRTC restrictions)
        if (!this._clientIp) this._clientIp = await this._detectLocalIP();

        try {
            const result = await BackendClient.validatePin(pin, machineName, this._clientIp);

            if (result.status === 'ok') {
                Toast.success('Authentication successful');
                this.onAuthenticated();
                return;
            }
        } catch (err) {
            this._handlePinError(err, input, btn);
        }
    }

    async _submitCertificate(file, machineInput, btn, certInput) {
        if (this._submitting) return;
        this._submitting = true;
        btn.disabled = true;
        btn.textContent = 'Verifying...';

        const machineName = machineInput ? machineInput.value.trim() : '';

        // Detect local IP now (user gesture avoids browser WebRTC restrictions)
        if (!this._clientIp) this._clientIp = await this._detectLocalIP();

        try {
            // Read the file content as text
            const content = await this._readFileAsText(file);

            // Send to server for validation
            const result = await BackendClient.validateCertificate(content, machineName, this._clientIp);

            if (result.status === 'ok') {
                Toast.success('Authentication successful');
                this.onAuthenticated();
                return;
            }
        } catch (err) {
            this._handleCertError(err, btn, certInput);
        }
    }

    _readFileAsText(file) {
        return new Promise((resolve, reject) => {
            const reader = new FileReader();
            reader.onload = (e) => resolve(e.target.result);
            reader.onerror = (e) => reject(new Error('Failed to read file'));
            reader.readAsText(file);
        });
    }

    _handlePinError(err, input, btn) {
        this._submitting = false;
        btn.disabled = false;
        btn.textContent = 'Unlock';
        input.value = '';
        input.focus();

        let errorMsg = 'Invalid PIN';
        const statusCode = err.statusCode || 0;
        const body = err.responseBody || {};

        if (statusCode === 429) {
            errorMsg = 'Too many failed attempts';
            if (body.lockout_seconds > 0) {
                this._startLockoutTimer(body.lockout_seconds);
            }
        } else if (statusCode === 401) {
            errorMsg = 'Invalid PIN';
            if (body.lockout_seconds > 0) {
                this._startLockoutTimer(body.lockout_seconds);
            }
            if (body.remaining !== undefined) {
                this._remaining = body.remaining;
            } else {
                this._remaining = Math.max(0, this._remaining - 1);
            }
        } else {
            errorMsg = err.message || 'Connection error';
        }

        this._error = errorMsg;
        this.render();
        this.bindEvents();
    }

    _handleCertError(err, btn, certInput) {
        this._submitting = false;
        btn.disabled = false;
        btn.textContent = 'Authenticate with Certificate';

        // Reset file input for retry
        if (certInput) certInput.value = '';

        const statusCode = err.statusCode || 0;
        const body = err.responseBody || {};
        let errorMsg = '';

        if (statusCode === 401 && body.error === 'invalid_certificate') {
            errorMsg = 'Invalid certificate. Check that you have the correct file.';
        } else {
            errorMsg = err.message || 'Verification failed';
        }

        this._error = errorMsg;
        this.render();
        this.bindEvents();
    }

    _startLockoutTimer(seconds) {
        this._stopLockoutTimer();
        this._lockoutRemaining = seconds;
        this._updateLockoutDisplay();

        this._lockoutTimer = setInterval(() => {
            this._lockoutRemaining--;
            if (this._lockoutRemaining <= 0) {
                this._stopLockoutTimer();
                this._lockoutRemaining = 0;
                this._error = '';
                this.render();
                this.bindEvents();
                this._focusInput();
            } else {
                this._updateLockoutDisplay();
            }
        }, 1000);
    }

    _stopLockoutTimer() {
        if (this._lockoutTimer) {
            clearInterval(this._lockoutTimer);
            this._lockoutTimer = null;
        }
    }

    _updateLockoutDisplay() {
        const lockout = this.container.querySelector('#login-lockout');
        const countdown = this.container.querySelector('#login-lockout-countdown');
        const input = this.container.querySelector('#login-pin-input');
        const btn = this.container.querySelector('#btn-login-unlock');

        if (lockout) lockout.style.display = '';
        if (countdown) countdown.textContent = this._lockoutRemaining;
        if (input) input.disabled = this._lockoutRemaining > 0;
        if (btn) btn.disabled = this._lockoutRemaining > 0;
    }

    _focusInput() {
        if (this._certMode) {
            const input = this.container.querySelector('#login-cert-input');
            if (input && !input.disabled) {
                setTimeout(() => input.focus(), 100);
            }
        } else {
            const input = this.container.querySelector('#login-pin-input');
            if (input && !input.disabled) {
                setTimeout(() => input.focus(), 100);
            }
        }
    }

    esc(text) {
        const div = document.createElement('div');
        div.textContent = text;
        return div.innerHTML;
    }
}
