/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';

/**
 * The client log on a TV on plain HTTP (issue #42): never on.
 *
 * Such a TV may ask the server only the routes listed for it (LanHttp), and
 * neither the session's status nor the client log is one of them. With the
 * server in debug mode, the mirror asked /api/auth/status every five seconds
 * and was refused each time (Mi TV bench, 13/11/2026).
 */

const LEVELS = ['log', 'info', 'warn', 'error', 'debug'];
let saved;

beforeEach(() => {
    saved = Object.fromEntries(LEVELS.map((l) => [l, console[l]]));
    vi.resetModules();
    vi.useFakeTimers();
});

afterEach(() => {
    for (const l of LEVELS) console[l] = saved[l];
    vi.useRealTimers();
    vi.unstubAllGlobals();
});

function deferred() {
    let resolve;
    const promise = new Promise((r) => (resolve = r));
    return { promise, resolve };
}

const answer = (status, body = {}) => ({ ok: status === 200, status, json: async () => body });

/** A server in debug mode, whose session routes refuse this page. */
function debugServer(firstHealth) {
    const fetch = vi.fn((url) => {
        if (url === '/api/health') {
            const first = fetch.mock.calls.filter(([u]) => u === '/api/health').length === 1;
            if (first && firstHealth) return firstHealth;
            return Promise.resolve(answer(200, { debug: true }));
        }
        return Promise.resolve(answer(404));
    });
    vi.stubGlobal('fetch', fetch);
    return fetch;
}

const asked = (fetch, url) => fetch.mock.calls.filter(([u]) => u === url).length;

describe('ClientLog on a TV on plain HTTP', () => {
    it('never asks for the session, even when the first probe answers after', async () => {
        const first = deferred();
        const fetch = debugServer(first.promise);
        const log = await import('../js/util/ClientLog.js');

        log.keepClientLogOff();
        first.resolve(answer(200, { debug: true }));
        await vi.advanceTimersByTimeAsync(20000);

        expect(log.serverDiag.debug).toBe(true);
        expect(console.log).toBe(saved.log);
        expect(asked(fetch, '/api/auth/status')).toBe(0);
    });

    it('stops a mirror already on, and stays off when the page asks again', async () => {
        const fetch = debugServer();
        const log = await import('../js/util/ClientLog.js');
        await vi.advanceTimersByTimeAsync(0);
        log.keepClientLogOff();
        const before = asked(fetch, '/api/auth/status');

        await log.refreshClientLog();
        await vi.advanceTimersByTimeAsync(20000);

        expect(console.log).toBe(saved.log);
        expect(asked(fetch, '/api/auth/status')).toBe(before);
    });

    it('any other page in debug mode still mirrors, and asks whether it is heard', async () => {
        const fetch = debugServer();
        await import('../js/util/ClientLog.js');
        await vi.advanceTimersByTimeAsync(5000);

        expect(console.log).not.toBe(saved.log);
        expect(asked(fetch, '/api/auth/status')).toBeGreaterThan(0);
    });
});
