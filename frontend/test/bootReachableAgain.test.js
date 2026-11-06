/*
 * MoonlightWeb — frontend TNR. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * The entry page (bootstrap/v1/boot.js) waiting for a machine to be reachable
 * again (06/11/2026). Past the first minute it used to stop calling, so a short
 * network cut or a machine back from a reboot went unnoticed until someone
 * refreshed; and "no direct connection" was never tried again at all.
 *
 * Pinned down: past that minute, a try at 30 s, 45 s, 1 min, 1.5 min, then every
 * 2 min, for half an hour, and the first one through carries the start on; none
 * of the fast ones for a failure that takes longer to pass; a try a second
 * after the network comes back, the half hour over or not; none while the
 * browser says it is offline, while the page is put away, or once the machine
 * is removed; a plain answer stops it all. And the way to start over by hand,
 * "Reload the page", beside "Not connected".
 */

/* global Response */

import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';

/** A machine of its own for each test's page, so that an earlier one can be told apart. */
const idOf = (n) => n.toString(36).padStart(26, 'a');
const WATCH_NOTE =
    'This page keeps trying and carries on by itself as soon as your machine can be reached.';
const WATCH_OVER_NOTE = 'This page has stopped trying. Reload it to try again.';
const DEFAULT_HINT =
    'If your machine is switched off or has no internet access, nothing here can reach it. ' +
    'That is by design, not a fault.';

/* ── What the connection does ───────────────────────────────────────────── */

/** Pages started so far. Only the latest is this test's: the others' listeners
 *  are still on the window, and an `online` event would wake them. */
let pages = 0;
/** What the n-th call does (0 first): an Error it fails with, or null to connect. */
let outcome;
/** When each call of this test's page was made, in seconds from its start. */
let calls;
let fetched;
let hostId;
let instances;
let t0;

const offline = () =>
    Object.assign(new Error('That machine is not online right now.'), { transient: true });
const busy = () => new Error('That machine already has as many connections as it accepts.');
/** The transport's own failures, from the real Tunnel (see beforeEach). */
let noRoute;
let silent;

vi.mock('../../bootstrap/v1/tunnel.js', async (original) => {
    const real = await original();
    return {
        ...real,
        RealTunnel: real.Tunnel,
        Tunnel: class {
            constructor(id) {
                this.id = id;
                this.firstContact = false;
                this.onstatus = null;
            }
            async connect() {
                // An earlier test's page, woken by this one's events: it never
                // gets an answer, so it never calls again.
                if (this.id !== hostId) return new Promise(() => {});
                const n = calls.length;
                calls.push((Date.now() - t0) / 1000);
                const e = outcome(n);
                if (e) throw e;
            }
            close() {}
            async fetch(path) {
                fetched.push(path);
                return new Response('', { status: 404 });
            }
        },
        hostIdFromLocation: () => hostId,
        knownInstances: () => instances,
        forgetInstance: (id) => {
            instances = instances.filter((e) => e.id !== id);
        },
        rememberLastHost: () => {},
    };
});

/* ── The browser around the page ────────────────────────────────────────── */

let visibility;
let onLine;
let reloads;

function setVisibility(value) {
    visibility = value;
    document.dispatchEvent(new Event('visibilitychange'));
}

function networkBack() {
    onLine = true;
    window.dispatchEvent(new Event('online'));
}

/** The page's clock moved on by @p seconds, every promise settled on the way. */
async function advance(seconds) {
    for (let left = seconds; left > 0; left -= 10) {
        await vi.advanceTimersByTimeAsync(Math.min(10, left) * 1000);
        await new Promise((resolve) => setImmediate(resolve));
    }
}

/** One start of the entry page, up to its first failure. */
async function startPage() {
    vi.resetModules();
    t0 = Date.now();
    await import('../../bootstrap/v1/boot.js');
    for (let i = 0; i < 100 && document.body.dataset.state !== 'failed'; i++) await advance(0.01);
    expect(document.body.dataset.state).toBe('failed');
}

const text = (id) => document.getElementById(id).textContent;
const gaps = (from) =>
    calls
        .filter((t) => t >= from)
        .map((t, i, a) => (i ? t - a[i - 1] : null))
        .slice(1);

