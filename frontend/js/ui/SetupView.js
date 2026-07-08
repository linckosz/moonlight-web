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
 * MoonlightWeb — First-run setup wizard (macOS / Linux)
 *
 * Windows ships a native Inno Setup installer that authorizes Internet Access
 * and installs + pairs the local Sunshine. macOS/Linux ship a bare app bundle,
 * so this in-app wizard (opened automatically in the browser on first launch)
 * covers the same steps by talking to /api/setup/{status,apply}.
 *
 * Steps: config (Internet + Sunshine choices) → progress (live checklist) → done.
 */
import { BackendClient } from '../api/BackendClient.js';
import { t } from '../i18n/i18n.js';
import { escapeHtml } from '../util/escapeHtml.js';

export class SetupView {
    constructor(container, onComplete) {
        this.container = container;
        this.onComplete = onComplete || (() => {});

        this._step = 'loading'; // loading | config | progress | done | error
        this._os = 'Unknown';
        this._sunshineInstalled = false;
        this._sunshinePaired = false;
        this._autostartInstalled = false;
        this._internetActive = false;
        this._domain = '';
        this._httpsPort = 443; // for the "Open MoonlightWeb" switch to HTTPS
        this._error = '';

        // User choices (config step). Internet Access is opt-in: opening the
        // machine to the Internet (UPnP + public DNS record) requires an
        // explicit click — never a pre-ticked box.
        this._internetAuth = false;
        this._installSunshine = true;
        this._autoStart = true;

        this._pollTimer = null;
        // Which checklist rows are relevant to the run in progress.
        this._activeSteps = [];
    }

    async start() {
        this._step = 'loading';
        this.render();
        try {
            const status = await BackendClient.getSetupStatus();
            this._os = status.os || 'Unknown';
            this._sunshineInstalled = !!(status.sunshine && status.sunshine.installed);
            this._sunshinePaired = !!(status.sunshine && status.sunshine.paired);
            this._autostartInstalled = !!status.autostart_installed;
            this._internetActive = !!(status.internet && status.internet.active);
            this._domain = (status.internet && status.internet.domain) || '';
            this._httpsPort = status.https_port || 443;
            // The backend says whether it can auto-install Sunshine here (macOS
            // DMG, or Linux .deb on Debian/Ubuntu-family distros with polkit).
            this._canAutoInstall = !!(status.sunshine && status.sunshine.can_auto_install);
            // Default the install checkbox off when Sunshine is already present or
            // cannot be auto-installed on this OS.
            this._installSunshine = this._canAutoInstall && !this._sunshineInstalled;
            this._step = 'config';
        } catch (err) {
            console.error('[Setup] status failed:', err);
            this._error = err.message || t('setup.errorGeneric');
            this._step = 'error';
        }
        this.render();
        this.bindEvents();
    }

    destroy() {
        this._stopPolling();
    }

    // ── Rendering ─────────────────────────────────────────────────────────────

    render() {
        let body = '';
        if (this._step === 'loading') body = this._renderLoading();
        else if (this._step === 'config') body = this._renderConfig();
        else if (this._step === 'progress') body = this._renderProgress();
        else if (this._step === 'done') body = this._renderDone();
        else body = this._renderError();

        this.container.innerHTML = `
            <div class="login-view" id="view-setup">
                <div class="login-box setup-box">
                    <div class="login-logo">
                        <span class="login-logo-img" role="img" aria-label="Moonlight"></span>
                    </div>
                    <h1 class="login-title">${t('setup.title')}</h1>
                    ${body}
                </div>
            </div>
        `;
    }

    _renderLoading() {
        return `
            <div class="login-loading">
                <div class="login-spinner"></div>
                <p>${t('common.loading')}</p>
            </div>`;
    }

