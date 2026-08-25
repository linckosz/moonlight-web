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
 * MoonlightWeb — REST API client
 */

/**
 * An `Error` decorated with the HTTP context of a failed request. Thrown by
 * every `BackendClient` method; `LoginView`, `PairDialog` and `MoonlightApp`
 * branch on these fields.
 *
 * @typedef {Error & {
 *   statusCode?: number,
 *   responseBody?: any,
 *   aborted?: boolean,
 * }} BackendError
 */

import { loadOrCreateIdentity, rememberHostIdentity } from '../util/pairingCrypto.js';

export class BackendClient {
    /** Cached promise for the per-run admin key (see _adminKey). */
    static _adminKeyPromise = null;

    /**
     * The host's admin key, or null when this browser is not entitled to one
     * (a remote session — its admin calls are refused server-side anyway).
     *
     * The backend requires it on every admin write on top of the request coming
     * from the host machine, because the source IP alone is something a
     * malicious page can borrow: it makes the victim's own browser issue the
     * request. A custom header cannot be sent cross-origin without a preflight
     * the backend never answers, so only same-origin code can present it.
     *
     * The key is regenerated whenever the server restarts, hence the refresh.
     */
    static async _adminKey({ refresh = false } = {}) {
        if (refresh) this._adminKeyPromise = null;
        if (!this._adminKeyPromise) {
            this._adminKeyPromise = (async () => {
                // Hard-bounded: every write waits on this, so a backend that
                // accepts the connection and then goes quiet must not freeze the
                // UI. Failing here only costs the header — the request still
                // goes out, and a 403 triggers one refresh + replay below.
                let controller = null;
                let timer = null;
                if (typeof AbortController !== 'undefined') {
                    controller = new AbortController();
                    timer = setTimeout(() => controller.abort(), 3000);
                }
                try {
                    const resp = await fetch('/api/admin/token', {
                        cache: 'no-store',
                        signal: controller ? controller.signal : undefined,
                    });
                    if (!resp.ok) return null;
                    const data = await resp.json();
                    return data.token || null;
                } catch (_) {
                    return null;
                } finally {
                    if (timer) clearTimeout(timer);
                }
            })();
        }
        return this._adminKeyPromise;
    }

    static async _handleError(resp, path = '') {
        let msg = '';
        let body = null;
        try {
            body = await resp.json();
            msg = body.message || body.error || '';
        } catch (_) {
            // Response body is not JSON or empty
        }

        // Force page reload on auth errors — session expired or revoked.
        // Skip auth endpoints (expected to return 401) and use sessionStorage
        // guard to prevent infinite reload loops.
        // Player routes answer 401 for a wrong PIN or a dead link — that is the
        // normal path for a guest who has no session at all, and reloading the
        // page would throw away the join screen instead of showing the error.
        const isAuthEndpoint =
            path.startsWith('/api/auth/') || path.startsWith('/api/share/player/');
        if (!isAuthEndpoint && (resp.status === 401 || msg === 'authentication_required')) {
            if (!sessionStorage.getItem('mw_auth_reload')) {
                sessionStorage.setItem('mw_auth_reload', '1');
                console.warn('[MW] Authentication required — reloading page');
                window.location.reload();
                return new Promise(() => {});
            }
            sessionStorage.removeItem('mw_auth_reload');
        }

        const error = /** @type {BackendError} */ (
            new Error(msg || `Request failed (${resp.status})`)
        );
        error.statusCode = resp.status;
        error.responseBody = body;
        throw error;
    }

    static async get(path) {
        const resp = await fetch(path);
        if (!resp.ok) return this._handleError(resp, path);
        return resp.json();
    }

    static async post(path, body = {}, { timeoutMs = 0, _retried = false } = {}) {
        // Optional client-side timeout: when the backend hangs (crashed/stuck)
        // without closing the socket, abort early so the UI gets fast feedback
        // instead of waiting for the browser/proxy timeout (~30s → 504).
        let controller = null;
        let timer = null;
        if (timeoutMs > 0 && typeof AbortController !== 'undefined') {
            controller = new AbortController();
            timer = setTimeout(() => controller.abort(), timeoutMs);
        }
        try {
            const headers = { 'Content-Type': 'application/json' };
            const adminKey = await this._adminKey();
            if (adminKey) headers['X-MW-Admin-Key'] = adminKey;

            const resp = await fetch(path, {
                method: 'POST',
                headers,
                body: JSON.stringify(body),
                signal: controller ? controller.signal : undefined,
            });
            // A 403 on an admin route means the key we hold is stale (the server
            // restarted and minted a new one). Refresh it and replay once — the
            // request was refused before any handler ran, so replaying is safe.
            if (resp.status === 403 && !_retried) {
                const fresh = await this._adminKey({ refresh: true });
                if (fresh && fresh !== adminKey)
                    return this.post(path, body, { timeoutMs, _retried: true });
            }
            if (!resp.ok) return this._handleError(resp, path);
            return resp.json();
        } catch (err) {
            if (err && err.name === 'AbortError') {
                const e = /** @type {BackendError} */ (new Error('server_timeout'));
                e.statusCode = 0;
                e.aborted = true;
                throw e;
            }
            throw err;
        } finally {
            if (timer) clearTimeout(timer);
        }
    }

