/*
 * MoonlightWeb — frontend TNR. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * A session of another browser's. An iOS home-screen app added from Chrome starts
 * with Chrome's cookie, and with a storage and so a pairing key of its own (seen
 * 05/11/2026). The host refuses to rebind the session and refuses every stream
 * from there, which is right: it cannot tell this from a stolen cookie. What was
 * wrong is the page: it showed the host list, and each launch then blamed the
 * host. It now asks for the PIN, and a refused key says what it is. No refusal may
 * ever walk down to wss, whichever side it came from.
 */

// Not localhost, where every browser has the admin page.
// @vitest-environment-options {"url": "https://192.168.1.66:48443/"}

import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { MoonlightApp } from '../js/app.js';
import { BackendClient } from '../js/api/BackendClient.js';
import { Toast } from '../js/ui/Toast.js';
import { t } from '../js/i18n/i18n.js';
import { WebRtcDataChannel } from '../js/api/WebRtcDataChannel.js';
import { WebRtcMedia } from '../js/api/WebRtcMedia.js';
import * as pairing from '../js/util/pairingCrypto.js';
import { IDENTITY_REFUSED, KEY_REFUSED, isBindRefusal } from '../js/util/pairingCrypto.js';

function refusalFromHost(statusCode) {
    const err = new Error('refused');
    err.statusCode = statusCode;
    return err;
}

describe('ensurePairingKey on a session already bound elsewhere', () => {
    const identity = { publicKeyBase64: 'a2V5', hostPublicKey: null };

    beforeEach(() => {
        vi.spyOn(pairing, 'loadOrCreateIdentity').mockResolvedValue(identity);
        vi.spyOn(console, 'warn').mockImplementation(() => {});
        vi.spyOn(console, 'log').mockImplementation(() => {});
    });
    afterEach(() => vi.restoreAllMocks());

    it('says so on a 409, the one answer that means another key', async () => {
        vi.spyOn(BackendClient, 'post').mockRejectedValue(refusalFromHost(409));
        const got = await BackendClient.ensurePairingKey();
        expect(got.sessionKeyRefused).toBe(true);
    });

    it.each([401, 403, 500])('stays silent on a %i', async (code) => {
        vi.spyOn(BackendClient, 'post').mockRejectedValue(refusalFromHost(code));
        const got = await BackendClient.ensurePairingKey();
        expect(got.sessionKeyRefused).toBeUndefined();
    });
});

