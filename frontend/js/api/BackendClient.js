/**
 * Moonlight-Web — REST API client
 */
export class BackendClient {
    static async get(path) {
        const resp = await fetch(path);
        if (!resp.ok)
            throw new Error(`GET ${path} failed: ${resp.status}`);
        return resp.json();
    }

    static async post(path, body = {}) {
        const resp = await fetch(path, {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify(body)
        });
        if (!resp.ok)
            throw new Error(`POST ${path} failed: ${resp.status}`);
        return resp.json();
    }

    static async getHosts()       { return this.get('/api/hosts'); }
    static async scanHosts()      { return this.post('/api/hosts/scan'); }
    static async addManualHost(address) { return this.post('/api/hosts/manual', { address }); }
    static async getPairState(hostId)     { return this.get(`/api/hosts/${hostId}/pair`); }
    static async confirmPairing(hostId)   { return this.post(`/api/hosts/${hostId}/pair`); }
}
