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
 * MoonlightWeb — applying a host update, watched from this tab.
 *
 * Two pages start it — the banner on the Hosts view and the Update section in
 * Settings — and one update runs at a time, so the run lives here rather than
 * in either page. A page subscribes, paints what it is told, and can go away
 * and come back mid-install: the run carries on without it, and the page it
 * lands on picks the bar up where it is.
 */
import { BackendClient } from '../api/BackendClient.js';
import { t } from '../i18n/i18n.js';

/** Set while a host update this tab started is under way; read by the entry
 *  page (bootstrap/v1/boot.js) so it waits for the host to come back. */
const UPDATE_PENDING_KEY = 'mw-update-pending';

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

/**
 * Whether this browser can have the update applied for it.
 *
 * The installer runs on the HOST, never here — offering a download link to a
 * phone streaming from the other side of the house would be a dead end. So
 * the pages only ever offer an action when the host can carry it out itself,
 * which needs two things:
 *   - the machine running MoonlightWeb is paired with its local Sunshine.
 *     That is the "already set up" marker: the installer has nothing left to
 *     ask (no credentials, no pairing), so it can run unattended.
 *   - the platform has an unattended elevation path at all (self_update.
 *     supported — see SelfUpdater).
 * Anything else degrades to "go update the host PC".
 *
 * @param {object|null} info /api/update/check
 * @param {Array<{isLocalHost: boolean, isPaired: boolean}>} hosts
 */
export function canSelfUpdate(info, hosts) {
    const cap = info && info.self_update;
    if (!cap || !cap.supported) return false;
    return hosts.some((h) => h.isLocalHost && h.isPaired);
}

/**
 * What the run shows, handed to every subscriber:
 *   { state: 'progress', label, percent }   — on every animation frame
 *   { state: 'failed', message }            — until a page clears it
 *   null                                    — nothing under way
 */
export class HostUpdateRun {
    /**
     * Phase 1: one status request may take this long before the host counts as
     * gone. A server killed without closing the socket leaves the request
     * pending for good, and the page with it.
     */
    static STATUS_TIMEOUT_MS = 5000;

    /**
     * Phase 2: after this long without any HTTP answer, reload the whole page.
     * A host that comes back with a certificate the browser has not accepted
     * fails every background request without a word, exactly like a host still
     * down — only a navigation shows the warning that lets the user accept it.
     * Longer than a normal install, so a slow one is not cut short.
     */
    static NETWORK_RELOAD_MS = 90 * 1000;

    constructor() {
        this.busy = false;
        // Seams for tests: time, waiting and the reload itself.
        this._now = () => Date.now();
        this._sleep = (ms) => new Promise((r) => setTimeout(r, ms));
        this._reload = () => location.reload();
        this._listeners = new Set();
        this._view = null;
        this._label = '';
        this._phase = 'downloading';
        this._shown = 0;
        this._floor = 0;
        this._raf = null;
        this._at = 0;
    }

    /**
     * Paint what the run shows now, then every change. Returns the unsubscribe.
     * @param {(view: object|null) => void} fn
     */
    subscribe(fn) {
        this._listeners.add(fn);
        if (this._view) fn(this._view);
        return () => this._listeners.delete(fn);
    }

    /** A page closed the failure row: no page shows it again. */
    clearFailure() {
        if (this.busy || !this._view || this._view.state !== 'failed') return;
        this._emit(null);
    }

    /** @param {object} info the /api/update/check answer the button was drawn from */
    async start(info) {
        if (this.busy) return;
        this.busy = true;
        // Fresh bar even if a previous attempt failed halfway up.
        this._stopAnimation();
        this._shown = 0;
        this._floor = 0;
        this._progress({ state: 'downloading', percent: 0 });
        try {
            await BackendClient.startUpdate();
        } catch (err) {
            this._fail(err.message);
            return;
        }
        // Through the rendezvous, the installer taking the host down closes the
        // connection and sends this tab back to the entry page before the new
        // build is up. This note tells that page to keep calling rather than
        // report the machine offline (bootstrap/v1/boot.js, same key).
        try {
            sessionStorage.setItem(UPDATE_PENDING_KEY, String(Date.now()));
        } catch (_) {
            /* the entry page gives up at once, as before; a refresh still works */
        }
        this._watch(info || {});
    }

