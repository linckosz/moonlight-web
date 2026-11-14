/*
 * MoonlightWeb — frontend TNR. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * Serving a link straight to one app (issue #63, app.js _serveAppLink), as
 * decided after the phone benches of 14/11:
 *  - the app's launch shows at once, not the list of hosts behind it, and the
 *    page comes back when nothing is launched;
 *  - nothing is launched with no tap from a session that is another browser's
 *    (an iOS web app: the page's cookie, a key of its own), wherever the link
 *    came from — the host's answer on the key is waited for just before, and a
 *    refusal asks for the PIN, the link waiting through it.
 */

// Not localhost, where every browser has the admin page.
// @vitest-environment-options {"url": "https://192.168.1.66:48443/"}

import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import { MoonlightApp } from '../js/app.js';
import { BackendClient } from '../js/api/BackendClient.js';
import {
    showAppLinkLoader,
    hideAppLinkLoader,
    appLinkLoaderShown,
} from '../js/ui/AppLinkLoader.js';

const ALIAS = 'x7Kq2AbC90';
const loader = () => document.querySelector('.app-link-loader');

let saved;
let openAppLink;
let askPin;

beforeEach(() => {
    saved = { hostListView: MoonlightApp.hostListView, state: MoonlightApp.state };
    document.body.innerHTML = '';
    sessionStorage.setItem('mw-app-link', JSON.stringify({ alias: ALIAS, at: Date.now() }));
    openAppLink = vi.fn(async () => {});
    MoonlightApp.hostListView = { openAppLink };
    MoonlightApp.state = 'host_list';
    askPin = vi.spyOn(MoonlightApp, '_askPinAgain').mockImplementation(() => {});
    vi.spyOn(MoonlightApp, '_isHostMachine').mockReturnValue(false);
    vi.spyOn(console, 'log').mockImplementation(() => {});
    vi.spyOn(console, 'warn').mockImplementation(() => {});
});

afterEach(() => {
    hideAppLinkLoader();
    Object.assign(MoonlightApp, saved);
    sessionStorage.clear();
    vi.useRealTimers();
    vi.restoreAllMocks();
});

describe('a link straight to one app, from a session that is another browser’s', () => {
    it('asks for the PIN rather than launching, even when the host is slow to say so', async () => {
        // The web app's first key took a while: the 409 came after the boot's 1.5 s.
        vi.useFakeTimers();
        vi.spyOn(BackendClient, 'ensurePairingKey').mockReturnValue(
            new Promise((resolve) => setTimeout(resolve, 3000, { sessionKeyRefused: true })),
        );
        const served = MoonlightApp._serveAppLink(ALIAS);
        expect(loader()).not.toBeNull();
        await vi.advanceTimersByTimeAsync(3000);
        await served;
        expect(askPin).toHaveBeenCalledTimes(1);
        expect(openAppLink).not.toHaveBeenCalled();
        expect(loader()).toBeNull();
        // Still waiting, for the page the PIN brings back.
        expect(JSON.parse(sessionStorage.getItem('mw-app-link')).alias).toBe(ALIAS);
    });

    it('does the same for a link that reached a shortcut already open', async () => {
        vi.spyOn(BackendClient, 'ensurePairingKey').mockResolvedValue({ sessionKeyRefused: true });
        MoonlightApp._followAppLink(ALIAS);
        await vi.waitFor(() => expect(askPin).toHaveBeenCalledTimes(1));
        expect(openAppLink).not.toHaveBeenCalled();
    });

    it('does not wait for ever on a host that never answers', async () => {
        vi.useFakeTimers();
        vi.spyOn(BackendClient, 'ensurePairingKey').mockReturnValue(new Promise(() => {}));
        const served = MoonlightApp._serveAppLink(ALIAS);
        await vi.advanceTimersByTimeAsync(10000);
        await served;
        expect(openAppLink).toHaveBeenCalledTimes(1);
        expect(askPin).not.toHaveBeenCalled();
    });

    it('asks nothing on the host machine, which has no PIN to give', async () => {
        MoonlightApp._isHostMachine.mockReturnValue(true);
        const ensure = vi.spyOn(BackendClient, 'ensurePairingKey');
        await MoonlightApp._serveAppLink(ALIAS);
        expect(ensure).not.toHaveBeenCalled();
        expect(openAppLink).toHaveBeenCalledTimes(1);
    });
});

describe('what the link shows while it opens', () => {
    beforeEach(() => {
        vi.spyOn(BackendClient, 'ensurePairingKey').mockResolvedValue({ hostPublicKey: 'a2V5' });
    });

    it('names the app once the host says it, and stays while the launch runs', async () => {
        openAppLink.mockImplementation(async (_alias, onName) => {
            // No locale is loaded here: t() gives the key, unnamed then named.
            expect(loader().textContent).toBe('appLink.openingAny');
            onName('Virtual Display');
            // The card's press reached a launch.
            MoonlightApp._launchCalls = (MoonlightApp._launchCalls || 0) + 1;
        });
        await MoonlightApp._serveAppLink(ALIAS);
        expect(openAppLink.mock.calls[0][0]).toBe(ALIAS);
        expect(loader().textContent).toBe('appLink.opening');
        // Gone once the stream is there.
        MoonlightApp.transition('streaming');
        expect(loader()).toBeNull();
    });

    it('goes when the launch ends back on the list', async () => {
        openAppLink.mockImplementation(async () => {
            MoonlightApp._launchCalls = (MoonlightApp._launchCalls || 0) + 1;
        });
        await MoonlightApp._serveAppLink(ALIAS);
        expect(loader()).not.toBeNull();
        MoonlightApp.transition('host_list');
        expect(loader()).toBeNull();
    });

    it('leaves the page at once when nothing was launched', async () => {
        // The link led nowhere, or a question was answered no.
        await MoonlightApp._serveAppLink(ALIAS);
        expect(openAppLink).toHaveBeenCalledTimes(1);
        expect(loader()).toBeNull();
    });

    it('leaves the page when opening the link failed outright', async () => {
        openAppLink.mockRejectedValue(new Error('down'));
        await expect(MoonlightApp._serveAppLink(ALIAS)).rejects.toThrow('down');
        expect(loader()).toBeNull();
    });

    it('steps aside for a question about the launch', () => {
        showAppLinkLoader();
        try {
            MoonlightApp._showAppRunningDialog({ uuid: 'h1' }, { id: 2, name: 'Display 1' }, 1);
        } catch {
            // Only the loader is of interest here, not the dialog's own needs.
        }
        expect(loader()).toBeNull();
        document.querySelector('.app-running-overlay')?.remove();
    });
});

describe('the loader itself', () => {
    it('is one element, which a second show only renames', () => {
        showAppLinkLoader();
        showAppLinkLoader('Desktop');
        expect(document.querySelectorAll('.app-link-loader').length).toBe(1);
        expect(loader().getAttribute('role')).toBe('status');
        expect(appLinkLoaderShown()).toBe(true);
        hideAppLinkLoader();
        expect(appLinkLoaderShown()).toBe(false);
    });

    it('writes the name as text', () => {
        showAppLinkLoader('<img src=x onerror=alert(1)>');
        expect(loader().querySelector('img')).toBeNull();
    });

    it('never stays up for good', async () => {
        vi.useFakeTimers();
        showAppLinkLoader();
        await vi.advanceTimersByTimeAsync(30000);
        expect(loader()).toBeNull();
    });
});
