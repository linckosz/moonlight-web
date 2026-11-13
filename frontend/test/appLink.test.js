// The link straight to one app a page may be opened with (js/net/appLink.js,
// issue #63): off the address as the module loads, kept in this tab until it
// is served, only ever in the exact shape the host mints, and never for a
// guest's page.
// @vitest-environment jsdom

import { describe, it, expect, beforeEach, vi } from 'vitest';

const ALIAS = 'x7Kq2AbC90';

/** The module, loaded afresh on the address given: it acts as it loads. */
async function loadAt(url) {
    vi.resetModules();
    window.history.replaceState(null, '', url);
    return import('../js/net/appLink.js');
}

beforeEach(() => {
    sessionStorage.clear();
});

describe('splitAppLink', () => {
    it('takes the alias out, keeping the rest of the fragment in its order', async () => {
        const { splitAppLink } = await loadAt('/');
        expect(splitAppLink(`#go=${ALIAS}`)).toEqual({ alias: ALIAS, hash: '' });
        expect(splitAppLink(`#abc&go=${ALIAS}&k=K`)).toEqual({ alias: ALIAS, hash: '#abc&k=K' });
    });

    it('drops an ill-formed one from the fragment without taking it', async () => {
        const { splitAppLink } = await loadAt('/');
        for (const bad of ['short', `${ALIAS}x`, 'x7Kq2AbC9-', 'x7Kq2AbC9%30', '']) {
            expect(splitAppLink(`#a=1&go=${bad}`)).toEqual({ alias: null, hash: '#a=1' });
        }
    });
});

describe('the alias a page is opened with', () => {
    it('leaves the address as the module loads, and waits in this tab', async () => {
        const mod = await loadAt(`/#go=${ALIAS}`);
        expect(window.location.hash).toBe('');
        expect(mod.pendingAppLink()).toBe(ALIAS);
        // Read, not taken: a page that reloads before serving it finds it.
        expect(mod.pendingAppLink()).toBe(ALIAS);
        const again = await loadAt('/');
        expect(again.pendingAppLink()).toBe(ALIAS);
    });

    it('is gone once served, so a reload or a Stop never opens the app again', async () => {
        const mod = await loadAt(`/#go=${ALIAS}`);
        mod.forgetAppLink();
        expect(mod.pendingAppLink()).toBeNull();
        const again = await loadAt('/');
        expect(again.pendingAppLink()).toBeNull();
    });

    it('keeps the rest of a tunnel page fragment as it was', async () => {
        const id = 'a'.repeat(26);
        await loadAt(`/#${id}&go=${ALIAS}&k=SEKRIT`);
        expect(window.location.hash).toBe(`#${id}&k=SEKRIT`);
    });

    it('keeps only the shape the host mints', async () => {
        const mod = await loadAt('/#go=..%2F..%2Fx1');
        expect(window.location.hash).toBe('');
        expect(mod.pendingAppLink()).toBeNull();
        expect(sessionStorage.length).toBe(0);
    });

    it('is nothing to a guest page, which keeps nothing', async () => {
        const mod = await loadAt(`/p#${'a'.repeat(26)}&t=TOKEN&go=${ALIAS}`);
        expect(window.location.hash).not.toContain('go=');
        expect(mod.pendingAppLink()).toBeNull();
        expect(sessionStorage.length).toBe(0);
    });

    it('is forgotten when it waited too long to be served', async () => {
        const mod = await loadAt(`/#go=${ALIAS}`);
        const stored = JSON.parse(sessionStorage.getItem('mw-app-link'));
        stored.at -= 11 * 60 * 1000;
        sessionStorage.setItem('mw-app-link', JSON.stringify(stored));
        const later = await loadAt('/');
        expect(later.pendingAppLink()).toBeNull();
        expect(sessionStorage.length).toBe(0);
        expect(mod.isAppLinkAlias(ALIAS)).toBe(true);
    });

    it('never reads one out of the query', async () => {
        const mod = await loadAt(`/?go=${ALIAS}`);
        expect(mod.pendingAppLink()).toBeNull();
    });
});

describe('appLinkAddress', () => {
    it('is the machine link through the rendezvous, the origin root otherwise', async () => {
        const { appLinkAddress } = await loadAt('/');
        const id = 'a'.repeat(26);
        expect(appLinkAddress(ALIAS, { origin: 'https://stream.example.test', tunnelId: id })).toBe(
            `https://stream.example.test/${id}#go=${ALIAS}`,
        );
        expect(appLinkAddress(ALIAS, { origin: 'https://192.168.1.10:8443/' })).toBe(
            `https://192.168.1.10:8443/#go=${ALIAS}`,
        );
        expect(appLinkAddress('nope', { origin: 'https://x.test' })).toBe('');
    });
});
