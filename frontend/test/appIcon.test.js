// A link straight to one app made into a home-screen icon (js/net/appIcon.js,
// issue #63): which page is that icon's, decided once as the module loads; the
// alias out of the query before anything else; the tunnel's word from its
// worker, never from the address; and no manifest left in the cache that still
// opens a link nobody asks for any more.
// @vitest-environment jsdom

import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';

const ALIAS = 'x7Kq2AbC90';
const tunnel = vi.hoisted(() => ({ on: false }));

vi.mock('../js/net/tunnelBridge.js', () => ({
    pageCameThroughTunnel: () => tunnel.on,
}));

const mark = (alias = ALIAS, at = Date.now()) =>
    sessionStorage.setItem('mw-app-icon', JSON.stringify({ alias, title: 'Desktop · PC', at }));

/** The modules, loaded afresh on the address given: they act as they load. */
async function loadAt(url) {
    vi.resetModules();
    window.history.replaceState(null, '', url);
    return import('../js/net/appIcon.js');
}

/** A Cache Storage holding @p manifest as the bootstrap's /manifest.webmanifest. */
function cacheHolding(manifest) {
    let body = manifest === null ? null : JSON.stringify(manifest);
    const cache = {
        match: vi.fn(async () => (body === null ? undefined : new Response(body))),
        put: vi.fn(async (_path, resp) => {
            body = await resp.text();
        }),
    };
    vi.stubGlobal('caches', {
        has: vi.fn(async (name) => name === 'mw-shell' && body !== null),
        open: vi.fn(async () => cache),
    });
    return { cache, held: () => (body === null ? null : JSON.parse(body)) };
}

beforeEach(() => {
    sessionStorage.clear();
    tunnel.on = false;
});

afterEach(() => {
    vi.unstubAllGlobals();
    vi.restoreAllMocks();
});

describe('straight to the host', () => {
    it('takes the alias out of the query first, with nothing asked of the network', async () => {
        mark();
        const fetchSpy = vi.fn();
        vi.stubGlobal('fetch', fetchSpy);
        const order = [];
        const replace = history.replaceState.bind(history);
        vi.spyOn(history, 'replaceState').mockImplementation((...args) => {
            order.push(`address ${args[2]}`);
            return replace(...args);
        });
        const mod = await loadAt(`/?icon=${ALIAS}`);
        // The first thing the module did, before anything went out.
        expect(order[1]).toBe(`address /#go=${ALIAS}`);
        expect(fetchSpy).not.toHaveBeenCalled();
        expect(window.location.search).toBe('');
        expect(window.location.hash).toBe(`#go=${ALIAS}`);
        expect(mod.appIconMode()).toEqual({ alias: ALIAS, title: 'Desktop · PC' });
    });

    it('is no icon without this tab’s mark, or with another alias’s', async () => {
        let mod = await loadAt(`/?icon=${ALIAS}`);
        expect(mod.appIconMode()).toBeNull();
        expect(window.location.search).toBe('');
        expect(window.location.hash).toBe('');
        mark('Zz9Zz9Zz9Z');
        mod = await loadAt(`/?icon=${ALIAS}`);
        expect(mod.appIconMode()).toBeNull();
        expect(sessionStorage.getItem('mw-app-icon')).toBeNull();
    });

    it('ends the icon when reloaded without the query: no navigation, no launch, no alias kept', async () => {
        mark();
        const fetchSpy = vi.fn();
        vi.stubGlobal('fetch', fetchSpy);
        // jsdom says so on the console when a page tries to navigate away.
        const error = vi.spyOn(console, 'error').mockImplementation(() => {});
        const replaced = [];
        const replace = history.replaceState.bind(history);
        vi.spyOn(history, 'replaceState').mockImplementation((...args) => {
            replaced.push(String(args[2]));
            return replace(...args);
        });
        const mod = await loadAt(`/#go=${ALIAS}`);
        const link = await import('../js/net/appLink.js');
        expect(mod.appIconMode()).toBeNull();
        expect(link.pendingAppLink()).toBeNull();
        expect(sessionStorage.getItem('mw-app-icon')).toBeNull();
        expect(window.location.href).not.toContain(ALIAS);
        // Past the address the test itself loaded, none carries the alias.
        expect(replaced.slice(1).some((url) => url.includes(ALIAS))).toBe(false);
        expect(fetchSpy).not.toHaveBeenCalled();
        expect(error).not.toHaveBeenCalledWith(expect.stringMatching(/navigation/i));
        for (const [first] of error.mock.calls) expect(String(first)).not.toMatch(/navigation/i);
    });

    it('is nothing to a guest page', async () => {
        mark();
        const mod = await loadAt(`/p?icon=${ALIAS}`);
        expect(mod.appIconMode()).toBeNull();
        expect(window.location.search).toBe('');
    });
});

