/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';

/**
 * The page's error trail (util/ErrorTrail.js): only console.error, bounded,
 * cheap on the page's thread, and handed to the host only after a stream, by
 * an owner's page or a guest's that left its box ticked.
 */

let savedError;
let fetchMock;
/** The instance a test loaded: emptied after it, as its pagehide listener stays. */
let current = null;

async function load() {
    vi.resetModules();
    current = await import('../js/util/ErrorTrail.js');
    return current;
}

function sentBodies() {
    return fetchMock.mock.calls.map((c) => JSON.parse(c[1].body));
}

beforeEach(() => {
    savedError = console.error;
    fetchMock = vi.fn(async () => ({ ok: true, status: 200 }));
    vi.stubGlobal('fetch', fetchMock);
    // No idle callback in jsdom: the timer path, run by hand.
    vi.useFakeTimers();
});

afterEach(() => {
    if (current) current._forTests.reset();
    current = null;
    console.error = savedError;
    vi.useRealTimers();
    vi.unstubAllGlobals();
});

describe('ErrorTrail', () => {
    it('keeps console.error only, and still prints it', async () => {
        const printed = vi.fn();
        console.error = printed;
        const trail = await load();
        console.log('a log');
        console.warn('a warning');
        console.error('[X] boom', 42);
        expect(printed).toHaveBeenCalledWith('[X] boom', 42);
        expect(trail._forTests.entries().map((e) => e.m)).toEqual(['[X] boom 42']);
    });

    it('counts the same line instead of keeping it twice', async () => {
        console.error = () => {};
        const trail = await load();
        for (let i = 0; i < 5; i++) console.error('[Decoder] failed frame');
        const entries = trail._forTests.entries();
        expect(entries).toHaveLength(1);
        expect(entries[0].n).toBe(5);
    });

    it('stays bounded, the oldest lines going first', async () => {
        console.error = () => {};
        const trail = await load();
        for (let i = 0; i < 100; i++) console.error(`[X] line ${i} ` + 'x'.repeat(200));
        const entries = trail._forTests.entries();
        expect(entries).toHaveLength(40);
        expect(entries[39].m.startsWith('[X] line 99 ')).toBe(true);
        expect(trail._forTests.dropped()).toBe(60);
        console.error('y'.repeat(5000));
        const last = trail._forTests.entries().at(-1).m;
        expect(last.length).toBeLessThanOrEqual(1024 + 4);
    });

    it('reads a huge object in bounded time and size', async () => {
        console.error = () => {};
        const trail = await load();
        const huge = {};
        for (let i = 0; i < 200000; i++) huge['k' + i] = { deep: { deeper: i } };
        huge.self = huge;
        const t0 = performance.now();
        console.error('[X] state', huge);
        const ms = performance.now() - t0;
        expect(ms).toBeLessThan(50);
        const m = trail._forTests.entries()[0].m;
        expect(m).toBe('[X] state [Object]');
        console.error('[X] refused', {
            name: 'NotAllowedError',
            message: 'denied',
            secret: 's3cret',
        });
        expect(trail._forTests.entries()[1].m).toBe(
            '[X] refused [Object name: NotAllowedError, message: denied]',
        );
    });

    it('keeps where in the code, not where it was served', async () => {
        console.error = () => {};
        const trail = await load();
        const err = new Error('bad frame');
        err.stack =
            'Error: bad frame\n    at decode (https://192.168.1.20:8443/js/stream/Video.js:120:7)\n' +
            '    at https://stream.moonlightweb.top/abc123/js/ui/StreamView.js:9:1';
        console.error('[Video]', err);
        const m = trail._forTests.entries()[0].m;
        expect(m).toContain('Error: bad frame');
        expect(m).toContain('/js/stream/Video.js:120:7');
        expect(m).toContain('/js/ui/StreamView.js:9:1');
        expect(m).not.toContain('192.168.1.20');
        expect(m).not.toContain('moonlightweb.top');
        expect(m).not.toContain('abc123');
        console.error('[Net] fetch failed: https://10.0.0.5:8443/api/hosts/xyz?token=s3cret');
        const net = trail._forTests.entries()[1].m;
        expect(net).toBe('[Net] fetch failed: <url>');
        console.error('[Join] POST /p/AbC123tok/join?pin=1234#x refused');
        expect(trail._forTests.entries()[2].m).toBe('[Join] POST /p/…/join refused');
    });

    it('sends nothing when nothing went wrong', async () => {
        const trail = await load();
        trail.scheduleErrorTrail();
        vi.runAllTimers();
        trail.sendErrorTrail();
        expect(fetchMock).not.toHaveBeenCalled();
    });

    it('hands the errors to its host once the stream is over, then forgets them', async () => {
        console.error = () => {};
        const trail = await load();
        console.error('[Stream] lost');
        trail.scheduleErrorTrail();
        trail.scheduleErrorTrail(); // one send, however often it is asked
        expect(fetchMock).not.toHaveBeenCalled(); // not before the page is idle
        vi.runAllTimers();
        expect(fetchMock).toHaveBeenCalledTimes(1);
        expect(fetchMock.mock.calls[0][0]).toBe('/api/errors/client');
        const body = sentBodies()[0];
        expect(body.lines.map((l) => l.m)).toEqual(['[Stream] lost']);
        expect(typeof body.sent).toBe('number');
        expect(body).not.toHaveProperty('ua');
        expect(trail.hasErrorTrail()).toBe(false);
    });

    it('a guest page sends only while its box says yes', async () => {
        console.error = () => {};
        const trail = await load();
        // The page under test is not a /p/ page; the guest's answer is what
        // setErrorTrailAllowed wires in, exercised here directly.
        trail.setErrorTrailAllowed(() => false);
        console.error('[X] guest error');
        trail.sendErrorTrail();
        expect(fetchMock).not.toHaveBeenCalled();
        expect(trail.hasErrorTrail()).toBe(true); // kept, not sent
        trail.setErrorTrailAllowed(() => true);
        trail.sendErrorTrail();
        expect(fetchMock).toHaveBeenCalledTimes(1);
        trail.setErrorTrailAllowed(() => {
            throw new Error('storage blocked');
        });
        console.error('[X] another');
        trail.sendErrorTrail();
        expect(fetchMock).toHaveBeenCalledTimes(1);
    });

    it('leaves with a beacon as the page goes away, only if something waits', async () => {
        console.error = () => {};
        const beacon = vi.fn(() => true);
        Object.defineProperty(navigator, 'sendBeacon', { configurable: true, value: beacon });
        const trail = await load();
        window.dispatchEvent(new Event('pagehide'));
        expect(beacon).not.toHaveBeenCalled();
        console.error('[X] closing');
        window.dispatchEvent(new Event('pagehide'));
        expect(beacon).toHaveBeenCalledTimes(1);
        expect(beacon.mock.calls[0][0]).toBe('/api/errors/client');
        expect(trail.hasErrorTrail()).toBe(false);
        delete navigator.sendBeacon;
    });
});
