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
        // Keeping the display awake rewrites the user's own power settings, so it
        // is opt-in like Internet Access — never pre-ticked, and only offered at
        // all when the backend says this desktop exposes the knobs.
        this._keepDisplayAwake = false;
        this._displaySleepSupported = false;
        this._displayKeptAwake = false;
        this._displaySleepError = '';

        // Sunshine credentials, held here so they survive the re-render that a
        // checkbox toggle triggers. A fresh install is provisioned with
        // admin/admin, so both come prefilled and the password shows in clear —
        // the user has to be able to read what is about to be applied. It turns
        // into a real password field on the first edit and stays that way, the
        // same one-way rule the Windows installer applies (SunshinePassChange in
        // backend/installer/moonlightweb.iss).
        this._userValue = '';
        this._passValue = '';
        this._passMasked = true;

        // True while the typed credentials are being tried against the Sunshine
        // already installed here (the config step waits, it does not advance).
        this._checking = false;

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
            this._displaySleepSupported = !!(
                status.display_sleep && status.display_sleep.supported
            );
            this._displayKeptAwake = !!(status.display_sleep && status.display_sleep.kept_awake);
            this._internetActive = !!(status.internet && status.internet.active);
            this._domain = (status.internet && status.internet.domain) || '';
            this._httpsPort = status.https_port || 443;
            // The backend says whether it can auto-install Sunshine here (macOS
            // DMG, or Linux .deb on Debian/Ubuntu-family distros with polkit).
            this._canAutoInstall = !!(status.sunshine && status.sunshine.can_auto_install);
            // Default the install checkbox off when Sunshine is already present or
            // cannot be auto-installed on this OS.
            this._installSunshine = this._canAutoInstall && !this._sunshineInstalled;
            // Prefill only when we are the ones creating the account. An already
            // installed Sunshine has credentials we don't know, so those fields
            // start empty — and masked, since nothing is there to be read.
            const fresh = !this._sunshineInstalled;
            this._userValue = fresh ? 'admin' : '';
            this._passValue = fresh ? 'admin' : '';
            this._passMasked = !fresh;
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

        // Display-sleep section: the whole section disappears on a desktop whose
        // settings we can't reach, rather than offering a box that would do
        // nothing. Already configured → a green "done" row, no control.
        let displayBlock = '';
        if (this._displaySleepSupported) {
            displayBlock = `
            <div class="setup-section">
                <h2 class="setup-section-title">${t('setup.displayTitle')}</h2>
                ${
                    this._displayKeptAwake
                        ? this._okNote(t('setup.displayAlreadyAwake'))
                        : `
                <p class="setup-note">${t('setup.displayBody')}</p>
                <label class="setup-check">
                    <input type="checkbox" id="chk-display-awake" ${
                        this._keepDisplayAwake ? 'checked' : ''
                    } />
                    <span>${t('setup.displayOption')}</span>
                </label>`
                }
            </div>`;
        }

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
            ${displayBlock}

            <div class="setup-section">
                <h2 class="setup-section-title">${t('setup.autostartTitle')}</h2>
                ${autostartBlock}
            </div>

            ${this._error ? `<p class="login-error">${this.esc(this._error)}</p>` : ''}

            <button id="btn-setup-start" class="btn btn-neutral login-submit"
                    ${this._checking ? 'disabled' : ''}>
                ${
                    this._checking
                        ? `<span class="tunnel-spinner"></span>${t('setup.checkingCreds')}`
                        : t('setup.start')
                }
            </button>
            <button id="btn-setup-skip" class="btn btn-link u-mt-2"
                    ${this._checking ? 'disabled' : ''}>${t('setup.skip')}</button>`;
    }

    // Green "already done" row shown in place of a step's controls.
    _okNote(text) {
        return `<p class="setup-note setup-ok"><span class="setup-ok-check">✓</span> ${this.esc(text)}</p>`;
    }

    // Sunshine credential fields (shared by the "installed" and "install" cases).
    _credsFields() {
        const needed = this._sunshineInstalled || this._installSunshine;
        const dis = needed && !this._checking ? '' : 'disabled';
        return `
            <div class="setup-creds">
                <div class="login-field">
                    <label class="login-label" for="setup-user">${t('setup.username')}</label>
                    <input type="text" id="setup-user" class="login-input" autocomplete="off"
                           value="${this.esc(this._userValue)}" ${dis} />
                </div>
                <div class="login-field">
                    <label class="login-label" for="setup-pass">${t('setup.password')}</label>
                    <input type="${this._passMasked ? 'password' : 'text'}" id="setup-pass"
                           class="login-input" autocomplete="off"
                           value="${this.esc(this._passValue)}" ${dis} />
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
        // The display setting is silent when it worked (the checkbox said what it
        // would do) but must speak up when it didn't: the user would otherwise
        // hit the 503 capture dialog later believing it was handled.
        const displayLine = this._displaySleepError
            ? `<p class="setup-note setup-warn">${t('setup.displayFailed', {
                  error: this.esc(this._displaySleepError),
              })}</p>`
            : '';
        return `
            <p class="login-subtitle">${t('setup.doneTitle')}</p>
            ${domainLine}
            ${permsLine}
            ${displayLine}
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
            const userEl = this.container.querySelector('#setup-user');
            if (userEl) {
                userEl.addEventListener('input', () => {
                    this._userValue = userEl.value;
                });
            }
            const passEl = this.container.querySelector('#setup-pass');
            if (passEl) {
                passEl.addEventListener('input', () => {
                    this._passValue = passEl.value;
                    if (this._passMasked) return;
                    // First edit: the prefilled default was the only thing worth
                    // showing, so mask from here on and never go back. Flip the
                    // element in place — re-rendering would drop the caret.
                    this._passMasked = true;
                    passEl.type = 'password';
                });
            }
            // No re-render on toggle: nothing else in the form depends on it, and
            // the value is read back in _apply() anyway. Kept in sync so a render
            // triggered elsewhere (e.g. the install checkbox) preserves the tick.
            const chkDisplay = this.container.querySelector('#chk-display-awake');
            if (chkDisplay) {
                chkDisplay.addEventListener('change', () => {
                    this._keepDisplayAwake = chkDisplay.checked;
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
        this._keepDisplayAwake =
            this._displaySleepSupported &&
            !this._displayKeptAwake &&
            !!this.container.querySelector('#chk-display-awake')?.checked;
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

        // A Sunshine that was already here has credentials we don't know, and a
        // wrong pair only shows up much later as a failed PIN push. Try them
        // against it now, with the button held on a spinner: the user stays on
        // this page with a real error instead of a broken pairing, and can fix
        // the fields or skip the Sunshine step altogether. A fresh install is not
        // probed — those credentials are the ones we are about to create.
        if (needPairing && haveCreds && !(await this._verifyCreds(user, pass))) return;

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
                keep_display_awake: this._keepDisplayAwake,
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
            this._displaySleepError = result.display_sleep_error || '';
            if (result.display_kept_awake) this._displayKeptAwake = true;
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
            // The A record is published but the TLS certificate order is still
            // running — it only gets the backend's event loop back now that
            // /apply has returned. Keep the checklist live until the domain is
            // actually usable: declaring success here hands the user a URL their
            // browser rejects, since the domain is served with the self-signed
            // fallback until the order lands.
            if (result.certificate_pending) {
                await this._awaitStep('arecord', 180000);
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

    // Ask the backend to open the local Sunshine with these credentials. Returns
    // true when the wizard may proceed; otherwise it has already re-rendered the
    // config step with the reason. The re-render is harmless: every field's value
    // lives on `this`, so nothing the user typed is lost.
    async _verifyCreds(user, pass) {
        this._checking = true;
        this._error = '';
        this.render();
        this.bindEvents();

        let result;
        try {
            result = await BackendClient.checkSunshineCredentials(user, pass);
        } catch (err) {
            console.error('[Setup] credential check failed:', err);
            result = { ok: false, reason: 'unreachable' };
        }
        this._checking = false;
        if (result && result.ok) return true;

        this._error =
            result && result.reason === 'unauthorized'
                ? t('setup.sunshineCredsWrong')
                : t('setup.sunshineUnreachable');
        this.render();
        this.bindEvents();
        return false;
    }

    // Poll the checklist until `key` reaches a terminal state or `timeoutMs`
    // elapses, refreshing the rendered rows meanwhile. The cap matches the
    // Windows installer's own budget for the same wait: an ACME order that has
    // not landed in three minutes is not going to, and the user is better served
    // by the admin page (which shows the live certificate state) than by a
    // wizard that never ends.
    async _awaitStep(key, timeoutMs) {
        const terminal = ['done', 'failed', 'skipped'];
        const deadline = Date.now() + timeoutMs;
        while (Date.now() < deadline) {
            await new Promise((resolve) => setTimeout(resolve, 1000));
            let status;
            try {
                status = await BackendClient.getSetupStatus();
            } catch (_e) {
                continue; // transient while the backend is busy — keep waiting
            }
            const el = this.container.querySelector('#setup-checklist');
            if (el && status.steps) el.innerHTML = this._renderChecklist(status.steps);
            if (status.steps && terminal.includes(status.steps[key])) return status.steps[key];
        }
        return 'timeout';
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
