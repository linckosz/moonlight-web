// The key a link of this machine carries, on a page the host serves itself
// (js/net/linkKey.js): read once, off the address before anything else runs.
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
        const { takeDirectLinkKey } = await loadAt('/admin?x=1#k=THE-KEY');
        expect(window.location.hash).toBe('');
        expect(window.location.pathname).toBe('/admin');
        expect(window.location.search).toBe('?x=1');
        expect(window.location.href).not.toContain('THE-KEY');
        expect(takeDirectLinkKey()).toBe('THE-KEY');
    });

    it('is handed out once', async () => {
        const { takeDirectLinkKey } = await loadAt('/#k=ONCE');
        expect(takeDirectLinkKey()).toBe('ONCE');
        expect(takeDirectLinkKey()).toBeNull();
    });

    it('leaves the rest of the fragment in the address', async () => {
        const { takeDirectLinkKey } = await loadAt('/#a=1&k=K&b=2');
        expect(window.location.hash).toBe('#a=1&b=2');
        expect(takeDirectLinkKey()).toBe('K');
    });

    it('is nothing on an address that carries none, which is left alone', async () => {
        const replace = vi.spyOn(window.history, 'replaceState');
        const { takeDirectLinkKey } = await loadAt('/settings#t=invitation');
        // One call: the test's own, to set the address.
        expect(replace).toHaveBeenCalledTimes(1);
        expect(window.location.hash).toBe('#t=invitation');
        expect(takeDirectLinkKey()).toBeNull();
    });

    it('is not this module to take on a page that came through the rendezvous', async () => {
        // A controlling service worker: the bootstrap's, the only one there is.
        Object.defineProperty(navigator, 'serviceWorker', {
            value: { controller: {} },
            configurable: true,
        });
        const { takeDirectLinkKey } = await loadAt('/#k=THE-LINE-KEY');
        expect(window.location.hash).toBe('#k=THE-LINE-KEY');
        expect(takeDirectLinkKey()).toBeNull();
    });

    it('is not used at all when the address cannot be rewritten', async () => {
        vi.resetModules();
        window.history.replaceState(null, '', '/#k=STUCK');
        vi.spyOn(window.history, 'replaceState').mockImplementation(() => {
            throw new Error('not allowed here');
        });
        const { takeDirectLinkKey } = await import('../js/net/linkKey.js');
        expect(takeDirectLinkKey()).toBeNull();
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

    it('no longer reads a key from the query', () => {
        expect(app).not.toMatch(/get\('mwk'\)/);
    });
});