    static async del(path) {
        /** @type {Record<string, string>} */
        const headers = {};
        const adminKey = await this._adminKey();
        if (adminKey) headers['X-MW-Admin-Key'] = adminKey;

        const resp = await fetch(path, { method: 'DELETE', headers });
        if (!resp.ok) return this._handleError(resp, path);
        return resp.json();
    }

    static async getHosts() {
        return this.get('/api/hosts');
    }
    static async scanHosts() {
        return this.post('/api/hosts/scan');
    }
    static async addManualHost(address) {
        return this.post('/api/hosts/manual', { address });
    }
    static async removeHost(uuid) {
        return this.del(`/api/hosts/${uuid}`);
    }
    static async wakeHost(uuid) {
        return this.post(`/api/hosts/${uuid}/wol`);
    }
    /** Give a host a local alias. An empty name clears it — the card then shows
     *  the name the host reports for itself again. Nothing is written host-side. */
    static async renameHost(uuid, name) {
        return this.post(`/api/hosts/${uuid}/name`, { name });
    }
    /** Restart the streaming service the host runs. Only offered where the host
     *  list said `restartSupported`; anywhere else the server answers 501. */
    static async restartHost(uuid) {
        return this.post(`/api/hosts/${uuid}/restart`, {}, { timeoutMs: 30000 });
    }
    static async startPairing(hostId) {
        return this.post(`/api/hosts/${hostId}/pair/start`);
    }
    static async confirmPairing(hostId) {
        return this.post(`/api/hosts/${hostId}/pair`);
    }
    static async getAppList(hostId) {
        return this.get(`/api/hosts/${hostId}/apps`);
    }
    /** Backend types this server can drive, so the UI never hardcodes them. */
    static async getBackendTypes() {
        return this.get('/api/backends');
    }
    /**
     * Declare which backend drives a host, then pair it. Answers only once the
     * pairing has settled, so give it room: the handshake talks to the host.
     *
     * Omit apiToken to keep the stored one — the browser is never sent it back,
     * so this is how the URL gets fixed without retyping the secret.
     */
    static async setHostBackend(hostId, { type, apiUrl, apiToken, pairUser, pairPassword }) {
        const body = { type, apiUrl };
        if (apiToken) body.apiToken = apiToken;
        if (pairUser) body.pairUser = pairUser;
        if (pairPassword) body.pairPassword = pairPassword;
        return this.post(`/api/hosts/${hostId}/backend`, body, { timeoutMs: 90000 });
    }
    static async clearHostBackend(hostId) {
        return this.del(`/api/hosts/${hostId}/backend`);
    }
    /**
     * A host's backend configuration plus what that backend can do
     * (multiUser / provisioning / lobbies). Capabilities are absent on an
     * unmanaged host. Never includes the token.
     */
    static async getHostBackend(hostId) {
        return this.get(`/api/hosts/${hostId}/backend`);
    }
    /**
     * Seats as the host's backend defines them: a Windows account plus its
     * Apollo on MultiSeat, a paired client on Wolf, the host itself on a plain
     * GameStream host.
     */
    static async getHostSeats(hostId) {
        return this.get(`/api/hosts/${hostId}/seats`);
    }
    /** Only meaningful where capabilities.provisioning is true. */
    static async provisionHostSeat(hostId, params) {
        return this.post(`/api/hosts/${hostId}/seats`, params, { timeoutMs: 120000 });
    }
    static async teardownHostSeat(hostId, seatId) {
        return this.del(`/api/hosts/${hostId}/seats/${seatId}`);
    }
    /**
     * Free a seat from whoever owns it, leaving the seat itself intact. For a
     * seat stranded by a device that never came back — ownership is durable on
     * purpose, so nothing else releases it.
     */
    static async releaseHostSeatOwner(hostId, seatId) {
        return this.del(`/api/hosts/${hostId}/seats/${seatId}/owner`);
    }
    /**
     * Per-browser Sunshine client unique ID, persisted in localStorage.
     * Each browser gets its own 16-hex-char ID so Sunshine treats their
     * sessions independently (one browser won't cancel/take over another's).
     */
    static clientUniqueId() {
        let id = localStorage.getItem('mw_client_uniqueid');
        if (!id || !/^[0-9A-F]{16}$/.test(id)) {
            const bytes = new Uint8Array(8);
            crypto.getRandomValues(bytes);
            id = Array.from(bytes, (b) => b.toString(16).padStart(2, '0'))
                .join('')
                .toUpperCase();
            localStorage.setItem('mw_client_uniqueid', id);
        }
        return id;
    }

