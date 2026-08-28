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
        BackendClient._hadSession = false; // per-document, so per-test
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
        BackendClient._hadSession = true; // the reload path only runs for a session that existed
        sessionStorage.setItem('mw_auth_reload', '1'); // pretend we already reloaded once
        vi.stubGlobal('fetch', vi.fn().mockResolvedValue(jsonResponse({ message: 'authentication_required' }, { ok: false, status: 401 })));
        await expect(BackendClient.getHosts()).rejects.toMatchObject({ statusCode: 401 });
        expect(sessionStorage.getItem('mw_auth_reload')).toBeNull(); // guard cleared
    });

    // THE regression: the login screen called a session-gated endpoint, got the
    // 401 it was always going to get, and this turned it into a full reload — so
    // every first visit loaded the application twice. Reloading is how a RUNNING
    // application returns to the PIN screen; from the PIN screen it achieves
    // nothing, and any pre-authentication screen can trip it.
    it('does not reload on a 401 when no session was ever held', async () => {
        const reload = vi.fn();
        const original = window.location;
        Object.defineProperty(window, 'location', { value: { reload }, configurable: true });
        vi.stubGlobal('fetch', vi.fn().mockResolvedValue(jsonResponse({ message: 'authentication_required' }, { ok: false, status: 401 })));
        await expect(BackendClient.getHosts()).rejects.toMatchObject({ statusCode: 401 });
        Object.defineProperty(window, 'location', { value: original, configurable: true });
        expect(reload).not.toHaveBeenCalled();
        expect(sessionStorage.getItem('mw_auth_reload')).toBeNull(); // never armed either
    });

    it('reloads on a 401 once the session it had is gone', async () => {
        vi.stubGlobal('fetch', vi.fn().mockResolvedValue(jsonResponse({ authenticated: true })));
        await BackendClient.getAuthStatus(); // this is what records the session

        const reload = vi.fn();
        const original = window.location;
        Object.defineProperty(window, 'location', { value: { reload }, configurable: true });
        vi.stubGlobal('fetch', vi.fn().mockResolvedValue(jsonResponse({ message: 'authentication_required' }, { ok: false, status: 401 })));
        // The call never settles — _handleError hands back a pending promise so
        // the caller cannot race the navigation.
        BackendClient.getHosts();
        await Promise.resolve();
        await Promise.resolve();
        Object.defineProperty(window, 'location', { value: original, configurable: true });
        expect(reload).toHaveBeenCalled();
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

// ── The endpoint map ────────────────────────────────────────────────────────
// Most of this client is one thin method per backend route: no logic, just a
// path and a body. What breaks there is the route itself — a typo in a path, a
// verb that drifted, a renamed body field — and none of it shows up until the
// server answers 404 in front of a user. The map is small enough to assert
// whole, so it is.

/** @type {[string, () => Promise<any>, string][]} */
const GET_ROUTES = [
    ['getAppList', () => BackendClient.getAppList('h1'), '/api/hosts/h1/apps'],
    ['getBackendTypes', () => BackendClient.getBackendTypes(), '/api/backends'],
    ['getHostBackend', () => BackendClient.getHostBackend('h1'), '/api/hosts/h1/backend'],
    ['getHostSeats', () => BackendClient.getHostSeats('h1'), '/api/hosts/h1/seats'],
    ['getShareStatus', () => BackendClient.getShareStatus(), '/api/share/status'],
    ['getAuthSessions', () => BackendClient.getAuthSessions(), '/api/auth/sessions'],
    ['getServerHostname', () => BackendClient.getServerHostname(), '/api/server/hostname'],
    ['getServerStatus', () => BackendClient.getServerStatus(), '/api/server/status'],
    ['getUpdateStatus', () => BackendClient.getUpdateStatus(), '/api/update/status'],
    ['checkForUpdate', () => BackendClient.checkForUpdate(), '/api/update/check'],
    ['getAdminSettings', () => BackendClient.getAdminSettings(), '/api/admin/settings'],
    ['getStreamingSettings', () => BackendClient.getStreamingSettings(), '/api/settings/streaming'],
    ['getSetupStatus', () => BackendClient.getSetupStatus(), '/api/setup/status'],
    ['getMetricsConsent', () => BackendClient.getMetricsConsent(), '/api/metrics/consent'],
    ['getInternetStatus', () => BackendClient.getInternetStatus(), '/api/internet/status'],
    // The token rides in the query string here — it is the guest's only
    // credential and there is no session to carry it.
    [
        'playerInfo',
        () => BackendClient.playerInfo('tok en/1'),
        '/api/share/player/info?token=tok%20en%2F1',
    ],
];

/** @type {[string, () => Promise<any>, string, object][]} */
const POST_ROUTES = [
    ['scanHosts', () => BackendClient.scanHosts(), '/api/hosts/scan', {}],
    ['wakeHost', () => BackendClient.wakeHost('u1'), '/api/hosts/u1/wol', {}],
    [
        'renameHost',
        () => BackendClient.renameHost('u1', 'Den'),
        '/api/hosts/u1/name',
        { name: 'Den' },
    ],
    ['restartHost', () => BackendClient.restartHost('u1'), '/api/hosts/u1/restart', {}],
    ['startPairing', () => BackendClient.startPairing('h1'), '/api/hosts/h1/pair/start', {}],
    ['confirmPairing', () => BackendClient.confirmPairing('h1'), '/api/hosts/h1/pair', {}],
    [
        'provisionHostSeat',
        () => BackendClient.provisionHostSeat('h1', { user: 'ada' }),
        '/api/hosts/h1/seats',
        { user: 'ada' },
    ],
    ['stopHostSession', () => BackendClient.stopHostSession('h1'), '/api/hosts/h1/stop-session', {}],
    [
        'shareActivate',
        () => BackendClient.shareActivate(2, { host_uuid: 'u1', app_id: 7 }),
        '/api/share/slots/2/activate',
        { host_uuid: 'u1', app_id: 7 },
    ],
    [
        'shareCredentials',
        () => BackendClient.shareCredentials(1),
        '/api/share/slots/1/credentials',
        {},
    ],
    [
        'sharePermissions',
        () => BackendClient.sharePermissions(1, { mouse: true, keyboard: false }),
        '/api/share/slots/1/permissions',
        { mouse: true, keyboard: false },
    ],
    ['shareTtl', () => BackendClient.shareTtl(1, 600), '/api/share/slots/1/ttl', { ttl_secs: 600 }],
    [
        'shareRename',
        () => BackendClient.shareRename(1, 'Bob'),
        '/api/share/slots/1/name',
        { name: 'Bob' },
    ],
    ['shareDeactivate', () => BackendClient.shareDeactivate(1), '/api/share/slots/1/deactivate', {}],
    [
        'playerPin',
        () => BackendClient.playerPin('tok', '1234'),
        '/api/share/player/pin',
        { token: 'tok', pin: '1234' },
    ],
    ['playerLeave', () => BackendClient.playerLeave(), '/api/share/player/leave', {}],
    [
        'openScreenRecordingSettings',
        () => BackendClient.openScreenRecordingSettings(),
        '/api/system/open-screen-recording',
        {},
    ],
    ['stopSunshine', () => BackendClient.stopSunshine(), '/api/system/stop-sunshine', {}],
    ['startSunshine', () => BackendClient.startSunshine(), '/api/system/start-sunshine', {}],
    ['regeneratePin', () => BackendClient.regeneratePin(), '/api/auth/regenerate', {}],
    ['clearPin', () => BackendClient.clearPin(), '/api/admin/pin/clear', {}],
    ['logout', () => BackendClient.logout(), '/api/auth/logout', {}],
    ['redeemHostKey', () => BackendClient.redeemHostKey('K1'), '/api/auth/host-key', { key: 'K1' }],
    [
        'adminUnlock',
        () => BackendClient.adminUnlock('hunter2'),
        '/api/auth/admin-unlock',
        { password: 'hunter2' },
    ],
    [
        'saveRemoteAdmin',
        () => BackendClient.saveRemoteAdmin({ enabled: true }),
        '/api/admin/password',
        { enabled: true },
    ],
    [
        'regenerateCertificate',
        () => BackendClient.regenerateCertificate(),
        '/api/admin/certificate/regenerate',
        {},
    ],
    [
        'revokeSession',
        () => BackendClient.revokeSession('t1'),
        '/api/auth/sessions/revoke',
        { token: 't1' },
    ],
    [
        'renameSession',
        () => BackendClient.renameSession('t1', 'Laptop'),
        '/api/auth/sessions/rename',
        { token: 't1', machine_name: 'Laptop' },
    ],
    ['startUpdate', () => BackendClient.startUpdate(), '/api/update/start', {}],
    [
        'saveAdminSettings',
        () => BackendClient.saveAdminSettings({ theme: 'dark' }),
        '/api/admin/settings',
        { theme: 'dark' },
    ],
    [
        'saveStreamingSettings',
        () => BackendClient.saveStreamingSettings({ bitrate: 30000 }),
        '/api/settings/streaming',
        { bitrate: 30000 },
    ],
    [
        'applySetup',
        () => BackendClient.applySetup({ install: true }),
        '/api/setup/apply',
        { install: true },
    ],
    [
        'checkSunshineCredentials',
        () => BackendClient.checkSunshineCredentials('ada', 'pw'),
        '/api/setup/sunshine-check',
        { username: 'ada', password: 'pw' },
    ],
    [
        'setMetricsConsent',
        () => BackendClient.setMetricsConsent(true, 'the exact wording', 'banner'),
        '/api/metrics/consent',
        { granted: true, message: 'the exact wording', source: 'banner' },
    ],
    [
        'enableInternet',
        () => BackendClient.enableInternet({ domain: 'mw' }),
        '/api/internet/enable',
        { domain: 'mw' },
    ],
    ['disableInternet', () => BackendClient.disableInternet(), '/api/internet/disable', {}],
    ['refreshInternet', () => BackendClient.refreshInternet(), '/api/internet/refresh', {}],
    ['renewCert', () => BackendClient.renewCert(), '/api/internet/renew-cert', {}],
];

/** @type {[string, () => Promise<any>, string][]} */
const DELETE_ROUTES = [
    ['clearHostBackend', () => BackendClient.clearHostBackend('h1'), '/api/hosts/h1/backend'],
    ['teardownHostSeat', () => BackendClient.teardownHostSeat('h1', 's2'), '/api/hosts/h1/seats/s2'],
    [
        'releaseHostSeatOwner',
        () => BackendClient.releaseHostSeatOwner('h1', 's2'),
        '/api/hosts/h1/seats/s2/owner',
    ],
];

describe('BackendClient endpoint map', () => {
    beforeEach(() => {
        vi.restoreAllMocks();
        localStorage.clear();
        sessionStorage.clear();
        BackendClient._adminKeyPromise = null;
        BackendClient._hadSession = false;
    });

    it.each(GET_ROUTES)('%s reads its route', async (_name, call, path) => {
        const fetchMock = mockFetch(jsonResponse({ ok: true }));
        await call();
        expect(fetchMock.apiCalls()[0][0]).toBe(path);
        // A GET carries no init at all — no verb, no body, no admin key.
        expect(fetchMock.apiCalls()[0][1]).toBeUndefined();
    });

    it.each(POST_ROUTES)('%s writes to its route', async (_name, call, path, body) => {
        const fetchMock = mockFetch(jsonResponse({ ok: true }));
        await call();
        const [called, init] = fetchMock.apiCalls()[0];
        expect(called).toBe(path);
        expect(init.method).toBe('POST');
        expect(JSON.parse(init.body)).toEqual(body);
    });

    it.each(DELETE_ROUTES)('%s deletes its route', async (_name, call, path) => {
        const fetchMock = mockFetch(jsonResponse({ ok: true }));
        await call();
        const [called, init] = fetchMock.apiCalls()[0];
        expect(called).toBe(path);
        expect(init.method).toBe('DELETE');
    });
});

describe('BackendClient request shaping', () => {
    beforeEach(() => {
        vi.restoreAllMocks();
        localStorage.clear();
        sessionStorage.clear();
        BackendClient._adminKeyPromise = null;
        BackendClient._hadSession = false;
    });

    // Omitting the token is how the URL gets fixed without retyping the secret:
    // the browser is never sent the stored one back, so an empty field must not
    // travel as an empty token and wipe it.
    it('setHostBackend sends only the optional fields that were filled in', async () => {
        const fetchMock = mockFetch(jsonResponse({ ok: true }));
        await BackendClient.setHostBackend('h1', { type: 'wolf', apiUrl: 'https://w' });
        expect(fetchMock.apiCalls()[0][0]).toBe('/api/hosts/h1/backend');
        expect(JSON.parse(fetchMock.apiCalls()[0][1].body)).toEqual({
            type: 'wolf',
            apiUrl: 'https://w',
        });

        const withSecrets = mockFetch(jsonResponse({ ok: true }));
        await BackendClient.setHostBackend('h1', {
            type: 'multiseat',
            apiUrl: 'https://m',
            apiToken: 'T',
            pairUser: 'ada',
            pairPassword: 'pw',
        });
        expect(JSON.parse(withSecrets.apiCalls()[0][1].body)).toEqual({
            type: 'multiseat',
            apiUrl: 'https://m',
            apiToken: 'T',
            pairUser: 'ada',
            pairPassword: 'pw',
        });
    });

    // Opened from a running stream the invitation binds to it, so there is no
    // target to name — but the body still has to be an object.
    it('shareActivate without a target posts an empty body', async () => {
        const fetchMock = mockFetch(jsonResponse({ ok: true }));
        await BackendClient.shareActivate(3);
        expect(JSON.parse(fetchMock.apiCalls()[0][1].body)).toEqual({});
    });

    // No aspect means "reuse the ratio the owner's session settled on", which is
    // said by leaving the field out, not by sending an empty one.
    it('playerJoin only sends an aspect when it was overridden', async () => {
        const plain = mockFetch(jsonResponse({ ok: true }));
        await BackendClient.playerJoin('tok', 1080);
        expect(JSON.parse(plain.apiCalls()[0][1].body)).toEqual({ token: 'tok', height: 1080 });

        const forced = mockFetch(jsonResponse({ ok: true }));
        await BackendClient.playerJoin('tok', 1080, '16:9');
        expect(JSON.parse(forced.apiCalls()[0][1].body)).toEqual({
            token: 'tok',
            height: 1080,
            aspect: '16:9',
        });
    });

    it('quitApp scopes to one slot when the caller names it', async () => {
        const fetchMock = mockFetch(jsonResponse({ ok: true }));
        await BackendClient.quitApp('h1', { session_slot: 1, client_uniqueid: 'AAAA' });
        const [path, init] = fetchMock.apiCalls()[0];
        expect(path).toBe('/api/hosts/h1/quit');
        // The slot's own uniqueid wins over the browser's — quitting the twin
        // session instead of the one asked for is what kills a live stream.
        expect(JSON.parse(init.body)).toEqual({ session_slot: 1, client_uniqueid: 'AAAA' });
    });

    it('quitApp defaults to this browser when no slot is named', async () => {
        const fetchMock = mockFetch(jsonResponse({ ok: true }));
        await BackendClient.quitApp('h1');
        expect(JSON.parse(fetchMock.apiCalls()[0][1].body).client_uniqueid).toBe(
            BackendClient.clientUniqueId(),
        );
    });

    // Slot 1 must be a STABLE second identity: Sunshine keys sessions by
    // uniqueid, and a standby that changed id on every reload could never
    // /resume itself.
    it('clientUniqueIdForSlot derives a stable, distinct id for the standby slot', () => {
        const base = BackendClient.clientUniqueId();
        expect(BackendClient.clientUniqueIdForSlot(0)).toBe(base);
        const standby = BackendClient.clientUniqueIdForSlot(1);
        expect(standby).toMatch(/^[0-9A-F]{16}$/);
        expect(standby).not.toBe(base);
        expect(standby.slice(0, 14)).toBe(base.slice(0, 14));
        expect(BackendClient.clientUniqueIdForSlot(1)).toBe(standby);
    });

    it('redeemHostKey records that a session is now held', async () => {
        mockFetch(jsonResponse({ ok: true }));
        await BackendClient.redeemHostKey('K1');
        expect(BackendClient._hadSession).toBe(true);
    });
});

// Both of these deliberately swallow failures: they run while the host is
// restarting or offline, where an exception would be noise and the shared error
// path would reload the page.
describe('BackendClient best-effort probes', () => {
    beforeEach(() => {
        vi.restoreAllMocks();
        sessionStorage.clear();
        BackendClient._adminKeyPromise = null;
        BackendClient._hadSession = false;
    });

    it('checkForUpdate resolves null instead of throwing', async () => {
        vi.stubGlobal(
            'fetch',
            vi
                .fn()
                .mockResolvedValue(
                    jsonResponse({ message: 'rate limited' }, { ok: false, status: 503 }),
                ),
        );
        await expect(BackendClient.checkForUpdate()).resolves.toBeNull();
    });

    it('probeHealth answers the payload, or null while the host is down', async () => {
        vi.stubGlobal('fetch', vi.fn().mockResolvedValue(jsonResponse({ version: '0.3.0' })));
        await expect(BackendClient.probeHealth()).resolves.toEqual({ version: '0.3.0' });

        vi.stubGlobal(
            'fetch',
            vi.fn().mockResolvedValue(jsonResponse({}, { ok: false, status: 502 })),
        );
        await expect(BackendClient.probeHealth()).resolves.toBeNull();

        vi.stubGlobal('fetch', vi.fn().mockRejectedValue(new TypeError('failed to fetch')));
        await expect(BackendClient.probeHealth()).resolves.toBeNull();
    });
});
