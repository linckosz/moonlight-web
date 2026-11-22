/*
 * MoonlightWeb — frontend TNR. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * A link straight to one app made into a home-screen icon (issue #63, app.js
 * _makeAppIcon, _showAppIcon, _endAppIcon): the page reloads already being the
 * link — through the introduction server only its fragment changes and the
 * alias is in no request; straight to the host on /?icon= — and everything is
 * put back on Done (with a reload), on a touch elsewhere or on a launch
 * (without one), and at the end of the session.
 */

// @vitest-environment-options {"url": "https://stream.moonlightweb.top/"}

import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';

const ID = '0123456789abcdefghjkmnpqrs';
const ALIAS = 'x7Kq2AbC90';
const tb = vi.hoisted(() => ({ id: null, worker: false }));

vi.mock('../js/net/tunnelBridge.js', async (importOriginal) => ({
    ...(await importOriginal()),
    tunnelHostId: () => tb.id,
    pageCameThroughTunnel: () => tb.worker,
}));
vi.mock('../js/ui/ConfirmDialog.js', async (importOriginal) => ({
    ...(await importOriginal()),
    confirmAction: async () => true,
}));

import { MoonlightApp } from '../js/app.js';
import { SettingsView } from '../js/ui/SettingsView.js';
import { BackendClient } from '../js/api/BackendClient.js';
import { Toast } from '../js/ui/Toast.js';

const banner = () => document.querySelector('.app-link-banner');
const mark = () => JSON.parse(sessionStorage.getItem('mw-app-icon') || 'null');
const appleTitle = () =>
    document.querySelector('meta[name="apple-mobile-web-app-title"]').getAttribute('content');
const manifestHref = () => document.querySelector('link[rel="manifest"]').getAttribute('href');

/** The bootstrap's cache, holding @p manifest as /manifest.webmanifest. */
function workerCache(manifest = null) {
    let body = manifest === null ? null : JSON.stringify(manifest);
    const cache = {
        match: vi.fn(async () => (body === null ? undefined : new Response(body))),
        put: vi.fn(async (_path, resp) => {
            body = await resp.text();
        }),
    };
    vi.stubGlobal('caches', {
        has: vi.fn(async () => true),
        open: vi.fn(async () => cache),
    });
    return { cache, held: () => (body === null ? null : JSON.parse(body)) };
}

let reload;
let open;

beforeEach(() => {
    sessionStorage.clear();
    history.replaceState(null, '', '/');
    document.head.innerHTML =
        '<meta name="apple-mobile-web-app-title" content="MoonlightWeb">' +
        '<link rel="manifest" href="/manifest.webmanifest">';
    // After the head is written, or the <title> it makes is wiped with it.
    document.title = 'MoonlightWeb';
    document.body.innerHTML = '<button id="card">Display 1</button>';
    reload = vi.spyOn(MoonlightApp, '_reloadPage').mockImplementation(() => {});
    open = vi.spyOn(MoonlightApp, '_openAddress').mockImplementation(() => {});
    vi.spyOn(MoonlightApp, '_runsAsShortcut').mockReturnValue(false);
    vi.spyOn(console, 'log').mockImplementation(() => {});
    vi.spyOn(console, 'warn').mockImplementation(() => {});
});

afterEach(() => {
    vi.restoreAllMocks();
    vi.unstubAllGlobals();
    document.head.innerHTML = '';
    document.body.innerHTML = '';
    tb.id = null;
    tb.worker = false;
});

