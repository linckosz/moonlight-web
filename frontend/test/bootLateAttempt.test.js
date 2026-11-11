/*
 * MoonlightWeb — frontend TNR. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * The entry page (bootstrap/v1/boot.js) and a call that passes its deadline
 * (11/11/2026). Nothing stopped such a call: on a phone on weak 5G its channel
 * opened a few seconds later, under a page that was by then waiting for its
 * next try. It wrote "Connected." over the failure and went nowhere, and the
 * page had to be reloaded.
 *
 * Pinned down: the transport says when it gave up on time alone, and tells
 * whoever asks if the channel opens after that; the page keeps that one attempt
 * aside, silent, carries the start on with it if it opens before the next try,
 * and puts it down as the next try starts, or when the machine is removed. Any
 * other failed attempt is put down at once, the first one included: that one
 * used to be left open as well.
 */

/* global Response */

import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';

const idOf = (n) => (n + 5000).toString(36).padStart(26, 'a');
const WATCH_NOTE =
    'This page keeps trying and carries on by itself as soon as your machine can be reached.';

let pages = 0;
/** What the n-th call does: an Error it fails with, or null to connect. */
let outcome;
let calls;
/** Every attempt of this test's page, in order. */
let made;
let fetched;
let hostId;
let instances;
let t0;

const offline = () =>
    Object.assign(new Error('That machine is not online right now.'), { transient: true });
/** The deadline passing on a machine that had answered. */
const outOfTime = (tunnel) => {
    tunnel.overdue = true;
    return Object.assign(
        new Error('Your machine answered, but no direct connection could be opened.'),
        { answered: true, mayPass: true },
    );
};

// The pairing layer needs WebCrypto and IndexedDB, which is not what is under
// test: an identity that exists is all the real connect() needs.
vi.mock('../../bootstrap/v1/pairing.js', () => ({
    loadIdentity: async () => ({ keyId: 'k', publicKeyBase64: 'p' }),
    helloMessage: () => ({ type: 'hello' }),
    verifyOffer: async () => true,
    signAnswer: async () => ({}),
}));

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
                this.onlate = null;
                this.overdue = false;
                this.connected = false;
                this.closed = false;
                if (id === hostId) made.push(this);
            }
            async connect() {
                if (this.id !== hostId) return new Promise(() => {});
                const n = calls.length;
                calls.push((Date.now() - t0) / 1000);
                const e = outcome(n, this);
                if (e) throw e;
                this.connected = true;
            }
            close() {
                this.closed = true;
                this.connected = false;
                this.onlate = null;
            }
            /** The channel opening, as the real transport reports it. */
            opens() {
                if (this.closed) return;
                this.connected = true;
                this.onstatus?.('ready');
                if (this.overdue) this.onlate?.();
            }
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

async function advance(seconds) {
    for (let left = seconds; left > 0; left -= 10) {
        await vi.advanceTimersByTimeAsync(Math.min(10, left) * 1000);
        await new Promise((resolve) => setImmediate(resolve));
    }
}

async function startPage() {
    vi.resetModules();
    t0 = Date.now();
    await import('../../bootstrap/v1/boot.js');
    for (let i = 0; i < 100 && document.body.dataset.state !== 'failed'; i++) await advance(0.01);
    expect(document.body.dataset.state).toBe('failed');
}

const text = (id) => document.getElementById(id).textContent;

class FakeWebSocket {
    static OPEN = 1;
    constructor() {
        this.readyState = FakeWebSocket.OPEN;
    }
    send() {}
    close() {}
}

beforeEach(() => {
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
    vi.stubGlobal('WebSocket', FakeWebSocket);
    Object.defineProperty(document, 'visibilityState', {
        configurable: true,
        get: () => 'visible',
    });
    vi.stubGlobal('navigator', { userAgent: 'test', serviceWorker: {}, onLine: true });
    outcome = () => offline();
    calls = [];
    made = [];
    fetched = [];
    hostId = idOf(++pages);
    instances = [];
    sessionStorage.clear();
    document.body.innerHTML =
        '<div id="instance-menu" hidden></div>' +
        '<h1 id="stage"></h1><button id="reload" hidden></button>' +
        '<p id="detail"></p><div id="bar"></div><p id="status"></p><p id="note"></p>' +
        '<button id="forget" hidden></button>';
    delete document.body.dataset.state;
    vi.spyOn(console, 'warn').mockImplementation(() => {});
    vi.spyOn(console, 'info').mockImplementation(() => {});
});

afterEach(() => {
    vi.useRealTimers();
    vi.restoreAllMocks();
    vi.unstubAllGlobals();
    delete document.visibilityState;
});

