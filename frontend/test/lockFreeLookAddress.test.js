/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
/* global history, localStorage */
import { describe, it, expect, afterEach, vi } from 'vitest';

/**
 * ?mw_lookfree=1 must survive app.js rewriting the address to '/' at
 * start-up, long before a stream asks for it (12/11/2026: it did not).
 */
describe('the lock-free look key given in the address', () => {
    afterEach(() => {
        history.replaceState(null, '', '/');
        localStorage.removeItem('mw_lookfree');
        vi.resetModules();
    });

    async function openAt(address) {
        history.replaceState(null, '', address);
        vi.resetModules();
        const mod = await import('../js/util/BrowserDetect.js');
        history.replaceState(null, '', '/');
        return mod;
    }

    it('is kept once the address is rewritten', async () => {
        const { lockFreeLookWanted } = await openAt('/?mw_lookfree=1');
        expect(lockFreeLookWanted()).toBe(true);
    });

    it('is turned off by ?mw_lookfree=0', async () => {
        localStorage.setItem('mw_lookfree', '1');
        const { lockFreeLookWanted } = await openAt('/?mw_lookfree=0');
        expect(lockFreeLookWanted()).toBe(false);
    });

    it('stays off without it', async () => {
        const { lockFreeLookWanted } = await openAt('/');
        expect(lockFreeLookWanted()).toBe(false);
    });

    it('leaves the default (on for iOS) to the caller, and the key beats it', async () => {
        const { lockFreeLookWanted } = await openAt('/');
        expect(lockFreeLookWanted(true)).toBe(true);
        localStorage.setItem('mw_lookfree', '0');
        expect(lockFreeLookWanted(true)).toBe(false);
    });
});
