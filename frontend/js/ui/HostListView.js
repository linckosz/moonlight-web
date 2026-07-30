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
 * MoonlightWeb — Host list view component
 *
 * Single-page model: each host box carries its own state inline.
 *   - online + paired  → app grid (loaded per host, click to launch)
 *   - online + locked  → centered "Pair" action
 *   - offline + MAC    → centered "Wake on LAN" action
 *   - offline          → centered offline message
 * The online/offline badge sits next to the host name; "Remove" lives in a
 * per-host "…" menu.
 */
import { BackendClient } from '../api/BackendClient.js';
import { Host } from '../models/Host.js';
import { App } from '../models/App.js';
import { PairDialog } from './PairDialog.js';
import { Toast } from './Toast.js';
import { t } from '../i18n/i18n.js';
import { Icons } from './icons.js';
import { escapeHtml } from '../util/escapeHtml.js';

/**
 * Update progress is driven by a local animation, not by the poll samples.
 *
 * The host reports a figure every ~900 ms at best, and stops reporting at all
 * the moment the installer kills it — a bar painted straight from those samples
 * reads as "jump, jump, freeze". Instead each phase owns a slice of the bar and
 * eases towards its ceiling without ever reaching it, so the bar is always
 * moving; a figure coming from the host only ever pushes it forward.
 *
 * `tau` is the exponential time constant in seconds: ~63 % of the remaining
 * distance to the ceiling is covered in that time.
 */
const UPDATE_PHASES = {
    // Seconds on any usable link — a small slice, quickly filled.
    downloading: { ceiling: 30, tau: 4 },
    // The host flips to `restarting` as soon as it has spawned the installer,
    // but it is the install that is actually running (and it still answers us),
    // so both share one slow budget.
    installing: { ceiling: 90, tau: 18 },
    restarting: { ceiling: 90, tau: 18 },
    // Host gone: nothing left to poll but /api/health. Slightly brisker — this
    // is the tail, and the bar should read as nearly there when the new build
    // answers and the page reloads.
    finalizing: { ceiling: 99, tau: 16 },
};

export class HostListView {
    // Backoff before re-attempting an app-list fetch that failed transiently
    // (host momentarily unreachable — e.g. remote path warming up post-reboot).
    static APP_LIST_RETRY_MS = 4000;

    // Update-banner cadence. Asking the host is free — it answers from a cache it
    // refreshes on its own timer (UpdateChecker::kCacheHours), never per request —
    // so 30 min just bounds how long a banner can lag behind that cache. MIN_MS
    // floors the extra checks fired
    // by visibilitychange (a phone can wake this page many times a minute).
    static UPDATE_CHECK_MS = 30 * 60 * 1000;
    static UPDATE_CHECK_MIN_MS = 5 * 60 * 1000;