describe('making a link’s icon through the introduction server', () => {
    beforeEach(() => {
        tb.id = ID;
        tb.worker = true;
        vi.stubGlobal(
            'fetch',
            vi.fn(async () => ({
                ok: true,
                json: async () => ({
                    name: 'MoonlightWeb [PC]',
                    start_url: `/${ID}#k=K_1`,
                    id: `/${ID}`,
                }),
            })),
        );
    });

    it('reloads on its own address once the cache holds the link’s manifest, the alias in no request', async () => {
        const store = workerCache();
        await MoonlightApp._makeAppIcon(ALIAS, 'Display 1 · PC');

        expect(fetch).toHaveBeenCalledTimes(1);
        for (const [url] of fetch.mock.calls) expect(String(url)).not.toContain(ALIAS);
        // Named after the icon by the page itself (I63NAME): nothing asked.
        expect(store.held()).toEqual({
            name: 'Display 1 · PC',
            // I63FULL: the name whole is the one a phone proposes.
            short_name: 'Display 1 · PC',
            start_url: `/${ID}#go=${ALIAS}&k=K_1`,
            id: `/${ID}?go=${ALIAS}`,
        });

        // Only the fragment changed, then the same address reloaded.
        expect(window.location.pathname).toBe('/');
        expect(window.location.search).toBe('');
        expect(window.location.hash).toBe(`#go=${ALIAS}`);
        expect(reload).toHaveBeenCalledTimes(1);
        expect(open).not.toHaveBeenCalled();
        expect(mark()).toMatchObject({ alias: ALIAS, title: 'Display 1 · PC' });
    });

    it('proposes the name whole through the tunnel too (I63FULL)', async () => {
        const store = workerCache();
        await MoonlightApp._makeAppIcon(ALIAS, 'Display 1 - PC - MoonlightWeb');
        expect(store.held()).toMatchObject({
            name: 'Display 1 - PC - MoonlightWeb',
            short_name: 'Display 1 - PC - MoonlightWeb',
        });
        for (const [url] of fetch.mock.calls) expect(String(url)).not.toContain('Display');
    });

    it('makes nothing on a page that lost its worker, and asks nothing with the alias', async () => {
        tb.worker = false;
        workerCache();
        const error = vi.spyOn(Toast, 'error').mockImplementation(() => {});
        await MoonlightApp._makeAppIcon(ALIAS, 'Display 1 · PC');
        expect(error).toHaveBeenCalled();
        for (const [url] of fetch.mock.calls) expect(String(url)).not.toContain(ALIAS);
        expect(reload).not.toHaveBeenCalled();
        expect(open).not.toHaveBeenCalled();
        expect(mark()).toBeNull();
        expect(window.location.hash).toBe('');
    });
});

describe('making a link’s icon straight to the host', () => {
    it('reloads on /?icon= with this tab’s mark, asking nothing', async () => {
        vi.stubGlobal('fetch', vi.fn());
        await MoonlightApp._makeAppIcon(ALIAS, 'Display 1 · PC');
        // The icon's name with it, for the host to give back (I63NAME).
        expect(open).toHaveBeenCalledWith(`/?icon=${ALIAS}&n=Display%201%20%C2%B7%20PC`);
        expect(fetch).not.toHaveBeenCalled();
        expect(mark()).toMatchObject({ alias: ALIAS });
    });

    it('sends the name alone, no short name: the host proposes it whole (I63FULL)', async () => {
        vi.stubGlobal('fetch', vi.fn());
        await MoonlightApp._makeAppIcon(ALIAS, 'Display 1 - PC - MoonlightWeb');
        expect(open).toHaveBeenCalledWith(
            `/?icon=${ALIAS}&n=Display%201%20-%20PC%20-%20MoonlightWeb`,
        );
        expect(String(open.mock.calls[0][0])).not.toContain('&s=');
    });

    it('sends no name that would not hold as one', async () => {
        vi.stubGlobal('fetch', vi.fn());
        await MoonlightApp._makeAppIcon(ALIAS, 'Display‮1');
        expect(open).toHaveBeenCalledWith(`/?icon=${ALIAS}`);
    });

    it('makes nothing on a page a worker answers without a tunnel', async () => {
        tb.worker = true;
        const error = vi.spyOn(Toast, 'error').mockImplementation(() => {});
        await MoonlightApp._makeAppIcon(ALIAS, 'Display 1 · PC');
        expect(error).toHaveBeenCalled();
        expect(open).not.toHaveBeenCalled();
        expect(mark()).toBeNull();
    });
});

