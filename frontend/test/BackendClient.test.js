/*
 * Moonlight-Web — TNR suite. Copyright (C) 2026 Bruno Martin.
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

describe('BackendClient', () => {
    beforeEach(() => {
        vi.restoreAllMocks();
        localStorage.clear();
        sessionStorage.clear();
    });

    it('GET returns parsed JSON on success', async () => {
        const fetchMock = vi.fn().mockResolvedValue(jsonResponse({ hosts: [] }));
        vi.stubGlobal('fetch', fetchMock);
        await expect(BackendClient.getHosts()).resolves.toEqual({ hosts: [] });
        expect(fetchMock).toHaveBeenCalledWith('/api/hosts');
    });

    it('POST sends a JSON body with the right headers', async () => {
        const fetchMock = vi.fn().mockResolvedValue(jsonResponse({ ok: true }));
        vi.stubGlobal('fetch', fetchMock);
        await BackendClient.addManualHost('10.0.0.9');
        const [path, init] = fetchMock.mock.calls[0];
        expect(path).toBe('/api/hosts/manual');
        expect(init.method).toBe('POST');
        expect(JSON.parse(init.body)).toEqual({ address: '10.0.0.9' });
        expect(init.headers['Content-Type']).toBe('application/json');
    });

    it('DELETE hits the right endpoint', async () => {
        const fetchMock = vi.fn().mockResolvedValue(jsonResponse({}));
        vi.stubGlobal('fetch', fetchMock);
        await BackendClient.removeHost('uuid-1');
        expect(fetchMock).toHaveBeenCalledWith('/api/hosts/uuid-1', { method: 'DELETE' });
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
        vi.stubGlobal(
            'fetch',
            vi.fn().mockImplementation(
                (_p, init) =>
                    new Promise((_resolve, reject) => {
                        init.signal.addEventListener('abort', () => {
                            const e = new Error('aborted');
                            e.name = 'AbortError';
                            reject(e);
                        });
                    }),
            ),
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
        const fetchMock = vi.fn().mockResolvedValue(jsonResponse({ started: true }));
        vi.stubGlobal('fetch', fetchMock);
        await BackendClient.launchApp('host1', 42, { bitrate: 20000 });
        const body = JSON.parse(fetchMock.mock.calls[0][1].body);
        expect(body).toMatchObject({ appId: 42, bitrate: 20000 });
        expect(body.client_uniqueid).toMatch(/^[0-9A-F]{16}$/);
    });

    it('downloadCertificate returns raw text', async () => {
        vi.stubGlobal('fetch', vi.fn().mockResolvedValue(jsonResponse('CERT-TOKEN')));
        await expect(BackendClient.downloadCertificate()).resolves.toBe('CERT-TOKEN');
    });
});
