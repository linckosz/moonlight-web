// The key a link of this machine carries (js/net/linkKey.js): read once, off
// the address before anything else runs; through the rendezvous, taken along
// by a page that starts over until the host has answered, and kept nowhere
// that outlives the page.
// @vitest-environment jsdom

import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, resolve } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));

/** The module, loaded afresh on the address given: it acts as it loads. */
async function loadAt(url) {
    vi.resetModules();
    window.history.replaceState(null, '', url);
    return import('../js/net/linkKey.js');
}

describe('splitLinkKey', () => {
    let splitLinkKey;
    beforeEach(async () => {
        ({ splitLinkKey } = await loadAt('/'));
    });

    it('takes the key out of a fragment that holds nothing else', () => {
        expect(splitLinkKey('#k=abc-DEF_123')).toEqual({ key: 'abc-DEF_123', hash: '' });
        expect(splitLinkKey('k=abc')).toEqual({ key: 'abc', hash: '' });
    });

    it('keeps the rest of the fragment, in its order', () => {
        expect(splitLinkKey('#a=1&k=K&b=2')).toEqual({ key: 'K', hash: '#a=1&b=2' });
        expect(splitLinkKey('#k=K&p=/admin')).toEqual({ key: 'K', hash: '#p=/admin' });
    });

    it('reads a key as the link wrote it', () => {
        expect(splitLinkKey('#k=a%2Fb%3D').key).toBe('a/b=');
    });

    it('leaves a fragment without a key as it was', () => {
        expect(splitLinkKey('')).toEqual({ key: null, hash: '' });
        expect(splitLinkKey('#')).toEqual({ key: null, hash: '' });
        expect(splitLinkKey('#section')).toEqual({ key: null, hash: '#section' });
        expect(splitLinkKey('#t=token')).toEqual({ key: null, hash: '#t=token' });
        // Not the key's name, only something that ends like it.
        expect(splitLinkKey('#ok=1')).toEqual({ key: null, hash: '#ok=1' });
        // An empty key is no key, and stays where it was.
        expect(splitLinkKey('#k=')).toEqual({ key: null, hash: '#k=' });
    });

    it('takes the first key only', () => {
        expect(splitLinkKey('#k=one&k=two')).toEqual({ key: 'one', hash: '#k=two' });
    });

    it('drops a key that cannot be read, rather than leave it in the address', () => {
        expect(splitLinkKey('#k=%E0%A4%A')).toEqual({ key: null, hash: '' });
    });
});

describe('the key of the link this page was opened with', () => {
    afterEach(() => {
        vi.restoreAllMocks();
        delete navigator.serviceWorker;
        window.history.replaceState(null, '', '/');
    });

    it('is off the address as soon as the module has loaded', async () => {
        const { takeLinkKey } = await loadAt('/admin?x=1#k=THE-KEY');
        expect(window.location.hash).toBe('');
        expect(window.location.pathname).toBe('/admin');
        expect(window.location.search).toBe('?x=1');
        expect(window.location.href).not.toContain('THE-KEY');
        expect(takeLinkKey()).toEqual({ key: 'THE-KEY', direct: true });
    });

    it('is handed out once', async () => {
        const { takeLinkKey } = await loadAt('/#k=ONCE');
        expect(takeLinkKey()).toEqual({ key: 'ONCE', direct: true });
        expect(takeLinkKey()).toBeNull();
    });

    it('leaves the rest of the fragment in the address', async () => {
        const { takeLinkKey } = await loadAt('/#a=1&k=K&b=2');
        expect(window.location.hash).toBe('#a=1&b=2');
        expect(takeLinkKey().key).toBe('K');
    });

    it('is nothing on an address that carries none, which is left alone', async () => {
        const replace = vi.spyOn(window.history, 'replaceState');
        const { takeLinkKey, carriedLinkKey, putLinkKeyBack } =
            await loadAt('/settings#t=invitation');
        // One call: the test's own, to set the address.
        expect(replace).toHaveBeenCalledTimes(1);
        expect(window.location.hash).toBe('#t=invitation');
        expect(takeLinkKey()).toBeNull();
        expect(carriedLinkKey()).toBeNull();
        putLinkKeyBack();
        expect(replace).toHaveBeenCalledTimes(1);
    });

    it('is not used at all when the address cannot be rewritten', async () => {
        vi.resetModules();
        window.history.replaceState(null, '', '/#k=STUCK');
        vi.spyOn(window.history, 'replaceState').mockImplementation(() => {
            throw new Error('not allowed here');
        });
        const { takeLinkKey, carriedLinkKey } = await import('../js/net/linkKey.js');
        expect(takeLinkKey()).toBeNull();
        expect(carriedLinkKey()).toBeNull();
    });

    it('is not taken along by a page the host serves itself', async () => {
        const { takeLinkKey, carriedLinkKey, putLinkKeyBack } = await loadAt('/admin#k=DIRECT');
        expect(carriedLinkKey()).toBeNull();
        putLinkKeyBack();
        expect(window.location.hash).toBe('');
        expect(takeLinkKey()).toEqual({ key: 'DIRECT', direct: true });
        putLinkKeyBack();
        expect(window.location.href).not.toContain('DIRECT');
    });
});

