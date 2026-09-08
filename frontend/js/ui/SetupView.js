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
 * Windows ships a native Inno Setup installer that authorizes Internet Access;
 * macOS/Linux ship a bare app bundle, so this in-app wizard (opened
 * automatically in the browser on first launch) covers the same ground by
 * talking to /api/setup/{status,apply}.
 *
 * Steps: config (Internet + host choices) → progress (live checklist) → done.
 *
 * Sunshine appears here only on a machine that cannot host itself. Since the
 * native engine reached macOS and Linux (design §19, §20) the usual first
 * launch has nothing to install — the app captures and encodes this desktop —
 * and the wizard says so instead of asking for a second streaming server's
 * credentials. `status.native.possible` is the whole switch, and it is
 * deliberately not `available`: macOS says false until Screen Recording is
 * granted, which is a checkbox to point at, not a reason to install Sunshine.
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
        // Can this machine stream itself? `possible` covers "it can, once the
        // user grants something" — see the file header. Until the status call
        // answers, assume it cannot: that is the shape this wizard had before
        // the native engine existed, and it offers help rather than withholding
        // it if the field is ever missing (an older server, a failed probe).
        this._nativePossible = false;
        this._nativeAvailable = false;
        this._nativeNeedsPermission = false;
        this._sunshineInstalled = false;
        this._sunshinePaired = false;
        this._autostartInstalled = false;
        this._internetActive = false;
        this._domain = '';
        this._destroyed = false;
        // The address a fresh install is actually reached at. `_domain` is the
        // legacy sub-domain and is empty on every install made since it was
        // retired, so it can no longer be the only thing this screen knows.
        this._rendezvousUrl = '';
        this._httpsPort = 443; // for the "Open MoonlightWeb" switch to HTTPS
        this._error = '';

        // User choices (config step). Internet Access is opt-in: opening the
        // machine to the Internet (a per-session UPnP mapping, an outgoing line
        // to the introduction server) requires an explicit click.
        //
        // null until one of the two buttons is pressed, and the page will not
        // apply while it is null. A checkbox nobody touched cannot be told apart
        // from a question nobody read, and this answer is recorded and stored —
        // it has to be one somebody actually gave.
        this._internetAuth = null;
        // Whether this machine offers its own screen as a host — the answer that
        // becomes `native_host_enabled`. null until one of the two buttons is
        // pressed, exactly like the Internet question above and for the same
        // reason: this is recorded, so it has to be an answer somebody gave.
        //
        // ⚠️ Saying no removes this machine's own host card and NOTHING else.
        // MoonlightWeb goes on finding and relaying the other hosts on the
        // network — a person who does not want their desktop streamed may still
        // want to reach their gaming PC from this browser.
        this._nativeAuth = null;
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

    /**
     * The address to put in front of the user, whole: the rendezvous one when
     * this instance has claimed it, the legacy sub-domain otherwise. Same order
     * as the admin page and the share links, and for the same reason — two live
     * addresses for one machine is an invitation to hand out the one that stops
     * working in February 2027. Empty when neither is known, which is what
     * `internetActiveLan` is worded for.
     */
    _publicAddress() {
        return this._rendezvousUrl || this._domain;
    }

    async start() {
        this._step = 'loading';
        this.render();
        try {
            const status = await BackendClient.getSetupStatus();
            this._os = status.os || 'Unknown';
            this._nativePossible = !!(status.native && status.native.possible);
            this._nativeAvailable = !!(status.native && status.native.available);
            this._nativeNeedsPermission = !!(status.native && status.native.needs_permission);
            this._sunshineInstalled = !!(status.sunshine && status.sunshine.installed);
            this._sunshinePaired = !!(status.sunshine && status.sunshine.paired);
            this._autostartInstalled = !!status.autostart_installed;
            this._displaySleepSupported = !!(
                status.display_sleep && status.display_sleep.supported
            );
            this._displayKeptAwake = !!(status.display_sleep && status.display_sleep.kept_awake);
            this._internetActive = !!(status.internet && status.internet.active);
            this._domain = (status.internet && status.internet.domain) || '';
            this._rendezvousUrl =
                (status.internet && status.internet.rendezvous && status.internet.rendezvous.url) ||
                '';
            this._httpsPort = status.https_port || 443;
            // The backend says whether it can auto-install Sunshine here (macOS
            // DMG, or Linux .deb on Debian/Ubuntu-family distros with polkit).
            this._canAutoInstall = !!(status.sunshine && status.sunshine.can_auto_install);
            // Default the install checkbox off when Sunshine is already present,
            // cannot be auto-installed on this OS, or is simply not needed
            // because this machine hosts itself.
            this._installSunshine =
                this._canAutoInstall && !this._sunshineInstalled && !this._nativePossible;
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
        // Also ends the background address poll (see _fillPublicAddressLater):
        // a closed wizard must not keep asking, nor re-render a detached view.
        this._destroyed = true;
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
        // What will stream this machine. Two mutually exclusive shapes: either
        // the app itself does (nothing to install — at most a permission to
        // grant), or it cannot here and Sunshine is offered as before.
        const hostBlock = this._nativePossible
            ? this._renderNativeBlock()
            : this._renderSunshineBlock();
        // A block with nothing to say takes its title with it, rather than
        // leaving a bare heading over an empty band.
        const hostSection = hostBlock
            ? `
            <div class="setup-section">
                <h2 class="setup-section-title">${
                    this._nativePossible ? t('setup.hostTitle') : t('setup.sunshineTitle')
                }</h2>
                ${hostBlock}
            </div>`
            : '';

        const address = this._publicAddress();
        const internetBlock = this._internetActive
            ? this._okNote(
                  address
                      ? t('setup.internetActive', { domain: address })
                      : t('setup.internetActiveLan'),
              )
            : `
                <p class="setup-note">${t('setup.internetBody')}</p>
                <p class="consent-highlight">${t('setup.internetOption')}</p>
                <div class="setup-choice" role="group"
                     aria-label="${this.esc(t('setup.internetTitle'))}">
                    <button type="button" id="btn-internet-skip"
                            class="btn btn-neutral setup-choice-btn${
                                this._internetAuth === false ? ' is-chosen' : ''
                            }" aria-pressed="${this._internetAuth === false}">
                        ${t('setup.internetSkip')}
                    </button>
                    <button type="button" id="btn-internet-accept"
                            class="btn btn-neutral setup-choice-btn${
                                this._internetAuth === true ? ' is-chosen' : ''
                            }" aria-pressed="${this._internetAuth === true}">
                        ${t('setup.internetAccept')}
                    </button>
                </div>`;

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

            ${hostSection}
            ${displayBlock}

            <div class="setup-section">
                <h2 class="setup-section-title">${t('setup.autostartTitle')}</h2>
                ${autostartBlock}
            </div>

            ${this._error ? `<p class="login-error">${this.esc(this._error)}</p>` : ''}

            <button id="btn-setup-start" class="btn btn-neutral login-submit"
                    ${this._checking || this._needsAnswer() ? 'disabled' : ''}>
                ${
                    this._checking
                        ? `<span class="tunnel-spinner"></span>${t('setup.checkingCreds')}`
                        : t('setup.done')
                }
            </button>`;
    }

    // This machine CAN host itself — so it is asked whether it should, rather
    // than told that it will. Streaming one's own desktop is not a detail to
    // discover afterwards from a card that appeared on its own.
    //
    // The macOS permission line rides along: the app cannot grant screen capture
    // for the user, and a sentence here beats a host card that never appears
    // with nothing to explain it. It is shown only once the answer is yes —
    // pointing at a checkbox in System Settings would be noise for someone who
    // just said they do not want this machine streamed.
    _renderNativeBlock() {
        const permission =
            this._nativeAuth === true && !this._nativeAvailable && this._nativeNeedsPermission
                ? `<p class="setup-note setup-warn">${t('setup.hostPermission')}</p>`
                : '';
        return `
                <p class="setup-note">${t('setup.hostBody')}</p>
                <p class="consent-highlight">${t('setup.hostOption')}</p>
                <div class="setup-choice" role="group"
                     aria-label="${this.esc(t('setup.hostTitle'))}">
                    <button type="button" id="btn-host-skip"
                            class="btn btn-neutral setup-choice-btn${
                                this._nativeAuth === false ? ' is-chosen' : ''
                            }" aria-pressed="${this._nativeAuth === false}">
                        ${t('setup.hostSkip')}
                    </button>
                    <button type="button" id="btn-host-accept"
                            class="btn btn-neutral setup-choice-btn${
                                this._nativeAuth === true ? ' is-chosen' : ''
                            }" aria-pressed="${this._nativeAuth === true}">
                        ${t('setup.hostAccept')}
                    </button>
                </div>
                ${permission}`;
    }

    _renderSunshineBlock() {
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
        return sunshineBlock;
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
        const address = this._publicAddress();
        const domainLine =
            this._internetActive && address
                ? `<p class="setup-note">${t('setup.doneDomain', { domain: this.esc(address) })}</p>`
                : '';
        // macOS TCC hint, for whichever program is going to capture this screen:
        // the app itself when it hosts this Mac (the permission it is still
        // missing is the only thing between here and a working host card), or
        // Sunshine when it was actually touched this run — a fully-paired setup
        // revisit has nothing left to grant.
        let permsLine = '';
        if (this._os === 'macOS') {
            if (this._nativePossible && this._nativeNeedsPermission)
                permsLine = `<p class="setup-note setup-warn">${t('setup.donePermissionsNative')}</p>`;
            else if (
                !this._nativePossible &&
                (this._activeSteps.includes('install') || this._activeSteps.includes('pairing'))
            )
                permsLine = `<p class="setup-note setup-warn">${t('setup.donePermissions')}</p>`;
        }
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
            // The two Internet buttons re-render: the pressed one takes the
            // chosen style and Start stops being disabled.
            const choose = (value) => {
                this._internetAuth = value;
                this.render();
                this.bindEvents();
            };
            const skipNet = this.container.querySelector('#btn-internet-skip');
            if (skipNet) skipNet.addEventListener('click', () => choose(false));
            const acceptNet = this.container.querySelector('#btn-internet-accept');
            if (acceptNet) acceptNet.addEventListener('click', () => choose(true));

            // Same shape as the Internet pair: pressing one takes the chosen
            // style, and Done stops being disabled once both questions have an
            // answer.
            const chooseHost = (value) => {
                this._nativeAuth = value;
                this.render();
                this.bindEvents();
            };
            const skipHost = this.container.querySelector('#btn-host-skip');
            if (skipHost) skipHost.addEventListener('click', () => chooseHost(false));
            const acceptHost = this.container.querySelector('#btn-host-accept');
            if (acceptHost) acceptHost.addEventListener('click', () => chooseHost(true));

            const start = this.container.querySelector('#btn-setup-start');
            if (start) start.addEventListener('click', () => this._apply());
        } else if (this._step === 'done') {
            const finish = this.container.querySelector('#btn-setup-finish');
            if (finish) finish.addEventListener('click', () => this._finish());
        } else if (this._step === 'error') {
            const retry = this.container.querySelector('#btn-setup-retry');
            if (retry) retry.addEventListener('click', () => this.start());
        }
    }

    // True while the Internet question is on screen and still unanswered. An
    // instance whose link is already up is not asking anything, so it never
    // holds the page.
    _internetNeedsAnswer() {
        return !this._internetActive && this._internetAuth === null;
    }

    // Same, for "should this computer be streamable". Only asked on a machine
    // that could host itself; where it cannot, Sunshine is offered instead and
    // there is no question to hold the page.
    _hostNeedsAnswer() {
        return this._nativePossible && this._nativeAuth === null;
    }

    // Every question this page asks that has not been answered yet. This is what
    // greys out Done — and the reason there is no "skip for now" any more: both
    // questions can be answered with a no, so leaving without answering is not a
    // thing a user needs. Nothing is assumed on their behalf.
    _needsAnswer() {
        return this._internetNeedsAnswer() || this._hostNeedsAnswer();
    }

    async _apply() {
        if (this._needsAnswer()) return;
        // Steps already satisfied are rendered as "✓ done" (no controls) and
        // must not run again: their flags are forced off here.
        this._internetAuth = !this._internetActive && this._internetAuth === true;
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

        // A machine that hosts itself was never shown the Sunshine block, so
        // there is nothing to install and nothing to pair — not even when a
        // Sunshine happens to be installed here: pairing it is the hosts page's
        // job, on the user's initiative, not a first-run step.
        const willInstall =
            !this._nativePossible &&
            this._installSunshine &&
            this._canAutoInstall &&
            !this._sunshineInstalled;
        const needPairing =
            !this._nativePossible && this._sunshineInstalled && !this._sunshinePaired;
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
                // Exact agreement text displayed — recorded server-side as the
                // versioned consent record (legal traceability).
                consent_message: this._internetAuth
                    ? t('setup.internetBody') + ' / ' + t('setup.internetOption')
                    : '',
                autostart: this._autoStart,
                keep_display_awake: this._keepDisplayAwake,
                // Sent only when the question was actually asked. On a machine
                // that cannot host itself the field is absent, and the server
                // leaves the setting alone rather than recording a "no" nobody
                // said — the machine may gain the ability later (a GPU driver,
                // a macOS permission) and must not find itself switched off.
                ...(this._nativePossible ? { native_host_enabled: this._nativeAuth } : {}),
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
                this._rendezvousUrl = (result.rendezvous && result.rendezvous.url) || '';
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
            // Claiming the rendezvous address is a round-trip that only gets the
            // backend's event loop back now that /apply has returned, so it is
            // almost never in the response above. The done screen goes up
            // immediately and the address drops in when it lands — holding the
            // wizard on a finished checklist for it would read as a hang, and on
            // a fresh install this screen is where its owner learns the address.
            if (this._internetActive && !this._publicAddress()) this._fillPublicAddressLater();
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

    // Poll for the rendezvous address in the background and re-render the done
    // screen once it lands. Gives up quietly: the screen then says the link is
    // active without naming an address — what it did before the address existed
    // — and the admin page shows it live whenever the claim completes. Stops as
    // soon as the user leaves the done screen, so a closed wizard polls nothing.
    async _fillPublicAddressLater() {
        const live = () => this._step === 'done' && !this._destroyed;
        const deadline = Date.now() + 30000;
        while (Date.now() < deadline && live()) {
            await new Promise((resolve) => setTimeout(resolve, 1000));
            if (!live()) return;
            let status;
            try {
                status = await BackendClient.getSetupStatus();
            } catch (_e) {
                continue; // transient while the backend is busy — keep waiting
            }
            const url =
                (status.internet && status.internet.rendezvous && status.internet.rendezvous.url) ||
                '';
            if (!url) continue;
            this._rendezvousUrl = url;
            if (!live()) return;
            this.render();
            this.bindEvents();
            return;
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

    // ⚠️ There is no _skip() any more. It existed because the page used to make
    // claims the user could only accept — "install Sunshine", "open to the
    // Internet" — and needed a way out that agreed to nothing. Both are
    // questions now, each answerable with a no, so a third door would only be a
    // way to leave the machine in a state nobody chose. `mw_setup_dismissed` is
    // still READ by the startup gate: browsers that pressed the old button keep
    // their dismissal, and nothing writes it again.

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
