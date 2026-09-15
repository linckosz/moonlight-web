/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';
import { VersionGuard } from '../js/util/VersionGuard.js';

function versionFetch(value, { ok = true } = {}) {
    return vi.fn(async () => ({ ok, json: async () => ({ version: value }) }));
}

describe('VersionGuard', () => {
    beforeEach(() => {
        VersionGuard._boot = null;
        VersionGuard._interval = null;
        document.body.className = '';
        vi.useFakeTimers();
    });
    afterEach(() => {
        if (VersionGuard._interval) clearInterval(VersionGuard._interval);
        vi.useRealTimers();
        vi.unstubAllGlobals();
    });

    it('_fetch returns the version on success', async () => {
        vi.stubGlobal('fetch', versionFetch('1.2.3'));
        await expect(VersionGuard._fetch()).resolves.toBe('1.2.3');
    });

    it('_fetch returns null on a non-OK response', async () => {
        vi.stubGlobal('fetch', versionFetch('x', { ok: false }));
        await expect(VersionGuard._fetch()).resolves.toBeNull();
    });

    it('_fetch returns null when the request throws (offline)', async () => {
        vi.stubGlobal('fetch', vi.fn().mockRejectedValue(new Error('offline')));
        await expect(VersionGuard._fetch()).resolves.toBeNull();
    });

    it('start() captures the boot version and arms the interval', async () => {
        vi.stubGlobal('fetch', versionFetch('1.0.0'));
        await VersionGuard.start();
        expect(VersionGuard._boot).toBe('1.0.0');
        expect(VersionGuard._interval).not.toBeNull();
    });

    it('start() disables the guard silently when version.json is missing', async () => {
        vi.stubGlobal('fetch', versionFetch(null, { ok: false }));
        await VersionGuard.start();
        expect(VersionGuard._boot).toBeNull();
        expect(VersionGuard._interval).toBeNull();
    });

    it('_check does not reload during an active stream', async () => {
        VersionGuard._boot = '1.0.0';
        document.body.classList.add('streaming-active');
        const reload = vi.fn();
        // location.reload would otherwise be called — make it observable.
        vi.spyOn(VersionGuard, '_fetch').mockResolvedValue('2.0.0');
        const original = window.location;
        Object.defineProperty(window, 'location', { value: { reload }, configurable: true });
        await VersionGuard._check();
        Object.defineProperty(window, 'location', { value: original, configurable: true });
        expect(reload).not.toHaveBeenCalled(); // deferred while streaming
    });

    it('_check is a no-op when the version is unchanged', async () => {
        VersionGuard._boot = '1.0.0';
        vi.spyOn(VersionGuard, '_fetch').mockResolvedValue('1.0.0');
        const reload = vi.fn();
        const original = window.location;
        Object.defineProperty(window, 'location', { value: { reload }, configurable: true });
        await VersionGuard._check();
        Object.defineProperty(window, 'location', { value: original, configurable: true });
        expect(reload).not.toHaveBeenCalled();
    });

    // Through the rendezvous a reload cannot pick up an update and would spin
    // forever: the page is served by a service worker from a cache that only
    // the entry page can refill, so reloading serves the same old files, which
    // see the same new version, and reload again. The update has to go back
    // through the entry page.
    it('_check goes through the entry page when the app came down a tunnel', async () => {
        VersionGuard._boot = '1.0.0';
        vi.spyOn(VersionGuard, '_fetch').mockResolvedValue('2.0.0');
        vi.stubGlobal('navigator', { serviceWorker: { controller: {} } });
        const bridge = await import('../js/net/tunnelBridge.js');
        vi.spyOn(bridge, 'tunnelHostId').mockReturnValue('a'.repeat(26));
        // The address carries what the link arrived with (see bootstrapAddress);
        // what matters here is that the guard goes through it, not a bare id.
        vi.spyOn(bridge, 'bootstrapAddress').mockReturnValue('/' + 'a'.repeat(26) + '#k=x');

        const reload = vi.fn();
        const replace = vi.fn();
        const original = window.location;
        Object.defineProperty(window, 'location', {
            value: { reload, replace },
            configurable: true,
        });
        await VersionGuard._check();
        Object.defineProperty(window, 'location', { value: original, configurable: true });

        expect(reload).not.toHaveBeenCalled();
        expect(replace).toHaveBeenCalledWith('/' + 'a'.repeat(26) + '#k=x');
    });
});