    constructor(container) {
        this.container = container;
        this.hosts = [];
        this.pollTimer = null;
        this.eventsBound = false;
        this.onLaunchApp = null; // called with (host, app) when an app card is clicked
        // Per-host app cache:
        //   { apps: App[] }              → loaded
        //   { loading: true }            → request in flight
        //   { error, sticky: true }      → definitive rejection (not paired / not found)
        //   { error }                    → transient failure (unreachable) → auto-retried
        this.appsByHost = {};
        // Pending one-shot app-list retry timers, keyed by host uuid.
        this._appRetryTimers = {};
        this._active = false;
        this._destroyed = false;
        // Update banner: the cached GitHub-Releases result, and whether an update
        // is currently being applied on the host (which then owns the banner).
        this._updateCheckedAt = 0;
        this._updateTimer = null;
        // Bounded fast retries while the host's first GitHub fetch is in flight.
        this._updateWarmupTries = 0;
        this._updateInfo = null;
        this._updateBusy = false;
        this._hostsLoaded = false;
        this.renderShell();

        // Auto-scan when page gains focus. Also re-ask about updates: a phone
        // keeps this page alive for days behind a locked screen, and coming back
        // to it is the moment the user would actually read a banner.
        this._visibilityHandler = () => {
            if (document.hidden || this._destroyed) return;
            this._autoScan();
            this._checkForUpdate();
        };
        document.addEventListener('visibilitychange', this._visibilityHandler);

        // Close any open kebab menu when clicking outside of it.
        this._docClickHandler = (e) => {
            if (this._destroyed) return;
            if (!e.target.closest('.host-card-menu')) this._closeAllMenus();
        };
        document.addEventListener('click', this._docClickHandler);

        // Button delegation — bound once, cleaned up in destroy()
        this._clickHandler = (e) => {
            if (this._destroyed) return;

            // ── Update banner: apply on the host ───────────────────────────
            if (e.target.closest('.update-banner-apply')) {
                this._startUpdate();
                return;
            }

            // ── Update banner: close without silencing the version ─────────
            // The ✕ on a *failure* only clears the row. Recording the version as
            // dismissed there would make a failed update bury its own retry
            // button: the banner would never come back for that release, so the
            // only way to try again would be to clear localStorage by hand.
            if (e.target.closest('.update-banner-close')) {
                const banner = this.container.querySelector('#update-banner');
                if (banner) {
                    banner.hidden = true;
                    banner.innerHTML = '';
                    this._stopProgressAnimation();
                    this._progressFill = null;
                }
                // Only for this page load — a reload offers the update again.
                this._updateInfo = null;
                return;
            }

            // ── Update banner dismiss ──────────────────────────────────────
            const dismissBtn = e.target.closest('.update-banner-dismiss');
            if (dismissBtn) {
                // Remember the dismissed version so only a newer release re-shows.
                localStorage.setItem('mw_update_dismissed', dismissBtn.dataset.version || '');
                // Drop the cached result too, or the next host refresh would
                // repaint the banner we were just asked to hide.
                this._updateInfo = null;
                const banner = this.container.querySelector('#update-banner');
                if (banner) {
                    banner.hidden = true;
                    banner.innerHTML = '';
                    this._stopProgressAnimation();
                    this._progressFill = null;
                }
                return;
            }

            // ── Kebab menu toggle ──────────────────────────────────────────
            const menuBtn = e.target.closest('.btn-host-menu');
            if (menuBtn) {
                e.stopPropagation();
                const card = menuBtn.closest('.host-card');
                const menu = card && card.querySelector('.host-menu');
                if (!menu) return;
                const wasHidden = menu.hasAttribute('hidden');
                this._closeAllMenus();
                if (wasHidden) {
                    menu.removeAttribute('hidden');
                    menuBtn.setAttribute('aria-expanded', 'true');
                }
                return;
            }

            const pairBtn = e.target.closest('.btn-pair');
            if (pairBtn) {
                const uuid = pairBtn.dataset.uuid;
                const host = this.hosts.find((h) => h.uuid === uuid);
                if (host) {
                    this.stop();
                    const dialog = new PairDialog(host);
                    dialog.onComplete = () => {
                        this.start(); // start() already calls refresh()
                    };
                    dialog.onCancel = () => this.start();
                    dialog.show();
                }
                return;
            }

            const wolBtn = e.target.closest('.btn-wol');
            if (wolBtn) {
                const uuid = wolBtn.dataset.uuid;
                const host = this.hosts.find((h) => h.uuid === uuid);
                wolBtn.disabled = true;
                BackendClient.wakeHost(uuid)
                    .then(() => {
                        Toast.show(
                            t('hosts.wolSent', { name: host ? host.displayName : uuid }),
                            'success',
                        );
                    })
                    .catch((err) => {
                        console.error('[MW] Wake-on-LAN failed:', err);
                        Toast.show(err.message, 'error');
                    })
                    .finally(() => {
                        wolBtn.disabled = false;
                    });
                return;
            }

            const removeBtn = e.target.closest('.btn-remove');
            if (removeBtn) {
                removeBtn.disabled = true;
                removeBtn.textContent = t('common.removing');
                const uuid = removeBtn.dataset.uuid;
                BackendClient.removeHost(uuid)
                    .then(() => {
                        this.hosts = this.hosts.filter((h) => h.uuid !== uuid);
                        delete this.appsByHost[uuid];
                        this.renderList();
                    })
                    .catch((err) => {
                        console.error('[MW] Remove host failed:', err);
                        this.refresh();
                    });
                return;
            }

            // ── App card launch ───────────────────────────────────────────
            const appCard = e.target.closest('.app-card');
            if (appCard) {
                // A launch already in flight blocks ALL app cards (not just the
                // launching one): a second click would start a concurrent
                // session and take over the one being launched.
                if (this.container.querySelector('.app-card--launching')) return;
                const hostCard = appCard.closest('.host-card');
                const uuid = hostCard && hostCard.dataset.uuid;
                const host = this.hosts.find((h) => h.uuid === uuid);
                const entry = this.appsByHost[uuid];
                const appId = parseInt(appCard.dataset.appId, 10);
                const app = entry && entry.apps && entry.apps.find((a) => a.id === appId);
                if (host && app && this.onLaunchApp) {
                    this.setLaunching(appCard);
                    this.stop(); // pause polling so a refresh can't wipe the launching card
                    this.onLaunchApp(host, app);
                }
            }
        };
        this.container.addEventListener('click', this._clickHandler);

        // App-card keyboard activation (Enter/Space) — cards are role="button".
        this._keydownHandler = (e) => {
            if (this._destroyed) return;
            if (e.key !== 'Enter' && e.key !== ' ') return;
            const appCard = e.target.closest && e.target.closest('.app-card');
            if (!appCard) return;
            e.preventDefault();
            appCard.click();
        };
        this.container.addEventListener('keydown', this._keydownHandler);
    }

    // --- Public API ---

    start() {
        this._active = true;
        this._autoScan();
        this.scheduleNextPoll(0); // immediate first refresh
        this._checkForUpdate(); // discreet "new version available" banner
        this._scheduleUpdateCheck();
    }

    // --- Update banner ---

    // Re-ask periodically so a page left open for days still learns about a
    // release. Cheap: the backend answers from its own cache.
    _scheduleUpdateCheck() {
        if (this._updateTimer || this._destroyed) return;
        this._updateTimer = setInterval(() => {
            if (this._destroyed || !this._active) return;
            this._checkForUpdate();
        }, HostListView.UPDATE_CHECK_MS);
    }

