// The link straight to one app a page may be opened with (js/net/appLink.js,
// issue #63): off the address as the module loads, kept in this tab until it
// is served, only ever in the exact shape the host mints, and never for a
// guest's page.
// @vitest-environment jsdom

import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';

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

describe('the page this tab is making into an icon (net/appIcon.js)', () => {
    const mark = (alias, at = Date.now()) =>
        sessionStorage.setItem('mw-app-icon', JSON.stringify({ alias, title: 'Desktop · PC', at }));

    it('leaves its own alias in the address, and launches nothing', async () => {
        mark(ALIAS);
        const mod = await loadAt(`/#go=${ALIAS}`);
        expect(window.location.hash).toBe(`#go=${ALIAS}`);
        expect(mod.pendingAppLink()).toBeNull();
        expect(mod.appIconMark()).toEqual({ alias: ALIAS, title: 'Desktop · PC' });
    });

    it('is a link like any other for another alias, or once the mark is old', async () => {
        mark('Zz9Zz9Zz9Z');
        let mod = await loadAt(`/#go=${ALIAS}`);
        expect(mod.pendingAppLink()).toBe(ALIAS);
        sessionStorage.clear();
        mark(ALIAS, Date.now() - 11 * 60 * 1000);
        mod = await loadAt(`/#go=${ALIAS}`);
        expect(mod.pendingAppLink()).toBe(ALIAS);
        expect(sessionStorage.getItem('mw-app-icon')).toBeNull();
    });

    it('notes only an alias, and forgets it on request', async () => {
        const mod = await loadAt('/');
        expect(mod.markAppIcon('nope', 'x')).toBe(false);
        expect(mod.markAppIcon(ALIAS, 'Desktop · PC')).toBe(true);
        expect(mod.appIconMark().alias).toBe(ALIAS);
        mod.clearAppIconMark();
        expect(mod.appIconMark()).toBeNull();
    });
});

describe('a link that reaches the page without a load (watchAppLinks)', () => {
    /** matchMedia answering @p modes, as the page sees it while it loads. */
    const displayModes = (...modes) =>
        vi.stubGlobal('matchMedia', (query) => ({
            matches: modes.some((mode) => query === `(display-mode: ${mode})`),
        }));

    const changeFragment = (hash) => {
        window.history.replaceState(null, '', `/${hash}`);
        window.dispatchEvent(new HashChangeEvent('hashchange'));
    };

    afterEach(() => {
        vi.unstubAllGlobals();
        Object.defineProperty(document, 'fullscreenElement', { value: null, configurable: true });
    });

    it('starts nothing in a tab, even once it went full screen for a stream', async () => {
        displayModes('browser');
        const mod = await loadAt('/');
        const onLink = vi.fn();
        const stop = mod.watchAppLinks(onLink);
        // The tab goes full screen afterwards: still a tab.
        displayModes('fullscreen');
        Object.defineProperty(document, 'fullscreenElement', {
            value: document.body,
            configurable: true,
        });
        changeFragment(`#go=${ALIAS}`);
        expect(onLink).not.toHaveBeenCalled();
        expect(mod.pendingAppLink()).toBeNull();
        stop();
    });

    it('follows one in a shortcut loaded full screen, sorted as at load', async () => {
        displayModes('fullscreen');
        const mod = await loadAt('/');
        const onLink = vi.fn();
        const stop = mod.watchAppLinks(onLink);
        changeFragment(`#go=${ALIAS}`);
        expect(onLink).toHaveBeenCalledWith(ALIAS);
        expect(window.location.hash).toBe('');
        expect(mod.pendingAppLink()).toBe(ALIAS);
        // The same refusals as at load: an ill-formed one, a guest's page.
        mod.forgetAppLink();
        onLink.mockClear();
        changeFragment('#go=..%2Fx');
        changeFragment(`#t=TOKEN&go=${ALIAS}`);
        expect(onLink).not.toHaveBeenCalled();
        expect(mod.pendingAppLink()).toBeNull();
        stop();
    });

    it('follows one in a standalone shortcut too', async () => {
        displayModes('standalone');
        const mod = await loadAt('/');
        const onLink = vi.fn();
        const stop = mod.watchAppLinks(onLink);
        changeFragment(`#go=${ALIAS}`);
        expect(onLink).toHaveBeenCalledWith(ALIAS);
        stop();
        onLink.mockClear();
        changeFragment(`#go=${ALIAS}`);
        expect(onLink).not.toHaveBeenCalled();
    });

    it('is decided as the module loads: loaded in a full-screen tab, it never listens', async () => {
        displayModes('fullscreen');
        Object.defineProperty(document, 'fullscreenElement', {
            value: document.body,
            configurable: true,
        });
        const mod = await loadAt('/');
        const onLink = vi.fn();
        mod.watchAppLinks(onLink);
        changeFragment(`#go=${ALIAS}`);
        expect(onLink).not.toHaveBeenCalled();
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
