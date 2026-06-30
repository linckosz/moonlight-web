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
 * MoonlightWeb — Login View
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
import { t } from '../i18n/i18n.js';

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
        this._certAuthAvailable = false; // Server has cert auth enabled
        this._certMode = false; // User chose cert upload mode
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
            this._error = t('login.unableToConnect');
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
                    <h1 class="login-title">MoonlightWeb</h1>

                    <div id="login-form-area">
                        ${
                            this._loading
                                ? `
                            <div class="login-loading">
                                <div class="login-spinner"></div>
                                <p>${t('login.connecting')}</p>
                            </div>
                        `
                                : this._certMode
                                  ? this._renderCertForm()
                                  : this._renderPinForm()
                        }
                    </div>

                    ${
                        !this._loading && this._certAuthAvailable
                            ? `
                        <div id="login-method-toggle" class="u-mt-3 u-center">
                            <button id="btn-toggle-auth-method" class="btn btn-link">
                                ${
                                    this._certMode
                                        ? t('login.usePinInstead')
                                        : t('login.useCertInstead')
                                }
                            </button>
                        </div>
                    `
                            : ''
                    }

                    ${
                        !this._loading && !this._certMode
                            ? `
                        <p class="login-hint">${t('login.pinHint')}</p>
                    `
                            : ''
                    }
                </div>
            </div>
        `;
    }

    _renderPinForm() {
        return `
            <div class="login-form">
                <p class="login-subtitle">${t('login.pinSubtitle')}</p>

                <!-- Machine name -->
                <div class="login-field">
                    <label class="login-label" for="login-machine-input">${t('login.nameSession')}</label>
                    <input type="text" id="login-machine-input" class="login-input"
                           placeholder="${this.esc(t('login.namePlaceholder'))}"
                           value="${this.esc(this._machineName)}"
                           maxlength="24"
                           autocomplete="off" />
                </div>

                <!-- PIN input -->
                <div class="login-field">
                    <label class="login-label" for="login-pin-input">${t('login.pinLabel')}</label>
                    <div class="login-pin-input-wrap">
                        <input type="text" id="login-pin-input" class="login-pin-input"
                               inputmode="numeric" pattern="[0-9]*"
                               placeholder="${this.esc(t('login.pinPlaceholder'))}"
                               autocomplete="off"
                               ${this._lockoutRemaining > 0 ? 'disabled' : ''} />
                    </div>
                </div>

                ${this._error ? `<p id="login-error" class="login-error">${this.esc(this._error)}</p>` : '<p id="login-error" class="login-error"></p>'}
                <p id="login-remaining" class="login-remaining">${this._remaining > 0 && this._remaining <= 3 ? this.esc(t('login.attemptsRemaining', { count: this._remaining })) : ''}</p>
                ${
                    this._lockoutRemaining > 0
                        ? `
                    <div id="login-lockout" class="login-lockout">
                        ${t('login.lockout', { seconds: `<span id="login-lockout-countdown">${this._lockoutRemaining}</span>` })}
                    </div>
                `
                        : '<div id="login-lockout" class="login-lockout" style="display:none;"></div>'
                }
                <button id="btn-login-unlock" class="btn btn-neutral login-submit"
                        disabled
                        ${this._lockoutRemaining > 0 || this._submitting ? 'disabled' : ''}>
                    ${this._submitting ? t('common.verifying') : t('login.unlock')}
                </button>
            </div>
        `;
    }

    _renderCertForm() {
        return `
            <div class="login-form">
                <p class="login-subtitle">${t('login.certSubtitle')}</p>

                <!-- Machine name -->
                <div class="login-field">
                    <label class="login-label" for="login-machine-input">${t('login.nameSession')}</label>
                    <input type="text" id="login-machine-input" class="login-input"
                           placeholder="${this.esc(t('login.namePlaceholder'))}"
                           value="${this.esc(this._machineName)}"
                           maxlength="24"
                           autocomplete="off" />
                </div>

                <!-- File upload -->
                <div class="login-field">
                    <label class="login-label" for="login-cert-input">${t('login.certLabel')}</label>
                    <div class="login-cert-upload-wrap">
                        <input type="file" id="login-cert-input" class="login-cert-input"
                               accept=".txt,.cert,.pem,.key,text/plain"
                               ${this._submitting ? 'disabled' : ''} />
                    </div>
                    ${
                        this._selectedFileName
                            ? `
                        <p class="login-file-selected">
                            ${this.esc(t('login.selected', { name: this._selectedFileName }))}
                        </p>
                    `
                            : ''
                    }
                </div>

                ${this._error ? `<p id="login-error" class="login-error">${this.esc(this._error)}</p>` : '<p id="login-error" class="login-error"></p>'}
                <button id="btn-login-cert-auth" class="btn btn-neutral login-submit"
                        disabled
                        ${this._submitting ? 'disabled' : ''}>
                    ${this._submitting ? t('common.verifying') : t('login.certAuthBtn')}
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
            // Mask: fill a "DDD DDD" template left-to-right — each typed digit
            // replaces the next dash. Caret stays right after the last digit.
            const digits = input.value.replace(/\D/g, '').slice(0, 6);
            const filled = digits.padEnd(6, '-');
            input.value = filled.slice(0, 3) + ' ' + filled.slice(3);
            // Caret right after the last digit (start when none yet).
            const caret = digits.length + (digits.length > 3 ? 1 : 0);
            input.setSelectionRange(caret, caret);
            btn.disabled = digits.length === 0 || this._lockoutRemaining > 0 || this._submitting;
        });

        // Show the "--- ---" template (centered) while focused, caret on the
        // first dash; clear it on blur so the dim placeholder shows when empty.
        input.addEventListener('focus', () => {
            const digits = input.value.replace(/\D/g, '');
            if (digits.length === 0) {
                input.value = '--- ---';
            }
            // Caret on the first dash: right after the last digit (start when none).
            // Defer so it wins over the mouse-click caret placement.
            const caret = digits.length + (digits.length > 3 ? 1 : 0);
            setTimeout(() => input.setSelectionRange(caret, caret), 0);
        });
        input.addEventListener('blur', () => {
            if (input.value.replace(/\D/g, '').length === 0) {
                input.value = '';
            }
        });

        input.addEventListener('keydown', (e) => {
            if (e.key === 'Enter' && input.value.length > 0 && !btn.disabled) {
                e.preventDefault();
                this._submitPin(input, machineInput, btn);
            }
        });

        // Enter on "Name this session" triggers submit too
        if (machineInput) {
            machineInput.addEventListener('keydown', (e) => {
                if (e.key === 'Enter' && input.value.length > 0 && !btn.disabled) {
                    e.preventDefault();
                    this._submitPin(input, machineInput, btn);
                }
            });
        }

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
                fileLabel.textContent = t('login.selected', { name: file.name });
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

        // Enter on "Name this session" triggers submit (with file selected)
        if (machineInput) {
            machineInput.addEventListener('keydown', (e) => {
                if (e.key === 'Enter' && certInput.files && certInput.files[0] && !btn.disabled) {
                    e.preventDefault();
                    this._submitCertificate(certInput.files[0], machineInput, btn, certInput);
                }
            });
        }
    }

    async _submitPin(input, machineInput, btn) {
        if (this._submitting) return;
        this._submitting = true;
        btn.disabled = true;
        btn.textContent = t('common.verifying');

        const pin = input.value.replace(/\D/g, '');
        const machineName = machineInput ? machineInput.value.trim() : '';

        try {
            const result = await BackendClient.validatePin(pin, machineName);

            if (result.status === 'ok') {
                Toast.success(t('login.authSuccess'));
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
        btn.textContent = t('common.verifying');

        const machineName = machineInput ? machineInput.value.trim() : '';

        try {
            // Read the file content as text
            const content = await this._readFileAsText(file);

            // Send to server for validation
            const result = await BackendClient.validateCertificate(content, machineName);

            if (result.status === 'ok') {
                Toast.success(t('login.authSuccess'));
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
            reader.onerror = (e) => reject(new Error(t('login.readFileError')));
            reader.readAsText(file);
        });
    }

    _handlePinError(err, input, btn) {
        this._submitting = false;
        btn.disabled = false;
        btn.textContent = t('login.unlock');
        input.value = '';
        input.focus();

        let errorMsg = t('login.invalidPin');
        const statusCode = err.statusCode || 0;
        const body = err.responseBody || {};

        if (statusCode === 429) {
            errorMsg = t('login.tooManyAttempts');
            if (body.lockout_seconds > 0) {
                this._startLockoutTimer(body.lockout_seconds);
            }
        } else if (statusCode === 401) {
            errorMsg = t('login.invalidPin');
            if (body.lockout_seconds > 0) {
                this._startLockoutTimer(body.lockout_seconds);
            }
            if (body.remaining !== undefined) {
                this._remaining = body.remaining;
            } else {
                this._remaining = Math.max(0, this._remaining - 1);
            }
        } else {
            errorMsg = err.message || t('login.connectionError');
        }

        this._error = errorMsg;
        this.render();
        this.bindEvents();
    }

    _handleCertError(err, btn, certInput) {
        this._submitting = false;
        btn.disabled = false;
        btn.textContent = t('login.certAuthBtn');

        // Reset file input for retry
        if (certInput) certInput.value = '';

        const statusCode = err.statusCode || 0;
        const body = err.responseBody || {};
        let errorMsg = '';

        if (statusCode === 401 && body.error === 'invalid_certificate') {
            errorMsg = t('login.invalidCert');
        } else {
            errorMsg = err.message || t('login.verificationFailed');
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
