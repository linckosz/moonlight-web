/*
 * MoonlightWeb — frontend TNR. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * The entry page holding a machine's interface against the official one
 * (bootstrap/v1/boot.js, D26), from the manifest to the handover.
 *
 * Where the check is enforced, a machine that serves a changed file is refused
 * with nothing of it stored, so nothing of it can ever be served; a file that is
 * no part of the interface — the leftover of an earlier install, typically — is
 * left out and the start goes on; and only a service worker that serves nothing
 * but the checked files is told the cache was checked. Where it only reports,
 * the start is exactly what it was, with the verdict in the console.
 *
 * A machine that answered was found, whatever fails after that — its interface
 * refused or cut off, the worker refused or too old, the application not
 * starting — so the caption of one this browser does not know stays empty
 * rather than say "No server found".
 */

/* global Response, Request */

import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
import { webcrypto, createHash } from 'node:crypto';

import { canonicalBytes } from '../../bootstrap/v1/official.js';

const REPO = join(dirname(fileURLToPath(import.meta.url)), '..', '..');
const HOST_ID = 'a'.repeat(26);
const VERSION = '0.4.0';

const bytesOf = (text) => new TextEncoder().encode(text);
const sha = (bytes) => createHash('sha256').update(bytes).digest('hex');

/* ── What the machine serves, and the list of its release ──────────────── */

const INDEX = readFileSync(join(REPO, 'frontend', 'index.html'), 'utf8')
    .split('__MW_VERSION__')
    .join(VERSION);
/** The main page as the tunnel serves it: CRLF, and the machine's name written in. */
const SERVED_INDEX = INDEX.replace('content="MoonlightWeb"', 'content="MoonlightWeb [PC]"').replace(
    /\n/g,
    '\r\n',
);
const OFFICIAL = {
    '/index.html': SERVED_INDEX,
    '/js/app.js': 'export const app = 1;\n',
    '/js/util/VersionGuard.js': '// mw-shell-recheck\n',
};
const LIST = {
    version: VERSION,
    files: {
        '/index.html': sha(canonicalBytes('/index.html', bytesOf(INDEX))),
        '/js/app.js': sha(bytesOf(OFFICIAL['/js/app.js'])),
        '/js/util/VersionGuard.js': sha(bytesOf(OFFICIAL['/js/util/VersionGuard.js'])),
    },
};

/* ── The browser around the page ────────────────────────────────────────── */

/** CacheStorage, enough of it. */
function memoryCaches() {
    const stores = new Map();
    const store = () => {
        const map = new Map();
        const key = (r) => new URL(typeof r === 'string' ? r : r.url, location.origin).href;
        return {
            map,
            put: async (r, response) =>
                void map.set(key(r), new Uint8Array(await response.arrayBuffer())),
            match: async (r) => (map.has(key(r)) ? new Response(map.get(key(r))) : undefined),
            keys: async () => [...map.keys()].map((u) => new Request(u)),
            delete: async (r) => map.delete(key(r)),
        };
    };
    return {
        stores,
        open: async (name) => {
            if (!stores.has(name)) stores.set(name, store());
            return stores.get(name);
        },
        delete: async (name) => stores.delete(name),
        /** The paths in the application's cache. */
        shell: () =>
            stores.has('mw-shell')
                ? [...stores.get('mw-shell').map.keys()].map((u) => new URL(u).pathname)
                : [],
    };
}

/** The browser's SHA-256, answered at once (see beforeEach). */
async function digestAtOnce(algorithm, data) {
    if (algorithm !== 'SHA-256' || !ArrayBuffer.isView(data))
        throw new TypeError('SHA-256 of bytes only');
    const bytes = new Uint8Array(data.buffer, data.byteOffset, data.byteLength);
    return new Uint8Array(createHash('sha256').update(bytes).digest()).buffer;
}

/** The service worker in charge, answering the revision it is given. */
function worker(revision) {
    const told = [];
    return {
        told,
        postMessage(message, ports) {
            if (message.type === 'mw-sw-revision') {
                if (revision) ports[0].postMessage({ revision });
                return;
            }
            told.push(message.type);
        },
    };
}

let served;
let tunnelClosed;
let policy;
/** What the connection fails with, if it does. */
let connectError;
/** The file the connection drops at, if it does. */
let droppedAt;

vi.mock('../../bootstrap/v1/official.js', async (original) => ({
    ...(await original()),
    interfacePolicy: () => policy,
}));

vi.mock('../../bootstrap/v1/tunnel.js', async (original) => {
    const real = await original();
    return {
        ...real,
        Tunnel: class {
            constructor() {
                this.firstContact = false;
                this.onstatus = null;
            }
            async connect() {
                if (connectError) throw connectError;
            }
            close() {
                tunnelClosed = true;
            }
            async fetch(path) {
                if (path === droppedAt) throw new Error('the channel closed');
                if (path === '/api/app/manifest') {
                    const files = Object.keys(served);
                    return new Response(
                        JSON.stringify({ version: `${VERSION}-0123456789ab-89abcdef`, files }),
                    );
                }
                return served[path] === undefined
                    ? new Response('', { status: 404 })
                    : new Response(served[path], { headers: { 'Content-Type': 'x/y' } });
            }
        },
        hostIdFromLocation: () => HOST_ID,
        knownInstances: () => [],
        rememberLastHost: () => {},
    };
});

