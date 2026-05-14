/**
 * Moonlight-Web — REST API client
 */
export class BackendClient {
    static async _handleError(resp) {
        let msg = '';
        try {
            const body = await resp.json();
            msg = body.message || body.error || '';
        } catch (_) {
            // Response body is not JSON or empty
        }
        throw new Error(msg || `Request failed (${resp.status})`);
    }

    static async get(path) {
        const resp = await fetch(path);
        if (!resp.ok) return this._handleError(resp);
        return resp.json();
    }

    static async post(path, body = {}) {
        const resp = await fetch(path, {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify(body)
        });
        if (!resp.ok) return this._handleError(resp);
        return resp.json();
    }

    static async del(path) {
        const resp = await fetch(path, { method: 'DELETE' });
        if (!resp.ok) return this._handleError(resp);
        return resp.json();
    }

    static async getHosts()       { return this.get('/api/hosts'); }
    static async scanHosts()      { return this.post('/api/hosts/scan'); }
    static async addManualHost(address) { return this.post('/api/hosts/manual', { address }); }
    static async removeHost(uuid) { return this.del(`/api/hosts/${uuid}`); }
    static async getPairState(hostId)     { return this.get(`/api/hosts/${hostId}/pair`); }
    static async confirmPairing(hostId)   { return this.post(`/api/hosts/${hostId}/pair`); }
    static async getAppList(hostId)       { return this.get(`/api/hosts/${hostId}/apps`); }
    static async launchApp(hostId, appId) { return this.post(`/api/hosts/${hostId}/start`, { appId }); }
    static async quitApp(hostId)          { return this.post(`/api/hosts/${hostId}/quit`); }

    // ── Admin Settings ───────────────────────────────────────────────────────────

    static async getAdminSettings()               { return this.get('/api/admin/settings'); }
    static async saveAdminSettings(settings)      { return this.post('/api/admin/settings', settings); }

    // ── Streaming Settings ───────────────────────────────────────────────────────

    static async getStreamingSettings()            { return this.get('/api/settings/streaming'); }
    static async saveStreamingSettings(settings)   { return this.post('/api/settings/streaming', settings); }

    // ── Cloudflare tunnel (remote access via cloudflared) ──────────────────────────────────

    static async getTunnelStatus()                 { return this.get('/api/tunnel/status'); }
    static async configureTunnel(options)           { return this.post('/api/tunnel/configure', options); }
    static async disableTunnel()                    { return this.post('/api/tunnel/disable'); }
}
