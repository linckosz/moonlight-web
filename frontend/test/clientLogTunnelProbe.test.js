/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';

/**
 * The client log through the rendezvous: a late "no answer" turns nothing off.
 *
 * The page's first /api/health is made before the tunnel is up, and the
 * service worker can hold it until it gives up — a 504, tens of seconds on.
 * By then the call made once the tunnel was up has switched the mirror on;
 * the 504 used to read as "debug off" and give the console back for the rest
 * of the visit.
 */

const LEVELS = ['log', 'info', 'warn', 'error', 'debug'];
let saved;

beforeEach(() => {
    saved = Object.fromEntries(LEVELS.map((l) => [l, console[l]]));
    vi.resetModules();
    // Served by the bootstrap's worker: down a tunnel.
    Object.defineProperty(navigator, 'serviceWorker', {
        configurable: true,
        value: { controller: {} },
    });
});

afterEach(() => {
    for (const l of LEVELS) console[l] = saved[l];
    vi.unstubAllGlobals();
    delete navigator.serviceWorker;
});

function deferred() {
    let resolve;
    const promise = new Promise((r) => (resolve = r));
    return { promise, resolve };
}

const health = (body) => ({ ok: true, status: 200, json: async () => body });
const timedOut = { ok: false, status: 504, json: async () => ({}) };

describe('ClientLog through a tunnel', () => {
    it('a first probe answered 504 after the mirror is on keeps it on', async () => {
        const first = deferred();
        const fetch = vi.fn((url) => {
            if (url === '/api/health' && fetch.mock.calls.length === 1) return first.promise;
            if (url === '/api/health') return Promise.resolve(health({ debug: true }));
            return Promise.resolve({ ok: false, status: 401, json: async () => ({}) });
        });
        vi.stubGlobal('fetch', fetch);
        const log = await import('../js/util/ClientLog.js');
        const wrapped = console.log;
        expect(wrapped).not.toBe(saved.log);

        await log.refreshClientLog(); // the tunnel is up
        expect(log.serverDiag.debug).toBe(true);

        first.resolve(timedOut);
        await first.promise;
        await new Promise((r) => setTimeout(r, 0));
        expect(log.serverDiag.debug).toBe(true);
        expect(console.log).toBe(wrapped);
    });

    it('an answer that says debug is off is heard as such: warnings and errors only', async () => {
        vi.stubGlobal(
            'fetch',
            vi.fn(async () => health({ debug: false })),
        );
        const log = await import('../js/util/ClientLog.js');
        await log.refreshClientLog();
        expect(log.serverDiag.debug).toBe(false);
        // An owner's page keeps its mirror, for its warnings and errors
        // (clientLogQuiet.test.js); a guest's gives the console back.
        expect(console.log).not.toBe(saved.log);
    });
});