/** One start of the entry page, to its verdict and the handover. */
async function start({ revision = 2, registerError = null } = {}) {
    const sw = worker(revision);
    const registration = { active: sw, update: vi.fn(async () => {}) };
    vi.stubGlobal('navigator', {
        serviceWorker: {
            register: async () => {
                if (registerError) throw registerError;
                return registration;
            },
            ready: Promise.resolve(registration),
        },
    });
    vi.resetModules();
    await import('../../bootstrap/v1/boot.js');
    // The page has spoken once it handed over or failed. Its clock is driven
    // here: the bar's floor and an older worker's grace are seconds of waiting.
    const spoken = () => sw.told.length > 0 || document.body.dataset.state === 'failed';
    for (let i = 0; i < 1000 && !spoken(); i++) {
        await vi.advanceTimersByTimeAsync(50);
        await new Promise((resolve) => setImmediate(resolve));
    }
    if (!spoken()) throw new Error('the entry page never got to its verdict');
    return { sw, registration };
}

let cachesStub;
let warnings;

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
    if (!globalThis.crypto?.subtle) vi.stubGlobal('crypto', webcrypto);
    // Answered on this thread, at once. Node answers a digest from its thread
    // pool, in real time, while start() drives the page's clock through fifty
    // seconds in a few milliseconds: under the load of the whole suite, a start
    // that waits out an older worker's grace had not got to its verdict by then.
    vi.spyOn(crypto.subtle, 'digest').mockImplementation(digestAtOnce);
    vi.stubGlobal('requestAnimationFrame', () => 0);
    cachesStub = memoryCaches();
    vi.stubGlobal('caches', cachesStub);
    vi.stubGlobal(
        'fetch',
        vi.fn(async (url) =>
            url === `/v1/interfaces/${VERSION}.json`
                ? new Response(JSON.stringify(LIST), { status: 200 })
                : new Response('', { status: 404 }),
        ),
    );
    localStorage.clear();
    sessionStorage.clear();
    document.body.innerHTML =
        '<h1 id="stage"></h1><button id="reload" hidden></button><p id="detail"></p>' +
        '<div id="bar"></div><p id="status"></p><p id="note"></p>' +
        '<div id="instance-menu" hidden></div>';
    delete document.body.dataset.state;
    served = { ...OFFICIAL };
    tunnelClosed = false;
    connectError = null;
    droppedAt = null;
    warnings = [];
    vi.spyOn(console, 'warn').mockImplementation((...a) => warnings.push(a.join(' ')));
    vi.spyOn(console, 'info').mockImplementation(() => {});
    vi.spyOn(console, 'log').mockImplementation(() => {});
    // The handover navigates, which jsdom does not do.
    const error = console.error;
    vi.spyOn(console, 'error').mockImplementation((...a) => {
        if (!String(a[0]).includes('Not implemented: navigation')) error(...a);
    });
});

afterEach(() => {
    vi.useRealTimers();
    vi.restoreAllMocks();
    vi.unstubAllGlobals();
});

describe('where the check is enforced', () => {
    beforeEach(() => {
        policy = { enforce: true, unreleasedAllowed: true };
    });

    it('opens an official machine, its Windows line endings and its name included', async () => {
        const { sw } = await start();
        expect(sw.told).toEqual(['mw-shell-verified']);
        expect(cachesStub.shell().sort()).toEqual([
            '/',
            '/index.html',
            '/js/app.js',
            '/js/util/VersionGuard.js',
        ]);
    });

    it('refuses a machine that changed a file, and stores nothing of it', async () => {
        served['/js/app.js'] = 'steal(localStorage);\n';
        const { sw } = await start();
        expect(document.body.dataset.state).toBe('failed');
        expect(document.getElementById('stage').textContent).toBe('Not opened');
        expect(document.getElementById('detail').textContent).toBe(
            'This machine serves a modified interface.',
        );
        expect(document.getElementById('note').textContent).toBe(
            'MoonlightWeb only opens the official one, so nothing from it was run.',
        );
        expect(cachesStub.shell()).toEqual([]);
        expect(sw.told).toEqual([]);
        expect(tunnelClosed).toBe(true);
        // A reload would only ask the same machine for the same files.
        expect(document.getElementById('reload').hidden).toBe(true);
        expect(localStorage.getItem('mw-shell-stamp')).toBeNull();
    });

    it('refuses a changed main page as much as a changed script', async () => {
        served['/index.html'] = SERVED_INDEX.replace(
            '</head>',
            '<script src="/js/x.js"></script></head>',
        );
        await start();
        expect(document.getElementById('detail').textContent).toBe(
            'This machine serves a modified interface.',
        );
        expect(cachesStub.shell()).toEqual([]);
    });

    it('leaves out a file that is no part of the interface, and goes on', async () => {
        served['/js/ui/ConsentBar.js'] = 'export const stale = 1;\n';
        const { sw } = await start();
        expect(sw.told).toEqual(['mw-shell-verified']);
        expect(cachesStub.shell()).not.toContain('/js/ui/ConsentBar.js');
    });

    it('throws out what another page planted in a cache it trusted', async () => {
        // A first start fills the cache and stamps it.
        await start();
        const shell = await cachesStub.open('mw-shell');
        await shell.put('/js/app.js', new Response('steal(localStorage);\n'));
        await shell.put('/js/planted.js', new Response('steal();\n'));
        // The next one finds the stamp, holds the copy against the list, and
        // fetches the interface again.
        const { sw } = await start();
        expect(sw.told).toEqual(['mw-shell-verified']);
        const app = await (await cachesStub.open('mw-shell')).match('/js/app.js');
        expect(await app.text()).toBe(OFFICIAL['/js/app.js']);
        expect(cachesStub.shell()).not.toContain('/js/planted.js');
    });

    it('refuses a release no list was published for', async () => {
        vi.stubGlobal(
            'fetch',
            vi.fn(async () => new Response('', { status: 404 })),
        );
        await start();
        expect(document.getElementById('detail').textContent).toBe(
            'This machine serves a modified interface.',
        );
        expect(cachesStub.shell()).toEqual([]);
    });

    it('does not hand over to a service worker that would ask the machine for a missing file', async () => {
        const { sw, registration } = await start({ revision: 0 });
        expect(registration.update).toHaveBeenCalled();
        expect(document.getElementById('detail').textContent).toBe(
            'This page is out of date — reload it.',
        );
        expect(sw.told).toEqual([]);
    });
});

