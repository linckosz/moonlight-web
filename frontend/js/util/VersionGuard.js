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
 * Version guard — forces a reload when a new build is deployed while the PWA
 * is still open, so the app never keeps running stale code/CSS after an update.
 *
 * Single source of truth: /version.json ({ "version": "..." }). The backend
 * synthesises this from the running app version (MW_VERSION, baked in from the
 * git tag at build) — nothing to bump by hand. No service worker needed:
 * combined with the server's `Cache-Control: no-cache` + ETag revalidation on
 * text assets, location.reload() pulls a fully fresh app (HTML/CSS/JS
 * revalidated, and the version-stamped stylesheet URLs change per release).
 *
 * The version is captured at boot and compared on a timer and whenever the PWA
 * returns to the foreground (the common iOS resume case). A reload is never
 * triggered during an active stream — it would kill the session — so it is
 * deferred until streaming ends.
 *
 * Through the rendezvous a reload is the one thing that CANNOT pick up an
 * update, and would loop forever trying: the page is served by a service worker
 * out of a cache filled once, so reloading serves the same old files, which
 * notice the same new version, and reload again. Only the entry page can refill
 * that cache — it holds the manifest and the connection to fetch from — so
 * there the update goes through it instead.
 *
 * The timer alone misses an update that happened while no page was open. The
 * entry page checks the cache against the host's manifest, but only when the
 * machine's link is opened: a home-screen icon (start_url "/"), a restored tab
 * or a reload on /admin is answered from the cache by the service worker and
 * connects on its own, straight to the last machine. The baseline captured
 * above is then already the NEW version, the timer never sees a difference, and
 * the old interface runs against the updated host for the whole visit. So on
 * the way in, the cache's stamp is compared with the host's manifest first.
 */
import { bootstrapAddress, pageCameThroughTunnel, tunnelHostId } from '../net/tunnelBridge.js';

/** Written by the entry page (bootstrap/v1/boot.js) when it fills the cache. */
const SHELL_STAMP_KEY = 'mw-shell-stamp';
/** The refill already asked for in this tab, so a refill that cannot help does
 *  not turn into a loop between this page and the entry page. */
const SHELL_RECHECK_KEY = 'mw-shell-recheck';

function readStorage(storage, key) {
    try {
        return storage.getItem(key);
    } catch (_) {
        return null;
    }
}

function writeStorage(storage, key, value) {
    try {
        if (value === null) storage.removeItem(key);
        else storage.setItem(key, value);
    } catch (_) {
        /* storage refused — the loop guard is lost, nothing else */
    }
}

export const VersionGuard = {
    _boot: null,
    _interval: null,

    async start() {
        if (await this._shellIsStale()) return; // leaving for the entry page

        this._boot = await this._fetch();
        if (!this._boot) return; // version.json missing — disable guard silently

        this._interval = setInterval(() => this._check(), 60_000);
        document.addEventListener('visibilitychange', () => {
            if (document.visibilityState === 'visible') this._check();
        });
    },

    async _check() {
        const v = await this._fetch();
        if (!v || v === this._boot) return;
        // Never interrupt an active stream — retry on the next check.
        if (document.body.classList.contains('streaming-active')) return;
        const hostId = tunnelHostId();
        if (pageCameThroughTunnel() && hostId) {
            console.log(
                '[MW] New version',
                v,
                '(was',
                this._boot + ') — fetching it from the host',
            );
            location.replace(bootstrapAddress());
            return;
        }
        console.log('[MW] New version', v, '(was', this._boot + ') — reloading');
        location.reload();
    },

    /**
     * Through the rendezvous: is the interface in the cache the one the host
     * serves now? When it is not, go back through the entry page, which refills
     * the cache and hands over to the page this one was on.
     *
     * Compared on the manifest's version rather than /version.json's, because it
     * is the string the stamp was written from — and it carries a fingerprint of
     * the files, so a frontend edit under an unchanged build counts too.
     *
     * Every "cannot tell" answer (no tunnel, no stamp, manifest unreachable)
     * keeps the page: a guess that sends the user away is worse than the timer.
     *
     * @returns {Promise<boolean>} true when the page is navigating away.
     */
    async _shellIsStale() {
        const hostId = tunnelHostId();
        if (!pageCameThroughTunnel() || !hostId) return false;

        let stamp = null;
        try {
            stamp = JSON.parse(readStorage(localStorage, SHELL_STAMP_KEY) || 'null');
        } catch (_) {
            return false;
        }
        if (!stamp) return false;

        const live = await this._fetchManifestVersion();
        if (!live) return false;

        if (stamp.hostId === hostId && stamp.version === live) {
            writeStorage(sessionStorage, SHELL_RECHECK_KEY, null);
            return false;
        }

        // The entry page writes the stamp from the very manifest it filled the
        // cache from, so a second mismatch on the same version right after a
        // refill means the refill cannot fix it. Keep running rather than bounce.
        const target = hostId + ' ' + live;
        if (readStorage(sessionStorage, SHELL_RECHECK_KEY) === target) {
            console.warn('[MW] Interface cache still differs from the host after a refill:', live);
            return false;
        }
        writeStorage(sessionStorage, SHELL_RECHECK_KEY, target);

        console.log(
            '[MW] Cached interface',
            stamp.version,
            'is not the host’s',
            live,
            '— fetching it from the host',
        );
        location.replace(bootstrapAddress());
        return true;
    },

    async _fetchManifestVersion() {
        try {
            const r = await fetch('/api/app/manifest', { cache: 'no-store' });
            if (!r.ok) return null;
            return (await r.json()).version || null;
        } catch (_) {
            return null;
        }
    },

    async _fetch() {
        try {
            // no-store + cache-bust query: belt-and-suspenders against iOS WebKit.
            const r = await fetch('/version.json?_=' + Date.now(), { cache: 'no-store' });
            if (!r.ok) return null;
            return (await r.json()).version || null;
        } catch (_) {
            return null; // offline / server down — keep running
        }
    },
};
