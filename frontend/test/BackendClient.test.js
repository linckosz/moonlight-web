/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect, beforeEach, vi } from 'vitest';
import { BackendClient } from '../js/api/BackendClient.js';

// Build a fake Response with the given status + JSON body.
function jsonResponse(body, { ok = true, status = 200 } = {}) {
    return {
        ok,
        status,
        json: async () => body,
        text: async () => (typeof body === 'string' ? body : JSON.stringify(body)),
    };
}

// Mock fetch that answers the admin-key probe every write performs, and routes
// everything else to `handler`. Returns the mock so callers can inspect the
// non-token calls via `apiCalls()`.
function mockFetch(handler, { adminKey = 'ADMIN-KEY' } = {}) {
    const fetchMock = vi.fn().mockImplementation((path, init) => {
        if (path === '/api/admin/token') return Promise.resolve(jsonResponse({ token: adminKey }));
        return typeof handler === 'function' ? handler(path, init) : Promise.resolve(handler);
    });
    fetchMock.apiCalls = () => fetchMock.mock.calls.filter((c) => c[0] !== '/api/admin/token');
    vi.stubGlobal('fetch', fetchMock);
    return fetchMock;
}

describe('BackendClient', () => {
    beforeEach(() => {
        vi.restoreAllMocks();
        localStorage.clear();
        sessionStorage.clear();
        BackendClient._adminKeyPromise = null; // cached across calls by design
    });

    it('GET returns parsed JSON on success', async () => {
        const fetchMock = vi.fn().mockResolvedValue(jsonResponse({ hosts: [] }));
        vi.stubGlobal('fetch', fetchMock);
        await expect(BackendClient.getHosts()).resolves.toEqual({ hosts: [] });
        expect(fetchMock).toHaveBeenCalledWith('/api/hosts');
    });

    it('POST sends a JSON body with the right headers', async () => {
        const fetchMock = mockFetch(jsonResponse({ ok: true }));
        await BackendClient.addManualHost('10.0.0.9');
        const [path, init] = fetchMock.apiCalls()[0];
        expect(path).toBe('/api/hosts/manual');
        expect(init.method).toBe('POST');
        expect(JSON.parse(init.body)).toEqual({ address: '10.0.0.9' });
        expect(init.headers['Content-Type']).toBe('application/json');
    });

    // The admin key is what separates our own frontend from a page that merely
    // borrowed the browser's address: a custom header cannot cross origins
    // without a preflight the backend never answers.
    it('POST carries the admin key on writes', async () => {
        const fetchMock = mockFetch(jsonResponse({ ok: true }));
        await BackendClient.addManualHost('10.0.0.9');
        expect(fetchMock.apiCalls()[0][1].headers['X-MW-Admin-Key']).toBe('ADMIN-KEY');
    });

    it('POST refreshes a stale admin key once on 403 and replays', async () => {
        // First probe hands out the key the client already had; the server has
        // since restarted and only honours the second one.
        const keys = ['OLD-KEY', 'NEW-KEY'];
        const fetchMock = vi.fn().mockImplementation((path, init) => {
            if (path === '/api/admin/token')
                return Promise.resolve(jsonResponse({ token: keys.shift() || 'NEW-KEY' }));
            if (init.headers['X-MW-Admin-Key'] !== 'NEW-KEY')
                return Promise.resolve(
                    jsonResponse({ error: 'forbidden' }, { ok: false, status: 403 }),
                );
            return Promise.resolve(jsonResponse({ ok: true }));
        });
        vi.stubGlobal('fetch', fetchMock);
        await expect(BackendClient.generatePin()).resolves.toEqual({ ok: true });
        expect(fetchMock.mock.calls.filter((c) => c[0] === '/api/admin/token')).toHaveLength(2);
    });

    it('remote clients get no key and still send the request', async () => {
        const fetchMock = vi.fn().mockImplementation((path) => {
            if (path === '/api/admin/token')
                return Promise.resolve(jsonResponse({ error: 'forbidden' }, { ok: false, status: 403 }));
            return Promise.resolve(jsonResponse({ ok: true }));
        });
        vi.stubGlobal('fetch', fetchMock);
        await BackendClient.addManualHost('10.0.0.9');
        const init = fetchMock.mock.calls.filter((c) => c[0] !== '/api/admin/token')[0][1];
        expect(init.headers['X-MW-Admin-Key']).toBeUndefined();
    });

    it('DELETE hits the right endpoint', async () => {
        const fetchMock = mockFetch(jsonResponse({}));
        await BackendClient.removeHost('uuid-1');
        const [path, init] = fetchMock.apiCalls()[0];
        expect(path).toBe('/api/hosts/uuid-1');
        expect(init.method).toBe('DELETE');
        expect(init.headers['X-MW-Admin-Key']).toBe('ADMIN-KEY');
    });

    it('throws a rich error on a non-auth failure', async () => {
        vi.stubGlobal('fetch', vi.fn().mockResolvedValue(jsonResponse({ message: 'boom' }, { ok: false, status: 500 })));
        await expect(BackendClient.getHosts()).rejects.toMatchObject({
            message: 'boom',
            statusCode: 500,
        });
    });

    it('does not reload on a 401 from an auth endpoint', async () => {
        vi.stubGlobal('fetch', vi.fn().mockResolvedValue(jsonResponse({ error: 'nope' }, { ok: false, status: 401 })));
        await expect(BackendClient.getAuthStatus()).rejects.toMatchObject({ statusCode: 401 });
    });

    it('breaks the reload loop using the sessionStorage guard', async () => {
        sessionStorage.setItem('mw_auth_reload', '1'); // pretend we already reloaded once
        vi.stubGlobal('fetch', vi.fn().mockResolvedValue(jsonResponse({ message: 'authentication_required' }, { ok: false, status: 401 })));
        await expect(BackendClient.getHosts()).rejects.toMatchObject({ statusCode: 401 });
        expect(sessionStorage.getItem('mw_auth_reload')).toBeNull(); // guard cleared
    });

    it('aborts a POST after the client timeout', async () => {
        // fetch rejects with an AbortError when the signal fires.
        mockFetch(
            (_p, init) =>
                new Promise((_resolve, reject) => {
                    init.signal.addEventListener('abort', () => {
                        const e = new Error('aborted');
                        e.name = 'AbortError';
                        reject(e);
                    });
                }),
        );
        await expect(BackendClient.post('/slow', {}, { timeoutMs: 5 })).rejects.toMatchObject({
            message: 'server_timeout',
            aborted: true,
        });
    });

    it('generates and persists a 16-hex-char client unique id', () => {
        const id = BackendClient.clientUniqueId();
        expect(id).toMatch(/^[0-9A-F]{16}$/);
        expect(BackendClient.clientUniqueId()).toBe(id); // stable across calls
        expect(localStorage.getItem('mw_client_uniqueid')).toBe(id);
    });

    it('launchApp includes the client unique id and streaming settings', async () => {
        const fetchMock = mockFetch(jsonResponse({ started: true }));
        await BackendClient.launchApp('host1', 42, { bitrate: 20000 });
        const body = JSON.parse(fetchMock.apiCalls()[0][1].body);
        expect(body).toMatchObject({ appId: 42, bitrate: 20000 });
        expect(body.client_uniqueid).toMatch(/^[0-9A-F]{16}$/);
    });

    it('downloadCertificate returns raw text', async () => {
        vi.stubGlobal('fetch', vi.fn().mockResolvedValue(jsonResponse('CERT-TOKEN')));
        await expect(BackendClient.downloadCertificate()).resolves.toBe('CERT-TOKEN');
    });
});