    // Ask the backend (cached GitHub Releases check) whether a newer MoonlightWeb
    // build exists and, if so, paint a discreet dismissible banner at the top of
    // the Hosts view. Failures stay silent.
    //
    // Not a one-shot: the backend's own check is stale-while-revalidate, so the
    // very first answer after its boot is "no update" even when there is one.
    // Asking once per page load meant a long-lived tab could never see a banner.
    async _checkForUpdate() {
        // Nothing to learn while the banner shows an update in flight, and don't
        // re-ask more often than the backend could plausibly change its answer.
        if (this._updateBusy) return;
        const now = Date.now();
        if (now - this._updateCheckedAt < HostListView.UPDATE_CHECK_MIN_MS) return;
        this._updateCheckedAt = now;
        const info = await BackendClient.checkForUpdate();
        if (this._destroyed) return;
        // `checked_at` empty means the host has never completed a check — its
        // fetch is in flight right now and this "no update" is a placeholder.
        // Don't wait for the next tick to find out.
        if (info && !info.checked_at && this._updateWarmupTries < 3) {
            this._updateWarmupTries++;
            this._updateCheckedAt = 0;
            setTimeout(() => {
                if (!this._destroyed && this._active) this._checkForUpdate();
            }, 10000);
            return;
        }
        if (!info || !info.update_available) return;
        // Respect a prior dismissal — but only for that exact version, so a newer
        // release re-surfaces the banner.
        if (localStorage.getItem('mw_update_dismissed') === info.latest) return;
        this._updateInfo = info;
        this._renderUpdateBanner();
    }

    /**
     * Whether this browser can have the update applied for it.
     *
     * The installer runs on the HOST, never here — offering a download link to a
     * phone streaming from the other side of the house would be a dead end. So
     * the banner only ever offers an action when the host can carry it out
     * itself, which needs two things:
     *   - the machine running MoonlightWeb is paired with its local Sunshine.
     *     That is the "already set up" marker: the installer has nothing left to
     *     ask (no credentials, no pairing), so it can run unattended.
     *   - the platform has an unattended elevation path at all (self_update.
     *     supported — see SelfUpdater).
     * Anything else degrades to "go update the host PC".
     */
    _canSelfUpdate() {
        const cap = this._updateInfo && this._updateInfo.self_update;
        if (!cap || !cap.supported) return false;
        return this.hosts.some((h) => h.isLocalHost && h.isPaired);
    }

    _renderUpdateBanner() {
        // An update in flight owns the banner (progress bar); never repaint over it.
        if (this._updateBusy) return;
        // Wait for the host list: painting "update the host PC by hand" and then
        // flipping it to a one-click button a second later reads as a glitch.
        if (!this._hostsLoaded) return;
        const info = this._updateInfo;
        const banner = this.container.querySelector('#update-banner');
        if (!banner || !info) return;

        const version = this.esc(info.latest);
        const notes = info.release_url || '';
        const canUpdate = this._canSelfUpdate();
        // Whatever this replaces, the progress row's nodes are gone with it.
        this._stopProgressAnimation();
        this._progressFill = null;
        banner.innerHTML = `
            <span class="update-banner-icon" aria-hidden="true">${Icons.power}</span>
            <span class="update-banner-text">${
                canUpdate
                    ? t('update.available', { version })
                    : t('update.availableManual', { version })
            }</span>
            <span class="update-banner-actions">
                ${
                    canUpdate
                        ? `<button class="btn btn-secondary update-banner-apply">${t('update.updateNow')}</button>`
                        : ''
                }
                ${
                    notes
                        ? `<a class="update-banner-notes" href="${this.esc(notes)}"
                              target="_blank" rel="noopener noreferrer">${t('update.releaseNotes')}</a>`
                        : ''
                }
            </span>
            <button class="update-banner-dismiss" data-version="${version}"
                    aria-label="${this.esc(t('update.dismiss'))}">${Icons.close || '✕'}</button>
        `;
        banner.hidden = false;
    }

    // --- Applying the update (host-side, watched from here) ---

    // Repaint the banner as a progress row. No dismiss button: the host is being
    // replaced under us, hiding the bar would only lose the user.
    //
    // The row is built once and then updated in place — re-rendering the markup
    // on every poll would hand the animation a brand new element each time, with
    // no width to continue from.
    _renderUpdateProgress(status) {
        const banner = this.container.querySelector('#update-banner');
        if (!banner) return;

        if (!this._progressFill || !banner.contains(this._progressFill)) {
            banner.innerHTML = `
                <span class="update-banner-icon" aria-hidden="true">${Icons.power}</span>
                <span class="update-banner-text"></span>
                <span class="update-banner-progress" role="progressbar"
                      aria-valuenow="0" aria-valuemin="0" aria-valuemax="100">
                    <span class="update-banner-progress-fill" style="width:0%"></span>
                </span>
            `;
            this._progressTrack = banner.querySelector('.update-banner-progress');
            this._progressFill = banner.querySelector('.update-banner-progress-fill');
            this._progressLabel = banner.querySelector('.update-banner-text');
            this._progressShown = 0;
            this._progressFloor = 0;
        }
        banner.hidden = false;

        let label;
        if (status.requires_host_confirmation && status.state !== 'downloading')
            label = t('update.confirmOnHost');
        else if (status.state === 'downloading') label = t('update.downloading');
        else if (status.state === 'installing') label = t('update.installing');
        else label = t('update.restarting');
        if (this._progressLabel.textContent !== label) this._progressLabel.textContent = label;

        this._progressPhase = UPDATE_PHASES[status.state] ? status.state : 'installing';
        // The host's own figure is a floor, never a setpoint: it may be stale by
        // up to a poll, and it must not drag the bar backwards.
        const pct = Math.max(0, Math.min(100, status.percent || 0));
        if (pct > this._progressFloor) this._progressFloor = pct;
        this._startProgressAnimation();
    }

