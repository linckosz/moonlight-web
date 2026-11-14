/*
 * MoonlightWeb — frontend TNR. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * A stream is started or ended only by the browser its session was paired with
 * (LaunchGate on the backend). A launch, a Stop and stop-session carry this
 * browser's signature over the host's single-use challenge and over what they
 * do; a stream's closing page sends the quit ticket it signed as the stream
 * started. A refusal ends the launch and asks for the PIN, never retried.
 */
import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import { BackendClient, isLaunchKeyRefusal } from '../js/api/BackendClient.js';
import { MoonlightApp } from '../js/app.js';
import { Host } from '../js/models/Host.js';
import { Toast } from '../js/ui/Toast.js';
import * as pairing from '../js/util/pairingCrypto.js';
import {
    LAUNCH_LABEL,
    QUIT_TICKET_LABEL,
    launchDigestInput,
    quitProofFields,
    quitTicketFields,
    startProofFields,
    stopProofFields,
} from '../js/util/pairingCrypto.js';

const hex = (bytes) => Array.from(bytes, (b) => b.toString(16).padStart(2, '0')).join('');
const NONCE = new Uint8Array([1, 2, 3]);
const refused = (code = 'key_refused') =>
    Object.assign(new Error('This browser is not the one this session was paired with'), {
        statusCode: 409,
        responseBody: { code },
    });

describe('the bytes a launch, a Stop and a ticket are signed over', () => {
    // The same vectors are pinned in backend/tests/test_launch_gate.cpp.
    it('match the host’s, byte for byte', () => {
        expect(
            hex(
                launchDigestInput(
                    LAUNCH_LABEL,
                    'H',
                    NONCE,
                    startProofFields('U', { appId: 7, standby: true, session_slot: 1 }),
                ),
            ),
        ).toBe(
            '000000114d572d42494e442d76317c6c61756e6368' +
                '0000000148' +
                '00000003010203' +
                '000000057374617274' +
                '0000000155' +
                '0000000137' +
                '0000000131' +
                '0000000131',
        );
        expect(hex(launchDigestInput(LAUNCH_LABEL, 'H', NONCE, quitProofFields('U', {})))).toBe(
            '000000114d572d42494e442d76317c6c61756e6368' +
                '0000000148' +
                '00000003010203' +
                '0000000471756974' +
                '0000000155' +
                '000000022d31',
        );
        expect(hex(launchDigestInput(LAUNCH_LABEL, 'H', NONCE, stopProofFields('U')))).toBe(
            '000000114d572d42494e442d76317c6c61756e6368' +
                '0000000148' +
                '00000003010203' +
                '0000000473746f70' +
                '0000000155',
        );
        expect(
            hex(launchDigestInput(QUIT_TICKET_LABEL, 'H', NONCE, quitTicketFields('U', 0, 's1'))),
        ).toBe(
            '0000000f4d572d42494e442d76317c71756974' +
                '0000000148' +
                '00000003010203' +
                '0000000155' +
                '0000000130' +
                '000000027331',
        );
    });

    it('read the body the way the host does', () => {
        // No slot is -1, a launch that is not a standby is 0, an id that is not
        // a whole number is 0: what QJsonValue makes of them.
        expect(startProofFields('U', { appId: 3 })).toEqual(['start', 'U', '3', '0', '-1']);
        expect(startProofFields('U', { appId: '3', standby: 'yes', session_slot: null })).toEqual([
            'start',
            'U',
            '0',
            '0',
            '-1',
        ]);
        expect(quitProofFields('U', { session_slot: 5 })).toEqual(['quit', 'U', '5']);
    });
});