describe('the transport, past its deadline', () => {
    /** A real connect() left to run out of time, and its rejection. */
    async function overdueTunnel() {
        vi.resetModules();
        const { RealTunnel } = await import('../../bootstrap/v1/tunnel.js');
        const tunnel = new RealTunnel(hostId, 'https://example.test');
        let failure = null;
        tunnel.connect().catch((e) => {
            failure = e;
        });
        await advance(24);
        expect(failure).toBeNull();
        expect(tunnel.overdue).toBe(false);
        await advance(2);
        expect(failure.message).toBe('Your machine did not answer in time.');
        return tunnel;
    }

    it('says it gave up on time alone, and tells when the channel opens after that', async () => {
        const tunnel = await overdueTunnel();
        expect(tunnel.overdue).toBe(true);
        let late = 0;
        tunnel.onlate = () => late++;
        tunnel._adoptChannel({ readyState: 'open' });
        expect(late).toBe(1);
        expect(tunnel.connected).toBe(true);
    });

    it('tells nobody once it is closed', async () => {
        const tunnel = await overdueTunnel();
        let late = 0;
        tunnel.onlate = () => late++;
        tunnel.close();
        expect(tunnel.onlate).toBeNull();
        expect(late).toBe(0);
    });

    it('is not overdue for a failure that is an answer', async () => {
        vi.resetModules();
        const { RealTunnel } = await import('../../bootstrap/v1/tunnel.js');
        const tunnel = new RealTunnel(hostId, 'https://example.test');
        let failure = null;
        tunnel.connect().catch((e) => {
            failure = e;
        });
        await advance(1);
        tunnel._ws.onerror();
        await advance(1);
        expect(failure.message).toBe('Could not reach the introduction server.');
        expect(tunnel.overdue).toBe(false);
    });
});

describe('a call that passed its deadline', () => {
    it('carries the start on if it opens before the next try', async () => {
        outcome = (n, tunnel) => outOfTime(tunnel);
        await startPage();
        expect(text('note')).toContain(WATCH_NOTE);
        expect(made[0].closed).toBe(false);

        await advance(5);
        made[0].opens();
        await advance(1);

        // The same attempt, and no other call: the page went on to the interface.
        expect(calls).toHaveLength(1);
        expect(made).toHaveLength(1);
        expect(made[0].closed).toBe(false);
        expect(fetched).toEqual(['/api/app/manifest']);
    });

    it('writes nothing over the failure while it is set aside', async () => {
        outcome = (n, tunnel) => outOfTime(tunnel);
        await startPage();
        // What the transport does as its channel opens, with nobody to take it.
        made[0].onlate = null;
        made[0].opens();
        expect(text('stage')).toBe('Not connected');
        expect(document.body.dataset.state).toBe('failed');
    });

    it('is put down as the next try starts', async () => {
        outcome = (n, tunnel) => (n === 0 ? outOfTime(tunnel) : offline());
        await startPage();
        await advance(29);
        expect(made[0].closed).toBe(false);
        expect(calls).toHaveLength(1);

        await advance(2);
        expect(calls).toHaveLength(2);
        expect(made[0].closed).toBe(true);
        // That try failed with an answer: put down at once, as before.
        expect(made[1].closed).toBe(true);
        expect(fetched).toEqual([]);
    });

    it('is replaced by a later one that runs out of time too', async () => {
        outcome = (n, tunnel) => outOfTime(tunnel);
        await startPage();
        await advance(31);
        expect(calls).toHaveLength(2);
        expect(made[0].closed).toBe(true);
        expect(made[1].closed).toBe(false);

        made[1].opens();
        await advance(1);
        expect(calls).toHaveLength(2);
        expect(fetched).toEqual(['/api/app/manifest']);
    });

    it('is put down when the machine is removed from the list', async () => {
        instances = [{ id: hostId, name: 'Machine' }];
        outcome = (n, tunnel) => outOfTime(tunnel);
        await startPage();
        document.getElementById('forget').click();
        expect(made[0].closed).toBe(true);

        made[0].opens();
        await advance(200);
        expect(calls).toHaveLength(1);
        expect(fetched).toEqual([]);
    });
});

describe('one call at a time, whatever the failure says', () => {
    /** The attempts of this page that are neither closed nor failed for good. */
    const open = () => made.filter((t) => !t.closed);
    const busy = () => new Error('That machine already has as many connections as it accepts.');

    it('puts an overdue call down before the try that follows a passing failure', async () => {
        // Out of time, and reported as a failure that passes within the minute.
        outcome = (n, tunnel) => {
            if (n === 0) tunnel.overdue = true;
            return offline();
        };
        await startPage();
        expect(made[0].closed).toBe(false);
        await advance(3);
        expect(calls).toHaveLength(2);
        expect(made[0].closed).toBe(true);
        expect(open()).toEqual([]);
    });

    it('puts an overdue call down under a failure that is final', async () => {
        outcome = (n, tunnel) => {
            tunnel.overdue = true;
            return busy();
        };
        await startPage();
        expect(text('note')).not.toContain(WATCH_NOTE);
        expect(made).toHaveLength(1);
        expect(open()).toEqual([]);
    });

    it('puts it down when a later try ends on a failure that is final', async () => {
        outcome = (n, tunnel) => {
            tunnel.overdue = true;
            return n === 0 ? outOfTime(tunnel) : busy();
        };
        await startPage();
        await advance(31);
        expect(calls).toHaveLength(2);
        expect(text('note')).not.toContain(WATCH_NOTE);
        expect(open()).toEqual([]);
    });

    it('leaves one call open, the one the start carries on with', async () => {
        outcome = (n, tunnel) => (n === 0 ? outOfTime(tunnel) : null);
        await startPage();
        await advance(31);
        expect(calls).toHaveLength(2);
        expect(open()).toEqual([made[1]]);
        expect(fetched).toEqual(['/api/app/manifest']);
    });
});

describe('any other failed call', () => {
    it('is put down at once', async () => {
        await startPage();
        expect(made[0].closed).toBe(true);
    });
});