    // Ease the bar towards the ceiling of its phase, one frame at a time.
    _startProgressAnimation() {
        if (this._progressRaf) return;
        this._progressAt = performance.now();
        const step = () => {
            this._progressRaf = null;
            if (this._destroyed || !this._progressFill) return;
            const now = performance.now();
            // Clamp: a backgrounded tab stops firing frames, and the catch-up
            // step on return should not slam the bar into its ceiling.
            const dt = Math.min((now - this._progressAt) / 1000, 1);
            this._progressAt = now;

            const phase = UPDATE_PHASES[this._progressPhase] || UPDATE_PHASES.installing;
            let value = this._progressShown;
            // Catching up to a figure the host reported is still a slide, not a
            // snap — the download typically lands whole in the very first poll.
            if (value < this._progressFloor)
                value += (this._progressFloor - value) * (1 - Math.exp(-dt / 0.6));
            if (value < phase.ceiling)
                value += (phase.ceiling - value) * (1 - Math.exp(-dt / phase.tau));
            this._progressShown = Math.min(value, phase.ceiling);

            this._paintProgress(this._progressShown);
            this._progressRaf = requestAnimationFrame(step);
        };
        this._progressRaf = requestAnimationFrame(step);
    }

    _stopProgressAnimation() {
        if (this._progressRaf) cancelAnimationFrame(this._progressRaf);
        this._progressRaf = null;
    }

    _paintProgress(value) {
        const pct = Math.max(0, Math.min(100, value));
        this._progressFill.style.width = `${pct.toFixed(1)}%`;
        if (this._progressTrack)
            this._progressTrack.setAttribute('aria-valuenow', String(Math.round(pct)));
    }

    _renderUpdateFailed(message) {
        this._updateBusy = false;
        this._stopProgressAnimation();
        this._progressFill = null;
        const banner = this.container.querySelector('#update-banner');
        if (!banner) return;
        banner.innerHTML = `
            <span class="update-banner-icon" aria-hidden="true">${Icons.power}</span>
            <span class="update-banner-text">${t('update.failed')}${
                message ? ` — ${this.esc(message)}` : ''
            }</span>
            <button class="update-banner-close"
                    aria-label="${this.esc(t('update.dismiss'))}">${Icons.close || '✕'}</button>
        `;
        banner.hidden = false;
    }

    async _startUpdate() {
        if (this._updateBusy) return;
        this._updateBusy = true;
        // Fresh bar even if a previous attempt failed halfway up.
        this._stopProgressAnimation();
        this._progressFill = null;
        this._renderUpdateProgress({ state: 'downloading', percent: 0 });
        try {
            await BackendClient.startUpdate();
        } catch (err) {
            this._renderUpdateFailed(err.message);
            return;
        }
        this._watchUpdate();
    }

    /**
     * Follow the update to its end, across the moment the server disappears.
     *
     * Phase 1 polls /api/update/status for real progress. The installer then
     * replaces (and on Windows kills) that very server, so the polling failing is
     * the expected transition, not an error. Phase 2 waits for the new build to
     * answer /api/health and reloads the app on it.
     */
    async _watchUpdate() {
        const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
        const info = this._updateInfo || {};

        // Phase 1 — the host is still answering. Bounded: on the platforms whose
        // installer needs a prompt on the host desktop, nothing kills this server
        // and an unanswered dialog would keep us polling forever.
        let alive = true;
        for (let i = 0; i < 600 && !this._destroyed && alive; i++) {
            await sleep(900);
            let status;
            try {
                status = await BackendClient.getUpdateStatus();
            } catch (_) {
                alive = false; // server gone: the installer took it down
                break;
            }
            if (status.state === 'failed') {
                this._renderUpdateFailed(status.error);
                return;
            }
            // `idle` from a server that we told to update means we are already
            // talking to its freshly started replacement — phase 2's version
            // check is what settles that.
            if (status.state === 'idle') {
                alive = false;
                break;
            }
            this._renderUpdateProgress(status);
        }
        if (this._destroyed) return;
        if (alive) {
            // Still up after the whole budget — the install never went through.
            this._renderUpdateFailed(t('update.timeout'));
            return;
        }

        // Phase 2 — wait for the new build. Generous budget: a Windows install
        // plus the server rebinding its ports can take a couple of minutes, and
        // an elevation prompt on the host desktop may be waiting for someone.
        // Nothing reports progress any more, so the bar rides on its own easing
        // from wherever phase 1 left it — no jump to a fixed 98 % that would
        // then sit still for the whole restart.
        this._renderUpdateProgress({ state: 'finalizing', percent: 0 });
        for (let i = 0; i < 200 && !this._destroyed; i++) {
            await sleep(3000);
            const health = await BackendClient.probeHealth();
            // Wait for a version that is no longer the one we started from. The
            // old server can keep answering for a few seconds (or phase 1 may
            // have broken on a transient error), and reloading onto it would
            // silently look like the update did nothing.
            if (health && health.version && health.version !== info.current) {
                localStorage.removeItem('mw_update_dismissed');
                // Close the bar before the reload wipes it — the last thing seen
                // should be a full bar, not one frozen at 96 %.
                this._stopProgressAnimation();
                if (this._progressFill) this._paintProgress(100);
                location.reload();
                return;
            }
        }
        if (!this._destroyed) this._renderUpdateFailed(t('update.timeout'));
    }