describe('what this page sends', () => {
    let posts;

    const host = (answers = {}) => {
        posts = [];
        vi.spyOn(BackendClient, 'post').mockImplementation(async (path, body) => {
            posts.push({ path, body: structuredClone(body) });
            const route = path.split('/').pop();
            if (route in answers) {
                const a = answers[route];
                if (a instanceof Error) throw a;
                return a;
            }
            return { ok: true };
        });
    };
    const sent = (route) => posts.filter((p) => p.path.endsWith(`/${route}`));
    const CHALLENGE = { required: true, nonce: 'bm9uY2U=', host_id: 'host-id' };

    beforeEach(() => {
        BackendClient._quitTickets.clear();
        BackendClient.onLaunchKeyRefused = null;
        vi.spyOn(pairing, 'signLaunchProof').mockResolvedValue('c2ln');
        vi.spyOn(console, 'warn').mockImplementation(() => {});
    });
    afterEach(() => {
        BackendClient._quitTickets.clear();
        BackendClient.onLaunchKeyRefused = null;
        vi.restoreAllMocks();
    });

    it('signs a launch over what it asks for, and sends that', async () => {
        host({ 'launch-proof': CHALLENGE });
        await BackendClient.launchApp('h1', 42, { standby: true, session_slot: 1, bitrate: 9 });
        expect(posts[0].path).toBe('/api/hosts/h1/launch-proof');
        const [start] = sent('start');
        expect(start.body.launch_proof).toEqual({ nonce: 'bm9uY2U=', sig: 'c2ln' });
        expect(pairing.signLaunchProof).toHaveBeenCalledWith(LAUNCH_LABEL, 'host-id', 'bm9uY2U=', [
            'start',
            'h1',
            '42',
            '1',
            '1',
        ]);
        expect(start.body).toMatchObject({ appId: 42, standby: true, session_slot: 1, bitrate: 9 });
    });

    it('signs nothing where the host asks for nothing', async () => {
        host({ 'launch-proof': { required: false } });
        await BackendClient.launchApp('h1', 42, {});
        expect(pairing.signLaunchProof).not.toHaveBeenCalled();
        expect(sent('start')[0].body.launch_proof).toBeUndefined();
    });

    it('signs a Stop and stop-session each for its own route', async () => {
        host({ 'launch-proof': CHALLENGE });
        await BackendClient.quitApp('h1', { session_slot: 0 });
        expect(sent('quit')[0].body.launch_proof).toEqual({ nonce: 'bm9uY2U=', sig: 'c2ln' });
        expect(pairing.signLaunchProof).toHaveBeenLastCalledWith(
            LAUNCH_LABEL,
            'host-id',
            'bm9uY2U=',
            ['quit', 'h1', '0'],
        );
        await BackendClient.quitApp('h1');
        expect(pairing.signLaunchProof).toHaveBeenLastCalledWith(
            LAUNCH_LABEL,
            'host-id',
            'bm9uY2U=',
            ['quit', 'h1', '-1'],
        );
        await BackendClient.stopHostSession('h1');
        expect(sent('stop-session')[0].body.launch_proof).toEqual({
            nonce: 'bm9uY2U=',
            sig: 'c2ln',
        });
        expect(pairing.signLaunchProof).toHaveBeenLastCalledWith(
            LAUNCH_LABEL,
            'host-id',
            'bm9uY2U=',
            ['stop', 'h1'],
        );
    });

    it('signs a quit ticket for each stream once it runs, and its closing sends it', async () => {
        host({
            'launch-proof': CHALLENGE,
            start: { status: 'streaming', slot: 1, stream_id: 'stream-A' },
            'quit-ticket': { required: true, nonce: 'dGlja2V0', host_id: 'host-id' },
        });
        // Nothing to send yet: the bare Stop it always sent.
        expect(BackendClient.quitBeaconBodies('h1')).toEqual([{}]);
        await BackendClient.launchApp('h1', 42, { standby: true, session_slot: 1 });
        await vi.waitFor(() =>
            expect(BackendClient.quitBeaconBodies('h1')).toEqual([
                { session_slot: 1, quit_ticket: { nonce: 'dGlja2V0', sig: 'c2ln' } },
            ]),
        );
        // Asked only once the stream is up, for that one stream.
        expect(posts.map((p) => p.path.split('/').pop())).toEqual([
            'launch-proof',
            'start',
            'quit-ticket',
        ]);
        expect(sent('quit-ticket')[0].body).toEqual({ session_slot: 1, stream_id: 'stream-A' });
        expect(pairing.signLaunchProof).toHaveBeenLastCalledWith(
            QUIT_TICKET_LABEL,
            'host-id',
            'dGlja2V0',
            ['h1', '1', 'stream-A'],
        );
        // Another host's streams are not this one's to end.
        expect(BackendClient.quitBeaconBodies('h2')).toEqual([{}]);
        // A Stop of its own spends it.
        await BackendClient.quitApp('h1', { session_slot: 1 });
        expect(BackendClient.quitBeaconBodies('h1')).toEqual([{}]);
    });

    it('keeps no ticket a later launch on that slot replaced, and none it was refused', async () => {
        let release;
        const late = new Promise((resolve) => (release = resolve));
        host({
            'launch-proof': { required: false },
            start: { status: 'streaming', slot: 0, stream_id: 'stream-A' },
        });
        BackendClient.post.mockImplementation(async (path, body) => {
            posts.push({ path, body });
            if (path.endsWith('/start')) return { status: 'streaming', slot: 0, stream_id: body.n };
            if (path.endsWith('/quit-ticket')) {
                if (body.stream_id === 'old') await late;
                return { required: true, nonce: 'bm9uY2U=', host_id: 'host-id' };
            }
            return { required: false };
        });
        await BackendClient.launchApp('h1', 1, { n: 'old' });
        await BackendClient.launchApp('h1', 1, { n: 'new' });
        release();
        await vi.waitFor(() =>
            expect(BackendClient.quitBeaconBodies('h1')).toEqual([
                { session_slot: 0, quit_ticket: { nonce: 'bm9uY2U=', sig: 'c2ln' } },
            ]),
        );
        expect(BackendClient._quitTickets.get('h1|0').streamId).toBe('new');

        // The host keeps nothing for a page whose stream it would not ticket.
        BackendClient._quitTickets.clear();
        BackendClient.post.mockImplementation(async (path) => {
            if (path.endsWith('/start')) return { status: 'streaming', slot: 0, stream_id: 's' };
            if (path.endsWith('/quit-ticket')) throw refused('no_such_stream');
            return { required: false };
        });
        await BackendClient.launchApp('h1', 1, {});
        await new Promise((resolve) => setTimeout(resolve, 0));
        expect(BackendClient.quitBeaconBodies('h1')).toEqual([{}]);
    });

    it('says a refused Stop to the page, and still fails it', async () => {
        const told = vi.fn();
        BackendClient.onLaunchKeyRefused = told;
        host({ 'launch-proof': CHALLENGE, quit: refused(), 'stop-session': refused() });
        await expect(BackendClient.quitApp('h1', { session_slot: 0 })).rejects.toThrow();
        await expect(BackendClient.stopHostSession('h1')).rejects.toThrow();
        expect(told).toHaveBeenCalledTimes(2);
        // Never another key, never pairing-key again.
        expect(posts.some((p) => p.path.includes('pairing-key'))).toBe(false);
    });

    it('knows the host’s two refusals, and nothing else', () => {
        expect(isLaunchKeyRefusal(refused())).toBe(true);
        expect(isLaunchKeyRefusal(refused('launch_proof_required'))).toBe(true);
        expect(isLaunchKeyRefusal(refused('app_running'))).toBe(false);
        expect(isLaunchKeyRefusal(Object.assign(new Error('x'), { statusCode: 502 }))).toBe(false);
        expect(isLaunchKeyRefusal(null)).toBe(false);
    });
});