describe('through the introduction server', () => {
    it('never believes the query: only the fragment and this tab’s mark', async () => {
        tunnel.on = true;
        mark();
        let mod = await loadAt(`/?icon=${ALIAS}`);
        expect(mod.appIconMode()).toBeNull();
        expect(window.location.search).toBe('');
        mark();
        mod = await loadAt(`/#go=${ALIAS}`);
        expect(mod.appIconMode().alias).toBe(ALIAS);
        // Left in the address, for a phone that bookmarks the address.
        expect(window.location.hash).toBe(`#go=${ALIAS}`);
    });

    it('fails closed on a page of the tunnel that lost its worker: nowhere to go, nothing launched', async () => {
        // A hard reload or an update: the mark and #go= are there, the worker is not.
        tunnel.on = false;
        mark();
        const fetchSpy = vi.fn();
        vi.stubGlobal('fetch', fetchSpy);
        const error = vi.spyOn(console, 'error').mockImplementation(() => {});
        const replaced = [];
        const replace = history.replaceState.bind(history);
        vi.spyOn(history, 'replaceState').mockImplementation((...args) => {
            replaced.push(String(args[2]));
            return replace(...args);
        });
        const id = '0123456789abcdefghjkmnpqrs';
        const mod = await loadAt(`/#${id}&go=${ALIAS}`);
        const link = await import('../js/net/appLink.js');
        expect(mod.appIconMode()).toBeNull();
        expect(link.pendingAppLink()).toBeNull();
        expect(window.location.hash).toBe(`#${id}`);
        expect(replaced.slice(1).some((url) => url.includes(ALIAS) || url.includes('icon='))).toBe(
            false,
        );
        expect(fetchSpy).not.toHaveBeenCalled();
        for (const [first] of error.mock.calls) expect(String(first)).not.toMatch(/navigation/i);
    });
});

describe('the manifest the worker’s cache holds', () => {
    const machine = {
        name: 'MoonlightWeb [PC]',
        start_url: '/0123456789abcdefghjkmnpqrs#k=K_1',
        id: '/0123456789abcdefghjkmnpqrs',
    };

    it('is made into the link’s here, named nothing, with the alias first', async () => {
        const { manifestWithAppLink } = await loadAt('/');
        const made = manifestWithAppLink(machine, ALIAS);
        expect(made.start_url).toBe(`/0123456789abcdefghjkmnpqrs#go=${ALIAS}&k=K_1`);
        expect(made.id).toBe(`/0123456789abcdefghjkmnpqrs?go=${ALIAS}`);
        expect(made).not.toHaveProperty('name');
        expect(made).not.toHaveProperty('short_name');
        expect(manifestWithAppLink(machine, 'x7Kq2#k=AA')).toBe(machine);
    });

    it('loses its link on any load that is no icon’s, keeping the key', async () => {
        const store = cacheHolding({
            start_url: `/0123456789abcdefghjkmnpqrs#go=${ALIAS}&k=K_1`,
            id: `/0123456789abcdefghjkmnpqrs?go=${ALIAS}`,
        });
        await loadAt('/');
        await vi.waitFor(() => expect(store.cache.put).toHaveBeenCalled());
        expect(store.held()).toEqual({
            start_url: '/0123456789abcdefghjkmnpqrs#k=K_1',
            id: '/0123456789abcdefghjkmnpqrs',
        });
    });

    it('loses it too when this tab’s mark is too old', async () => {
        tunnel.on = true;
        mark(ALIAS, Date.now() - 11 * 60 * 1000);
        const store = cacheHolding({ start_url: `/#go=${ALIAS}`, id: `/?go=${ALIAS}` });
        const mod = await loadAt(`/#go=${ALIAS}`);
        expect(mod.appIconMode()).toBeNull();
        await vi.waitFor(() => expect(store.held()).toEqual({ start_url: '/', id: '/' }));
    });

    it('keeps it on the icon’s own page, and leaves a fresh one alone', async () => {
        tunnel.on = true;
        mark();
        const linked = { start_url: `/#go=${ALIAS}`, id: `/?go=${ALIAS}` };
        const store = cacheHolding(linked);
        const mod = await loadAt(`/#go=${ALIAS}`);
        expect(mod.appIconMode().alias).toBe(ALIAS);
        await Promise.resolve();
        expect(store.cache.put).not.toHaveBeenCalled();

        // Written over meanwhile by the page: not undone with the old one.
        const fresh = { start_url: '/#k=K_2' };
        let reads = 0;
        store.cache.match.mockImplementation(
            async () => new Response(JSON.stringify(reads++ === 0 ? linked : fresh)),
        );
        expect(await mod.forgetCachedAppLink()).toBe(false);
        expect(store.cache.put).not.toHaveBeenCalled();
    });

    it('is left alone when there is no worker’s cache at all', async () => {
        vi.stubGlobal('caches', { has: vi.fn(async () => false), open: vi.fn() });
        const mod = await loadAt('/');
        expect(await mod.forgetCachedAppLink()).toBe(false);
        expect(caches.open).not.toHaveBeenCalled();
    });
});