// A page answered from the service worker's cache (home-screen icon, restored
// tab, reload on /admin) connects on its own and never passes the entry page's
// manifest check. If the host was updated while no page was open, the baseline
// start() captures is already the new version and the timer never fires — so
// start() compares the cache's stamp with the host's manifest first.
describe('VersionGuard — interface cache on the way in', () => {
    const HOST = 'a'.repeat(26);
    const ENTRY = '/' + HOST + '#p=%2Fadmin';
    let replace;
    let originalLocation;

    function hostServes(manifestVersion, { manifestOk = true } = {}) {
        return vi.fn(async (url) => {
            if (String(url).startsWith('/api/app/manifest')) {
                return {
                    ok: manifestOk,
                    json: async () => ({ version: manifestVersion, files: ['/index.html'] }),
                };
            }
            return { ok: true, json: async () => ({ version: '0.3.1' }) };
        });
    }

    function stamp(hostId, version) {
        localStorage.setItem('mw-shell-stamp', JSON.stringify({ hostId, version }));
    }

    beforeEach(async () => {
        // The suite above leaves its _fetch spies in place.
        vi.restoreAllMocks();
        VersionGuard._boot = null;
        VersionGuard._interval = null;
        localStorage.clear();
        sessionStorage.clear();
        vi.useFakeTimers();
        vi.stubGlobal('navigator', { serviceWorker: { controller: {} } });
        const bridge = await import('../js/net/tunnelBridge.js');
        vi.spyOn(bridge, 'tunnelHostId').mockReturnValue(HOST);
        vi.spyOn(bridge, 'bootstrapAddress').mockReturnValue(ENTRY);
        replace = vi.fn();
        originalLocation = window.location;
        Object.defineProperty(window, 'location', { value: { replace }, configurable: true });
    });
    afterEach(() => {
        Object.defineProperty(window, 'location', { value: originalLocation, configurable: true });
        if (VersionGuard._interval) clearInterval(VersionGuard._interval);
        vi.useRealTimers();
        vi.unstubAllGlobals();
        vi.restoreAllMocks();
    });

    it('keeps the page when the cache holds what the host serves', async () => {
        stamp(HOST, '0.3.1-abc');
        vi.stubGlobal('fetch', hostServes('0.3.1-abc'));
        await VersionGuard.start();
        expect(replace).not.toHaveBeenCalled();
        expect(VersionGuard._boot).toBe('0.3.1');
        expect(VersionGuard._interval).not.toBeNull();
    });

    it('goes through the entry page when the host was updated since the cache was filled', async () => {
        stamp(HOST, '0.3.0-abc');
        vi.stubGlobal('fetch', hostServes('0.3.1-def'));
        await VersionGuard.start();
        expect(replace).toHaveBeenCalledWith(ENTRY);
        expect(VersionGuard._interval).toBeNull(); // leaving — nothing armed
    });

    // Same build, edited frontend file: the manifest's fingerprint moves.
    it('treats a changed file fingerprint under the same version as stale', async () => {
        stamp(HOST, '0.3.1-abc');
        vi.stubGlobal('fetch', hostServes('0.3.1-fff'));
        await VersionGuard.start();
        expect(replace).toHaveBeenCalledWith(ENTRY);
    });

    it('goes through the entry page when the cache belongs to another machine', async () => {
        stamp('b'.repeat(26), '0.3.1-abc');
        vi.stubGlobal('fetch', hostServes('0.3.1-abc'));
        await VersionGuard.start();
        expect(replace).toHaveBeenCalledWith(ENTRY);
    });

    it('does not bounce twice for the same version in one tab', async () => {
        stamp(HOST, '0.3.0-abc');
        vi.stubGlobal('fetch', hostServes('0.3.1-def'));
        vi.spyOn(console, 'warn').mockImplementation(() => {});
        await VersionGuard.start();
        expect(replace).toHaveBeenCalledTimes(1);

        // The refill did not change the stamp (storage trouble, a race): keep
        // running instead of looping between the two pages.
        await VersionGuard.start();
        expect(replace).toHaveBeenCalledTimes(1);
        expect(VersionGuard._interval).not.toBeNull();
    });

    it('bounces again for a later update once the cache had caught up', async () => {
        stamp(HOST, '0.3.0-abc');
        vi.stubGlobal('fetch', hostServes('0.3.1-def'));
        await VersionGuard.start();
        expect(replace).toHaveBeenCalledTimes(1);

        stamp(HOST, '0.3.1-def'); // the entry page refilled it
        await VersionGuard.start();
        expect(replace).toHaveBeenCalledTimes(1);

        if (VersionGuard._interval) clearInterval(VersionGuard._interval);
        vi.stubGlobal('fetch', hostServes('0.3.2-ghi'));
        await VersionGuard.start();
        expect(replace).toHaveBeenCalledTimes(2);
    });

    it('keeps the page when the manifest cannot be read', async () => {
        stamp(HOST, '0.3.0-abc');
        vi.stubGlobal('fetch', hostServes(null, { manifestOk: false }));
        await VersionGuard.start();
        expect(replace).not.toHaveBeenCalled();
    });

    it('keeps the page when there is no stamp to compare with', async () => {
        vi.stubGlobal('fetch', hostServes('0.3.1-def'));
        await VersionGuard.start();
        expect(replace).not.toHaveBeenCalled();
    });

    it('does not ask for the manifest on a direct connection', async () => {
        vi.stubGlobal('navigator', {}); // no controlling service worker
        stamp(HOST, '0.3.0-abc');
        const fetchMock = hostServes('0.3.1-def');
        vi.stubGlobal('fetch', fetchMock);
        await VersionGuard.start();
        expect(replace).not.toHaveBeenCalled();
        expect(fetchMock.mock.calls.some(([u]) => String(u).startsWith('/api/app/manifest'))).toBe(
            false,
        );
    });
});