    /**
     * Follow the update to its end, across the moment the server disappears.
     *
     * Phase 1 polls /api/update/status for real progress. The installer then
     * replaces (and on Windows kills) that very server, so the polling failing is
     * the expected transition, not an error. Phase 2 waits for the new build to
     * answer /api/health and reloads the app on it.
     */
    async _watch(info) {
        const sleep = this._sleep;
        const now = this._now;

        // Phase 1 — the host is still answering. Bounded: on the platforms whose
        // installer needs a prompt on the host desktop, nothing kills this server
        // and an unanswered dialog would keep us polling forever. A minute past
        // the host's own deadline (SelfUpdater::armWatchdog: 15 minutes with a
        // prompt, 3 without), so that what it reports — "failed", with its
        // reason — is what this page shows, rather than a guess made first.
        // Counted from the end of the download, as the host's is.
        let startedAt = now();
        let budgetMs = 4 * 60 * 1000;
        const hardStop = now() + 30 * 60 * 1000; // a download that never ends
        let alive = true;
        while (now() - startedAt < budgetMs && now() < hardStop && alive) {
            await sleep(900);
            let status;
            try {
                status = await BackendClient.getUpdateStatus({
                    timeoutMs: HostUpdateRun.STATUS_TIMEOUT_MS,
                });
            } catch (_) {
                alive = false; // server gone: the installer took it down
                break;
            }
            if (status.state === 'failed') {
                this._fail(status.error);
                return;
            }
            // `idle` from a server that we told to update means we are already
            // talking to its freshly started replacement — phase 2's version
            // check is what settles that.
            if (status.state === 'idle') {
                alive = false;
                break;
            }
            if (status.state === 'downloading') startedAt = now();
            if (status.requires_host_confirmation) budgetMs = 16 * 60 * 1000;
            this._progress(status);
        }
        if (alive) {
            // Still up after the whole budget — the install never went through.
            this._fail(t('update.timeout'));
            return;
        }

        // Phase 2 — wait for the new build. Generous budget: a Windows install
        // plus the server rebinding its ports can take a couple of minutes, and
        // an elevation prompt on the host desktop may be waiting for someone.
        // Nothing reports progress any more, so the bar rides on its own easing
        // from wherever phase 1 left it — no jump to a fixed 98 % that would
        // then sit still for the whole restart.
        this._progress({ state: 'finalizing', percent: 0 });
        // Since when nothing at all has answered (no HTTP status, not even an
        // error page); null while answers come back.
        let silentSince = null;
        for (let i = 0; i < 200; i++) {
            await sleep(3000);
            const outcome = await BackendClient.probeHealthOutcome();
            const health = outcome.health;
            if (outcome.network) {
                if (silentSince === null) silentSince = now();
                if (now() - silentSince >= HostUpdateRun.NETWORK_RELOAD_MS) {
                    // A navigation: either the new build loads, or the browser
                    // shows the certificate warning the background requests
                    // could not. Through the rendezvous, the pending note has
                    // the entry page wait for a host still down; on the LAN a
                    // host still down shows the browser's own error page.
                    this._stopAnimation();
                    this._reload();
                    return;
                }
            } else {
                silentSince = null;
            }
            // Wait for a version that is no longer the one we started from. The
            // old server can keep answering for a few seconds (or phase 1 may
            // have broken on a transient error), and reloading onto it would
            // silently look like the update did nothing.
            if (health && health.version && health.version !== info.current) {
                localStorage.removeItem('mw_update_dismissed');
                // Close the bar before the reload wipes it — the last thing seen
                // should be a full bar, not one frozen at 96 %.
                this._stopAnimation();
                this._emit({ state: 'progress', label: this._label, percent: 100 });
                this._reload();
                return;
            }
        }
        this._fail(t('update.timeout'));
    }

    /** A status sample: new words, a new phase, and a floor for the bar. */
    _progress(status) {
        if (status.requires_host_confirmation && status.state !== 'downloading')
            this._label = t('update.confirmOnHost');
        else if (status.state === 'downloading') this._label = t('update.downloading');
        else if (status.state === 'installing') this._label = t('update.installing');
        else this._label = t('update.restarting');

        this._phase = UPDATE_PHASES[status.state] ? status.state : 'installing';
        // The host's own figure is a floor, never a setpoint: it may be stale by
        // up to a poll, and it must not drag the bar backwards.
        const pct = Math.max(0, Math.min(100, status.percent || 0));
        if (pct > this._floor) this._floor = pct;
        this._emit({ state: 'progress', label: this._label, percent: this._shown });
        this._startAnimation();
    }

    _fail(message) {
        this.busy = false;
        try {
            sessionStorage.removeItem(UPDATE_PENDING_KEY);
        } catch (_) {
            /* expires on its own */
        }
        this._stopAnimation();
        this._emit({ state: 'failed', message: message || '' });
    }

    // Ease the bar towards the ceiling of its phase, one frame at a time.
    _startAnimation() {
        if (this._raf) return;
        this._at = performance.now();
        const step = () => {
            this._raf = null;
            const now = performance.now();
            // Clamp: a backgrounded tab stops firing frames, and the catch-up
            // step on return should not slam the bar into its ceiling.
            const dt = Math.min((now - this._at) / 1000, 1);
            this._at = now;

            const phase = UPDATE_PHASES[this._phase] || UPDATE_PHASES.installing;
            let value = this._shown;
            // Catching up to a figure the host reported is still a slide, not a
            // snap — the download typically lands whole in the very first poll.
            if (value < this._floor) value += (this._floor - value) * (1 - Math.exp(-dt / 0.6));
            if (value < phase.ceiling)
                value += (phase.ceiling - value) * (1 - Math.exp(-dt / phase.tau));
            this._shown = Math.min(value, phase.ceiling);

            this._emit({ state: 'progress', label: this._label, percent: this._shown });
            this._raf = requestAnimationFrame(step);
        };
        this._raf = requestAnimationFrame(step);
    }

    _stopAnimation() {
        if (this._raf) cancelAnimationFrame(this._raf);
        this._raf = null;
    }

    _emit(view) {
        this._view = view;
        for (const fn of this._listeners) fn(view);
    }
}

/** The one run this tab can have. */
export const hostUpdate = new HostUpdateRun();