describe('where the check only reports', () => {
    beforeEach(() => {
        policy = { enforce: false, unreleasedAllowed: false };
    });

    it('starts a machine that changed a file as before, and says so in the console', async () => {
        served['/js/app.js'] = 'steal(localStorage);\n';
        const { sw } = await start();
        // Both words: an older worker opens on the second.
        expect(sw.told).toEqual(['mw-shell-verified', 'mw-shell-checked']);
        expect(cachesStub.shell()).toContain('/js/app.js');
        // Once: the check of the fresh cache after the download keeps quiet.
        expect(
            warnings.filter((w) => w.includes('/js/app.js differ from the official 0.4.0')),
        ).toHaveLength(1);
    });

    it('keeps what is not part of the interface, as before', async () => {
        served['/js/ui/ConsentBar.js'] = 'export const stale = 1;\n';
        await start();
        expect(cachesStub.shell()).toContain('/js/ui/ConsentBar.js');
    });
});

describe('the caption under a machine this browser has never reached', () => {
    // Enforced, which is where a refusal and a worker too old can stop a start.
    beforeEach(() => {
        policy = { enforce: true, unreleasedAllowed: true };
    });

    /** What the caption shows: nothing when it is hidden. */
    function caption() {
        const mount = document.getElementById('instance-menu');
        return mount.hidden ? '' : mount.textContent;
    }

    it('says "No server found" where no machine answered', async () => {
        connectError = new Error('Your machine did not answer.');
        await start();
        expect(document.getElementById('stage').textContent).toBe('Not connected');
        expect(caption()).toBe('No server found');
        expect(document.getElementById('reload').hidden).toBe(false);
    });

    it('stays empty where the machine answered but no direct connection opened', async () => {
        connectError = Object.assign(
            new Error('Your machine answered, but no direct connection could be opened.'),
            { answered: true },
        );
        await start();
        expect(document.getElementById('stage').textContent).toBe('Not connected');
        expect(caption()).toBe('');
    });

    it('stays empty under a machine it refused', async () => {
        served['/js/app.js'] = 'steal(localStorage);\n';
        await start();
        expect(document.getElementById('stage').textContent).toBe('Not opened');
        expect(caption()).toBe('');
    });

    it('stays empty under an interface cut off on its way down', async () => {
        droppedAt = '/js/app.js';
        await start();
        expect(document.getElementById('detail').textContent).toBe(
            'The interface did not come through: the channel closed.',
        );
        expect(caption()).toBe('');
    });

    it('stays empty when the browser refuses to install the worker', async () => {
        await start({ registerError: new Error('The operation is insecure') });
        expect(document.getElementById('detail').textContent).toBe(
            'This browser refused to install the application: The operation is insecure.',
        );
        expect(caption()).toBe('');
    });

    it('stays empty when the worker in charge is too old for this page', async () => {
        await start({ revision: 0 });
        expect(document.getElementById('detail').textContent).toBe(
            'This page is out of date — reload it.',
        );
        expect(caption()).toBe('');
    });

    it('stays empty when the handover did not reach the application', async () => {
        // The last handover's reload is still on the record: it never got there.
        sessionStorage.setItem('mw-handover-reload', String(Date.now()));
        await start();
        expect(document.getElementById('detail').textContent).toBe(
            'The application is installed but did not start.',
        );
        expect(caption()).toBe('');
    });
});
