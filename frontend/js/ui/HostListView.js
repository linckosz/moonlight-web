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
import { shortGpuNames } from '../util/gpuName.js';
import { Host } from '../models/Host.js';
import { App, sortApps } from '../models/App.js';
import { PairDialog } from './PairDialog.js';
import { BackendDialog } from './BackendDialog.js';
import { ShareBoard } from './ShareBoard.js';
import { Toast } from './Toast.js';
import { t } from '../i18n/i18n.js';
import { Icons } from './icons.js';
import { appPlaceholderHtml } from './AppPlaceholder.js';
import { hostArtHtml } from './HostArt.js';
import { escapeHtml } from '../util/escapeHtml.js';
import {
    loadCachedApps,
    saveCachedApps,
    forgetCachedApps,
    appsSignature,
    boxArtWasSeen,
    noteBoxArtSeen,
} from '../util/appCache.js';
import { noteHostUse, forgetHostUse, hostUsageRanker } from '../util/hostUsage.js';
import { loadCachedHosts, saveCachedHosts } from '../util/hostListCache.js';
import { bootMark } from '../util/bootTiming.js';
import * as RemoteNav from './RemoteNav.js';
import { confirmAction } from './ConfirmDialog.js';
import { hostUpdate, canSelfUpdate } from './HostUpdate.js';

/**
 * Where the empty host list sends someone who has nothing to stream from yet.
 *
 * The project's releases page rather than a file: every platform's build is
 * behind it, the maintainers keep it current, and what the user gets is what
 * LizardByte published — no mirror of ours to go stale, and no download this
 * app started on its own.
 */
const SUNSHINE_DOWNLOAD_URL = 'https://github.com/LizardByte/Sunshine/releases/latest';

export class HostListView {
    // First backoff before re-attempting an app-list fetch that failed
    // transiently (host momentarily unreachable — e.g. remote path warming up
    // post-reboot). Each consecutive failure doubles it, up to _MAX_MS.
    //
    // The doubling is what keeps a host that is simply DOWN from being asked
    // forever at a fixed cadence: its fetch does not fail fast, it hangs until
    // the backend gives up and answers 504 ("App list request timed out"), so a
    // flat retry means one stalled request per host permanently in flight.
    static APP_LIST_RETRY_MS = 4000;
    static APP_LIST_RETRY_MAX_MS = 60000;

    // Retry ladder for a cover this browser has already seen once. Short enough
    // that a host busy for a moment fills its grid back in while the user is
    // still looking at it, few enough that a host which is really gone costs
    // three requests, not a stream of them.
    static BOX_ART_RETRY_MS = [2000, 5000, 15000];

    // How long a refused launch keeps its orange marker before it has faded
    // back to neutral. Must match the CSS animation on .app-card--launch-failed.
    static LAUNCH_FAIL_FADE_MS = 1000;

    // Update-banner cadence. Asking the host is free — it answers from a cache it
    // refreshes on its own timer (UpdateChecker::kCacheHours), never per request —
    // so 30 min just bounds how long a banner can lag behind that cache. MIN_MS
    // floors the extra checks fired
    // by visibilitychange (a phone can wake this page many times a minute).
    static UPDATE_CHECK_MS = 30 * 60 * 1000;
    // How often the host list asks a per-device host (MultiSeat) what this
    // device's seat runs: each answer costs a request to the seat's Apollo.
    static DEVICE_RUNNING_MS = 15 * 1000;
    static UPDATE_CHECK_MIN_MS = 5 * 60 * 1000;

