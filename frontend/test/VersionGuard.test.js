/*
 * Moonlight-Web — TNR suite. Copyright (C) 2026 Bruno Martin.
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
});
