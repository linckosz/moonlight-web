/**
 * Moonlight-Web — REST API client
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

    static async post(path, body = {}) {
        const resp = await fetch(path, {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify(body)
        });
        if (!resp.ok) return this._handleError(resp, path);
        return resp.json();
    }

    static async del(path) {
        const resp = await fetch(path, { method: 'DELETE' });
        if (!resp.ok) return this._handleError(resp, path);
        return resp.json();
    }

    static async getHosts()       { return this.get('/api/hosts'); }
    static async scanHosts()      { return this.post('/api/hosts/scan'); }
    static async addManualHost(address) { return this.post('/api/hosts/manual', { address }); }
    static async removeHost(uuid) { return this.del(`/api/hosts/${uuid}`); }
    static async getPairState(hostId)     { return this.get(`/api/hosts/${hostId}/pair`); }
    static async confirmPairing(hostId)   { return this.post(`/api/hosts/${hostId}/pair`); }
    static async getAppList(hostId)       { return this.get(`/api/hosts/${hostId}/apps`); }
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
            id = Array.from(bytes, b => b.toString(16).padStart(2, '0')).join('').toUpperCase();
            localStorage.setItem('mw_client_uniqueid', id);
        }
        return id;
    }

    static async launchApp(hostId, appId, streamingSettings = {}) {
        return this.post(`/api/hosts/${hostId}/start`,
            { appId, client_uniqueid: this.clientUniqueId(), ...streamingSettings });
    }
    static async quitApp(hostId) {
        return this.post(`/api/hosts/${hostId}/quit`,
            { client_uniqueid: this.clientUniqueId() });
    }

    // ── Auth API ───────────────────────────────────────────────────────────

    static async validatePin(pin, machineName) {
        return this.post('/api/auth/validate', { pin, machine_name: machineName });
    }
    static async generatePin()                  { return this.post('/api/admin/pin/generate'); }
    static async regeneratePin()                { return this.post('/api/auth/regenerate'); }
    static async clearPin()                     { return this.post('/api/admin/pin/clear'); }
    static async getAuthStatus()                { return this.get('/api/auth/status'); }

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
            machine_name: machineName
        });
    }

    /** Regenerate the certificate token (invalidates all existing certificates). */
    static async regenerateCertificate()          { return this.post('/api/admin/certificate/regenerate'); }

    // ── Sessions API (admin, localhost only) ──────────────────────────────

    static async getAuthSessions()              { return this.get('/api/auth/sessions'); }
    static async revokeSession(token)           { return this.post('/api/auth/sessions/revoke', { token }); }

    // ── Server Info ──────────────────────────────────────────────────────────

    static async getServerHostname()            { return this.get('/api/server/hostname'); }

    // ── Admin Settings ───────────────────────────────────────────────────────────

    static async getAdminSettings()               { return this.get('/api/admin/settings'); }
    static async saveAdminSettings(settings)      { return this.post('/api/admin/settings', settings); }

    // ── Streaming Settings ───────────────────────────────────────────────────────

    static async getStreamingSettings()            { return this.get('/api/settings/streaming'); }
    static async saveStreamingSettings(settings)   { return this.post('/api/settings/streaming', settings); }

    // ── deSEC Internet Access ────────────────────────────────────────────────────────────

    static async getInternetStatus()                { return this.get('/api/internet/status'); }
    static async enableInternet(options)            { return this.post('/api/internet/enable', options); }
    static async disableInternet()                  { return this.post('/api/internet/disable'); }
    static async refreshInternet()                  { return this.post('/api/internet/refresh'); }
    static async renewCert()                        { return this.post('/api/internet/renew-cert'); }
}
