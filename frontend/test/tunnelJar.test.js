/*
 * MoonlightWeb — frontend TNR. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * Two rules the rendezvous path depends on, both of which have already been got
 * wrong once.
 *
 * The cookie jar is the user agent for the far end — the browser's own jar
 * cannot be used, because a Set-Cookie on a Response a service worker builds is
 * ignored. Getting its lifetime wrong is not cosmetic: the access PIN
 * regenerates after every successful use, so a browser that forgets a session it
 * was told to keep leaves its owner unable to get back in without walking to the
 * machine. That is what these tests pin.
 *
 * The identifier resolution decides WHICH machine a page talks to. The order
 * matters for a reason worth stating: a remembered machine must never override
 * an identifier that is actually in the URL, or following a link to a second
 * machine would silently open the first.
 */

import { describe, it, expect, beforeEach, vi } from 'vitest';

function memoryStorage() {
    const map = new Map();
    return {
        getItem: (k) => (map.has(k) ? map.get(k) : null),
        setItem: (k, v) => map.set(k, String(v)),
        removeItem: (k) => map.delete(k),
        clear: () => map.clear(),
        get size() {
            return map.size;
        },
    };
}

let tunnelModule;

beforeEach(async () => {
    vi.stubGlobal('sessionStorage', memoryStorage());
    vi.stubGlobal('localStorage', memoryStorage());
    vi.resetModules();
    tunnelModule = await import('../../bootstrap/tunnel.js');
});

describe('cookie jar — where a credential is kept follows what the cookie asks', () => {
    // The jar is not exported (it is an implementation detail of Tunnel), so it
    // is exercised the way the code does: through a Tunnel's own jar.
    const jarOf = (hostId) => new tunnelModule.Tunnel(hostId, 'https://example.test')._cookies;

    it('keeps a cookie with Max-Age across a browser restart', () => {
        const jar = jarOf('a'.repeat(26));
        jar.absorb('mw_session=tok; HttpOnly; Secure; Path=/; SameSite=Strict; Max-Age=7776000');

        // A new browser session: sessionStorage is gone, localStorage is not.
        const survivor = memoryStorage();
        for (const key of ['mw-jar:' + 'a'.repeat(26)])
            survivor.setItem(key, localStorage.getItem(key));
        vi.stubGlobal('sessionStorage', memoryStorage());
        vi.stubGlobal('localStorage', survivor);

        expect(jarOf('a'.repeat(26)).header()).toBe('mw_session=tok');
    });

    it('drops a cookie with no Max-Age when the tab goes', () => {
        const jar = jarOf('b'.repeat(26));
        jar.absorb('mw_session=tok; HttpOnly; Secure; Path=/; SameSite=Strict');

        // "Uncheck on a shared computer" is exactly this cookie, and it must not
        // outlive the tab — storing it forever would quietly ignore the choice.
        vi.stubGlobal('sessionStorage', memoryStorage());
        expect(jarOf('b'.repeat(26)).header()).toBeNull();
    });

    it('honours a deletion, so signing out actually signs out', () => {
        const id = 'c'.repeat(26);
        const jar = jarOf(id);
        jar.absorb('mw_session=tok; Max-Age=7776000');
        expect(jar.header()).toBe('mw_session=tok');

        jar.absorb('mw_session=; HttpOnly; Secure; Path=/; SameSite=Strict; Max-Age=0');
        expect(jar.header()).toBeNull();
        // And the stored copy is cleared too, or the next start would sign the
        // user back in with a token the host has already revoked.
        expect(jarOf(id).header()).toBeNull();
    });

    it('treats an already-past Expires as a deletion', () => {
        const jar = jarOf('d'.repeat(26));
        jar.absorb('mw_session=tok; Expires=Thu, 01 Jan 1970 00:00:00 GMT');
        expect(jar.header()).toBeNull();
    });

    it('never lets one machine read another machine’s session', () => {
        // Every host is reached through ONE origin here, so this separation is
        // the jar's own job — there is no domain to do it.
        jarOf('e'.repeat(26)).absorb('mw_session=first; Max-Age=7776000');
        expect(jarOf('f'.repeat(26)).header()).toBeNull();
    });
});