describe('a launch the host refused for this browser’s key', () => {
    let saved;
    let launch;
    let askPin;
    let toast;

    beforeEach(() => {
        saved = { ...MoonlightApp };
        document.body.innerHTML = '<div id="app"></div>';
        vi.spyOn(console, 'warn').mockImplementation(() => {});
        vi.spyOn(console, 'error').mockImplementation(() => {});
        vi.spyOn(console, 'log').mockImplementation(() => {});
        vi.spyOn(MoonlightApp, '_noteLiveQuality').mockImplementation(() => {});
        launch = vi.spyOn(BackendClient, 'launchApp').mockRejectedValue(refused());
        askPin = vi.spyOn(MoonlightApp, '_showKeyRefused').mockImplementation(() => {});
        toast = vi.spyOn(Toast, 'error').mockImplementation(() => {});
        vi.spyOn(MoonlightApp, '_onStreamingQuit').mockImplementation(() => {});
        MoonlightApp._lastStreamHost = { uuid: 'host-1' };
        MoonlightApp._lastStreamApp = { id: 7 };
        MoonlightApp._lastStreamingSettings = {};
        MoonlightApp._degradeOverrides = {};
        MoonlightApp._transportChain = ['webrtc-dc-udp', 'webrtc-dc-tcp', 'wss'];
        MoonlightApp._transportIndex = 0;
        MoonlightApp._relaunching = false;
        MoonlightApp._standbyView = null;
        MoonlightApp._startupAborted = false;
        MoonlightApp._launchRetried = false;
        MoonlightApp._firstTransportRetried = false;
        MoonlightApp._reconnectOf = null;
        MoonlightApp.hostListView = {
            confirmAppExists: vi.fn(async () => true),
            clearLaunching: vi.fn(),
            start: vi.fn(),
        };
        MoonlightApp._nav = { ...(MoonlightApp._nav || {}), overlay: 'streaming' };
    });
    afterEach(() => {
        vi.restoreAllMocks();
        Object.assign(MoonlightApp, saved);
        document.body.innerHTML = '';
    });

    const settle = () => new Promise((resolve) => setTimeout(resolve, 20));

    it('asks for the PIN on the first launch, once, and launches nothing else', async () => {
        const transition = vi.spyOn(MoonlightApp, 'transition').mockImplementation(() => {});
        vi.spyOn(MoonlightApp, '_enterLaunchFullscreen').mockImplementation(() => {});
        vi.spyOn(MoonlightApp, '_probeUdpCached').mockImplementation(async () => null);
        await MoonlightApp.launchApp(
            new Host({ uuid: 'host-1', name: 'Desk', state: 'online', pairState: 'paired' }),
            { id: 7, name: 'Desktop' },
            undefined,
            undefined,
            { skipSelfStreamWarn: true, runningChecked: true },
        );
        await settle();
        expect(launch).toHaveBeenCalledTimes(1);
        expect(askPin).toHaveBeenCalledTimes(1);
        expect(toast).not.toHaveBeenCalled();
        expect(transition).toHaveBeenLastCalledWith('app_list');
    });

    it('ends a relaunch on the first rung, never walking down the chain', async () => {
        MoonlightApp.streamView = {
            onQuit: () => {},
            captureViewState: () => null,
            captureLastFrameAsync: async () => null,
            quit: vi.fn(async () => {}),
            _firstFrameRendered: true,
        };
        await MoonlightApp._relaunchTransport(0);
        await settle();
        expect(launch).toHaveBeenCalledTimes(1);
        expect(askPin).toHaveBeenCalledTimes(1);
    });
});