    /**
     * Uniqueid for a dual-stream slot. Slot 0 is the browser's regular ID;
     * slot 1 derives a STABLE second ID (last byte XOR 0xFF) so the standby
     * session coexists with the primary in Sunshine's per-uniqueid keying and
     * survives reloads (a reload can /resume its own standby session).
     */
    static clientUniqueIdForSlot(slot) {
        const id = this.clientUniqueId();
        if (!slot) return id;
        const last = parseInt(id.slice(14, 16), 16) ^ 0xff;
        return id.slice(0, 14) + last.toString(16).padStart(2, '0').toUpperCase();
    }

    static async launchApp(hostId, appId, streamingSettings = {}) {
        return this.post(
            `/api/hosts/${hostId}/start`,
            {
                appId,
                client_uniqueid: this.clientUniqueId(),
                ...streamingSettings,
            },
            // Fail fast if the backend hangs/crashes instead of waiting for the
            // browser/proxy ~30s gateway timeout.
            //
            // 25s, not 15s: a launch that lands while a previous stream worker
            // is still tearing down waits on that child (killed after 5s at the
            // latest) and only THEN starts talking to Sunshine, which itself
            // spends ~12s finishing the old app before /resume answers. 15s cut
            // that legitimate sequence off mid-flight — the browser gave up, the
            // backend kept going, and the next attempt hit the very same wall.
            { timeoutMs: 25000 },
        );
    }
    static async quitApp(hostId, extra = {}) {
        // Fail fast if the backend is dead — don't wait 30s for a /quit that'll
        // never arrive while the UI is stuck in the quit animation.
        // extra: dual-stream scoping — { session_slot, client_uniqueid } quits
        // one slot only (the caller passes the slot's own uniqueid).
        return this.post(
            `/api/hosts/${hostId}/quit`,
            { client_uniqueid: this.clientUniqueId(), ...extra },
            { timeoutMs: 5000 },
        );
    }

    /**
     * Force the host's running app down, whoever started it. The scoped
     * quitApp above needs a live view to name its slot and uniqueid; this is
     * the way out when there is none left.
     */
    static async stopHostSession(hostId) {
        return this.post(`/api/hosts/${hostId}/stop-session`, {}, { timeoutMs: 15000 });
    }

    // ── Session sharing ────────────────────────────────────────────────────
    // Owner side: the three player rows behind the Share button. Any
    // authenticated user may share; no admin key involved.

    static async getShareStatus() {
        return this.get('/api/share/status');
    }
    /** Mint a fresh link + PIN for a player slot, revoking the previous pair. */
    static async shareActivate(slot) {
        return this.post(`/api/share/slots/${slot}/activate`);
    }
    /**
     * The link and PIN of a share that is already live, for an owner reopening
     * the popin. Answers {available:false} when the backend restarted since:
     * only the digests are persisted.
     */
    static async shareCredentials(slot) {
        return this.post(`/api/share/slots/${slot}/credentials`);
    }
    /** Update input permissions while the popin is still open. */
    static async sharePermissions(slot, permissions) {
        return this.post(`/api/share/slots/${slot}/permissions`, permissions);
    }
    /** Freeze the permissions for the rest of this activation (popin closed). */
    static async shareLock(slot) {
        return this.post(`/api/share/slots/${slot}/lock`);
    }
    /** Revoke a share: link, PIN and any live stream on that slot. */
    static async shareDeactivate(slot) {
        return this.post(`/api/share/slots/${slot}/deactivate`);
    }

    // Player side: no session cookie, no admin key — the mw_player cookie the
    // PIN buys is the only credential, and it only opens these four.

    static async playerInfo(token) {
        return this.get(`/api/share/player/info?token=${encodeURIComponent(token)}`);
    }
    static async playerPin(token, pin) {
        return this.post('/api/share/player/pin', { token, pin });
    }
    /** @param {string} [aspect] "W:H" override; omitted, the backend reuses the
     *  ratio the owner's session settled on for that host. */
    static async playerJoin(token, height, aspect) {
        const body = aspect ? { token, height, aspect } : { token, height };
        return this.post('/api/share/player/join', body, { timeoutMs: 25000 });
    }
    static async playerLeave() {
        return this.post('/api/share/player/leave', {}, { timeoutMs: 5000 });
    }