describe('the boot of a page holding such a session', () => {
    let saved;
    const pinScreen = () => document.getElementById('login-pin-input');
    const note = () => document.getElementById('login-session-key-refused');

    beforeEach(() => {
        saved = {
            loginView: MoonlightApp.loginView,
            state: MoonlightApp.state,
            _hasAdminAccess: MoonlightApp._hasAdminAccess,
            _isRealHostMachine: MoonlightApp._isRealHostMachine,
            _adminUnlockAvailable: MoonlightApp._adminUnlockAvailable,
        };
        document.body.innerHTML = '<div id="main-content"></div>';
        vi.spyOn(console, 'warn').mockImplementation(() => {});
        vi.spyOn(console, 'log').mockImplementation(() => {});
    });
    afterEach(() => {
        MoonlightApp.loginView?.destroy?.();
        Object.assign(MoonlightApp, saved);
        vi.useRealTimers();
        vi.restoreAllMocks();
    });

    const signedIn = (extra = {}) =>
        vi
            .spyOn(BackendClient, 'getAuthStatus')
            .mockResolvedValue({ authenticated: true, is_localhost: false, ...extra });

    it('asks for the PIN, with the reason, and not for the host list', async () => {
        signedIn();
        vi.spyOn(BackendClient, 'ensurePairingKey').mockResolvedValue({ sessionKeyRefused: true });

        expect(await MoonlightApp._checkAuth()).toBe(false);
        await vi.waitFor(() => expect(pinScreen()).not.toBeNull());
        expect(note()).not.toBeNull();
    });

    it('goes on to the application once the PIN is right', async () => {
        signedIn();
        vi.spyOn(BackendClient, 'ensurePairingKey').mockResolvedValue({ sessionKeyRefused: true });
        const pin = vi.spyOn(BackendClient, 'validatePin').mockResolvedValue({ status: 'ok' });
        vi.spyOn(Toast, 'success').mockImplementation(() => {});
        const ready = vi.spyOn(MoonlightApp, '_initAsync').mockImplementation(() => {});
        vi.spyOn(MoonlightApp, '_initRouter').mockImplementation(() => {});
        vi.spyOn(MoonlightApp, '_initNavButtons').mockImplementation(() => {});

        await MoonlightApp._checkAuth();
        await vi.waitFor(() => expect(pinScreen()).not.toBeNull());
        pinScreen().value = '123456';
        pinScreen().dispatchEvent(new Event('input', { bubbles: true }));
        document.getElementById('btn-login-unlock').click();

        await vi.waitFor(() => expect(pin).toHaveBeenCalled());
        await vi.waitFor(() => expect(ready).toHaveBeenCalled());
    });

    it('never calls logout: that would end the other browser’s session', async () => {
        signedIn();
        vi.spyOn(BackendClient, 'ensurePairingKey').mockResolvedValue({ sessionKeyRefused: true });
        const logout = vi.spyOn(BackendClient, 'logout').mockResolvedValue({});

        await MoonlightApp._checkAuth();
        await vi.waitFor(() => expect(pinScreen()).not.toBeNull());
        expect(logout).not.toHaveBeenCalled();
    });

    it.each([
        ['the host machine, which has no PIN to ask for', { is_localhost: true }],
        ['a booth, which would refuse the PIN', { admin_locked: true }],
    ])('leaves %s to the application', async (_who, extra) => {
        signedIn(extra);
        vi.spyOn(BackendClient, 'ensurePairingKey').mockResolvedValue({ sessionKeyRefused: true });

        expect(await MoonlightApp._checkAuth()).toBe(true);
        expect(pinScreen()).toBeNull();
    });

    it('lets a browser paired as it should be straight through', async () => {
        signedIn();
        vi.spyOn(BackendClient, 'ensurePairingKey').mockResolvedValue({ hostPublicKey: 'a2V5' });

        expect(await MoonlightApp._checkAuth()).toBe(true);
        expect(pinScreen()).toBeNull();
    });

    it('does not hold up the application on a host slow to answer', async () => {
        signedIn();
        vi.spyOn(BackendClient, 'ensurePairingKey').mockReturnValue(new Promise(() => {}));
        vi.useFakeTimers();

        const boot = MoonlightApp._checkAuth();
        await vi.advanceTimersByTimeAsync(1500);
        expect(await boot).toBe(true);
    });

    it('survives the call failing outright', async () => {
        signedIn();
        vi.spyOn(BackendClient, 'ensurePairingKey').mockRejectedValue(new Error('no'));

        expect(await MoonlightApp._checkAuth()).toBe(true);
    });
});