describe('the page as a link’s icon, and back', () => {
    const show = () => {
        sessionStorage.setItem(
            'mw-app-icon',
            JSON.stringify({ alias: ALIAS, title: 'Display 1 · PC', at: Date.now() }),
        );
        document
            .querySelector('link[rel="manifest"]')
            .setAttribute('href', `/manifest.webmanifest?go=${ALIAS}`);
        MoonlightApp._showAppIcon({ alias: ALIAS, title: 'Display 1 · PC' });
    };

    it('shows the link and the app, the banner pointing at the browser’s button', () => {
        show();
        expect(window.location.pathname + window.location.hash).toBe(`/#go=${ALIAS}`);
        expect(document.title).toBe('Display 1 · PC');
        expect(appleTitle()).toBe('Display 1 · PC');
        expect(banner()).not.toBeNull();
        expect(banner().querySelector('[data-act="share"]')).toBeNull();
    });

    it('a touch elsewhere puts it all back without a reload, and the touch goes on', async () => {
        show();
        const card = document.getElementById('card');
        const clicked = vi.fn();
        card.addEventListener('click', clicked);
        card.dispatchEvent(new Event('pointerdown', { bubbles: true }));
        card.click();
        await vi.waitFor(() => expect(document.title).toBe('MoonlightWeb'));
        expect(banner()).toBeNull();
        expect(clicked).toHaveBeenCalledTimes(1);
        expect(mark()).toBeNull();
        expect(appleTitle()).toBe('MoonlightWeb');
        expect(manifestHref()).toBe('/manifest.webmanifest');
        expect(window.location.pathname + window.location.search + window.location.hash).toBe('/');
        expect(reload).not.toHaveBeenCalled();
        expect(open).not.toHaveBeenCalled();
    });

    it('Done puts it all back and reloads, straight to the host on its root', async () => {
        show();
        banner().querySelector('[data-act="done"]').click();
        await vi.waitFor(() => expect(open).toHaveBeenCalledWith('/'));
        expect(mark()).toBeNull();
        expect(manifestHref()).toBe('/manifest.webmanifest');
        expect(reload).not.toHaveBeenCalled();
    });

    it('through the tunnel, the worker’s cache goes back to the machine’s own, then Done reloads', async () => {
        tb.id = ID;
        tb.worker = true;
        const back = vi
            .spyOn(MoonlightApp, '_offerHomeScreenHandoffWithin')
            .mockResolvedValue('ready');
        show();
        banner().querySelector('[data-act="done"]').click();
        await vi.waitFor(() => expect(reload).toHaveBeenCalledTimes(1));
        expect(back).toHaveBeenCalledTimes(1);
        expect(open).not.toHaveBeenCalled();
    });

    it('through the tunnel, the link leaves the cache even when the host does not answer', async () => {
        tb.id = ID;
        tb.worker = true;
        vi.spyOn(MoonlightApp, '_offerHomeScreenHandoffWithin').mockResolvedValue('timeout');
        const store = workerCache({
            start_url: `/${ID}#go=${ALIAS}&k=K_1`,
            id: `/${ID}?go=${ALIAS}`,
        });
        show();
        document.getElementById('card').dispatchEvent(new Event('pointerdown', { bubbles: true }));
        await vi.waitFor(() =>
            expect(store.held()).toEqual({ start_url: `/${ID}#k=K_1`, id: `/${ID}` }),
        );
        expect(reload).not.toHaveBeenCalled();
    });

    it('a launch closes it and puts the page back, without a reload', async () => {
        show();
        vi.spyOn(MoonlightApp, '_isHostMachine').mockReturnValue(true);
        const warned = vi
            .spyOn(MoonlightApp, '_showSelfStreamWarning')
            .mockImplementation(() => {});
        MoonlightApp.state = 'host_list';
        await MoonlightApp.launchApp({ isLocalHost: true }, { name: 'Display 1' });
        expect(warned).toHaveBeenCalled();
        expect(banner()).toBeNull();
        await vi.waitFor(() => expect(mark()).toBeNull());
        expect(manifestHref()).toBe('/manifest.webmanifest');
        expect(reload).not.toHaveBeenCalled();
        expect(open).not.toHaveBeenCalled();
    });
});

describe('the end of the session', () => {
    it('leaves no link’s icon behind: neither this tab’s mark nor the cached manifest', async () => {
        sessionStorage.setItem(
            'mw-app-icon',
            JSON.stringify({ alias: ALIAS, title: 'Display 1 · PC', at: Date.now() }),
        );
        const store = workerCache({
            start_url: `/${ID}#go=${ALIAS}&k=K_1`,
            id: `/${ID}?go=${ALIAS}`,
        });
        vi.spyOn(BackendClient, 'logout').mockResolvedValue({});
        const btn = document.createElement('button');
        // The reload that follows is the browser's to do: jsdom only says so.
        vi.spyOn(console, 'error').mockImplementation(() => {});
        await SettingsView.prototype._logout.call({}, btn);
        expect(mark()).toBeNull();
        expect(store.held()).toEqual({ start_url: `/${ID}#k=K_1`, id: `/${ID}` });
    });
});