    constructor(container) {
        this.container = container;
        this.hosts = [];
        // Which machine this page belongs to (instances.js id), set by the app
        // before start(): the host list it last answered with is drawn at once
        // under that name (hostListCache.js). Unset, nothing is remembered.
        this.memoryKey = '';
        // Whether this browser holds the server's admin rights (the app's
        // /api/auth/status reading): the virtual display's GPU is chosen on its
        // card, and a host's backend from its menu, by the administrator only.
        this.canAdminister = false;
        // The cards on screen came from that memory and the machine has not
        // answered yet: its first answer replaces them, and drops any host it
        // no longer has.
        this._hostsFromMemory = false;
        // What THIS device's seat runs, per host where each device has its own
        // (host.runningAppPerDevice): uuid -> app id, and when it was asked.
        this._deviceRunning = new Map();
        this._deviceRunningAt = new Map();
        this.pollTimer = null;
        this.eventsBound = false;
        this.onLaunchApp = null; // called with (host, app) when an app card is clicked
        // Per-host app state. `apps` is what the grid paints, and it can be on
        // screen while a fetch is still in flight — that is the whole point of
        // the remembered list:
        //   { apps: App[] }              → loaded from the host
        //   { apps, fromCache: true }    → last visit's list, not yet confirmed
        //   { pending: Promise }         → request in flight (may carry apps too)
        //   { error, sticky: true }      → definitive rejection (not paired / not found)
        //   { error }                    → transient failure (unreachable) → auto-retried
        this.appsByHost = {};
        // Pending one-shot app-list retry timers, keyed by host uuid.
        this._appRetryTimers = {};
        // Backoff state for those retries, keyed by host uuid: the delay the
        // NEXT retry will use, and the timestamp before which no automatic
        // caller may re-ask. The deadline is what stops the host poll from
        // jumping the queue — the timer alone only paces _scheduleAppsRetry.
        this._appRetryDelay = {};
        this._appRetryAt = {};
        // Cards currently fading from "launch refused" orange back to neutral,
        // each with the timer that will strip the class.
        this._launchFailTimers = new Map();
        this._active = false;
        this._destroyed = false;
        // Update banner: the cached GitHub-Releases result, and whether an update
        // is currently being applied on the host (which then owns the banner).
        this._updateCheckedAt = 0;
        this._updateTimer = null;
        // Bounded fast retries while the host's first GitHub fetch is in flight.
        this._updateWarmupTries = 0;
        this._updateInfo = null;
        this._hostsLoaded = false;
        this.renderShell();
        // An update under way (started here or from Settings) owns the banner.
        // After the shell: the banner it paints into is part of it.
        this._unsubUpdate = hostUpdate.subscribe((view) => this._paintUpdateRun(view));

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
                hostUpdate.start(this._updateInfo);
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
                    this._progressFill = null;
                }
                hostUpdate.clearFailure();
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
                    this._progressFill = null;
                }
                return;
            }

            // ── Kebab menu: its own Close (a TV remote has no Escape) ─────
            if (e.target.closest('.btn-menu-close')) {
                e.stopPropagation();
                this._closeMenuBackToButton();
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

            const permBtn = e.target.closest('.btn-mac-perm');
            if (permBtn) {
                // Opens the pane on the Mac itself: only the Mac's own browser
                // may ask (the backend answers anyone else with a refusal), so
                // a remote viewer is told where to go instead.
                const open =
                    permBtn.dataset.pane === 'accessibility'
                        ? BackendClient.openAccessibilitySettings()
                        : BackendClient.openScreenRecordingSettings();
                open.then(
                    () => Toast.show(t('hosts.nativePermOpened'), 'success'),
                    () => Toast.show(t('hosts.nativePermOnTheMac'), 'info'),
                );
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

            const stopSessionBtn = e.target.closest('.btn-stop-session');
            if (stopSessionBtn) {
                const uuid = stopSessionBtn.dataset.uuid;
                // This closes the game for everyone, including any invited
                // player — worth one confirmation.
                confirmAction(t('hosts.stopSessionConfirm'), { danger: true }).then((yes) => {
                    if (!yes) return;
                    stopSessionBtn.disabled = true;
                    stopSessionBtn.textContent = t('hosts.stopSessionWorking');
                    BackendClient.stopHostSession(uuid)
                        .catch((err) => console.error('[MW] Stop session failed:', err))
                        .finally(() => {
                            this._closeAllMenus();
                            this.refresh();
                        });
                });
                return;
            }

            const shareBtn = e.target.closest('.btn-share');
            if (shareBtn) {
                this._closeAllMenus();
                const host = this.hosts.find((h) => h.uuid === shareBtn.dataset.uuid);
                if (host) this._openShareBoard(host);
                return;
            }

            const backendBtn = e.target.closest('.btn-backend');
            if (backendBtn) {
                this._closeAllMenus();
                const host = this.hosts.find((h) => h.uuid === backendBtn.dataset.uuid);
                if (host) {
                    new BackendDialog(host, { onSaved: () => this.refresh() }).show();
                }
                return;
            }

            const renameBtn = e.target.closest('.btn-rename');
            if (renameBtn) {
                this._closeAllMenus();
                const host = this.hosts.find((h) => h.uuid === renameBtn.dataset.uuid);
                if (host) this._renameHost(host);
                return;
            }

            const restartBtn = e.target.closest('.btn-restart');
            if (restartBtn) {
                const uuid = restartBtn.dataset.uuid;
                const host = this.hosts.find((h) => h.uuid === uuid);
                // The service going down takes every stream on that host with
                // it, this browser's included — same weight as "Stop session".
                confirmAction(t('hosts.restartConfirm'), { danger: true }).then((yes) => {
                    if (!yes) return;
                    restartBtn.disabled = true;
                    restartBtn.textContent = t('hosts.restartWorking');
                    BackendClient.restartHost(uuid)
                        .then(() => {
                            Toast.show(
                                t('hosts.restartSent', { name: host ? host.displayName : uuid }),
                                'success',
                            );
                        })
                        .catch((err) => {
                            console.error('[MW] Restart host failed:', err);
                            Toast.show(err.message, 'error');
                        })
                        .finally(() => {
                            this._closeAllMenus();
                            // The host is mid-restart: let the poll repaint it as it
                            // drops offline and comes back.
                            this.refresh();
                        });
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
                        forgetCachedApps(uuid);
                        forgetHostUse(uuid);
                        this.renderList();
                    })
                    .catch((err) => {
                        console.error('[MW] Remove host failed:', err);
                        this.refresh();
                    });
                return;
            }

            // ── Quit the app a stopped stream left running (issue #24) ─────
            const quitAppBtn = e.target.closest('.app-card-quit');
            if (quitAppBtn) {
                const hostCard = quitAppBtn.closest('.host-card');
                const uuid = hostCard && hostCard.dataset.uuid;
                const host = this.hosts.find((h) => h.uuid === uuid);
                const entry = this.appsByHost[uuid];
                const appId = parseInt(quitAppBtn.dataset.appId, 10);
                const app = entry && entry.apps && entry.apps.find((a) => a.id === appId);
                const name = app ? app.displayName : '';
                // Closes the game on the host, for everyone on it: one
                // confirmation, as Moonlight asks.
                confirmAction(t('apps.quitConfirm', { name }), { danger: true }).then((yes) => {
                    if (!yes) return;
                    quitAppBtn.disabled = true;
                    quitAppBtn.textContent = t('apps.quitting');
                    BackendClient.stopHostSession(uuid)
                        .then((res) => {
                            const id = Number(res && res.currentGameId) || 0;
                            if (host) host.currentGameId = id;
                            if (host && host.runningAppPerDevice)
                                this.forgetDeviceRunning(uuid, id);
                        })
                        .catch((err) => {
                            console.error('[MW] Quit app failed:', err);
                            Toast.error(t('apps.quitFailed', { name }));
                        })
                        .finally(() => {
                            this.renderList();
                            this.refresh();
                        });
                });
                return;
            }

            // The GPU list on the virtual display's card is its own control.
            if (e.target.closest('.app-card-gpu--choice')) return;

            // ── App card launch ───────────────────────────────────────────
            const appCard = e.target.closest('.app-card');
            // A display a stream turned off opens nothing until it is back.
            if (appCard && appCard.classList.contains('app-card--off')) return;
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
                    // Counted here, on the click, rather than on the session
                    // actually coming up: the view is torn down by the launch,
                    // and a refused launch is still a statement of intent about
                    // which machine this browser cares about.
                    noteHostUse(host.uuid);
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
            // Escape (a pad's B, through RemoteNav) closes an open kebab menu,
            // and the focus goes back to the button that opened it.
            if (e.key === 'Escape' && this._closeMenuBackToButton()) {
                e.preventDefault();
                return;
            }
            if (e.key !== 'Enter' && e.key !== ' ') return;
            // The Quit button of a running app is a real <button>: let it act.
            if (e.target.closest && e.target.closest('.app-card-quit')) return;
            // So is the GPU list: Enter and Space open it, never the stream.
            if (e.target.closest && e.target.closest('.app-card-gpu--choice')) return;
            const appCard = e.target.closest && e.target.closest('.app-card');
            if (!appCard) return;
            e.preventDefault();
            appCard.click();
        };
        this.container.addEventListener('keydown', this._keydownHandler);

        // The GPU the virtual display is drawn by, picked on its card: saved
        // for its next start, and the grid asks the host again, which repaints
        // the card with what it now says. Refused, the list goes back to what
        // it showed.
        this._changeHandler = (e) => {
            if (this._destroyed) return;
            const select = e.target.closest && e.target.closest('.app-card-gpu-select');
            if (!select) return;
            const hostCard = select.closest('.host-card');
            const uuid = hostCard && hostCard.dataset.uuid;
            if (!uuid) return;
            const before = select.dataset.selected || '';
            select.disabled = true;
            BackendClient.setVirtualDisplayGpu(uuid, select.value)
                .then(() => this._refreshApps(uuid))
                .catch((err) => {
                    console.error('[MW] Virtual display GPU not saved:', err);
                    select.value = before;
                    Toast.error(t('apps.gpuSaveFailed'));
                })
                .finally(() => {
                    if (select.isConnected) select.disabled = false;
                });
        };
        this.container.addEventListener('change', this._changeHandler);
    }

    // --- Public API ---

    start() {
        this._active = true;
        // Back on the list, maybe from a stream just stopped: ask again at once.
        this._deviceRunningAt.clear();
        this._drawRememberedHosts();
        this._autoScan();
        this.scheduleNextPoll(0); // immediate first refresh
        // Discreet "new version available" banner. Fresh: opening the app and
        // coming back from a stream are when the answer gets read.
        this._checkForUpdate({ fresh: true });
        this._scheduleUpdateCheck();
    }

    /**
     * Draw the host list this machine sent last time, before it answers now
     * (hostListCache.js). Only on the page's first sight of the list: once the
     * machine has answered, what is on screen is already newer than any memory.
     */
    _drawRememberedHosts() {
        if (this._hostsLoaded || this.hosts.length > 0) return;
        const remembered = loadCachedHosts(this.memoryKey);
        if (!remembered) return;
        this.hosts = remembered.map((h) => {
            const host = new Host(h);
            // What ran last time is no news about now: no card offers to resume
            // a game on the strength of a memory.
            host.currentGameId = 0;
            return host;
        });
        this._hostsFromMemory = true;
        bootMark('hosts-remembered', `${this.hosts.length} hosts`);
        this.renderList();
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
    //
    // `fresh` has the host look at the releases now instead of answering from
    // its cache; the host itself bounds how often that really happens.
    async _checkForUpdate({ fresh = false } = {}) {
        // Nothing to learn while the banner shows an update in flight, and don't
        // re-ask more often than the backend could plausibly change its answer.
        if (hostUpdate.busy) return;
        const now = Date.now();
        if (now - this._updateCheckedAt < HostListView.UPDATE_CHECK_MIN_MS) return;
        this._updateCheckedAt = now;
        const info = await BackendClient.checkForUpdate({ fresh });
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

    /** Whether this browser can have the update applied for it (HostUpdate.js). */
    _canSelfUpdate() {
        return canSelfUpdate(this._updateInfo, this.hosts);
    }

    _renderUpdateBanner() {
        // An update in flight owns the banner (progress bar); never repaint over it.
        if (hostUpdate.busy) return;
        // A booth's visitors can do nothing about an update: the banner stays hidden.
        if (BackendClient.adminLocked) return;
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

    // --- Applying the update (host-side, watched from HostUpdate.js) ---

    /** Paint what the update run shows (HostUpdate.js subscribe). */
    _paintUpdateRun(view) {
        if (this._destroyed || !view) return;
        if (view.state === 'failed') this._renderUpdateFailed(view.message);
        else this._renderUpdateProgress(view.label, view.percent);
    }

    // Repaint the banner as a progress row. No dismiss button: the host is being
    // replaced under us, hiding the bar would only lose the user.
    //
    // The row is built once and then updated in place — re-rendering the markup
    // on every frame would restart the sheen and churn the DOM for nothing.
    _renderUpdateProgress(label, percent) {
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
        }
        banner.hidden = false;
        if (this._progressLabel.textContent !== label) this._progressLabel.textContent = label;
        const pct = Math.max(0, Math.min(100, percent));
        this._progressFill.style.width = `${pct.toFixed(1)}%`;
        this._progressTrack.setAttribute('aria-valuenow', String(Math.round(pct)));
    }

    _renderUpdateFailed(message) {
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

    async _autoScan() {
        // A booth offers the hosts it was given; discovery would add others.
        if (!this._active || BackendClient.adminLocked) return;
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
        this._appRetryDelay = {};
        this._appRetryAt = {};
    }

    destroy() {
        this._active = false;
        this._destroyed = true;
        this.stop();
        if (this._unsubUpdate) this._unsubUpdate();
        this._unsubUpdate = null;
        this._progressFill = null;
        this._launchFailTimers.forEach((id) => clearTimeout(id));
        this._launchFailTimers.clear();
        if (this._clickHandler) {
            this.container.removeEventListener('click', this._clickHandler);
            this._clickHandler = null;
        }
        if (this._keydownHandler) {
            this.container.removeEventListener('keydown', this._keydownHandler);
            this._keydownHandler = null;
        }
        if (this._changeHandler) {
            this.container.removeEventListener('change', this._changeHandler);
            this._changeHandler = null;
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
            const asked = performance.now();
            const data = await BackendClient.getHosts();
            // Re-check after await: stop()/destroy() may have run while the
            // request was in flight (e.g. a stream was launched) — a late
            // render here would overwrite the current view.
            if (!this._active || this._destroyed) return;
            const serverHosts = data.hosts || [];
            bootMark(
                'hosts-answered',
                `${serverHosts.length} hosts, asked ${Math.round(performance.now() - asked)} ms before`,
            );
            const before = this._fingerprint();
            // The cards drawn from memory give way to the machine's own list,
            // and a host it no longer has goes with them — the merge below
            // never removes anything.
            if (this._hostsFromMemory) {
                this._hostsFromMemory = false;
                const kept = new Set(serverHosts.map((h) => h.uuid));
                this.hosts = this.hosts.filter((h) => kept.has(h.uuid));
            }
            this._rememberHosts(serverHosts);
            // Merge — add/update, never remove
            for (const h of serverHosts) {
                const host = new Host(h);
                // This device's running app, as last asked (see Host).
                if (host.runningAppPerDevice)
                    host.currentGameId = this._deviceRunning.get(host.uuid) || 0;
                const idx = this.hosts.findIndex((ex) => ex.uuid === host.uuid);
                if (idx >= 0) this.hosts[idx] = host;
                else this.hosts.push(host);
            }
            this._refreshDeviceRunning();
            const firstLoad = !this._hostsLoaded;
            this._hostsLoaded = true;
            const after = this._fingerprint();
            // firstLoad, and not only a changed fingerprint: an empty answer
            // leaves the fingerprint exactly as it was before the request, so
            // the one case that most needs repainting — nothing found — is the
            // one no change signals. Same trap the update banner documents
            // below.
            if (firstLoad || before !== after) this.renderList();
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

    /**
     * Keep the list the machine just sent, for the next opening to draw at once.
     * Written only when it changed: the list is asked every few seconds.
     */
    _rememberHosts(serverHosts) {
        if (!this.memoryKey) return;
        const json = JSON.stringify(serverHosts);
        if (json === this._rememberedJson) return;
        this._rememberedJson = json;
        saveCachedHosts(this.memoryKey, serverHosts);
    }

    /**
     * What this device's own seat runs, on every host where each device has
     * one: the host list carries the machine's currentGameId, which is no one
     * device's. Asked at most every DEVICE_RUNNING_MS per host, and at once
     * after start() or forgetDeviceRunning().
     */
    _refreshDeviceRunning() {
        const now = Date.now();
        for (const host of this.hosts) {
            if (!host.runningAppPerDevice || host.state !== 'online') continue;
            const at = this._deviceRunningAt.get(host.uuid);
            if (at !== undefined && now - at < HostListView.DEVICE_RUNNING_MS) continue;
            this._deviceRunningAt.set(host.uuid, now);
            BackendClient.getRunningApp(host.uuid)
                .then((answer) => {
                    if (!this._active || this._destroyed) return;
                    const id = Number(answer && answer.currentGameId) || 0;
                    if ((this._deviceRunning.get(host.uuid) || 0) === id) return;
                    this._deviceRunning.set(host.uuid, id);
                    const current = this.hosts.find((h) => h.uuid === host.uuid);
                    if (current) current.currentGameId = id;
                    this.renderList();
                })
                .catch(() => {
                    /* unknown: keep the last answer, ask again next time */
                });
        }
    }

    /** Ask this host's seat again on the next refresh, and show nothing meanwhile. */
    forgetDeviceRunning(uuid, id = 0) {
        this._deviceRunning.set(uuid, id);
        this._deviceRunningAt.delete(uuid);
        const host = this.hosts.find((h) => h.uuid === uuid);
        if (host && host.runningAppPerDevice) host.currentGameId = id;
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
                h.customName,
                h.port,
                h.gpuModel,
                // A running session drives the kebab's "Stop session" entry and
                // the running app's card; without it here the card never
                // re-renders when a Stop leaves the Sunshine app running.
                h.currentGameId,
                h.resumableApps,
                h.restartSupported,
                // The native host's display state: without it here a virtual
                // display going off never reaches the card, nor the app list
                // the card then asks for again.
                h.nativeDisplay,
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
            host.customName,
            host.port,
            host.gpuModel,
            host.wakeSupported,
            host.restartSupported,
            host.currentGameId,
            host.resumableApps,
            // The native host's display state: a screen appearing or leaving
            // must repaint the card from empty state to grid and back.
            JSON.stringify(host.nativeDisplay),
            host.certificateChanged,
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
                        ${
                            BackendClient.adminLocked
                                ? ''
                                : `<button class="btn btn-neutral" id="btn-manual">${t('hosts.addManually')}</button>`
                        }
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

        // Empty state.
        //
        // An empty list is the one moment where the answer is not on this page:
        // MoonlightWeb streams a machine, it does not make one streamable, and a
        // user who has nothing here has nothing to click. So it says what is
        // missing and points at Sunshine.
        //
        // A link, and only a link. Sunshine is somebody else's program: it is
        // downloaded and installed by its owner, on the machine they want to
        // play from, exactly like any other application. MoonlightWeb stopped
        // installing it with H4 and does not do it from here either — no
        // credentials asked, no silent download, nothing to un-install later.
        //
        // Waits for the first list: the placeholder is what the view shows
        // before any answer has come back, and inviting someone to install a
        // second program before we have even looked would be a poor greeting on
        // a machine that has three hosts.
        if (this.hosts.length === 0) {
            list.innerHTML = `
                <div class="hosts-empty">
                    <span class="empty-icon">\u{1F5B4}</span>
                    <p>${t('hosts.empty')}</p>
                    <p class="hint">${t('hosts.emptyHint')}</p>
                    ${
                        this._hostsLoaded
                            ? `<div class="hosts-empty-cta">
                    <p class="hint">${t('hosts.emptyNeedsHost')}</p>
                    <a class="btn btn-secondary" href="${SUNSHINE_DOWNLOAD_URL}"
                       target="_blank" rel="noopener noreferrer">${t('hosts.emptyGetSunshine')}</a>
                </div>`
                            : ''
                    }
                </div>`;
            return;
        }

        // Remove empty placeholder if present
        const empty = list.querySelector('.hosts-empty');
        if (empty) empty.remove();

        // Paired hosts first, then the ones this browser actually streams from —
        // discovery order is the network's opinion, and it puts the machine
        // played on every evening under three boxes seen once. The score only
        // moves on a launch, so the list does not reshuffle under the cursor;
        // hosts never launched all score 0 and fall back to the default order:
        // the native host (always there, the one a fresh install streams
        // from), then the available hosts on this same PC, then the rest by
        // name rather than in whatever order they answered.
        const usage = hostUsageRanker();
        const isNative = (h) => (h.backendType === 'native' ? 1 : 0);
        const isHere = (h) => (h.isLocalHost && h.isAvailable ? 1 : 0);
        this.hosts.sort(
            (a, b) =>
                Number(b.isPaired) - Number(a.isPaired) ||
                usage(b.uuid) - usage(a.uuid) ||
                isNative(b) - isNative(a) ||
                isHere(b) - isHere(a) ||
                a.displayName.localeCompare(b.displayName, undefined, {
                    sensitivity: 'base',
                    numeric: true,
                }),
        );

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
                card.dataset.nativeDisplay = JSON.stringify(host.nativeDisplay);
            } else if (card.dataset.fingerprint !== fp) {
                // Data changed — replace just this card, unless its kebab is
                // open.
                //
                // A card is replaced whole, so a rebuild under an open menu
                // takes the menu with it, and the click already on its way to
                // "Share" lands on the card underneath instead. This is not a
                // narrow race: a host's status flaps on its own every few
                // seconds — a poll that fails, a session starting — and the
                // menu is open for as long as it takes to read it.
                //
                // The stale card keeps its old fingerprint, so it is rebuilt by
                // the render that follows the menu closing.
                if (card.querySelector('.host-menu:not([hidden])')) {
                    this._staleCards = true;
                } else {
                    // The native host's displays moved (the virtual display
                    // went on or off, and with "other screens off" the
                    // physical ones with it): the list this page holds may
                    // have been asked for in between, so it is confirmed
                    // again like a remembered one.
                    const entry = this.appsByHost[host.uuid];
                    if (
                        entry &&
                        entry.apps &&
                        card.dataset.nativeDisplay !== JSON.stringify(host.nativeDisplay)
                    )
                        entry.fromCache = true;
                    const tmp = document.createElement('div');
                    tmp.innerHTML = this.renderCard(host);
                    const newCard = /** @type {HTMLElement} */ (tmp.firstElementChild);
                    newCard.dataset.fingerprint = fp;
                    newCard.dataset.nativeDisplay = JSON.stringify(host.nativeDisplay);
                    card.replaceWith(newCard);
                    card = newCard;
                }
            }

            // Keep DOM order in sync with the sorted hosts array.
            const ref = list.children[i] || null;
            if (ref !== card) list.insertBefore(card, ref);
        }

        // Fill in app grids for available hosts (cache hit = instant).
        for (const host of this.hosts) {
            if (
                host.isAvailable &&
                !host.needsVirtualDisplay &&
                (!host.leftByRemoteDesktop || this._remoteDesktopHasApp(host)) &&
                !host.needsCapturePermission
            )
                this._ensureAppsLoaded(host);
        }
    }

    /**
     * A host whose session Remote Desktop left: whether its card shows its one
     * app ("This PC's screen", or "This PC's sign-in screen" outside a kiosk)
     * rather than the text alone.
     */
    _remoteDesktopHasApp(host) {
        return (
            host.returnsFromRemoteDesktop || (host.opensSignInScreen && !BackendClient.adminLocked)
        );
    }

    renderCard(host) {
        const cls = host.statusClass;
        // Booth mode leaves the menu one entry, Stop session, and hides it
        // while there is no app to stop.
        const booth = BackendClient.adminLocked;
        return `
            <div class="host-card ${cls}" data-uuid="${host.uuid}">
                <div class="host-card-head">
                    <div class="host-card-icon">
                        <span class="status-icon ${cls}">${host.statusIcon}</span>
                    </div>
                    <div class="host-card-info">
                        <div class="host-name" title="${this.esc(host.displayName)}">${this.esc(host.displayName)}</div>
                        ${
                            host.displayGpu
                                ? `<div class="host-gpu">${this.esc(host.displayGpu)}</div>`
                                : ''
                        }
                    </div>
                    <span class="status-badge ${cls}">${host.statusLabel}</span>
                    <div class="host-card-menu"${
                        booth && !(host.currentGameId > 0) ? ' style="display: none"' : ''
                    }>
                        <button class="btn-icon btn-host-menu" data-uuid="${host.uuid}"
                                aria-haspopup="true" aria-expanded="false"
                                aria-label="${this.esc(t('hosts.menuAria'))}">${Icons.menu}</button>
                        <div class="host-menu" hidden>
                            ${
                                booth
                                    ? ''
                                    : `<button class="host-menu-item btn-rename" data-uuid="${host.uuid}">${t('hosts.rename')}</button>`
                            }
                            ${
                                // Sharing without a stream: the board opens
                                // cold, the owner picks the app, and the first
                                // guest to spend their PIN is what launches it.
                                // Gated on the declared lobby capability, not
                                // the product name — a co-op host manages its
                                // own players in its own streamed UI.
                                !booth && host.isAvailable && host.supportsLobbies !== true
                                    ? `<button class="host-menu-item btn-share" data-uuid="${host.uuid}">${t('sharing.share')}</button>`
                                    : ''
                            }
                            ${
                                // Offered only where the server holds a control
                                // path of its own — its Sunshine, or a backend
                                // API that can bounce the service. It never asks
                                // the user for a host password, so on a plain
                                // remote host there is simply nothing to show.
                                !booth && host.restartSupported
                                    ? `<button class="host-menu-item btn-restart" data-uuid="${host.uuid}">${t('hosts.restartService')}</button>`
                                    : ''
                            }
                            ${
                                // Escape hatch: an app is still running on the
                                // host but this browser has no stream view to
                                // stop it from (tab closed, another device, or
                                // players holding the session open).
                                host.currentGameId > 0
                                    ? `<button class="host-menu-item btn-stop-session" data-uuid="${host.uuid}">${t('hosts.stopSession')}</button>`
                                    : ''
                            }
                            ${
                                // Only offered where there is something to
                                // manage: a backend already configured, or one
                                // the server detected on its own. A plain
                                // Sunshine card shows nothing at all, which is
                                // the point — the type is never a question put
                                // to the user.
                                //
                                // A host with no Sunshine management API is the
                                // third case: it speaks GameStream, so it runs
                                // something, and that something keeps its
                                // control API somewhere only its owner knows.
                                // Offering to be pointed at it is the only way
                                // such a host can ever be set up — and because
                                // the test is a Sunshine one, a Sunshine card
                                // still shows nothing at all.
                                !booth &&
                                this.canAdminister &&
                                (host.backendType ||
                                    host.multiSeatDetected ||
                                    host.mayHaveControlApi)
                                    ? `<button class="host-menu-item btn-backend" data-uuid="${host.uuid}">${
                                          host.backendType
                                              ? t('hosts.backendManage', { type: host.backendType })
                                              : t('hosts.backendSetup')
                                      }</button>`
                                    : ''
                            }
                            ${
                                booth
                                    ? ''
                                    : `<button class="host-menu-item btn-remove" data-uuid="${host.uuid}">${t('common.remove')}</button>`
                            }
                            ${
                                // A TV remote's Back never reaches the page: the
                                // menu carries its own way out there.
                                RemoteNav.isActive()
                                    ? `<button class="host-menu-item btn-menu-close">${t('common.close')}</button>`
                                    : ''
                            }
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
        if (host.needsVirtualDisplay) {
            // Nothing plugged in and no "MoonlightWeb Virtual Display" to
            // open either: say what is missing. (With it installed the card
            // is an ordinary grid — the display is its one app.) Where none
            // is shipped, the installer is never named.
            const text = host.canHaveVirtualDisplay
                ? t('vdisplay.emptyBody')
                : t('vdisplay.emptyBodyNoDriver');
            return `<div class="host-body-center host-empty-display">
                        <p class="host-empty-display-text">${text}</p>
                    </div>`;
        }
        if (host.leftByRemoteDesktop && !this._remoteDesktopHasApp(host)) {
            // Windows: closing Remote Desktop left the session with no screen
            // at all, virtual ones included. Only its return to the PC's
            // screen brings one back: with the admin's setting on, the grid
            // below has "This PC's screen", which does that when opened.
            return `<div class="host-body-center host-empty-display">
                        <p class="host-empty-display-text">${t('hosts.sessionLeftByRemoteDesktop')}</p>
                    </div>`;
        }
        if (host.needsCapturePermission) {
            // The Mac's Screen Recording grant went away after the install (an
            // update, a reset). The card stays and says which switch to flip:
            // one that vanished would say nothing at all.
            return `<div class="host-body-center host-empty-display">
                        <p class="host-empty-display-text">${t('hosts.nativeCapturePermission')}</p>
                        ${
                            BackendClient.adminLocked
                                ? ''
                                : `<button class="btn btn-secondary btn-mac-perm" data-pane="screen">${t('setup.openScreenRecording')}</button>`
                        }
                    </div>`;
        }
        if (host.isOnline && host.certificateChanged) {
            // Another machine answers at this address, and nothing is asked of
            // it. "Remove", in the menu, is the way out this text names.
            return `<div class="host-body-center host-offline-msg">${t('hosts.certificateChangedMessage')}</div>`;
        }
        if (host.isAvailable) {
            // App grid filled asynchronously by _ensureAppsLoaded(). A Mac that
            // lost Accessibility still streams, but takes no input: said above
            // the grid, since macOS says nothing.
            const inputWarn = host.lacksInputPermission
                ? `<div class="host-perm-warn"><p>${t('hosts.nativeInputPermission')}</p>${
                      BackendClient.adminLocked
                          ? ''
                          : `<button class="btn btn-secondary btn-mac-perm" data-pane="accessibility">${t('setup.openAccessibility')}</button>`
                  }</div>`
                : '';
            return inputWarn + `<div class="host-apps" data-uuid="${host.uuid}"></div>`;
        }
        // A booth pairs nothing: an unpaired host just shows its badge.
        if (host.isLocked && BackendClient.adminLocked) return '';
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

    /**
     * What `appsByHost[uuid]` holds. Every field is optional on purpose: a grid
     * can carry a remembered list AND a request in flight at the same time.
     * @typedef {Object} AppsEntry
     * @property {App[]} [apps] the list the grid paints
     * @property {boolean} [fromCache] painted from the last visit, unconfirmed
     * @property {Promise<AppsEntry>} [pending] fetch in flight
     * @property {string} [error]
     * @property {boolean} [sticky] the error is definitive — don't retry
     * @property {boolean} [certificateChanged] the host showed another certificate
     */

    // Ensure the app grid for an available host is on screen, and current.
    //
    // The list this browser saw last time is painted immediately — the host is
    // "ready", so its apps show up with the card rather than a few seconds of
    // spinner later — and the real fetch runs behind it, repainting only if the
    // host disagrees. A host never seen before still gets the spinner, because
    // there is genuinely nothing to show yet.
    //
    // The dataset flags say what is currently painted, so a re-render (host
    // poll) never repaints an identical grid and clobbers a launching card.
    _ensureAppsLoaded(host) {
        const uuid = host.uuid;
        const cont = this.container.querySelector(`.host-card[data-uuid="${uuid}"] .host-apps`);
        if (!cont) return;

        let entry = this.appsByHost[uuid];

        // A transient failure is already on a backoff timer. This path runs on
        // every re-render of the list, so without this gate the host poll
        // re-asks a host that is down every few seconds regardless of the
        // backoff, and the ladder in _scheduleAppsRetry never bites.
        const backoff = this._appRetryAt[uuid] > Date.now();

        // First sight of this host in this session: adopt what the last visit
        // saw so there is something to draw right now.
        if (!entry) {
            const remembered = loadCachedApps(uuid);
            if (remembered) {
                entry = {
                    apps: sortApps(remembered.map((a) => new App(a, uuid))),
                    fromCache: true,
                };
                this.appsByHost[uuid] = entry;
            }
        }

        // Something definitive to paint (apps, or a permanent "not paired" /
        // "not found" rejection). Transient errors deliberately fall through so
        // a recovered host reloads on its own instead of staying wedged behind a
        // red banner until a manual reload.
        if (entry && (entry.apps || entry.sticky)) {
            if (cont.dataset.loaded !== '1') {
                this._paintApps(cont, entry, uuid);
            }
            // A remembered list is shown on trust, never kept on trust: confirm
            // it against the host once, with no spinner over the grid that is
            // already there.
            if (entry.apps && entry.fromCache && !backoff) this._refreshApps(uuid);
            return;
        }
        if (entry && entry.pending) return; // request in flight, placeholder shown

        cont.innerHTML = `<div class="host-apps-loading">${t('apps.loading')}</div>`;
        cont.dataset.loaded = '';
        if (!backoff) this._refreshApps(uuid);
    }

    /**
     * Ask the host for its app list and reconcile the grid with the answer.
     *
     * Runs both as the very first load (spinner on screen) and as the silent
     * confirmation behind a remembered list, which is why it never paints a
     * spinner of its own and never tears down a grid it cannot replace.
     *
     * @returns {Promise<object>} the settled entry for this host — apps when the
     *          host answered, an error marker otherwise. One request at a time
     *          per host: a second caller joins the one in flight.
     */
    _refreshApps(uuid) {
        const prev = this.appsByHost[uuid];
        if (prev && prev.pending) return prev.pending;

        // Keep any list already on screen while the request runs.
        const entry = prev && prev.apps ? prev : {};
        this.appsByHost[uuid] = entry;

        const asked = performance.now();
        const pending = BackendClient.getAppList(uuid)
            .then((data) => {
                bootMark(
                    'apps-answered',
                    `asked ${Math.round(performance.now() - asked)} ms before`,
                );
                if (data && data.status === 'ok') {
                    const raw = data.apps || [];
                    saveCachedApps(uuid, raw);
                    // The host answered: the next failure starts the ladder over.
                    delete this._appRetryDelay[uuid];
                    delete this._appRetryAt[uuid];
                    return /** @type {AppsEntry} */ ({
                        apps: sortApps(raw.map((a) => new App(a, uuid))),
                    });
                }
                // The backend answered with a definitive rejection (host not
                // paired / not found) — retrying won't change the outcome.
                return /** @type {AppsEntry} */ ({
                    error: (data && data.message) || 'load_failed',
                    sticky: true,
                });
            })
            .catch((err) => {
                // Network error / timeout / 5xx: the host is momentarily
                // unreachable (typical right after a host reboot while the
                // remote path warms up and the backend poll re-confirms the
                // active address). Keep it transient and schedule an auto-retry
                // so the card self-heals without a manual page reload.
                //
                // A 4xx is the opposite: the backend looked and gave a verdict —
                // not paired, not found, no address. Retrying returns the same
                // answer forever, and the spinner below would never come down,
                // so show what it said instead.
                console.error('[MW] Failed to load app list:', err);
                if (err.responseBody?.code === 'certificate_changed')
                    return /** @type {AppsEntry} */ ({ certificateChanged: true });
                const verdict = err.statusCode >= 400 && err.statusCode < 500;
                return /** @type {AppsEntry} */ ({
                    error: err.message,
                    sticky: verdict || undefined,
                });
            })
            .then((result) => {
                if (this._destroyed) return result;

                // The card says this one itself, and nothing stays behind here:
                // a sticky verdict would outlast it once the right machine is
                // back, and the grid would never return without a reload.
                if (result.certificateChanged) {
                    delete this.appsByHost[uuid];
                    const refused = this.hosts.find((h) => h.uuid === uuid);
                    if (refused) refused.certificateChanged = true;
                    this.renderList();
                    return result;
                }

                const current = this.appsByHost[uuid];
                const shown = current && current.apps ? current.apps : null;

                // A transient failure must not cost the user the grid they can
                // see: keep the remembered list up and retry behind it.
                if (result.error && !result.sticky && shown) {
                    this.appsByHost[uuid] = { apps: shown, fromCache: current.fromCache };
                    this._scheduleAppsRetry(uuid);
                    return this.appsByHost[uuid];
                }

                this.appsByHost[uuid] = result;

                const cont = this.container.querySelector(
                    `.host-card[data-uuid="${uuid}"] .host-apps`,
                );
                if (!cont) return result;

                if (result.error && !result.sticky) {
                    // Nothing to fall back on — keep the spinner (not the red
                    // error) while a retry is pending, then re-attempt shortly.
                    cont.innerHTML = `<div class="host-apps-loading">${t('apps.loading')}</div>`;
                    cont.dataset.loaded = '';
                    this._scheduleAppsRetry(uuid);
                    return result;
                }

                // Repaint only on a real difference. A confirmation that agrees
                // with the remembered list must be invisible — repainting every
                // grid on every visit is the flash this whole path exists to
                // avoid — and a launching card would not survive it.
                const sig = result.apps ? appsSignature(result.apps) : '';
                const unchanged = cont.dataset.loaded === '1' && cont.dataset.appsSig === sig;
                if (!unchanged && !cont.querySelector('.app-card--launching')) {
                    this._paintApps(cont, result, uuid);
                }
                return result;
            });

        entry.pending = pending;
        // Clear the in-flight marker on the entry that is current by then — the
        // handlers above may well have replaced it.
        pending.finally(() => {
            const settled = this.appsByHost[uuid];
            if (settled) delete settled.pending;
        });
        return pending;
    }

    /**
     * Whether an app the user just tried to launch still exists on its host.
     *
     * A card can be painted from the list this browser remembered, and the app
     * behind it may have been removed in Sunshine since — a launch then fails
     * like any other failure, with nothing saying why. So a failed launch
     * re-asks the host: the answer both repaints the grid (the dead card
     * disappears on its own) and tells the caller what to say.
     *
     * Answers `true` whenever it cannot tell — an unreachable host is not
     * evidence that the app is gone, and blaming the app there would be worse
     * than the generic message.
     */
    async confirmAppExists(host, app) {
        if (!host || !app) return true;
        const entry = await this._refreshApps(host.uuid);
        if (!entry || !entry.apps) return true;
        return entry.apps.some((a) => a.id === app.id);
    }

    // Auto-retry an app-list fetch that failed transiently (unreachable host).
    // Decoupled from the host-list poll so it fires even when nothing else about
    // the host changes; one pending retry per host.
    _scheduleAppsRetry(uuid) {
        if (this._appRetryTimers[uuid]) return;
        const delay = this._appRetryDelay[uuid] || HostListView.APP_LIST_RETRY_MS;
        this._appRetryDelay[uuid] = Math.min(delay * 2, HostListView.APP_LIST_RETRY_MAX_MS);
        this._appRetryAt[uuid] = Date.now() + delay;
        this._appRetryTimers[uuid] = setTimeout(() => {
            delete this._appRetryTimers[uuid];
            if (this._destroyed || !this._active) return;
            const host = this.hosts.find((h) => h.uuid === uuid);
            if (!host || !host.isAvailable) return;
            const entry = this.appsByHost[uuid];
            if (!entry || entry.sticky || entry.pending) return;
            // A remembered list still on screen is refreshed in place; a card
            // stuck on the spinner goes back through the full load.
            if (entry.apps) {
                this._refreshApps(uuid);
            } else {
                delete this.appsByHost[uuid]; // clear transient error → re-fetch
                this._ensureAppsLoaded(host);
            }
        }, delay);
    }

    // Paints the grid AND records what is on screen: `loaded` says the container
    // holds a definitive result, `appsSig` says which one, so a later refresh can
    // tell "same list" from "the host changed something".
    _paintApps(cont, entry, uuid) {
        cont.dataset.loaded = '1';
        cont.dataset.appsSig = entry && entry.apps ? appsSignature(entry.apps) : '';
        if (!entry || entry.error) {
            // Prefer what the backend actually said — "no longer paired" tells
            // the user what to do next, where the generic line leaves them
            // guessing. Only a verdict carries a message worth showing; a
            // transient failure falls back to the generic wording.
            const reason =
                entry && entry.sticky && entry.error ? entry.error : t('apps.loadFailed');
            cont.innerHTML = `<div class="host-apps-msg host-apps-error">${escapeHtml(reason)}</div>`;
            return;
        }
        const apps = entry.apps || [];
        if (apps.length === 0) {
            cont.innerHTML = `<div class="host-apps-msg">${t('apps.empty')}</div>`;
            return;
        }
        bootMark('apps-painted', entry.fromCache ? 'remembered' : 'from the host');
        const devices = apps.map((a) => a.device).filter(Boolean);
        const host = this.hosts.find((h) => h.uuid === uuid);
        const runningId = host && host.resumableApps ? host.currentGameId : 0;
        cont.innerHTML = `<div class="apps-grid">${apps.map((a) => this.renderApp(a, devices, runningId)).join('')}</div>`;
        this._wireBoxArt(cont, uuid);
    }

    /**
     * Wire what happens to a cover that does not arrive. The CSP (script-src
     * 'self') blocks inline onerror attributes, so this cannot live in the
     * markup.
     *
     * A cover that reached the screen once is never given up on. /appasset is
     * an HTTPS round trip to the host, serialized behind every other cover with
     * a five-second timeout, so a busy host — or one whose backend was just
     * restarted, emptying its art cache — answers 502 for art it holds
     * perfectly well. The old swap took that as the final word and replaced the
     * cover with the generic pad, permanently: nothing repaints a grid whose
     * app list has not changed, so the card stayed generic for the rest of the
     * visit. Now the pad is only what fills the frame while the request is
     * retried, and the cover comes back on its own.
     *
     * Art never seen still falls back for good — for an app whose host has no
     * cover for it, the pad IS the answer, and retrying would only cost
     * requests through the tunnel to be told the same thing three more times.
     *
     * The two stand-ins look different on purpose. While a cover is on its way
     * back the frame shows the plain default screen (HostArt's default-monitor),
     * a neutral "something goes here"; the app's own arcade sprite is kept for
     * an app that has no cover, so it never flashes up for a few seconds on a
     * card about to show its real one.
     */
    _wireBoxArt(cont, uuid) {
        cont.querySelectorAll('.app-card-image img').forEach((img) => {
            const card = img.closest('.app-card');
            const frame = img.parentElement;
            const appId = card ? Number(card.dataset.appId) : 0;
            const src = img.getAttribute('src');
            let attempt = 0;

            // One stand-in at a time in the frame: two is a frame with a
            // second, smaller frame in it.
            const standIn = (html, waiting) => {
                const prev = img.previousElementSibling;
                if (prev && prev.classList.contains('app-icon')) {
                    if (prev.classList.contains('app-icon--host') === waiting) return;
                    prev.remove();
                }
                img.insertAdjacentHTML('beforebegin', html);
                frame.classList.toggle('app-card-image--host', waiting);
            };

            const onError = () => {
                const ladder = HostListView.BOX_ART_RETRY_MS;
                if (!boxArtWasSeen(uuid, appId) || attempt >= ladder.length) {
                    // Never seen, or the host has stopped answering: the arcade
                    // sprite stands in for the cover for good, and the <img>
                    // goes away.
                    standIn(appPlaceholderHtml(appId, t), false);
                    img.remove();
                    return;
                }
                img.hidden = true;
                standIn(hostArtHtml(null), true);
                const delay = ladder[attempt];
                attempt++;
                setTimeout(() => {
                    // A distinct URL per attempt: the failed response may itself
                    // be what the HTTP cache would hand back.
                    if (!this._destroyed && img.isConnected) img.src = `${src}&retry=${attempt}`;
                }, delay);
            };

            const onLoad = () => {
                img.hidden = false;
                const prev = img.previousElementSibling;
                if (prev && prev.classList.contains('app-icon')) prev.remove();
                frame.classList.remove('app-card-image--host');
                noteBoxArtSeen(uuid, appId);
            };

            img.addEventListener('load', onLoad);
            img.addEventListener('error', onError);
            // Already settled before the listeners were attached (HTTP cache).
            if (img.complete && src) (img.naturalWidth === 0 ? onError : onLoad)();
        });
    }

    renderApp(app, devices = [], runningId = 0) {
        // The whole card is the launch action. role/tabindex keep it
        // keyboard-accessible (Enter/Space handled by the keydown delegate).
        //
        // A native host's display card draws the machine the host says it
        // is, on no frame. Its apps show the image the owner gave them, else
        // the stand-in every other host's app gets when it has no cover art.
        const cover = `<img src="${this.esc(app.boxArtUrl ?? '')}"
                               alt="${this.esc(app.displayName)}"
                               loading="lazy">`;
        const drawsMachine = app.device && !app.isHostApp;
        const image = drawsMachine
            ? hostArtHtml(app.device, devices)
            : app.boxArtUrl
              ? cover
              : appPlaceholderHtml(app.id, t);
        // The app a stopped stream left running (issue #24): the card resumes
        // it, and says so; a small Quit closes it on the host.
        const running = runningId > 0 && app.id === runningId;
        // A display a stream turned off (issue #39): drawn dark, opens nothing.
        const off = app.isOff;
        const aria = off
            ? t('apps.displayOffAria', { name: app.displayName })
            : running
              ? t('apps.resumeAria', { name: app.displayName })
              : t('apps.launchAria', { name: app.displayName });
        const state = off ? ' app-card--off' : running ? ' app-card--running' : '';
        return `
            <div class="app-card${state}" data-app-id="${app.id}"
                 role="button" tabindex="0"${off ? ' aria-disabled="true"' : ''}
                 aria-label="${this.esc(aria)}">
                <div class="app-card-image${drawsMachine ? ' app-card-image--host' : ''}">
                    ${image}
                    ${running && !off ? `<span class="app-card-badge">${this.esc(t('apps.running'))}</span>` : ''}
                    ${off ? `<span class="app-card-off-note">${this.esc(t('apps.displayOff'))}</span>` : ''}
                </div>
                <div class="app-card-name">${this.esc(app.displayName)}</div>
                ${drawsMachine ? this._gpuLine(app, devices) : ''}
                ${
                    running
                        ? `<button type="button" class="app-card-quit" data-app-id="${app.id}"
                                aria-label="${this.esc(t('apps.quitAria', { name: app.displayName }))}">${this.esc(t('apps.quit'))}</button>`
                        : ''
                }
            </div>
        `;
    }

    /**
     * The line under a native display's card that names the GPU drawing it,
     * on a PC with several (01/11/2026: DualRTX, and nothing said which screen
     * was on which card). Two GPUs of one name carry their number.
     *
     * On the virtual display's card, where the driver's settings are
     * MoonlightWeb's own, the line is where its administrator picks that GPU:
     * a list, "Auto" — the driver's own choice — first, then the GPUs by name,
     * the only thing the driver tells them apart by. Anyone else, and a booth,
     * sees the line only.
     *
     * Names read short (util/gpuName.js), each kept apart from the other cards'
     * and the list's; the full name stays in the tooltip and goes to the driver.
     */
    _gpuLine(app, devices = []) {
        const d = app.device;
        const name = typeof d.gpu === 'string' ? d.gpu : '';
        const numbered = (text) =>
            text && d.gpuNumber > 0 ? t('apps.gpuNumbered', { name: text, n: d.gpuNumber }) : text;
        const choice = d.gpuChoice;
        const offered = choice && Array.isArray(choice.options) ? choice.options : [];
        const shown = shortGpuNames(
            [...devices.map((o) => o.gpu), ...offered, choice && choice.selected, name].filter(
                (n) => typeof n === 'string' && n,
            ),
        );
        const label = numbered(shown.get(name) || name);
        const fullLabel = numbered(name);
        if (
            choice &&
            Array.isArray(choice.options) &&
            this.canAdminister &&
            !BackendClient.adminLocked
        ) {
            const selected = typeof choice.selected === 'string' ? choice.selected : '';
            const names = choice.options.filter((n) => typeof n === 'string' && n);
            if (selected && !names.includes(selected)) names.push(selected);
            // While the display is on, the GPU really drawing it is said too.
            const auto = label && !selected ? `${t('apps.gpuAuto')} · ${label}` : t('apps.gpuAuto');
            const option = (value, text) =>
                `<option value="${this.esc(value)}"${value === selected ? ' selected' : ''}>${this.esc(text)}</option>`;
            const options = [
                option('', auto),
                ...names.map((n) =>
                    option(n, n === selected && n === name ? label : shown.get(n) || n),
                ),
            ];
            const hint = t('apps.gpuChoiceHint');
            const tip = selected || fullLabel ? `${selected || fullLabel}\n${hint}` : hint;
            return `
                <label class="app-card-gpu app-card-gpu--choice" title="${this.esc(tip)}">
                    <select class="app-card-gpu-select" data-selected="${this.esc(selected)}"
                            aria-label="${this.esc(t('apps.gpuChoiceAria', { name: app.displayName }))}">${options.join('')}</select>
                </label>`;
        }
        return label
            ? `<div class="app-card-gpu" title="${this.esc(fullLabel)}">${this.esc(label)}</div>`
            : '';
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

    /**
     * Revert any card stuck in the "launching" state back to its idle
     * appearance and restore the original app name.
     *
     * Whatever the reason, the card gives up the focus the click left on it:
     * the focus ring is the same accent yellow as the launching edge, so a card
     * that keeps it still reads as "working on it" at the exact moment the
     * truth is the opposite — and the user has no cue that the thing to do is
     * click again.
     *
     * @param {{failed?: boolean}} [opts] `failed: true` when the launch never
     *        started and the host list is all the user has to go on: the card
     *        flashes an alert orange and fades back to neutral within a second.
     *        Long enough to be seen, short enough that no colour is left behind
     *        pretending to be a state. Left out when the stream itself died —
     *        that screen already showed the error, and a second alarm back on
     *        the host list would only report it twice.
     */
    clearLaunching(opts) {
        if (!this.container) return;
        const failed = !!(opts && opts.failed);
        this.container.querySelectorAll('.app-card--launching').forEach((card) => {
            card.classList.remove('app-card--launching');
            card.removeAttribute('aria-busy');
            const nameEl = card.querySelector('.app-card-name');
            if (nameEl && nameEl.dataset.label) {
                nameEl.textContent = nameEl.dataset.label;
                delete nameEl.dataset.label;
            }
            // Not with a remote: there the focus IS the pointer, and a card that
            // drops it leaves the next arrow press starting from nowhere.
            const focused = /** @type {HTMLElement | null} */ (document.activeElement);
            if (focused && card.contains(focused) && !RemoteNav.isActive()) focused.blur();
            if (failed) this._flashLaunchFailed(card);
        });
    }

    // The orange-to-neutral fade itself. The class comes off on a timer rather
    // than on `animationend`, because that event never fires where the animation
    // doesn't run (reduced motion, a background tab) and the card would then
    // stay orange for good.
    _flashLaunchFailed(card) {
        card.classList.add('app-card--launch-failed');
        clearTimeout(this._launchFailTimers.get(card));
        this._launchFailTimers.set(
            card,
            setTimeout(() => {
                card.classList.remove('app-card--launch-failed');
                this._launchFailTimers.delete(card);
            }, HostListView.LAUNCH_FAIL_FADE_MS),
        );
    }

    /**
     * Close the open kebab menu, focusing the button that opened it.
     * @returns {boolean} whether a menu was open
     */
    _closeMenuBackToButton() {
        const open = this.container.querySelector('.btn-host-menu[aria-expanded="true"]');
        if (!this.container.querySelector('.host-menu:not([hidden])')) return false;
        this._closeAllMenus();
        if (open instanceof HTMLElement) open.focus();
        return true;
    }

    _closeAllMenus() {
        this.container.querySelectorAll('.host-menu:not([hidden])').forEach((m) => {
            m.setAttribute('hidden', '');
        });
        this.container
            .querySelectorAll('.btn-host-menu[aria-expanded="true"]')
            .forEach((b) => b.setAttribute('aria-expanded', 'false'));
        if (this._staleCards) setTimeout(() => this._flushStaleCards(), 0);
    }

    /**
     * Rebuild the cards renderList() left alone while a menu was open.
     *
     * A tick late on purpose. Opening a kebab closes every other one first, and
     * a rebuild in that gap would detach the very menu about to be shown — so
     * this runs after the handler has finished and checks again that nothing is
     * open. If something is, the flag stays set and the next close gets it.
     */
    _flushStaleCards() {
        if (this._destroyed || !this._staleCards) return;
        if (this.container.querySelector('.host-menu:not([hidden])')) return;
        this._staleCards = false;
        this.renderList();
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

    /** Rename a host: ask for a name, then store it server-side. The alias is
     *  local to MoonlightWeb — nothing is written on the host — so it works just
     *  as well on an offline or never-paired card. */
    /**
     * The sharing board, opened cold from a host's kebab. Nothing is streaming,
     * so the board picks the app an invitation binds to and the first guest to
     * spend their PIN is what starts it.
     */
    async _openShareBoard(host) {
        const board = new ShareBoard({
            hostUuid: host.uuid,
            hostName: host.displayName || host.name,
            streaming: false,
        });
        board.onClose = () => this.refresh();
        // False means the backend was built with sharing off: every share route
        // answers 404, so say nothing rather than showing an empty overlay.
        if (!(await board.open())) Toast.show(t('sharing.unavailable'), 'error');
    }

    async _renameHost(host) {
        const name = await this._promptHostName(host);
        if (name === null) return; // cancelled

        try {
            const data = await BackendClient.renameHost(host.uuid, name);
            // Patch in place so the card renames the moment the dialog closes,
            // instead of waiting on the next poll.
            host.customName = data.customName || '';
            this.renderList();
        } catch (err) {
            console.error('[MW] Rename host failed:', err);
            Toast.show(err.message, 'error');
        }
    }

    /** Themed replacement for window.prompt(): asks for a name for one host.
     *  Resolves with the entered string (possibly empty, which clears the
     *  alias), or null on cancel/Escape/backdrop click. */
    _promptHostName(host) {
        return new Promise((resolve) => {
            if (document.querySelector('.host-rename-overlay')) return resolve(null);

            const overlay = document.createElement('div');
            overlay.className = 'pairing-overlay host-rename-overlay';
            overlay.innerHTML = `
                <div class="pairing-dialog">
                    <h3>${this.esc(t('hosts.rename'))}</h3>
                    <p class="pairing-instruction">${this.esc(t('hosts.renamePrompt'))}</p>
                    <input type="text" class="settings-input host-rename-input"
                           maxlength="64" autocomplete="off"
                           placeholder="${this.esc(host.reportedName)}"
                           autocapitalize="none" autocorrect="off" spellcheck="false" />
                    <div class="pairing-actions">
                        <button class="btn btn-secondary host-rename-cancel">${t('common.cancel')}</button>
                        <button class="btn host-rename-ok">${t('common.save')}</button>
                    </div>
                </div>
            `;

            const input = /** @type {HTMLInputElement} */ (
                overlay.querySelector('.host-rename-input')
            );
            // Only the alias, never the reported name: an untouched dialog on a
            // host with no alias must save nothing rather than freeze today's
            // hostname in place.
            input.value = host.customName || '';

            const done = (value) => {
                overlay.remove();
                resolve(value);
            };
            overlay.querySelector('.host-rename-ok').addEventListener('click', () => {
                done(input.value.trim());
            });
            overlay
                .querySelector('.host-rename-cancel')
                .addEventListener('click', () => done(null));
            overlay.addEventListener('click', (e) => {
                if (e.target === overlay) done(null);
            });
            input.addEventListener('keydown', (e) => {
                if (e.key === 'Enter') done(input.value.trim());
                else if (e.key === 'Escape') done(null);
            });

            document.body.appendChild(overlay);
            input.focus();
            input.select();
        });
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