    _renderConfig() {
        // Sunshine block: "installed & paired" (nothing to do), "installed but
        // unpaired" (creds to pair), an auto-install checkbox (macOS), or a
        // manual-install hint (other OS).
        let sunshineBlock;
        if (this._sunshineInstalled && this._sunshinePaired) {
            sunshineBlock = this._okNote(t('setup.sunshinePaired'));
        } else if (this._sunshineInstalled) {
            sunshineBlock = `
                <p class="setup-note">${t('setup.sunshineInstalled')}</p>
                ${this._credsFields()}`;
        } else if (this._canAutoInstall) {
            sunshineBlock = `
                <p class="setup-note">${t('setup.sunshineNotDetected')}</p>
                <label class="setup-check">
                    <input type="checkbox" id="chk-install" ${this._installSunshine ? 'checked' : ''} />
                    <span>${t('setup.installSunshine')}</span>
                </label>
                ${this._credsFields()}`;
        } else {
            sunshineBlock = `
                <p class="setup-note">${t('setup.sunshineManual')}</p>`;
        }

        const internetBlock = this._internetActive
            ? this._okNote(t('setup.internetActive', { domain: this._domain }))
            : `
                <p class="setup-note">${t('setup.internetBody')}</p>
                <label class="setup-check">
                    <input type="checkbox" id="chk-internet" ${this._internetAuth ? 'checked' : ''} />
                    <span class="consent-highlight">${t('setup.internetOption')}</span>
                </label>`;

        const autostartBlock = this._autostartInstalled
            ? this._okNote(t('setup.autostartInstalled'))
            : `
                <label class="setup-check">
                    <input type="checkbox" id="chk-autostart" ${this._autoStart ? 'checked' : ''} />
                    <span>${t('setup.autostartOption')}</span>
                </label>`;

        return `
            <p class="login-subtitle">${t('setup.intro')}</p>

            <div class="setup-section">
                <h2 class="setup-section-title">${t('setup.internetTitle')}</h2>
                ${internetBlock}
            </div>

            <div class="setup-section">
                <h2 class="setup-section-title">${t('setup.sunshineTitle')}</h2>
                ${sunshineBlock}
            </div>

            <div class="setup-section">
                <h2 class="setup-section-title">${t('setup.autostartTitle')}</h2>
                ${autostartBlock}
            </div>

            ${this._error ? `<p class="login-error">${this.esc(this._error)}</p>` : ''}

            <button id="btn-setup-start" class="btn btn-neutral login-submit">
                ${t('setup.start')}
            </button>
            <button id="btn-setup-skip" class="btn btn-link u-mt-2">${t('setup.skip')}</button>`;
    }

    // Green "already done" row shown in place of a step's controls.
    _okNote(text) {
        return `<p class="setup-note setup-ok"><span class="setup-ok-check">✓</span> ${this.esc(text)}</p>`;
    }

    // Sunshine credential fields (shared by the "installed" and "install" cases).
    _credsFields() {
        const needed = this._sunshineInstalled || this._installSunshine;
        const dis = needed ? '' : 'disabled';
        return `
            <div class="setup-creds">
                <div class="login-field">
                    <label class="login-label" for="setup-user">${t('setup.username')}</label>
                    <input type="text" id="setup-user" class="login-input" autocomplete="off"
                           value="${this._sunshineInstalled ? '' : 'admin'}" ${dis} />
                </div>
                <div class="login-field">
                    <label class="login-label" for="setup-pass">${t('setup.password')}</label>
                    <input type="password" id="setup-pass" class="login-input" autocomplete="off" ${dis} />
                </div>
            </div>`;
    }

    _renderProgress() {
        return `
            <p class="login-subtitle">${t('setup.working')}</p>
            <div id="setup-checklist">${this._renderChecklist({})}</div>`;
    }