    async _autoScan() {
        if (!this._active) return;
        try {
            await BackendClient.scanHosts();
        } catch (err) {
            // Silent — scan is best-effort, errors are expected
        }
    }

    stop() {
        this._active = false;
        if (this.pollTimer) {
            clearTimeout(this.pollTimer);
            this.pollTimer = null;
        }
        if (this._updateTimer) {
            clearInterval(this._updateTimer);
            this._updateTimer = null; // re-armed by start()
        }
        for (const uuid of Object.keys(this._appRetryTimers)) {
            clearTimeout(this._appRetryTimers[uuid]);
        }
        this._appRetryTimers = {};
    }

    destroy() {
        this._active = false;
        this._destroyed = true;
        this.stop();
        this._stopProgressAnimation();
        this._progressFill = null;
        if (this._clickHandler) {
            this.container.removeEventListener('click', this._clickHandler);
            this._clickHandler = null;
        }
        if (this._keydownHandler) {
            this.container.removeEventListener('keydown', this._keydownHandler);
            this._keydownHandler = null;
        }
        if (this._docClickHandler) {
            document.removeEventListener('click', this._docClickHandler);
            this._docClickHandler = null;
        }
        if (this._visibilityHandler) {
            document.removeEventListener('visibilitychange', this._visibilityHandler);
            this._visibilityHandler = null;
        }
    }

    scheduleNextPoll(delay = 3000) {
        if (!this._active || this._destroyed) return;
        this.pollTimer = setTimeout(async () => {
            if (!this._active || this._destroyed) return;
            await this.refresh();
            // Don't re-schedule if stop() was called during refresh
            if (this._active && !this._destroyed) this.scheduleNextPoll();
        }, delay);
    }

    async refresh() {
        if (!this._active) return;
        try {
            const data = await BackendClient.getHosts();
            // Re-check after await: stop()/destroy() may have run while the
            // request was in flight (e.g. a stream was launched) — a late
            // render here would overwrite the current view.
            if (!this._active || this._destroyed) return;
            const serverHosts = data.hosts || [];
            const before = this._fingerprint();
            // Merge — add/update, never remove
            for (const h of serverHosts) {
                const host = new Host(h);
                const idx = this.hosts.findIndex((ex) => ex.uuid === host.uuid);
                if (idx >= 0) this.hosts[idx] = host;
                else this.hosts.push(host);
            }
            const firstLoad = !this._hostsLoaded;
            this._hostsLoaded = true;
            const after = this._fingerprint();
            if (before !== after) this.renderList();
            // The update banner's shape depends on the local host's pair state,
            // so it can only be decided once the list is known — including the
            // "no hosts at all" case, which no fingerprint change signals.
            if (firstLoad || before !== after) this._renderUpdateBanner();
        } catch (err) {
            console.error('[MW] Failed to refresh hosts:', err);
            // Show error only on first load; keep existing data on refresh
            if (this.hosts.length === 0) this.renderError(err.message);
        }
    }

    // Stable fingerprint of host list — skips re-render when nothing changed
    _fingerprint() {
        return JSON.stringify(
            this.hosts.map((h) => [
                h.uuid,
                h.state,
                h.reachable,
                h.pairState,
                h.name,
                h.activeAddress,
                h.port,
                h.gpuModel,
                h.displayModes,
            ]),
        );
    }

    _cardFingerprint(host) {
        return [
            host.uuid,
            host.state,
            host.reachable,
            host.pairState,
            host.name,
            host.activeAddress,
            host.port,
            host.gpuModel,
            host.macAddress,
            JSON.stringify(host.displayModes),
        ].join('|');
    }

    // --- Rendering ---

    // Render the outer shell once (header + empty list container)
    renderShell() {
        this.container.innerHTML = `
            <div class="hosts-view" id="view-hosts">
                <div class="update-banner" id="update-banner" hidden></div>
                <div class="hosts-header">
                    <h2>${t('hosts.title')}</h2>
                    <div class="hosts-actions">
                        <button class="btn btn-neutral" id="btn-manual">${t('hosts.addManually')}</button>
                    </div>
                </div>
                <div class="hosts-list" id="hosts-list"></div>
            </div>
        `;
        this.bindEvents();
    }

