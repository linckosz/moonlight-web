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
import {
    HOST_KEY_MISSING,
    IDENTITY_REFUSED,
    KEY_REFUSED,
    isBindRefusal,
} from '../js/util/pairingCrypto.js';

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
        [
            'the host machine, which has no PIN to ask for',
            { is_localhost: true, is_host_machine: true },
        ],
        ['a booth, which would refuse the PIN', { admin_locked: true }],
    ])('leaves %s to the application', async (_who, extra) => {
        signedIn(extra);
        vi.spyOn(BackendClient, 'ensurePairingKey').mockResolvedValue({ sessionKeyRefused: true });

        expect(await MoonlightApp._checkAuth()).toBe(true);
        expect(pinScreen()).toBeNull();
    });

    describe('after the person asked for the PIN screen', () => {
        beforeEach(() => {
            sessionStorage.setItem('mw_pin_asked', '1');
            vi.spyOn(BackendClient, 'ensurePairingKey').mockResolvedValue({
                hostPublicKey: 'a2V5',
            });
        });
        afterEach(() => sessionStorage.clear());

        it('shows it, without saying whose session this is, and only once', async () => {
            signedIn();
            expect(await MoonlightApp._checkAuth()).toBe(false);
            await vi.waitFor(() => expect(pinScreen()).not.toBeNull());
            expect(note()).toBeNull();
            expect(sessionStorage.getItem('mw_pin_asked')).toBeNull();

            MoonlightApp.loginView?.destroy?.();
            document.getElementById('main-content').innerHTML = '';
            expect(await MoonlightApp._checkAuth()).toBe(true);
            expect(pinScreen()).toBeNull();
        });

        it('shows it to a LAN admin who asked, having their PIN by then', async () => {
            signedIn({ is_localhost: true, is_host_machine: false });
            expect(await MoonlightApp._checkAuth()).toBe(false);
            await vi.waitFor(() => expect(pinScreen()).not.toBeNull());
            expect(note()).toBeNull();
        });

        it.each([
            ['the host machine', { is_localhost: true, is_host_machine: true }],
            ['a booth', { admin_locked: true }],
        ])('leaves %s as it was, and forgets the request', async (_who, extra) => {
            signedIn(extra);
            expect(await MoonlightApp._checkAuth()).toBe(true);
            expect(pinScreen()).toBeNull();
            expect(sessionStorage.getItem('mw_pin_asked')).toBeNull();
        });
    });

    it('never locks a LAN admin out of the page that generates the PIN', async () => {
        // is_localhost means "has admin access" since the remote admin password.
        signedIn({ is_localhost: true, is_host_machine: false });
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

    it('counts every refusal as the same final one', () => {
        expect(isBindRefusal(IDENTITY_REFUSED)).toBe(true);
        expect(isBindRefusal(KEY_REFUSED)).toBe(true);
        expect(isBindRefusal(HOST_KEY_MISSING)).toBe(true);
        expect(isBindRefusal('launch_failed')).toBe(false);
        expect(isBindRefusal(undefined)).toBe(false);
    });

    it.each([IDENTITY_REFUSED, KEY_REFUSED, HOST_KEY_MISSING])(
        '%s leaves a guest no other rung',
        (reason) => {
            const chain = ['webrtc-dc-udp', 'webrtc-dc-tcp', 'wss'];
            expect(MoonlightApp._guestNextRung(reason, 0, chain)).toBeNull();
        },
    );

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
            '_hasAdminAccess',
            '_isRealHostMachine',
            '_keyRefusedToast',
            '_hostKeyAsked',
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
                _hasAdminAccess: false,
                _isRealHostMachine: false,
                _keyRefusedToast: null,
                // The host's key already asked for: the refusal goes straight to
                // its words. The asking itself has tests of its own below.
                _hostKeyAsked: true,
            });
            sessionStorage.clear();
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
            [HOST_KEY_MISSING, 'transport.hostKeyMissing'],
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
            MoonlightApp._keyRefusedToast = null;
        });

        const refusedToast = () => {
            vi.spyOn(MoonlightApp, '_relaunchTransport').mockImplementation(() => {});
            vi.spyOn(MoonlightApp, '_endAfterFailedLaunch').mockImplementation(() => {});
            return vi.spyOn(Toast, 'error').mockImplementation(() => ({ dismiss: vi.fn() }));
        };

        it('offers the PIN screen, stays until read, and never logs out', () => {
            const toast = refusedToast();
            const logout = vi.spyOn(BackendClient, 'logout').mockResolvedValue({});
            const reload = vi.spyOn(MoonlightApp, '_reloadPage').mockImplementation(() => {});

            MoonlightApp._onTransportFailed(KEY_REFUSED);

            const options = toast.mock.calls[0][1];
            expect(options.sticky).toBe(true);
            expect(options.action.label).toBe(t('transport.enterPin'));
            options.action.onClick();
            expect(sessionStorage.getItem('mw_pin_asked')).toBe('1');
            expect(reload).toHaveBeenCalledTimes(1);
            expect(logout).not.toHaveBeenCalled();
        });

        it('offers the PIN screen too when this browser holds no key of the host', () => {
            const toast = refusedToast();
            MoonlightApp._onTransportFailed(HOST_KEY_MISSING);
            const options = toast.mock.calls[0][1];
            expect(toast.mock.calls[0][0]).toBe(t('transport.hostKeyMissing'));
            expect(options.sticky).toBe(true);
            expect(options.action.label).toBe(t('transport.enterPin'));
        });

        describe('without the host key', () => {
            const launchSpies = () => ({
                relaunch: vi.spyOn(MoonlightApp, '_relaunchTransport').mockImplementation(() => {}),
                ended: vi.spyOn(MoonlightApp, '_endAfterFailedLaunch').mockImplementation(() => {}),
                toast: vi.spyOn(Toast, 'error').mockImplementation(() => ({ dismiss: vi.fn() })),
            });

            it('asks the host for its key once, and starts the same transport again', async () => {
                MoonlightApp._hostKeyAsked = false;
                MoonlightApp._transportIndex = 1;
                const { relaunch, ended, toast } = launchSpies();
                const ask = vi
                    .spyOn(BackendClient, 'ensurePairingKey')
                    .mockResolvedValue({ hostPublicKey: 'AAAA' });

                MoonlightApp._onTransportFailed(HOST_KEY_MISSING);
                await vi.waitFor(() => expect(relaunch).toHaveBeenCalledTimes(1));

                expect(ask).toHaveBeenCalledTimes(1);
                expect(relaunch).toHaveBeenCalledWith(1);
                expect(ended).not.toHaveBeenCalled();
                expect(toast).not.toHaveBeenCalled();
            });

            it('offers the PIN when the host gives no key, and asks only once', async () => {
                MoonlightApp._hostKeyAsked = false;
                const { relaunch, ended, toast } = launchSpies();
                const ask = vi
                    .spyOn(BackendClient, 'ensurePairingKey')
                    .mockResolvedValue({ hostPublicKey: null });

                MoonlightApp._onTransportFailed(HOST_KEY_MISSING);
                await vi.waitFor(() => expect(ended).toHaveBeenCalledTimes(1));
                MoonlightApp._onTransportFailed(HOST_KEY_MISSING);

                expect(ask).toHaveBeenCalledTimes(1);
                expect(relaunch).not.toHaveBeenCalled();
                expect(toast.mock.calls[0][0]).toBe(t('transport.hostKeyMissing'));
                expect(toast.mock.calls[0][1].action.label).toBe(t('transport.enterPin'));
            });

            it('offers the PIN when asking fails outright', async () => {
                MoonlightApp._hostKeyAsked = false;
                const { ended, toast } = launchSpies();
                vi.spyOn(BackendClient, 'ensurePairingKey').mockRejectedValue(new Error('down'));

                MoonlightApp._onTransportFailed(HOST_KEY_MISSING);
                await vi.waitFor(() => expect(ended).toHaveBeenCalledTimes(1));

                expect(toast.mock.calls[0][0]).toBe(t('transport.hostKeyMissing'));
            });

            it('does nothing once the stream was left while the host answered', async () => {
                MoonlightApp._hostKeyAsked = false;
                const { relaunch, ended } = launchSpies();
                let answer;
                vi.spyOn(BackendClient, 'ensurePairingKey').mockReturnValue(
                    new Promise((r) => (answer = r)),
                );

                MoonlightApp._onTransportFailed(HOST_KEY_MISSING);
                MoonlightApp._nav = { ...MoonlightApp._nav, overlay: null };
                answer({ hostPublicKey: 'AAAA' });
                await new Promise((r) => setTimeout(r, 0));

                expect(relaunch).not.toHaveBeenCalled();
                expect(ended).not.toHaveBeenCalled();
            });
        });

        it.each([
            [true, true],
            [false, false],
        ])(
            'on this LAN page, takes an unchecked offer when is_localhost is %s: %s',
            (isLocalhost, may) => {
                MoonlightApp._readAccess({ is_localhost: isLocalhost });
                expect(pairing.mayTakeUncheckedOffer()).toBe(may);
                pairing.noteLocalPrivilege(false);
            },
        );

        it('offers no PIN for a host that failed to prove itself', () => {
            const toast = refusedToast();
            MoonlightApp._onTransportFailed(IDENTITY_REFUSED);
            expect(toast.mock.calls[0][1]).toBeUndefined();
        });

        it.each([
            ['the host machine', () => (MoonlightApp._isRealHostMachine = true)],
            ['a booth', () => (BackendClient.adminLocked = true)],
        ])('offers no PIN on %s, which has none to ask for', (_who, arrange) => {
            const wasLocked = BackendClient.adminLocked;
            arrange();
            const toast = refusedToast();
            MoonlightApp._onTransportFailed(KEY_REFUSED);
            BackendClient.adminLocked = wasLocked;
            expect(toast.mock.calls[0][1].action).toBeUndefined();
        });

        it('tells the host machine to come back by the icon, with no PIN to offer', () => {
            MoonlightApp._isRealHostMachine = true;
            const toast = refusedToast();
            MoonlightApp._onTransportFailed(HOST_KEY_MISSING);
            expect(toast.mock.calls[0][0]).toBe(t('transport.hostKeyMissingOnHost'));
            expect(toast.mock.calls[0][1].action).toBeUndefined();
        });

        it('keeps the PIN words on a page that only guesses it is the host machine', () => {
            // localhost in the address bar is not the host's word.
            vi.spyOn(MoonlightApp, '_isHostMachine').mockReturnValue(true);
            const toast = refusedToast();
            MoonlightApp._onTransportFailed(HOST_KEY_MISSING);
            expect(toast.mock.calls[0][0]).toBe(t('transport.hostKeyMissing'));
        });

        it('offers the PIN on a LAN machine that merely has admin access', () => {
            MoonlightApp._hasAdminAccess = true;
            const toast = refusedToast();
            MoonlightApp._onTransportFailed(KEY_REFUSED);
            expect(toast.mock.calls[0][1].action).toBeDefined();
        });

        it('keeps one message however many launches are refused', () => {
            const dismiss = vi.fn();
            refusedToast().mockImplementation(() => ({ dismiss }));
            MoonlightApp._onTransportFailed(KEY_REFUSED);
            MoonlightApp._onTransportFailed(KEY_REFUSED);
            expect(dismiss).toHaveBeenCalledTimes(1);
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
        expect(locale(lang).transport.hostKeyMissing).toMatch(/PIN/);
        expect(locale(lang).transport.enterPin).toMatch(/PIN/);
    });

    it.each(['en', 'fr', 'zh'])('%s sends the host machine to the icon, not to a PIN', (lang) => {
        expect(locale(lang).transport.hostKeyMissingOnHost).toMatch(/MoonlightWeb/);
        expect(locale(lang).transport.hostKeyMissingOnHost).not.toMatch(/PIN/);
    });

    it('are the French ones Bruno accepted', () => {
        expect(locale('fr').login.sessionOtherBrowser).toBe(
            "Cette session a été appairée avec un autre navigateur. Saisissez un PIN pour appairer celui-ci ; l'autre reste connecté.",
        );
        expect(locale('fr').transport.enterPin).toBe('Saisir un PIN');
        expect(locale('fr').transport.hostKeyMissingOnHost).toBe(
            "Le stream n'a pas été lancé : cette page n'a pas pu vérifier l'hôte. Rouvrez MoonlightWeb depuis son icône sur ce PC, puis relancez le stream.",
        );
        expect(locale('fr').transport.keyRefused).toBe(
            "Cet hôte a refusé ce navigateur, le stream n'a donc pas été lancé. Appairez-le à nouveau avec un PIN.",
        );
    });
});