describe('the key of a link that came through the rendezvous', () => {
    const ID = 'a'.repeat(26);

    beforeEach(() => {
        // A controlling service worker: the bootstrap's, the only one there is.
        Object.defineProperty(navigator, 'serviceWorker', {
            value: { controller: {} },
            configurable: true,
        });
        sessionStorage.clear();
        localStorage.clear();
    });

    afterEach(() => {
        vi.restoreAllMocks();
        delete navigator.serviceWorker;
        window.history.replaceState(null, '', '/');
    });

    it('is off the address as soon as the module has loaded, the identifier left', async () => {
        const { takeLinkKey } = await loadAt(`/admin#${ID}&k=THE-LINE-KEY`);
        expect(window.location.hash).toBe(`#${ID}`);
        expect(window.location.href).not.toContain('THE-LINE-KEY');
        expect(takeLinkKey()).toEqual({ key: 'THE-LINE-KEY', direct: false });
        expect(takeLinkKey()).toBeNull();
    });

    it('is taken along until the host has answered, asked for or not', async () => {
        const { takeLinkKey, carriedLinkKey, forgetLinkKey } = await loadAt(`/#${ID}&k=CARRY`);
        expect(carriedLinkKey()).toBe('CARRY');
        takeLinkKey();
        // Sent, and no answer yet: still worth carrying.
        expect(carriedLinkKey()).toBe('CARRY');
        forgetLinkKey();
        expect(carriedLinkKey()).toBeNull();
        expect(takeLinkKey()).toBeNull();
    });

    it('goes back into the address just before a reload, after the identifier', async () => {
        const { putLinkKeyBack } = await loadAt(`/admin?x=1#${ID}&k=a%2Fb%3D`);
        expect(window.location.hash).toBe(`#${ID}`);
        putLinkKeyBack();
        expect(window.location.pathname).toBe('/admin');
        expect(window.location.search).toBe('?x=1');
        expect(window.location.hash).toBe(`#${ID}&k=a%2Fb%3D`);
    });

    it('goes back into an address the router left bare', async () => {
        const { putLinkKeyBack } = await loadAt(`/#${ID}&k=BARE`);
        window.history.replaceState(null, '', '/');
        putLinkKeyBack();
        expect(window.location.hash).toBe('#k=BARE');
    });

    it('is put back once, however often it is asked', async () => {
        const { putLinkKeyBack } = await loadAt(`/#${ID}&k=TWICE`);
        putLinkKeyBack();
        putLinkKeyBack();
        expect(window.location.hash).toBe(`#${ID}&k=TWICE`);
    });

    it('does not go back once the host has answered', async () => {
        const { takeLinkKey, forgetLinkKey, putLinkKeyBack } = await loadAt(`/#${ID}&k=SPENT`);
        takeLinkKey();
        forgetLinkKey();
        putLinkKeyBack();
        expect(window.location.hash).toBe(`#${ID}`);
    });

    it('is kept nowhere that outlives the page', async () => {
        const { takeLinkKey, putLinkKeyBack, forgetLinkKey } = await loadAt(`/#${ID}&k=NOWHERE`);
        takeLinkKey();
        putLinkKeyBack();
        forgetLinkKey();
        expect(sessionStorage.length).toBe(0);
        expect(localStorage.length).toBe(0);
        expect(document.cookie).toBe('');
    });
});

describe('where the application takes the key', () => {
    const app = readFileSync(resolve(here, '../js/app.js'), 'utf8');
    const module = readFileSync(resolve(here, '../js/net/linkKey.js'), 'utf8');

    it('imports this module before any other', () => {
        const first = app.match(/^import\s[^;]*?from\s+'([^']+)';/m);
        expect(first && first[1]).toBe('./net/linkKey.js');
    });

    it('lets nothing run before it: the module imports nothing', () => {
        expect(module).not.toMatch(/^\s*import\s/m);
        expect(module).not.toMatch(/\bimport\(/);
    });

    it('names no storage and no cookie in the module that holds the key', () => {
        expect(module).not.toMatch(/sessionStorage|localStorage|indexedDB|document\.cookie/);
    });

    it('reads the key from the address nowhere else', () => {
        const bridge = readFileSync(resolve(here, '../js/net/tunnelBridge.js'), 'utf8');
        expect(bridge).not.toMatch(/hostKeyFromLocation/);
        expect(app).not.toMatch(/hostKeyFromLocation|takeTunnelHostKey/);
    });

    it('no longer reads a key from the query', () => {
        expect(app).not.toMatch(/get\('mwk'\)/);
    });
});