    // Patch only the host cards — never touches the header
    renderList() {
        const list = this.container.querySelector('#hosts-list');
        if (!list) return;

        // Empty state
        if (this.hosts.length === 0) {
            list.innerHTML = `
                <div class="hosts-empty">
                    <span class="empty-icon">\u{1F5B4}</span>
                    <p>${t('hosts.empty')}</p>
                    <p class="hint">${t('hosts.emptyHint')}</p>
                </div>`;
            return;
        }

        // Remove empty placeholder if present
        const empty = list.querySelector('.hosts-empty');
        if (empty) empty.remove();

        // Paired hosts first — stable sort keeps discovery order within groups.
        this.hosts.sort((a, b) => Number(b.isPaired) - Number(a.isPaired));

        const currentUuids = new Set(this.hosts.map((h) => h.uuid));

        // Remove cards for hosts no longer in the list
        const existingCards = list.querySelectorAll('.host-card');
        for (const card of existingCards) {
            if (!currentUuids.has(card.dataset.uuid)) card.remove();
        }

        // Insert or update cards in order
        for (let i = 0; i < this.hosts.length; i++) {
            const host = this.hosts[i];
            const fp = this._cardFingerprint(host);
            let card = list.querySelector(`.host-card[data-uuid="${host.uuid}"]`);

            if (!card) {
                // New host — create the card, positioned below
                const tmp = document.createElement('div');
                tmp.innerHTML = this.renderCard(host);
                card = tmp.firstElementChild;
                card.dataset.fingerprint = fp;
            } else if (card.dataset.fingerprint !== fp) {
                // Data changed — replace just this card
                const tmp = document.createElement('div');
                tmp.innerHTML = this.renderCard(host);
                const newCard = /** @type {HTMLElement} */ (tmp.firstElementChild);
                newCard.dataset.fingerprint = fp;
                card.replaceWith(newCard);
                card = newCard;
            }

            // Keep DOM order in sync with the sorted hosts array.
            const ref = list.children[i] || null;
            if (ref !== card) list.insertBefore(card, ref);
        }

        // Fill in app grids for available hosts (cache hit = instant).
        for (const host of this.hosts) {
            if (host.isAvailable) this._ensureAppsLoaded(host);
        }
    }

    renderCard(host) {
        const cls = host.statusClass;
        return `
            <div class="host-card ${cls}" data-uuid="${host.uuid}">
                <div class="host-card-head">
                    <div class="host-card-icon">
                        <span class="status-icon ${cls}">${host.statusIcon}</span>
                    </div>
                    <div class="host-card-info">
                        <div class="host-name-row">
                            <span class="host-name">${this.esc(host.displayName)}</span>
                            <span class="status-badge ${cls}">${host.statusLabel}</span>
                        </div>
                        ${
                            host.displayGpu
                                ? `<div class="host-gpu">${this.esc(host.displayGpu)}</div>`
                                : ''
                        }
                        ${
                            host.resolutionText
                                ? `<div class="host-resolution">${this.esc(host.resolutionText)}</div>`
                                : ''
                        }
                    </div>
                    <div class="host-card-menu">
                        <button class="btn-icon btn-host-menu" data-uuid="${host.uuid}"
                                aria-haspopup="true" aria-expanded="false"
                                aria-label="${this.esc(t('hosts.menuAria'))}">${Icons.menu}</button>
                        <div class="host-menu" hidden>
                            <button class="host-menu-item btn-remove" data-uuid="${host.uuid}">${t('common.remove')}</button>
                        </div>
                    </div>
                </div>
                <div class="host-card-body">
                    ${this.renderBody(host)}
                </div>
            </div>
        `;
    }

    // Body content driven by the host state.
    renderBody(host) {
        if (host.isAvailable) {
            // App grid filled asynchronously by _ensureAppsLoaded().
            return `<div class="host-apps" data-uuid="${host.uuid}"></div>`;
        }
        if (host.isLocked) {
            return `<div class="host-body-center">
                        <button class="btn btn-secondary btn-pair" data-uuid="${host.uuid}">${t('common.pair')}</button>
                    </div>`;
        }
        if (host.canWake) {
            return `<div class="host-body-center">
                        <button class="btn btn-secondary btn-wol" data-uuid="${host.uuid}"
                                title="${this.esc(t('hosts.wakeTitle'))}">${Icons.power}${t('hosts.wakeBtn')}</button>
                    </div>`;
        }
        // Reachable at the IP level but the GameStream server isn't running —
        // the machine is already awake, so there's nothing to wake.
        if (host.isUnavailable) {
            return `<div class="host-body-center host-offline-msg">${t('hosts.unavailableMessage')}</div>`;
        }
        // Offline with no usable MAC — nothing to do but show status.
        return `<div class="host-body-center host-offline-msg">${t('hosts.offlineMessage')}</div>`;
    }

    // --- Per-host app loading ---