beforeEach(async () => {
    vi.useFakeTimers({
        toFake: [
            'setTimeout',
            'clearTimeout',
            'setInterval',
            'clearInterval',
            'Date',
            'performance',
        ],
    });
    vi.stubGlobal('requestAnimationFrame', () => 0);
    visibility = 'visible';
    onLine = true;
    Object.defineProperty(document, 'visibilityState', {
        configurable: true,
        get: () => visibility,
    });
    vi.stubGlobal('navigator', {
        userAgent: 'test',
        serviceWorker: {},
        get onLine() {
            return onLine;
        },
    });
    outcome = () => offline();
    calls = [];
    fetched = [];
    hostId = idOf(++pages);
    instances = [];
    reloads = 0;
    sessionStorage.clear();
    document.body.innerHTML =
        '<div id="instance-menu" hidden></div>' +
        '<h1 id="stage"></h1><button id="reload" hidden></button>' +
        '<p id="detail"></p><div id="bar"></div><p id="status"></p><p id="note"></p>' +
        '<button id="forget" hidden></button>';
    delete document.body.dataset.state;
    vi.spyOn(console, 'warn').mockImplementation(() => {});
    vi.spyOn(console, 'info').mockImplementation(() => {});
    const error = console.error;
    vi.spyOn(console, 'error').mockImplementation((...a) => {
        if (String(a[0]).includes('Not implemented: navigation')) reloads++;
        else error(...a);
    });

    vi.resetModules();
    const { RealTunnel } = await import('../../bootstrap/v1/tunnel.js');
    const t = new RealTunnel(hostId, 'https://example.test');
    silent = t._deadlineError();
    t._pc = {};
    noRoute = t._deadlineError();
});

afterEach(() => {
    vi.useRealTimers();
    vi.restoreAllMocks();
    vi.unstubAllGlobals();
    delete document.visibilityState;
});

describe('the transport', () => {
    it('marks a failure that takes longer than a minute to pass', () => {
        expect(noRoute.message).toBe(
            'Your machine answered, but no direct connection could be opened.',
        );
        expect(noRoute).toMatchObject({ answered: true, mayPass: true });
        expect(noRoute.transient).toBeUndefined();
        expect(silent.message).toBe('Your machine did not answer in time.');
        expect(silent.mayPass).toBe(true);
        expect(silent.transient).toBeUndefined();
    });
});

describe('past the first minute', () => {
    it('tries again slowly, and the first try through carries the start on', async () => {
        outcome = (n) => (n < 12 ? offline() : null);
        await startPage();
        await advance(60);
        // The first minute, as before.
        expect(calls).toEqual([0, 2, 5, 10, 15, 25, 35, 45, 55]);
        expect(text('note')).toBe(`${DEFAULT_HINT} ${WATCH_NOTE}`);

        await advance(240);
        expect(calls.slice(9)).toEqual([85, 130, 190, 280]);
        // The 13th call gets through, and the page goes on to the interface.
        expect(fetched).toEqual(['/api/app/manifest']);
    });

    it('stops after half an hour, says so, and starts again when the network comes back', async () => {
        await startPage();
        await advance(31 * 60);
        expect(gaps(55).slice(0, 6)).toEqual([30, 45, 60, 90, 120, 120]);
        expect(Math.max(...calls)).toBeLessThanOrEqual(30 * 60);
        expect(calls).toHaveLength(9 + 16);
        expect(text('note')).toBe(`${DEFAULT_HINT} ${WATCH_OVER_NOTE}`);

        // Nothing more on its own, however long it stays open...
        await advance(60 * 60);
        expect(calls).toHaveLength(25);

        // ...but the network coming back is worth another half hour.
        const at = (Date.now() - t0) / 1000;
        networkBack();
        await advance(2);
        expect(calls.slice(25)).toEqual([at + 1]);
        expect(text('note')).toBe(`${DEFAULT_HINT} ${WATCH_NOTE}`);
        await advance(30);
        expect(calls.slice(25)).toEqual([at + 1, at + 31]);
    });

    it('stops at a failure that is a plain answer', async () => {
        outcome = (n) => (n === 9 ? busy() : offline());
        await startPage();
        await advance(10 * 60);
        expect(calls).toHaveLength(10);
        expect(text('detail')).toBe('That machine already has as many connections as it accepts.');
        expect(text('note')).toBe(DEFAULT_HINT);
    });

    it('calls the machine this tab had open every 10 s while an update may last', async () => {
        sessionStorage.setItem('mw-opened-host', hostId);
        await startPage();
        await advance(12 * 60);
        const late = calls.filter((t) => t > 55);
        expect(late.slice(0, 3)).toEqual([65, 75, 85]);
        expect(late.filter((t) => t <= 600).every((t, i) => t === 65 + 10 * i)).toBe(true);
        // The last 10-s try falls before the ten minutes are out; then the
        // slow ones, from the start.
        expect(late.filter((t) => t >= 595).slice(0, 3)).toEqual([595, 625, 670]);
    });
});