describe('which machine a page talks to', () => {
    const A = 'a'.repeat(26);
    const B = 'b'.repeat(26);

    const at = (pathname, hash = '') => {
        vi.stubGlobal('location', { pathname, hash, origin: 'https://example.test' });
    };

    it('takes the identifier from the path', () => {
        at(`/${A}`);
        expect(tunnelModule.hostIdFromLocation()).toBe(A);
    });

    it('takes it from the fragment once the bootstrap has handed over', () => {
        at('/', `#${A}`);
        expect(tunnelModule.hostIdFromLocation()).toBe(A);
    });

    it('falls back to this tab’s machine when routing drops the fragment', () => {
        at('/', `#${A}`);
        tunnelModule.hostIdFromLocation(); // records it for the tab
        at('/settings');
        expect(tunnelModule.hostIdFromLocation()).toBe(A);
    });

    it('opens the last machine used when the address names none', () => {
        // The bare address is what someone types or bookmarks; before this it
        // produced an application with no hosts and no explanation.
        tunnelModule.rememberLastHost(A);
        at('/');
        expect(tunnelModule.hostIdFromLocation()).toBe(A);
    });

    it('lets an explicit identifier beat the remembered one', () => {
        // THE case that makes remembering safe: a link to a second machine must
        // open that machine, not the one this browser happens to have used.
        tunnelModule.rememberLastHost(A);
        at('/', `#${B}`);
        expect(tunnelModule.hostIdFromLocation()).toBe(B);

        tunnelModule.rememberLastHost(A);
        at(`/${B}`);
        expect(tunnelModule.hostIdFromLocation()).toBe(B);
    });

    it('answers null rather than guessing at a malformed identifier', () => {
        at('/not-an-identifier', '#also-not-one');
        expect(tunnelModule.hostIdFromLocation()).toBeNull();
    });

    // The fragment carries a second thing now: the host key the machine's own
    // tray puts there so its owner reaches admin under a trusted certificate.
    // It is in the fragment and not the query for one reason — a browser never
    // sends the fragment to the server it fetched the page from — so the
    // identifier has to survive sharing the space with it.
    describe('and the key it may carry', () => {
        it('reads the identifier and the key out of one fragment', () => {
            at('/', `#${A}&k=SEKRIT`);
            expect(tunnelModule.hostIdFromLocation()).toBe(A);
            expect(tunnelModule.hostKeyFromLocation()).toBe('SEKRIT');
        });

        it('reads a key that arrived alongside an identifier in the path', () => {
            // The shape of the link the tray hands out, before any handover.
            at(`/${A}`, '#k=SEKRIT');
            expect(tunnelModule.hostIdFromLocation()).toBe(A);
            expect(tunnelModule.hostKeyFromLocation()).toBe('SEKRIT');
        });

        it('decodes a key that had to be escaped', () => {
            at('/', `#${A}&k=${encodeURIComponent('a+b/c=d&e')}`);
            expect(tunnelModule.hostKeyFromLocation()).toBe('a+b/c=d&e');
        });

        it('answers null when the fragment carries no key', () => {
            at('/', `#${A}`);
            expect(tunnelModule.hostKeyFromLocation()).toBeNull();
            at(`/${A}`);
            expect(tunnelModule.hostKeyFromLocation()).toBeNull();
        });

        it('never reads a key out of the query, which the server would have seen', () => {
            at('/', '');
            vi.stubGlobal('location', {
                pathname: `/${A}`,
                hash: '',
                search: '?k=SEKRIT',
                origin: 'https://example.test',
            });
            expect(tunnelModule.hostKeyFromLocation()).toBeNull();
        });
    });

    // And a third thing: which page to land on. The address spends its path on
    // the identifier, so a link that means "the settings page" has nowhere else
    // to say so. The bootstrap hands this value to location.replace(), which is
    // why the shape is checked rather than trusted.
    describe('and the page it asks to land on', () => {
        it('reads the landing path out of the fragment', () => {
            at(`/${A}`, '#k=SEKRIT&p=/admin');
            expect(tunnelModule.landingPathFromLocation()).toBe('/admin');
            expect(tunnelModule.hostIdFromLocation()).toBe(A);
            expect(tunnelModule.hostKeyFromLocation()).toBe('SEKRIT');
        });

        it('answers null when the link asks for nothing', () => {
            at(`/${A}`, '#k=SEKRIT');
            expect(tunnelModule.landingPathFromLocation()).toBeNull();
        });

        it('refuses anything that could leave this origin', () => {
            // THE reason this is validated: every one of these is a URL the
            // browser would happily navigate to, and two of them go somewhere
            // else entirely.
            for (const bad of [
                '//evil.test',
                '/\\evil.test',
                'https://evil.test',
                '/admin/../../x',
                'admin',
                '/',
            ]) {
                at(`/${A}`, `#p=${encodeURIComponent(bad)}`);
                expect(tunnelModule.landingPathFromLocation()).toBeNull();
            }
        });

        it('refuses a path the application does not have room for', () => {
            at(`/${A}`, `#p=/${'x'.repeat(33)}`);
            expect(tunnelModule.landingPathFromLocation()).toBeNull();
        });
    });

    // And a fourth: the invitation an owner shared. It rides beside the landing
    // path rather than inside it, so the token never appears in a URL path at
    // any step — no navigation can carry it to the introduction server, whether
    // or not a service worker happens to be answering.
    describe('and the invitation it may carry', () => {
        const TOKEN = 'KCWUyMuvoh9WO5qmA9PfKc1NTyg0EIu6GkQFZs87f4g';

        it('reads the identifier, the landing page and the token together', () => {
            at(`/${A}`, `#p=/p&t=${TOKEN}`);
            expect(tunnelModule.hostIdFromLocation()).toBe(A);
            expect(tunnelModule.landingPathFromLocation()).toBe('/p');
            expect(tunnelModule.shareTokenFromLocation()).toBe(TOKEN);
        });

        it('still reads it after the handover moved the identifier', () => {
            at('/p', `#${A}&t=${TOKEN}`);
            expect(tunnelModule.hostIdFromLocation()).toBe(A);
            expect(tunnelModule.shareTokenFromLocation()).toBe(TOKEN);
        });

        // The whole difference from the host key next door. That one is burnt on
        // use; an invitation IS the guest's page and has to survive every reload
        // of it, so reading it must not consume it.
        it('answers the same token however often it is asked', () => {
            at('/p', `#${A}&t=${TOKEN}`);
            expect(tunnelModule.shareTokenFromLocation()).toBe(TOKEN);
            expect(tunnelModule.shareTokenFromLocation()).toBe(TOKEN);
            expect(tunnelModule.shareTokenFromLocation()).toBe(TOKEN);
        });

        it('decodes a token that had to be escaped', () => {
            at('/p', `#${A}&t=${encodeURIComponent(TOKEN)}`);
            expect(tunnelModule.shareTokenFromLocation()).toBe(TOKEN);
        });

        it('answers null when the fragment carries no invitation', () => {
            at(`/${A}`, '#p=/admin&k=SEKRIT');
            expect(tunnelModule.shareTokenFromLocation()).toBeNull();
            at(`/${A}`);
            expect(tunnelModule.shareTokenFromLocation()).toBeNull();
        });

        it('never reads a token out of the query, which the server would have seen', () => {
            at('/', '');
            vi.stubGlobal('location', {
                pathname: '/p',
                hash: '',
                search: `?t=${TOKEN}`,
                origin: 'https://example.test',
            });
            expect(tunnelModule.shareTokenFromLocation()).toBeNull();
        });

        // The value goes back into what is handed to location.replace() at
        // handover, so it is checked rather than trusted — same discipline as
        // the landing path.
        it('refuses anything this machine could not have minted', () => {
            for (const bad of ['', 'short', `${TOKEN}&k=SEKRIT`, '../../x', 'a'.repeat(129)]) {
                at('/p', `#${A}&t=${encodeURIComponent(bad)}`);
                expect(tunnelModule.shareTokenFromLocation()).toBeNull();
            }
        });
    });
});
