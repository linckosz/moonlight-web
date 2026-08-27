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
 */
import { pageCameThroughTunnel, tunnelHostId } from '../net/tunnelBridge.js';

export const VersionGuard = {
    _boot: null,
    _interval: null,

    async start() {
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
            console.log('[MW] New version', v, '(was', this._boot + ') — fetching it from the host');
            location.replace(`/${hostId}`);
            return;
        }
        console.log('[MW] New version', v, '(was', this._boot + ') — reloading');
        location.reload();
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