    // Open macOS' Screen Recording privacy pane on the host so the user can
    // grant Sunshine capture permission (localhost + macOS only backend-side).
    static async openScreenRecordingSettings() {
        return this.post('/api/system/open-screen-recording');
    }

    // Stop the local Sunshine server on the host (localhost-only backend-side).
    static async stopSunshine() {
        return this.post('/api/system/stop-sunshine');
    }

    // Start the local Sunshine server on the host (localhost-only backend-side).
    static async startSunshine() {
        return this.post('/api/system/start-sunshine');
    }

    // ── Auth API ───────────────────────────────────────────────────────────

    /** @param {boolean} remember Keep the session across browser restarts (the
     *  default). False asks the server for a session-scoped cookie and a short
     *  server-side lifetime — for a machine the visitor does not own. */
    static async validatePin(pin, machineName, remember = true) {
        const identity = await loadOrCreateIdentity();
        const resp = await this.post('/api/auth/validate', {
            pin,
            machine_name: machineName,
            remember: remember !== false,
            // MW-BIND-v1: pairing with a PIN is the moment this browser's key is
            // registered. It is the only path allowed from the internet — the
            // silent one below is restricted to peers that never crossed it.
            public_key: identity?.publicKeyBase64 || undefined,
        });
        if (resp?.host_public_key) {
            await rememberHostIdentity(resp.host_public_key, resp.host_id);
        }
        return resp;
    }

    /**
     * Register this browser's MW-BIND-v1 key on a session created before the
     * mechanism existed, and collect the host's own identity.
     *
     * Trust-on-first-use over the already-authenticated session cookie. The
     * server only accepts it from loopback, the LAN or a mesh VPN; from the
     * internet it answers 403 and the visitor pairs with a PIN instead. Silent
     * and best-effort by design: a browser that cannot bind still streams, it
     * simply does so without the fingerprint binding.
     */
    static async ensurePairingKey() {
        const identity = await loadOrCreateIdentity();
        if (!identity) return null;
        if (identity.hostPublicKey) return identity; // already paired with this host

        try {
            const resp = await this.post('/api/auth/pairing-key', {
                public_key: identity.publicKeyBase64,
            });
            if (resp?.host_public_key) {
                await rememberHostIdentity(resp.host_public_key, resp.host_id);
                return loadOrCreateIdentity();
            }
        } catch (e) {
            // All of these are expected states, not failures.
            if (e.statusCode === 401) {
                // No session at all. This is the host machine reaching itself by
                // localhost or a private-IP literal: it gets local privilege
                // without ever logging in, so there is no session to hang a key
                // on. Nothing to protect either — no relay sits between a
                // browser and the machine it is running on.
                console.log('[MW-BIND] Host-machine access, no session — binding not applicable');
            } else if (e.statusCode === 403) {
                console.log('[MW-BIND] Remote peer — pair with a PIN to bind this browser');
            } else {
                console.log('[MW-BIND] Silent pairing not available:', e.message);
            }
        }
        return identity;
    }
    static async generatePin() {
        return this.post('/api/admin/pin/generate');
    }
    static async regeneratePin() {
        return this.post('/api/auth/regenerate');
    }
    static async clearPin() {
        return this.post('/api/admin/pin/clear');
    }
    static async getAuthStatus() {
        return this.get('/api/auth/status');
    }
    /** End this browser's own session and clear its cookie. Works from anywhere
     *  (no localhost gate) — it is the visitor's way out of a public machine. */
    static async logout() {
        return this.post('/api/auth/logout');
    }
    /** Redeem the host key (?mwk=... from the host machine's own entry points)
     *  for a localhost-equivalent session over the public domain. */
    static async redeemHostKey(key) {
        return this.post('/api/auth/host-key', { key });
    }
    /** Spend the remote admin password to give this (already authenticated,
     *  LAN) session the same admin access the host machine has. */
    static async adminUnlock(password) {
        return this.post('/api/auth/admin-unlock', { password });
    }
    /** Change the remote admin password and/or turn remote administration on
     *  and off. Body may carry either or both of {password, enabled}. */
    static async saveRemoteAdmin(body) {
        return this.post('/api/admin/password', body);
    }

    // ── Certificate Authentication ─────────────────────────────────────────

