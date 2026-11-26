/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';

/**
 * The client log out of debug mode: an owner's page still sends its warnings
 * and errors, so "Download logs" holds the browser's side of a problem without
 * a box to tick first. Never its other lines, never a secret a line carries,
 * and never in a loop when the server refuses: it backs off, within bounds.
 */

const LEVELS = ['log', 'info', 'warn', 'error', 'debug'];
let saved;
let startPath;

beforeEach(() => {
    saved = Object.fromEntries(LEVELS.map((l) => [l, console[l]]));
    startPath = location.pathname;
    vi.resetModules();
    vi.useFakeTimers();
});

afterEach(() => {
    for (const l of LEVELS) console[l] = saved[l];
    history.replaceState(null, '', startPath);
    vi.useRealTimers();
    vi.unstubAllGlobals();
});

const answer = (status, body = {}) => ({ ok: status === 200, status, json: async () => body });

/**
 * A server out of debug mode, with a session; its client log answers
 * `logStatus` (a number, or a function of the call's index).
 */
function quietServer(logStatus = 200) {
    const sent = [];
    const fetch = vi.fn((url, init) => {
        if (url === '/api/health') return Promise.resolve(answer(200, { debug: false }));
        if (url === '/api/auth/status')
            return Promise.resolve(answer(200, { authenticated: true }));
        if (url === '/api/logs/client' || url === '/api/share/player/log') {
            sent.push(JSON.parse(init.body));
            const status = typeof logStatus === 'function' ? logStatus(sent.length) : logStatus;
            return Promise.resolve(answer(status));
        }
        return Promise.resolve(answer(404));
    });
    vi.stubGlobal('fetch', fetch);
    return { fetch, sent };
}

const allLines = (sent) => sent.flatMap((b) => b.lines);

describe('ClientLog out of debug mode', () => {
    it("sends an owner's warnings and errors, and nothing else", async () => {
        const { sent } = quietServer();
        await import('../js/util/ClientLog.js');
        await vi.advanceTimersByTimeAsync(0);
        console.log('a log line');
        console.info('an info line');
        console.debug('a debug line');
        console.warn('a warning');
        console.error('an error');
        await vi.advanceTimersByTimeAsync(5000);

        const lines = allLines(sent);
        expect(lines.map((l) => l.l)).toEqual(['warn', 'error']);
        expect(lines.map((l) => l.m)).toEqual(['a warning', 'an error']);
        // The mirror stays in place, for the next warning.
        expect(console.log).not.toBe(saved.log);
    });

    it('keeps of what came before the answer only what it sends', async () => {
        let answerHealth;
        const held = new Promise((r) => (answerHealth = r));
        const { fetch, sent } = quietServer();
        const fallback = fetch.getMockImplementation();
        fetch.mockImplementation((url, init) =>
            url === '/api/health' && !answerHealth.done
                ? ((answerHealth.done = true), held)
                : fallback(url, init),
        );
        await import('../js/util/ClientLog.js');
        console.log('before, a log line');
        console.error('before, an error');
        answerHealth(answer(200, { debug: false }));
        await vi.advanceTimersByTimeAsync(5000);
        expect(allLines(sent).map((l) => l.m)).toEqual(['before, an error']);
    });

    it('hides what a line carries that is no one else’s business', async () => {
        const { sent } = quietServer();
        const log = await import('../js/util/ClientLog.js');
        await vi.advanceTimersByTimeAsync(0);
        const k = 'AbCdEf0123456789xyz';
        console.error(`open https://app.moonlightweb.top/x#k=${k}&n=${k}`);
        console.warn('GET /api/booth/join?code=482913&x=1', { pin: '4829' });
        console.error(`Cookie: mw_session=${k}`);
        await vi.advanceTimersByTimeAsync(5000);

        const text = allLines(sent)
            .map((l) => l.m)
            .join('\n');
        expect(text).not.toContain(k);
        expect(text).not.toContain('482913');
        expect(text).not.toContain('4829"');
        expect(text).toContain('#k=(hidden)&n=(hidden)');
        expect(text).toContain('?code=(hidden)&x=1');
        expect(log.hideSecrets('nothing to hide: n=5')).toBe('nothing to hide: n=5');
        // An owner link's exchange answers its keys in a JSON body.
        expect(log.hideSecrets(`{"k":"${k}","n":"${k}","host":"x"}`)).toBe(
            '{"k":"(hidden)","n":"(hidden)","host":"x"}',
        );
        expect(log.hideSecrets('{"pin": 4829, "sid":"s", "pwd":"p", "passwd":"q"}')).toBe(
            '{"pin": "(hidden)", "sid":"(hidden)", "pwd":"(hidden)", "passwd":"(hidden)"}',
        );
    });

    it('backs off when refused, and its buffer stays bounded', async () => {
        const { sent } = quietServer(429);
        await import('../js/util/ClientLog.js');
        await vi.advanceTimersByTimeAsync(0);
        for (let i = 0; i < 5000; i++) console.warn('warning ' + i);
        await vi.advanceTimersByTimeAsync(60000);

        // Every 5 s without a back-off would be a dozen tries in a minute.
        expect(sent.length).toBeGreaterThan(1);
        expect(sent.length).toBeLessThanOrEqual(5);
        for (const batch of sent) expect(batch.lines.length).toBeLessThanOrEqual(2000);
        // The lines past the bound were let go, oldest first, and counted.
        const last = sent[sent.length - 1];
        expect(last.dropped).toBeGreaterThan(0);
        expect(last.lines[last.lines.length - 1].m).toBe('warning 4999');
    });

    it('lets go of a batch the server says is too large, and goes on', async () => {
        const { sent } = quietServer((n) => (n === 1 ? 413 : 200));
        await import('../js/util/ClientLog.js');
        await vi.advanceTimersByTimeAsync(0);
        console.error('first');
        await vi.advanceTimersByTimeAsync(5000);
        console.error('second');
        await vi.advanceTimersByTimeAsync(30000);

        expect(sent.length).toBe(2);
        expect(sent[1].lines.map((l) => l.m)).toEqual(['second']); // not sent again
        expect(sent[1].dropped).toBe(1); // but counted
    });

    it("gives a guest's console back, as before", async () => {
        history.replaceState(null, '', '/p/some-link');
        const { sent } = quietServer();
        await import('../js/util/ClientLog.js');
        await vi.advanceTimersByTimeAsync(0);
        console.error('a guest error');
        await vi.advanceTimersByTimeAsync(10000);
        expect(sent.length).toBe(0);
        expect(console.log).toBe(saved.log);
    });
});