    // Ensure the app grid for an available host is loaded and painted into its
    // card. Uses a dataset flag so a re-render (host poll) never repaints an
    // already-filled grid and clobbers a launching card.
    _ensureAppsLoaded(host) {
        const uuid = host.uuid;
        const cont = this.container.querySelector(`.host-card[data-uuid="${uuid}"] .host-apps`);
        if (!cont) return;

        const entry = this.appsByHost[uuid];

        // A definitive result (apps, or a permanent "not paired" / "not found"
        // rejection) is cached — paint it (cheap) and stop. Transient errors
        // deliberately fall through so a recovered host reloads on its own
        // instead of staying wedged behind a red banner until a manual reload.
        if (entry && (entry.apps || entry.sticky)) {
            if (cont.dataset.loaded !== '1') {
                this._paintApps(cont, entry);
                cont.dataset.loaded = '1';
            }
            return;
        }
        if (entry && entry.loading) return; // request in flight, placeholder shown

        cont.innerHTML = `<div class="host-apps-loading">${t('apps.loading')}</div>`;
        cont.dataset.loaded = '';
        this.appsByHost[uuid] = { loading: true };
        BackendClient.getAppList(uuid)
            .then((data) => {
                if (data && data.status === 'ok') {
                    this.appsByHost[uuid] = {
                        apps: (data.apps || []).map((a) => new App(a, uuid)),
                    };
                } else {
                    // The backend answered with a definitive rejection (host not
                    // paired / not found) — retrying won't change the outcome.
                    this.appsByHost[uuid] = {
                        error: (data && data.message) || 'load_failed',
                        sticky: true,
                    };
                }
            })
            .catch((err) => {
                // Network error / timeout / 5xx: the host is momentarily
                // unreachable (typical right after a host reboot while the
                // remote path warms up and the backend poll re-confirms the
                // active address). Keep it transient and schedule an auto-retry
                // so the card self-heals without a manual page reload.
                console.error('[MW] Failed to load app list:', err);
                this.appsByHost[uuid] = { error: err.message };
            })
            .finally(() => {
                if (this._destroyed) return;
                const c2 = this.container.querySelector(
                    `.host-card[data-uuid="${uuid}"] .host-apps`,
                );
                if (!c2) return;
                const e2 = this.appsByHost[uuid];
                const transient = e2 && !e2.apps && !e2.sticky;
                if (transient) {
                    // Keep the loading spinner (not the red error) while a retry
                    // is pending, then re-attempt shortly.
                    c2.innerHTML = `<div class="host-apps-loading">${t('apps.loading')}</div>`;
                    c2.dataset.loaded = '';
                    this._scheduleAppsRetry(uuid);
                } else {
                    this._paintApps(c2, e2);
                    c2.dataset.loaded = '1';
                }
            });
    }

    // Auto-retry an app-list fetch that failed transiently (unreachable host).
    // Decoupled from the host-list poll so it fires even when nothing else about
    // the host changes; one pending retry per host.
    _scheduleAppsRetry(uuid) {
        if (this._appRetryTimers[uuid]) return;
        this._appRetryTimers[uuid] = setTimeout(() => {
            delete this._appRetryTimers[uuid];
            if (this._destroyed || !this._active) return;
            const host = this.hosts.find((h) => h.uuid === uuid);
            if (!host || !host.isAvailable) return;
            const entry = this.appsByHost[uuid];
            // Only retry if still stuck on a transient error.
            if (entry && !entry.apps && !entry.sticky && !entry.loading) {
                delete this.appsByHost[uuid]; // clear transient error → re-fetch
                this._ensureAppsLoaded(host);
            }
        }, HostListView.APP_LIST_RETRY_MS);
    }

    _paintApps(cont, entry) {
        if (!entry || entry.error) {
            cont.innerHTML = `<div class="host-apps-msg host-apps-error">${t('apps.loadFailed')}</div>`;
            return;
        }
        const apps = entry.apps || [];
        if (apps.length === 0) {
            cont.innerHTML = `<div class="host-apps-msg">${t('apps.empty')}</div>`;
            return;
        }
        cont.innerHTML = `<div class="apps-grid">${apps.map((a) => this.renderApp(a)).join('')}</div>`;
        // Box-art fallback: the CSP (script-src 'self') blocks inline onerror
        // attributes, so the broken-image swap has to be wired up here.
        cont.querySelectorAll('.app-card-image img').forEach((img) => {
            const showFallback = () => {
                img.outerHTML = `<span class="app-icon">\u{1F3AE}</span>`;
            };
            if (img.complete && img.naturalWidth === 0) showFallback();
            else img.addEventListener('error', showFallback, { once: true });
        });
    }

    renderApp(app) {
        // The whole card is the launch action. role/tabindex keep it
        // keyboard-accessible (Enter/Space handled by the keydown delegate).
        return `
            <div class="app-card" data-app-id="${app.id}"
                 role="button" tabindex="0"
                 aria-label="${this.esc(t('apps.launchAria', { name: app.displayName }))}">
                <div class="app-card-image">
                    ${
                        app.boxArtUrl
                            ? `<img src="${this.esc(app.boxArtUrl)}"
                               alt="${this.esc(app.displayName)}"
                               loading="lazy">`
                            : `<span class="app-icon">\u{1F3AE}</span>`
                    }
                </div>
                <div class="app-card-name">${this.esc(app.displayName)}</div>
            </div>
        `;
    }