    /** Download the certificate token as a text file. Returns the raw text content. */
    static async downloadCertificate() {
        const resp = await fetch('/api/admin/certificate/download');
        if (!resp.ok) return this._handleError(resp);
        return resp.text();
    }

    /** Validate a certificate token (alternative to PIN). Sends the raw token content. */
    static async validateCertificate(certificateContent, machineName, remember = true) {
        const identity = await loadOrCreateIdentity();
        const resp = await this.post('/api/auth/validate', {
            certificate: certificateContent,
            machine_name: machineName,
            remember: remember !== false,
            public_key: identity?.publicKeyBase64 || undefined,
        });
        if (resp?.host_public_key) {
            await rememberHostIdentity(resp.host_public_key, resp.host_id);
        }
        return resp;
    }

    /** Regenerate the certificate token (invalidates all existing certificates). */
    static async regenerateCertificate() {
        return this.post('/api/admin/certificate/regenerate');
    }

    // ── Sessions API (admin, localhost only) ──────────────────────────────

    static async getAuthSessions() {
        return this.get('/api/auth/sessions');
    }
    static async revokeSession(token) {
        return this.post('/api/auth/sessions/revoke', { token });
    }
    static async renameSession(token, name) {
        return this.post('/api/auth/sessions/rename', { token, machine_name: name });
    }

    // ── Server Info ──────────────────────────────────────────────────────────

    static async getServerHostname() {
        return this.get('/api/server/hostname');
    }

    /** Ports the server actually listens on: { http_port, https_port }. Public
     *  (no session needed) — the login page uses it to build the https://localhost
     *  URL that lets the host machine re-prove itself. */
    static async getServerStatus() {
        return this.get('/api/server/status');
    }

    /** Check whether a newer MoonlightWeb release is available for this host.
     *  Returns { current, latest, update_available, download_url, release_url,
     *  asset_name, self_update: { supported, method, requires_host_confirmation } }.
     *  Best-effort — resolves null on any failure so callers can stay silent when
     *  offline / rate-limited. */
    static async checkForUpdate() {
        try {
            return await this.get('/api/update/check');
        } catch (_) {
            return null;
        }
    }

    /** Ask the host to download and apply the update, unattended. Rejects with
     *  409 when no update is available or one is already running. */
    static async startUpdate() {
        return this.post('/api/update/start');
    }

    /** Progress of the running update: { state, percent, version, error,
     *  requires_host_confirmation }. Stops answering once the installer takes the
     *  server down — that is the expected end of the polling loop. */
    static async getUpdateStatus() {
        return this.get('/api/update/status');
    }

    /** Liveness + version probe, used to detect the host coming back up after an
     *  update. Raw fetch on purpose: the shared error path would reload the page
     *  on the 401/connection failures that are normal while the server restarts. */
    static async probeHealth() {
        try {
            const resp = await fetch('/api/health', { cache: 'no-store' });
            if (!resp.ok) return null;
            return await resp.json();
        } catch (_) {
            return null;
        }
    }

    // ── Admin Settings ───────────────────────────────────────────────────────────

    static async getAdminSettings() {
        return this.get('/api/admin/settings');
    }
    static async saveAdminSettings(settings) {
        return this.post('/api/admin/settings', settings);
    }

    // ── Streaming Settings ───────────────────────────────────────────────────────

    static async getStreamingSettings() {
        return this.get('/api/settings/streaming');
    }
    static async saveStreamingSettings(settings) {
        return this.post('/api/settings/streaming', settings);
    }

    // ── First-run setup wizard (localhost only) ──────────────────────────────

    static async getSetupStatus() {
        return this.get('/api/setup/status');
    }
    static async applySetup(options) {
        // No client timeout: a Sunshine DMG download + install can take minutes.
        return this.post('/api/setup/apply', options, { timeoutMs: 0 });
    }
    // Are these the credentials of the Sunshine already installed here? Answers
    // { ok, reason: 'unauthorized' | 'unreachable' | 'missing' }.
    static async checkSunshineCredentials(username, password) {
        return this.post('/api/setup/sunshine-check', { username, password }, { timeoutMs: 20000 });
    }

    // ── Internet Access (PowerDNS) ───────────────────────────────────────────────────────

    static async getInternetStatus() {
        return this.get('/api/internet/status');
    }
    static async enableInternet(options) {
        return this.post('/api/internet/enable', options);
    }
    static async disableInternet() {
        return this.post('/api/internet/disable');
    }
    static async refreshInternet() {
        return this.post('/api/internet/refresh');
    }
    static async renewCert() {
        return this.post('/api/internet/renew-cert');
    }
}
