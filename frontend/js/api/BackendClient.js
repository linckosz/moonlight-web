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
export class BackendClient {
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
        const isAuthEndpoint = path.startsWith('/api/auth/');
        if (!isAuthEndpoint && (resp.status === 401 || msg === 'authentication_required')) {
            if (!sessionStorage.getItem('mw_auth_reload')) {
                sessionStorage.setItem('mw_auth_reload', '1');
                console.warn('[MW] Authentication required — reloading page');
                window.location.reload();
                return new Promise(() => {});
            }
            sessionStorage.removeItem('mw_auth_reload');
        }

        const error = new Error(msg || `Request failed (${resp.status})`);
        error.statusCode = resp.status;
        error.responseBody = body;
        throw error;
    }

    static async get(path) {
        const resp = await fetch(path);
        if (!resp.ok) return this._handleError(resp, path);
        return resp.json();
    }

    static async post(path, body = {}, { timeoutMs = 0 } = {}) {
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
            const resp = await fetch(path, {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify(body),
                signal: controller ? controller.signal : undefined,
            });
            if (!resp.ok) return this._handleError(resp, path);
            return resp.json();
        } catch (err) {
            if (err && err.name === 'AbortError') {
                const e = new Error('server_timeout');
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
        const resp = await fetch(path, { method: 'DELETE' });
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
    static async getPairState(hostId) {
        return this.get(`/api/hosts/${hostId}/pair`);
    }
    static async confirmPairing(hostId) {
        return this.post(`/api/hosts/${hostId}/pair`);
    }
    static async getAppList(hostId) {
        return this.get(`/api/hosts/${hostId}/apps`);
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
            { timeoutMs: 15000 },
        );
    }
    static async quitApp(hostId) {
        // Fail fast if the backend is dead — don't wait 30s for a /quit that'll
        // never arrive while the UI is stuck in the quit animation.
        return this.post(
            `/api/hosts/${hostId}/quit`,
            { client_uniqueid: this.clientUniqueId() },
            { timeoutMs: 5000 },
        );
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

    // ── Auth API ───────────────────────────────────────────────────────────

    static async validatePin(pin, machineName) {
        return this.post('/api/auth/validate', { pin, machine_name: machineName });
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

    // ── Certificate Authentication ─────────────────────────────────────────

    /** Download the certificate token as a text file. Returns the raw text content. */
    static async downloadCertificate() {
        const resp = await fetch('/api/admin/certificate/download');
        if (!resp.ok) return this._handleError(resp);
        return resp.text();
    }

    /** Validate a certificate token (alternative to PIN). Sends the raw token content. */
    static async validateCertificate(certificateContent, machineName) {
        return this.post('/api/auth/validate', {
            certificate: certificateContent,
            machine_name: machineName,
        });
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

    /** Check whether a newer MoonlightWeb release is available for this host.
     *  Returns { current, latest, update_available, download_url, release_url,
     *  asset_name }. Best-effort — resolves null on any failure so callers can
     *  stay silent when offline / rate-limited. */
    static async checkForUpdate() {
        try {
            return await this.get('/api/update/check');
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