    // Build the checklist from the current status.steps map. Only rows we chose
    // to run are shown (the rest are 'skipped' server-side).
    _renderChecklist(steps) {
        const labels = {
            install: t('setup.stepInstall'),
            pairing: t('setup.stepPairing'),
            arecord: t('setup.stepArecord'),
        };
        const items = this._activeSteps
            .map((key) => {
                const state = steps[key] || 'pending';
                let cls = 'step-pending';
                let marker = '<span class="step-dot">○</span>';
                if (state === 'done') {
                    cls = 'step-done';
                    marker = '<span class="step-check">✓</span>';
                } else if (state === 'failed') {
                    cls = 'step-failed';
                    marker = '<span class="step-check">✕</span>';
                } else if (state === 'running') {
                    cls = 'step-active';
                    marker = '<span class="tunnel-spinner"></span>';
                }
                return `<li class="${cls}">${marker}<span class="step-label">${this.esc(labels[key])}</span></li>`;
            })
            .join('');
        return `<ul class="activation-steps">${items}</ul>`;
    }

    _renderDone() {
        const domainLine =
            this._internetActive && this._domain
                ? `<p class="setup-note">${t('setup.doneDomain', { domain: this.esc(this._domain) })}</p>`
                : '';
        // TCC permissions hint only when Sunshine was actually touched this run
        // (a fully-paired setup revisit has nothing left to grant).
        const permsLine =
            this._os === 'macOS' &&
            (this._activeSteps.includes('install') || this._activeSteps.includes('pairing'))
                ? `<p class="setup-note setup-warn">${t('setup.donePermissions')}</p>`
                : '';
        return `
            <p class="login-subtitle">${t('setup.doneTitle')}</p>
            ${domainLine}
            ${permsLine}
            <button id="btn-setup-finish" class="btn btn-neutral login-submit">
                ${t('setup.finish')}
            </button>`;
    }

    _renderError() {
        return `
            <p class="login-error">${this.esc(this._error)}</p>
            <button id="btn-setup-retry" class="btn btn-neutral login-submit">${t('common.retry')}</button>`;
    }

    // ── Events ──────────────────────────────────────────────────────────────

    bindEvents() {
        if (this._step === 'config') {
            const chkInstall = this.container.querySelector('#chk-install');
            if (chkInstall) {
                chkInstall.addEventListener('change', () => {
                    this._installSunshine = chkInstall.checked;
                    // Toggle credential fields without losing typed values elsewhere.
                    this.render();
                    this.bindEvents();
                });
            }
            const start = this.container.querySelector('#btn-setup-start');
            if (start) start.addEventListener('click', () => this._apply());
            const skip = this.container.querySelector('#btn-setup-skip');
            if (skip) skip.addEventListener('click', () => this._skip());
        } else if (this._step === 'done') {
            const finish = this.container.querySelector('#btn-setup-finish');
            if (finish) finish.addEventListener('click', () => this._finish());
        } else if (this._step === 'error') {
            const retry = this.container.querySelector('#btn-setup-retry');
            if (retry) retry.addEventListener('click', () => this.start());
        }
    }