describe('a failure that takes longer than a minute to pass', () => {
    it('gets no fast tries, and keeps its own explanation', async () => {
        outcome = () => noRoute;
        await startPage();
        expect(text('detail')).toBe(noRoute.message);
        expect(text('note')).toBe(`${noRoute.hint} ${WATCH_NOTE}`);
        await advance(4 * 60);
        expect(calls).toEqual([0, 30, 75, 135, 225]);
    });

    it('covers a machine that said nothing in time', async () => {
        outcome = () => silent;
        await startPage();
        await advance(80);
        expect(calls).toEqual([0, 30, 75]);
        expect(text('note')).toBe(`${DEFAULT_HINT} ${WATCH_NOTE}`);
    });

    it('stops the caption saying "No server found" once a try reaches the machine', async () => {
        outcome = (n) => (n < 9 ? offline() : noRoute);
        const caption = () => {
            const mount = document.getElementById('instance-menu');
            return mount.hidden ? '' : mount.textContent;
        };
        await startPage();
        await advance(60);
        expect(caption()).toBe('No server found');
        await advance(30);
        expect(caption()).toBe('');
        expect(text('note')).toBe(`${noRoute.hint} ${WATCH_NOTE}`);
    });
});

describe('what wakes the tries, and what holds them', () => {
    it('tries a second after the network comes back, whatever the pause had left', async () => {
        await startPage();
        await advance(90);
        expect(calls.at(-1)).toBe(85);
        networkBack();
        await advance(2);
        expect(calls.at(-1)).toBe(91);
    });

    it('tries nothing while the browser says it is offline', async () => {
        await startPage();
        await advance(60);
        onLine = false;
        await advance(10 * 60);
        expect(calls).toHaveLength(9);
        networkBack();
        await advance(2);
        expect(calls).toHaveLength(10);
    });

    it('tries nothing while the page is put away', async () => {
        await startPage();
        await advance(60);
        setVisibility('hidden');
        await advance(10 * 60);
        // Shown again, it starts over: see bootComingBack.test.js, which has a
        // page to itself (the earlier pages here would hear the event too).
        expect(calls).toHaveLength(9);
    });

    it('stops calling a machine the person removed from the list', async () => {
        instances = [{ id: hostId, name: 'PC', url: `/${hostId}` }];
        await startPage();
        await advance(60);
        document.getElementById('forget').click();
        expect(text('note')).toBe('Removed from this browser’s list of machines.');
        await advance(10 * 60);
        expect(calls).toHaveLength(9);
        expect(text('note')).toBe('Removed from this browser’s list of machines.');
    });
});

describe('"Reload the page"', () => {
    it('sits beside "Not connected", and reloads the page', async () => {
        await startPage();
        expect(text('stage')).toBe('Not connected');
        const reload = document.getElementById('reload');
        expect(reload.hidden).toBe(false);
        reload.click();
        expect(reloads).toBe(1);
    });

    it('is not offered for an address that names no machine', async () => {
        hostId = null;
        await startPage();
        expect(text('detail')).toBe('This address does not name a machine.');
        expect(document.getElementById('reload').hidden).toBe(true);
    });
});