describe('a key the host refuses, at launch', () => {
    it.each([
        ['WebRtcDataChannel', () => new WebRtcDataChannel('ws://test.invalid')],
        ['WebRtcMedia', () => new WebRtcMedia('ws://test.invalid')],
    ])('%s reports it apart from a host that failed to prove itself', async (_name, make) => {
        class FakeSocket {
            close() {}
        }
        vi.stubGlobal('WebSocket', FakeSocket);
        vi.spyOn(console, 'log').mockImplementation(() => {});
        try {
            const transport = make();
            const onError = vi.fn();
            transport.onError = onError;
            transport.connect();
            await vi.waitFor(() => expect(transport.signalingWs).toBeTruthy());

            transport.signalingWs.onclose({ code: 1008, reason: 'pairing verification failed' });

            expect(onError).toHaveBeenCalledTimes(1);
            expect(onError.mock.calls[0][0].message).toBe(KEY_REFUSED);
        } finally {
            vi.unstubAllGlobals();
            vi.restoreAllMocks();
        }
    });

    it('counts both refusals as the same final one', () => {
        expect(isBindRefusal(IDENTITY_REFUSED)).toBe(true);
        expect(isBindRefusal(KEY_REFUSED)).toBe(true);
        expect(isBindRefusal('launch_failed')).toBe(false);
        expect(isBindRefusal(undefined)).toBe(false);
    });

    it.each([IDENTITY_REFUSED, KEY_REFUSED])('%s leaves a guest no other rung', (reason) => {
        const chain = ['webrtc-dc-udp', 'webrtc-dc-tcp', 'wss'];
        expect(MoonlightApp._guestNextRung(reason, 0, chain)).toBeNull();
    });

    it('lets a guest walk on after any other failure, to the end of the chain', () => {
        const chain = ['webrtc-dc-udp', 'wss'];
        expect(MoonlightApp._guestNextRung('ice timeout', 0, chain)).toBe(1);
        expect(MoonlightApp._guestNextRung('ice timeout', 1, chain)).toBeNull();
    });

    describe('in the application', () => {
        let saved;
        const FIELDS = [
            '_nav',
            '_standbyView',
            '_standbyLaunching',
            'streamView',
            '_transportChain',
            '_transportIndex',
            '_firstTransportRetried',
            '_launchRetried',
            '_startupAborted',
            '_relaunching',
            '_climbBack',
            '_climbsOver',
            '_reconnectOf',
        ];

        beforeEach(() => {
            saved = Object.fromEntries(FIELDS.map((f) => [f, MoonlightApp[f]]));
            Object.assign(MoonlightApp, {
                _nav: { ...(MoonlightApp._nav || {}), overlay: 'streaming' },
                _standbyView: null,
                _standbyLaunching: false,
                streamView: { quit: vi.fn(async () => {}), streamId: 's1' },
                _transportChain: ['webrtc-dc-udp', 'webrtc-dc-tcp', 'wss'],
                _transportIndex: 0,
                _firstTransportRetried: false,
                _launchRetried: false,
                _startupAborted: false,
                _relaunching: false,
                _climbBack: null,
                _climbsOver: false,
            });
            vi.spyOn(console, 'error').mockImplementation(() => {});
            vi.spyOn(console, 'warn').mockImplementation(() => {});
        });
        afterEach(() => {
            Object.assign(MoonlightApp, saved);
            vi.restoreAllMocks();
        });

        it.each([
            [KEY_REFUSED, 'transport.keyRefused'],
            [IDENTITY_REFUSED, 'transport.identityRefused'],
        ])('%s ends the launch, says %s, and tries no other transport', async (reason, words) => {
            const relaunch = vi
                .spyOn(MoonlightApp, '_relaunchTransport')
                .mockImplementation(() => {});
            const ended = vi
                .spyOn(MoonlightApp, '_endAfterFailedLaunch')
                .mockImplementation(() => {});
            const toast = vi.spyOn(Toast, 'error').mockImplementation(() => {});

            await MoonlightApp._onTransportFailed(reason);

            expect(relaunch).not.toHaveBeenCalled();
            expect(ended).toHaveBeenCalledTimes(1);
            expect(toast).toHaveBeenCalledTimes(1);
            expect(toast.mock.calls[0][0]).toBe(t(words));
        });

        it('is not taken for a quality climb the host could not hold', () => {
            MoonlightApp._climbBack = { quality: {}, transportIndex: 0, autoForced: false };
            const relaunch = vi
                .spyOn(MoonlightApp, '_relaunchTransport')
                .mockImplementation(() => {});

            expect(MoonlightApp._judgeFailedClimb(KEY_REFUSED)).toBe(false);
            expect(relaunch).not.toHaveBeenCalled();
            expect(MoonlightApp._climbBack).toBeNull();
        });
    });
});

describe('the words', () => {
    // vitest runs from frontend/, and this environment's URL is not a file's.
    const locale = (lang) =>
        JSON.parse(readFileSync(join(process.cwd(), 'locales', `${lang}.json`), 'utf8'));

    it.each(['en', 'fr', 'zh'])('%s has both, and asks for a PIN in each', (lang) => {
        expect(locale(lang).login.sessionOtherBrowser).toMatch(/PIN/);
        expect(locale(lang).transport.keyRefused).toMatch(/PIN/);
    });

    it('are the French ones Bruno accepted', () => {
        expect(locale('fr').login.sessionOtherBrowser).toBe(
            "Cette session a été appairée avec un autre navigateur. Saisissez un PIN pour appairer celui-ci ; l'autre reste connecté.",
        );
        expect(locale('fr').transport.keyRefused).toBe(
            "Cette session a été appairée avec un autre navigateur, le stream n'a donc pas été lancé. Déconnectez-vous ici, puis appairez ce navigateur avec un PIN.",
        );
    });
});