    async _apply() {
        // Steps already satisfied are rendered as "✓ done" (no controls) and
        // must not run again: their flags are forced off here.
        this._internetAuth =
            !this._internetActive && !!this.container.querySelector('#chk-internet')?.checked;
        this._autoStart =
            !this._autostartInstalled && !!this.container.querySelector('#chk-autostart')?.checked;
        const chkInstall = this.container.querySelector('#chk-install');
        if (chkInstall) this._installSunshine = chkInstall.checked;
        const user = (this.container.querySelector('#setup-user')?.value || '').trim();
        const pass = this.container.querySelector('#setup-pass')?.value || '';

        const willInstall =
            this._installSunshine && this._canAutoInstall && !this._sunshineInstalled;
        const needPairing = this._sunshineInstalled && !this._sunshinePaired;
        const haveCreds = !!user && !!pass;

        // Require credentials when they will actually be used (install or pairing).
        if ((willInstall || needPairing) && !haveCreds) {
            this._error = t('setup.credsRequired');
            this.render();
            this.bindEvents();
            return;
        }

        // Compute the checklist rows that will run for live rendering.
        this._activeSteps = [];
        if (willInstall) this._activeSteps.push('install');
        if (haveCreds && (willInstall || needPairing)) this._activeSteps.push('pairing');
        if (this._internetAuth) this._activeSteps.push('arecord');

        this._error = '';
        this._step = 'progress';
        this.render();
        this._startPolling();

        try {
            const result = await BackendClient.applySetup({
                internet_access_authorized: this._internetAuth,
                // Exact agreement text displayed — recorded server-side in the
                // DNS registration audit log (legal traceability).
                consent_message: this._internetAuth
                    ? t('setup.internetBody') + ' / ' + t('setup.internetOption')
                    : '',
                autostart: this._autoStart,
                sunshine: {
                    install: willInstall,
                    username: user,
                    password: pass,
                },
            });
            this._stopPolling();
            if (result.internet_active !== undefined) {
                this._internetActive = !!result.internet_active;
                this._domain = result.domain || '';
            }
            // Sunshine install can fail on its own (e.g. the user mistyped the OS
            // password in the polkit dialog) while /apply still returns 200. Don't
            // dead-end on the "done" screen: return to config with the error shown
            // so the user can retry (the password prompt re-appears), uncheck
            // Sunshine, or skip. Steps that succeeded above are reflected as done.
            if (result.sunshine_error) {
                this._error = t('setup.sunshineInstallFailed', {
                    error: result.sunshine_error,
                });
                this._step = 'config';
                this.render();
                this.bindEvents();
                return;
            }
            // A completed run re-arms the startup gate: if a step goes missing
            // again later, the wizard reappears even after a previous "Skip".
            try {
                localStorage.removeItem('mw_setup_dismissed');
            } catch (_e) {
                /* best-effort */
            }
            this._step = 'done';
            this.render();
            this.bindEvents();
        } catch (err) {
            this._stopPolling();
            console.error('[Setup] apply failed:', err);
            this._error = err.message || t('setup.errorGeneric');
            this._step = 'error';
            this.render();
            this.bindEvents();
        }
    }

    // Poll the live checklist while the (blocking) apply request runs.
    _startPolling() {
        this._stopPolling();
        this._pollTimer = setInterval(async () => {
            try {
                const status = await BackendClient.getSetupStatus();
                const el = this.container.querySelector('#setup-checklist');
                if (el && status.steps) el.innerHTML = this._renderChecklist(status.steps);
            } catch (_e) {
                // Transient while the backend is busy — ignore and retry.
            }
        }, 800);
    }

    _stopPolling() {
        if (this._pollTimer) {
            clearInterval(this._pollTimer);
            this._pollTimer = null;
        }
    }

    // Skip the wizard: mark setup complete server-side with no actions, and
    // dismiss it persistently for this browser (the startup gate re-shows the
    // wizard while steps are missing unless this flag is set).
    async _skip() {
        try {
            localStorage.setItem('mw_setup_dismissed', '1');
        } catch (_e) {
            /* best-effort */
        }
        try {
            await BackendClient.applySetup({ internet_access_authorized: false, sunshine: {} });
        } catch (_e) {
            /* best-effort */
        }
        this._finish();
    }

    _finish() {
        this._stopPolling();
        // Streaming needs a trusted TLS origin. The wizard normally already runs
        // over https://, but if it was reached over http:// switch now — the user
        // accepts the self-signed cert once here, then the host list works.
        // Same host, HTTPS port (omit :443).
        if (window.location.protocol === 'https:') {
            window.location.href = '/';
            return;
        }
        const port = this._httpsPort && this._httpsPort !== 443 ? ':' + this._httpsPort : '';
        window.location.href = 'https://' + window.location.hostname + port + '/';
    }

    esc(text) {
        return escapeHtml(text);
    }
}
