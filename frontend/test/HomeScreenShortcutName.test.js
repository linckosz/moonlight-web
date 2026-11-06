/*
 * MoonlightWeb — frontend TNR. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * A home-screen shortcut made through the introduction server keeps the
 * machine's name. The host leaves the name out of a shell served before the
 * PIN, so the signed-in page takes it from the manifest it fetches for the
 * shortcut (app.js _offerHomeScreenHandoff).
 */

// Through the introduction server, where the shortcut is made.
// @vitest-environment-options {"url": "https://stream.moonlightweb.top/"}

import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';

vi.mock('../js/net/tunnelBridge.js', async (importOriginal) => ({
    ...(await importOriginal()),
    tunnelHostId: () => '0123456789abcdefghjkmnpqrs',
}));

import { MoonlightApp } from '../js/app.js';

const shortcutName = () =>
    document.querySelector('meta[name="apple-mobile-web-app-title"]').getAttribute('content');

describe('the home-screen shortcut name after a PIN login', () => {
    let put;

    beforeEach(() => {
        // The shell as the host serves it before the PIN: the edition's name alone.
        document.head.innerHTML =
            '<meta name="apple-mobile-web-app-title" content="MoonlightWeb">' +
            '<link rel="manifest" href="/manifest.webmanifest">';
        put = vi.fn(async () => {});
        vi.stubGlobal('caches', { open: vi.fn(async () => ({ put })) });
        vi.spyOn(MoonlightApp, '_runsAsShortcut').mockReturnValue(false);
    });

    afterEach(() => {
        vi.restoreAllMocks();
        vi.unstubAllGlobals();
        document.head.innerHTML = '';
    });

    const answer = (manifest, ok = true) =>
        vi.stubGlobal(
            'fetch',
            vi.fn(async () => ({ ok, json: async () => manifest })),
        );

    it('takes the name the signed-in answer carries', async () => {
        const log = vi.spyOn(console, 'log').mockImplementation(() => {});
        answer({
            name: 'MoonlightWeb [Living-room PC]',
            start_url: '/0123456789abcdefghjkmnpqrs#k=key',
        });
        expect(await MoonlightApp._offerHomeScreenHandoff()).toBe('ready');
        expect(shortcutName()).toBe('MoonlightWeb [Living-room PC]');
        expect(put).toHaveBeenCalledWith('/manifest.webmanifest', expect.anything());
        expect(log).toHaveBeenCalledWith(expect.stringContaining('Home-screen shortcut prepared'));
    });

    it("keeps the shell's name when the answer has none, or when there is no answer", async () => {
        const warn = vi.spyOn(console, 'warn').mockImplementation(() => {});
        vi.spyOn(console, 'log').mockImplementation(() => {});
        answer({ start_url: '/0123456789abcdefghjkmnpqrs' });
        await MoonlightApp._offerHomeScreenHandoff();
        expect(shortcutName()).toBe('MoonlightWeb');

        answer({ name: 'MoonlightWeb [Living-room PC]' }, false);
        expect(await MoonlightApp._offerHomeScreenHandoff()).toBe('failed');
        expect(shortcutName()).toBe('MoonlightWeb');
        expect(warn).toHaveBeenCalledWith(
            expect.stringContaining('Home-screen shortcut not prepared'),
        );
    });

    it('says so when the manifest cannot be kept', async () => {
        const warn = vi.spyOn(console, 'warn').mockImplementation(() => {});
        answer({ name: 'MoonlightWeb [Living-room PC]' });
        vi.stubGlobal('caches', {
            open: vi.fn(async () => {
                throw new Error('no cache');
            }),
        });
        expect(await MoonlightApp._offerHomeScreenHandoff()).toBe('failed');
        expect(warn).toHaveBeenCalledWith(
            expect.stringContaining('Home-screen shortcut not prepared'),
            expect.any(Error),
        );
    });

    it('changes nothing in a shortcut, which has no share sheet', async () => {
        MoonlightApp._runsAsShortcut.mockReturnValue(true);
        answer({ name: 'MoonlightWeb [Living-room PC]' });
        expect(await MoonlightApp._offerHomeScreenHandoff()).toBe(null);
        expect(shortcutName()).toBe('MoonlightWeb');
        expect(fetch).not.toHaveBeenCalled();
    });
});