    // Switch a card into the "launching" state: a discreet Cyberpunk scan/glow
    // animation driven entirely by CSS, plus a status label over the name.
    setLaunching(card) {
        card.classList.add('app-card--launching');
        card.setAttribute('aria-busy', 'true');
        const nameEl = card.querySelector('.app-card-name');
        if (nameEl && !nameEl.dataset.label) {
            nameEl.dataset.label = nameEl.textContent;
            nameEl.textContent = t('apps.launching');
        }
    }

    // Revert any card stuck in the "launching" state (e.g. backend crash/timeout)
    // back to its idle appearance and restore the original app name.
    clearLaunching() {
        if (!this.container) return;
        this.container.querySelectorAll('.app-card--launching').forEach((card) => {
            card.classList.remove('app-card--launching');
            card.removeAttribute('aria-busy');
            const nameEl = card.querySelector('.app-card-name');
            if (nameEl && nameEl.dataset.label) {
                nameEl.textContent = nameEl.dataset.label;
                delete nameEl.dataset.label;
            }
        });
    }

    _closeAllMenus() {
        this.container.querySelectorAll('.host-menu:not([hidden])').forEach((m) => {
            m.setAttribute('hidden', '');
        });
        this.container
            .querySelectorAll('.btn-host-menu[aria-expanded="true"]')
            .forEach((b) => b.setAttribute('aria-expanded', 'false'));
    }

    renderError(message) {
        this.container.innerHTML = `
            <div class="hosts-view">
                <div class="hosts-header">
                    <h2>${t('hosts.title')}</h2>
                    <button class="btn btn-reload">${t('common.retry')}</button>
                </div>
                <div class="hosts-error">
                    <p>${t('hosts.connectionLost')}</p>
                    <p class="hint">${this.esc(message)}</p>
                </div>
            </div>
        `;
        this.container
            .querySelector('.btn-reload')
            .addEventListener('click', () => location.reload());
    }

    // --- Events ---

    bindEvents() {
        const manualBtn = this.container.querySelector('#btn-manual');
        if (manualBtn) {
            manualBtn.addEventListener('click', async () => {
                const addr = await this._promptManualAddress();
                if (!addr || !addr.trim()) return;
                manualBtn.disabled = true;
                manualBtn.classList.add('btn-loading');
                try {
                    const data = await BackendClient.addManualHost(addr.trim());
                    if (data.hosts && data.hosts.length > 0) {
                        const newHost = new Host(data.hosts[0]);
                        const idx = this.hosts.findIndex((h) => h.uuid === newHost.uuid);
                        if (idx >= 0) {
                            this.hosts[idx] = newHost;
                            this.renderList();
                            Toast.show(
                                t('hosts.alreadyInList', { name: newHost.displayName }),
                                'warning',
                            );
                        } else {
                            this.hosts.push(newHost);
                            this.renderList();
                            Toast.show(
                                t('hosts.addedSuccess', { name: newHost.displayName }),
                                'success',
                            );
                        }
                    } else {
                        Toast.show(t('hosts.noHostReturned'), 'error');
                    }
                } catch (err) {
                    console.error('[MW] Add host failed:', err);
                    Toast.show(err.message, 'error');
                } finally {
                    manualBtn.disabled = false;
                    manualBtn.classList.remove('btn-loading');
                }
            });
        }
    }

    /** Themed replacement for window.prompt(): asks for a host IP/hostname.
     *  Resolves with the entered string, or null on cancel/Escape/backdrop click. */
    _promptManualAddress() {
        return new Promise((resolve) => {
            // Guard against double-open (e.g. ghost click replaying on the button)
            if (document.querySelector('.manual-add-overlay')) return resolve(null);

            const overlay = document.createElement('div');
            overlay.className = 'pairing-overlay manual-add-overlay';
            overlay.innerHTML = `
                <div class="pairing-dialog">
                    <h3>${this.esc(t('hosts.addManually'))}</h3>
                    <p class="pairing-instruction">${this.esc(t('hosts.promptManual'))}</p>
                    <input type="text" class="settings-input manual-add-input"
                           placeholder="192.168.1.5" autocomplete="off"
                           autocapitalize="none" autocorrect="off" spellcheck="false" />
                    <div class="pairing-actions">
                        <button class="btn btn-secondary manual-add-cancel">${t('common.cancel')}</button>
                        <button class="btn manual-add-ok">${t('common.add')}</button>
                    </div>
                </div>
            `;

            const input = /** @type {HTMLInputElement} */ (
                overlay.querySelector('.manual-add-input')
            );
            const done = (value) => {
                overlay.remove();
                resolve(value);
            };
            overlay.querySelector('.manual-add-ok').addEventListener('click', () => {
                done(input.value.trim() || null);
            });
            overlay.querySelector('.manual-add-cancel').addEventListener('click', () => done(null));
            overlay.addEventListener('click', (e) => {
                if (e.target === overlay) done(null);
            });
            input.addEventListener('keydown', (e) => {
                if (e.key === 'Enter') done(input.value.trim() || null);
                else if (e.key === 'Escape') done(null);
            });

            document.body.appendChild(overlay);
            input.focus();
        });
    }

    // --- Helpers ---

    esc(text) {
        return escapeHtml(text);
    }
}
