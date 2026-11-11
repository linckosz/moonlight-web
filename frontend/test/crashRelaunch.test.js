/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
import { MoonlightApp } from '../js/app.js';
import { StreamView } from '../js/ui/StreamView.js';
import { BackendClient } from '../js/api/BackendClient.js';
import { Toast } from '../js/ui/Toast.js';
import { t } from '../js/i18n/i18n.js';
import {
    CRASH_RELAUNCH_MAX,
    CRASH_RELAUNCH_WINDOW_MS,
    LINK_STALL_MS,
    LINK_STALL_PROBE_MS,
    LIVE_RECHECK_MS,
    recordCrashRelaunch,
    settledStreamState,
    shouldRelaunchAfterCrash,
} from '../js/stream/CrashRelaunch.js';

/**
 * A stream survives the death of its --stream-worker.
 *
 * N95 bench, 21/10/2026: the worker was killed in the middle of a stream; the
 * page showed two red banners and went back to the host page, and the /quit it
 * sent on the way out took the guests and the host's app with it. Now the page
 * asks the host how the stream ended, and only a crash starts it again — once
 * or twice, behind a "Reconnecting…" veil, with no /quit. Everything else ends
 * exactly as before.
 */

const MIN = 60 * 1000;
const LOCALES = join(dirname(fileURLToPath(import.meta.url)), '..', 'locales');

describe('CrashRelaunch: whether a lost stream starts again', () => {
    it('starts again only after a crash', () => {
        expect(shouldRelaunchAfterCrash('crashed', [], 0)).toBe(true);
        // Or after a sign-in through a Windows PC's sign-in screen: the same
        // card then streams the PC's screen.
        expect(shouldRelaunchAfterCrash('signed_in', [], 0)).toBe(true);
        for (const state of ['live', 'ended', 'stopped', 'unknown', '', undefined])
            expect(shouldRelaunchAfterCrash(state, [], 0), String(state)).toBe(false);
    });

    it('twice in five minutes, then the screen it always had', () => {
        let history = [];
        const now = 10 * MIN;
        for (let i = 0; i < CRASH_RELAUNCH_MAX; i++) {
            expect(shouldRelaunchAfterCrash('crashed', history, now + i)).toBe(true);
            history = recordCrashRelaunch(history, now + i);
        }
        expect(shouldRelaunchAfterCrash('crashed', history, now + 2 * MIN)).toBe(false);
        // The oldest restart leaves the window: one more is allowed.
        expect(shouldRelaunchAfterCrash('crashed', history, now + CRASH_RELAUNCH_WINDOW_MS)).toBe(
            true,
        );
    });

    it('asks again once when the host still says live', async () => {
        vi.useFakeTimers();
        try {
            const ask = vi.fn().mockResolvedValueOnce('live').mockResolvedValueOnce('crashed');
            const answer = settledStreamState(ask);
            await vi.advanceTimersByTimeAsync(LIVE_RECHECK_MS);
            expect(await answer).toBe('crashed');
            expect(ask).toHaveBeenCalledTimes(2);
            const once = vi.fn().mockResolvedValue('stopped');
            expect(await settledStreamState(once)).toBe('stopped');
            expect(once).toHaveBeenCalledTimes(1);
        } finally {
            vi.useRealTimers();
        }
    });

    it('keeps only the restarts still in the window', () => {
        expect(recordCrashRelaunch([0, 2 * MIN], 6 * MIN)).toEqual([2 * MIN, 6 * MIN]);
        expect(recordCrashRelaunch(undefined, 5)).toEqual([5]);
    });
});

describe('BackendClient: how the host says a stream ended', () => {
    afterEach(() => vi.unstubAllGlobals());

    it("asks the owner's route and reads the state", async () => {
        const fetchMock = vi.fn(async () => ({
            ok: true,
            json: async () => ({ state: 'crashed' }),
        }));
        vi.stubGlobal('fetch', fetchMock);
        expect(await BackendClient.streamState('host-1', 'a b')).toBe('crashed');
        expect(fetchMock.mock.calls[0][0]).toBe('/api/hosts/host-1/stream-state?stream=a%20b');
    });

    it("asks the guest's route with the guest's cookie", async () => {
        const fetchMock = vi.fn(async () => ({ ok: true, json: async () => ({ state: 'live' }) }));
        vi.stubGlobal('fetch', fetchMock);
        expect(await BackendClient.playerStreamState('s1')).toBe('live');
        expect(fetchMock.mock.calls[0][0]).toBe('/api/share/player/stream-state?stream=s1');
    });

    it('reads anything else as unknown, which ends the stream as before', async () => {
        vi.stubGlobal(
            'fetch',
            vi.fn(async () => ({ ok: false, json: async () => ({}) })),
        );
        expect(await BackendClient.streamState('h', 's')).toBe('unknown');
        vi.stubGlobal(
            'fetch',
            vi.fn(async () => ({ ok: true, json: async () => ({}) })),
        );
        expect(await BackendClient.streamState('h', 's')).toBe('unknown');
        vi.stubGlobal(
            'fetch',
            vi.fn(async () => {
                throw new Error('aborted');
            }),
        );
        expect(await BackendClient.streamState('h', 's')).toBe('unknown');
    });

    it('names the lost stream when a guest rejoins', async () => {
        const post = vi.spyOn(BackendClient, 'post').mockResolvedValue({});
        await BackendClient.playerJoin('tok', 1080, undefined, 1, undefined, false, 's9');
        expect(post.mock.calls[0][1]).toMatchObject({ token: 'tok', reconnect_of: 's9' });
        await BackendClient.playerJoin('tok', 1080);
        expect(post.mock.calls[1][1].reconnect_of).toBe(undefined);
        post.mockRestore();
    });
});

/** A StreamView reduced to its transport handlers (setupWebRtc). */
function linkView(overrides = {}) {
    const sv = {
        webrtc: { connect: vi.fn() },
        _transport: 'webrtc-media',
        _nativeAudio: true,
        _everConnected: true,
        _quitting: false,
        _manualQuitting: false,
        _takenOver: false,
        _standby: false,
        connected: true,
        onConnectionFailed: null,
        onLinkLost: vi.fn(),
        setStatus: vi.fn(),
        quit: vi.fn(async () => {}),
        _reportConnectionFailed: StreamView.prototype._reportConnectionFailed,
        _handOffLinkLoss: StreamView.prototype._handOffLinkLoss,
        _linkLostDefault: StreamView.prototype._linkLostDefault,
        ...overrides,
    };
    StreamView.prototype.setupWebRtc.call(sv);
    return sv;
}

/** What a transport does when its link goes: onError, then onClose. */
function loseLink(sv) {
    sv.webrtc.onError(new Error('ICE disconnected'));
    sv.webrtc.onClose();
}

describe('StreamView: a link that goes once connected', () => {
    let toast;
    beforeEach(() => {
        vi.useFakeTimers();
        toast = vi.spyOn(Toast, 'error').mockImplementation(() => {});
        vi.spyOn(console, 'error').mockImplementation(() => {});
        vi.spyOn(console, 'warn').mockImplementation(() => {});
    });
    afterEach(() => {
        vi.useRealTimers();
        vi.restoreAllMocks();
    });

    it('is handed to the app once, and nothing shows yet', () => {
        const sv = linkView();
        loseLink(sv);
        vi.runAllTimers();
        expect(sv.onLinkLost).toHaveBeenCalledTimes(1);
        expect(toast).not.toHaveBeenCalled();
        expect(sv.quit).not.toHaveBeenCalled();
        expect(sv.connected).toBe(false);
    });

    it('ends as it always did when the app says so: both banners, then quit', () => {
        const sv = linkView();
        loseLink(sv);
        vi.runAllTimers();
        sv._linkLostDefault();
        expect(toast.mock.calls.map((c) => c[0])).toEqual([
            t('stream.webrtcError'),
            t('stream.streamDisconnected'),
        ]);
        vi.advanceTimersByTime(3000);
        expect(sv.quit).toHaveBeenCalledTimes(1);
    });

    it('ends at once, as before, when nobody takes it', () => {
        const sv = linkView({ onLinkLost: null });
        loseLink(sv);
        expect(toast).toHaveBeenCalledTimes(2);
        vi.advanceTimersByTime(3000);
        expect(sv.quit).toHaveBeenCalledTimes(1);
    });

    it('is not handed over after Stop, a take-over or a notice of the host', () => {
        for (const flags of [
            { _manualQuitting: true },
            { _quitting: true },
            { _takenOver: true },
        ]) {
            const sv = linkView(flags);
            loseLink(sv);
            vi.runAllTimers();
            expect(sv.onLinkLost, JSON.stringify(flags)).not.toHaveBeenCalled();
        }
    });

    it('stays out of the way of Stop pressed while the host is asked', () => {
        const sv = linkView();
        loseLink(sv);
        vi.runAllTimers();
        sv._manualQuitting = true;
        sv._linkLostDefault();
        expect(toast).not.toHaveBeenCalled();
        vi.advanceTimersByTime(3000);
        expect(sv.quit).not.toHaveBeenCalled();
    });

    it("leaves a standby leg's failure to the standby path", () => {
        const failed = vi.fn();
        const sv = linkView({ _standby: true, onConnectionFailed: failed });
        loseLink(sv);
        vi.runAllTimers();
        expect(sv.onLinkLost).not.toHaveBeenCalled();
        expect(failed).toHaveBeenCalledTimes(1);
    });

    it('leaves a link that never connected to the transport chain', () => {
        const failed = vi.fn();
        const sv = linkView({ _everConnected: false, onConnectionFailed: failed });
        loseLink(sv);
        vi.runAllTimers();
        expect(sv.onLinkLost).not.toHaveBeenCalled();
        expect(failed).toHaveBeenCalledTimes(1);
    });
});

/** The live view as _onLiveLinkLost sees it. */
function liveView(overrides = {}) {
    return {
        streamId: 'stream-1',
        _quitting: false,
        _manualQuitting: false,
        _takenOver: false,
        _linkLostDefault: vi.fn(),
        ...overrides,
    };
}

let saved;

function saveApp() {
    saved = {
        streamView: MoonlightApp.streamView,
        _lastStreamHost: MoonlightApp._lastStreamHost,
        _lastStreamApp: MoonlightApp._lastStreamApp,
        _transportIndex: MoonlightApp._transportIndex,
        _transportChain: MoonlightApp._transportChain,
        _crashRelaunches: MoonlightApp._crashRelaunches,
        _reconnectOf: MoonlightApp._reconnectOf,
        _relaunching: MoonlightApp._relaunching,
        _standbyView: MoonlightApp._standbyView,
        _standbyResult: MoonlightApp._standbyResult,
        _standbyTimer: MoonlightApp._standbyTimer,
        _dualSupported: MoonlightApp._dualSupported,
        _liveQuality: MoonlightApp._liveQuality,
        _lastStreamingSettings: MoonlightApp._lastStreamingSettings,
        _degradeOverrides: MoonlightApp._degradeOverrides,
        _launchRetried: MoonlightApp._launchRetried,
        _carryViewState: MoonlightApp._carryViewState,
        _startupAborted: MoonlightApp._startupAborted,
        _nav: MoonlightApp._nav,
    };
}

describe('MoonlightApp: the host says how the stream ended', () => {
    let relaunch;
    beforeEach(() => {
        saveApp();
        vi.spyOn(console, 'warn').mockImplementation(() => {});
        relaunch = vi.spyOn(MoonlightApp, '_relaunchTransport').mockImplementation(() => {});
        MoonlightApp._lastStreamHost = { uuid: 'host-1' };
        MoonlightApp._transportIndex = 2;
        MoonlightApp._crashRelaunches = [];
        MoonlightApp._reconnectOf = null;
        MoonlightApp._relaunching = false;
    });
    afterEach(() => {
        vi.restoreAllMocks();
        Object.assign(MoonlightApp, saved);
    });

    it('a crash starts the stream again, on the transport that worked', async () => {
        const state = vi.spyOn(BackendClient, 'streamState').mockResolvedValue('crashed');
        const sv = liveView();
        MoonlightApp.streamView = sv;
        await MoonlightApp._onLiveLinkLost(sv);
        expect(state).toHaveBeenCalledWith('host-1', 'stream-1');
        expect(relaunch).toHaveBeenCalledWith(2);
        expect(MoonlightApp._reconnectOf).toBe('stream-1');
        expect(sv._linkLostDefault).not.toHaveBeenCalled();
    });

    it('anything but a crash ends as it always did', async () => {
        vi.useFakeTimers();
        try {
            for (const answer of ['live', 'ended', 'stopped', 'unknown']) {
                vi.spyOn(BackendClient, 'streamState').mockResolvedValue(answer);
                const sv = liveView();
                MoonlightApp.streamView = sv;
                const done = MoonlightApp._onLiveLinkLost(sv);
                await vi.advanceTimersByTimeAsync(LIVE_RECHECK_MS);
                await done;
                expect(sv._linkLostDefault, answer).toHaveBeenCalledTimes(1);
            }
            expect(relaunch).not.toHaveBeenCalled();
        } finally {
            vi.useRealTimers();
        }
    });

    it('a crash the host had not seen yet is caught by the second question', async () => {
        vi.useFakeTimers();
        try {
            vi.spyOn(BackendClient, 'streamState')
                .mockResolvedValueOnce('live')
                .mockResolvedValueOnce('crashed');
            const sv = liveView();
            MoonlightApp.streamView = sv;
            const done = MoonlightApp._onLiveLinkLost(sv);
            await vi.advanceTimersByTimeAsync(LIVE_RECHECK_MS);
            await done;
            expect(relaunch).toHaveBeenCalledWith(2);
            expect(sv._linkLostDefault).not.toHaveBeenCalled();
        } finally {
            vi.useRealTimers();
        }
    });

    it('a stream with no id is not asked about', async () => {
        const state = vi.spyOn(BackendClient, 'streamState');
        const sv = liveView({ streamId: '' });
        MoonlightApp.streamView = sv;
        await MoonlightApp._onLiveLinkLost(sv);
        expect(state).not.toHaveBeenCalled();
        expect(sv._linkLostDefault).toHaveBeenCalledTimes(1);
    });

    it('a third crash in five minutes gets the screen it always had', async () => {
        vi.spyOn(BackendClient, 'streamState').mockResolvedValue('crashed');
        for (let i = 0; i < 3; i++) {
            const sv = liveView();
            MoonlightApp.streamView = sv;
            await MoonlightApp._onLiveLinkLost(sv);
            expect(sv._linkLostDefault).toHaveBeenCalledTimes(i < 2 ? 0 : 1);
        }
        expect(relaunch).toHaveBeenCalledTimes(2);
    });

    it('Stop, a notice or a newer view during the question ends nothing more', async () => {
        vi.spyOn(BackendClient, 'streamState').mockResolvedValue('crashed');
        for (const flags of [
            { _manualQuitting: true },
            { _quitting: true },
            { _takenOver: true },
        ]) {
            const sv = liveView(flags);
            MoonlightApp.streamView = sv;
            await MoonlightApp._onLiveLinkLost(sv);
            expect(sv._linkLostDefault).not.toHaveBeenCalled();
        }
        const old = liveView();
        MoonlightApp.streamView = liveView();
        await MoonlightApp._onLiveLinkLost(old);
        expect(old._linkLostDefault).not.toHaveBeenCalled();
        expect(relaunch).not.toHaveBeenCalled();
    });
});

const NOW = 1e6;

/** A StreamView reduced to what the stall watch reads. */
function stallView(overrides = {}) {
    return {
        _firstFrameRendered: true,
        _standby: false,
        _quitting: false,
        _manualQuitting: false,
        _takenOver: false,
        _linkLossHandedOff: false,
        // A page well into its life, as when a stream runs.
        _lastRxAt: NOW,
        _lastStallProbeAt: -Infinity,
        onLinkStall: vi.fn(),
        _checkLinkStall: StreamView.prototype._checkLinkStall,
        ...overrides,
    };
}

describe('StreamView: a link gone quiet', () => {
    it('asks once nothing has come for a moment, and not before', () => {
        const sv = stallView();
        sv._checkLinkStall(NOW);
        expect(sv.onLinkStall).not.toHaveBeenCalled();
        sv._lastRxAt = NOW - LINK_STALL_MS - 10;
        sv._checkLinkStall(NOW);
        expect(sv.onLinkStall).toHaveBeenCalledTimes(1);
    });

    it('asks again only after a while, through a stall that lasts', () => {
        const sv = stallView({ _lastRxAt: NOW - 10 * LINK_STALL_MS });
        sv._checkLinkStall(NOW);
        sv._checkLinkStall(NOW);
        expect(sv.onLinkStall).toHaveBeenCalledTimes(1);
        sv._lastStallProbeAt = NOW - LINK_STALL_PROBE_MS - 10;
        sv._checkLinkStall(NOW);
        expect(sv.onLinkStall).toHaveBeenCalledTimes(2);
    });

    it('stays quiet before the first frame, on a standby, and once the ending is under way', () => {
        const quiet = NOW - 10 * LINK_STALL_MS;
        for (const flags of [
            { _firstFrameRendered: false },
            { _standby: true },
            { _quitting: true },
            { _manualQuitting: true },
            { _takenOver: true },
            { _linkLossHandedOff: true },
            { _lastRxAt: 0 },
            { onLinkStall: null },
        ]) {
            const sv = stallView({ _lastRxAt: quiet, ...flags });
            const probe = sv.onLinkStall;
            sv._checkLinkStall(NOW);
            if (probe) expect(probe, JSON.stringify(flags)).not.toHaveBeenCalled();
        }
    });
});

describe('MoonlightApp: a quiet link and the host', () => {
    beforeEach(() => {
        saveApp();
        vi.spyOn(console, 'warn').mockImplementation(() => {});
        MoonlightApp._lastStreamHost = { uuid: 'host-1' };
    });
    afterEach(() => {
        vi.restoreAllMocks();
        Object.assign(MoonlightApp, saved);
    });

    it('a crash heard early ends the wait for ICE', async () => {
        const state = vi.spyOn(BackendClient, 'streamState').mockResolvedValue('crashed');
        const sv = { streamId: 'stream-1', _handOffLinkLoss: vi.fn() };
        MoonlightApp.streamView = sv;
        await MoonlightApp._onLiveLinkStall(sv);
        expect(state).toHaveBeenCalledWith('host-1', 'stream-1');
        expect(sv._handOffLinkLoss).toHaveBeenCalledTimes(1);
    });

    it('a link that is merely slow carries on', async () => {
        for (const answer of ['live', 'ended', 'stopped', 'unknown']) {
            vi.spyOn(BackendClient, 'streamState').mockResolvedValue(answer);
            const sv = { streamId: 'stream-1', _handOffLinkLoss: vi.fn() };
            MoonlightApp.streamView = sv;
            await MoonlightApp._onLiveLinkStall(sv);
            expect(sv._handOffLinkLoss, answer).not.toHaveBeenCalled();
        }
    });

    it('asks nothing without a stream id, and nothing for a view already replaced', async () => {
        const state = vi.spyOn(BackendClient, 'streamState').mockResolvedValue('crashed');
        const noId = { streamId: '', _handOffLinkLoss: vi.fn() };
        MoonlightApp.streamView = noId;
        await MoonlightApp._onLiveLinkStall(noId);
        expect(state).not.toHaveBeenCalled();
        const old = { streamId: 'stream-1', _handOffLinkLoss: vi.fn() };
        MoonlightApp.streamView = { streamId: 'stream-2' };
        await MoonlightApp._onLiveLinkStall(old);
        expect(old._handOffLinkLoss).not.toHaveBeenCalled();
    });
});

describe('MoonlightApp: the restart itself', () => {
    beforeEach(() => {
        saveApp();
        document.body.innerHTML = '<div id="app"></div>';
        vi.spyOn(console, 'warn').mockImplementation(() => {});
        vi.spyOn(console, 'log').mockImplementation(() => {});
        vi.spyOn(MoonlightApp, '_noteLiveQuality').mockImplementation(() => {});
        MoonlightApp._lastStreamHost = { uuid: 'host-1' };
        MoonlightApp._lastStreamApp = { id: 7 };
        MoonlightApp._lastStreamingSettings = { stream_height: 1080 };
        MoonlightApp._degradeOverrides = {};
        MoonlightApp._transportChain = ['webrtc-dc-udp', 'webrtc-dc-tcp'];
        MoonlightApp._transportIndex = 0;
        MoonlightApp._relaunching = false;
        MoonlightApp._standbyView = null;
        MoonlightApp._startupAborted = false;
        MoonlightApp._nav = { ...(MoonlightApp._nav || {}), overlay: 'streaming' };
    });
    afterEach(() => {
        vi.restoreAllMocks();
        Object.assign(MoonlightApp, saved);
        document.body.innerHTML = '';
    });

    function outgoing() {
        return {
            onQuit: () => {},
            captureViewState: () => null,
            captureLastFrameAsync: async () => null,
            quit: vi.fn(async () => {}),
        };
    }

    it('closes the lost view here, names it to the host, and veils the wait', async () => {
        const launch = vi
            .spyOn(BackendClient, 'launchApp')
            .mockResolvedValue({ status: 'streaming', transport_index: 0 });
        let veilAtLaunch = null;
        launch.mockImplementation(async () => {
            veilAtLaunch = document.querySelector('#stream-relaunch-loader');
            return { status: 'streaming', transport_index: 0 };
        });
        const next = { restoreViewState: vi.fn(), onFirstFrame: null };
        vi.spyOn(MoonlightApp, '_startStreamView').mockImplementation(() => {
            MoonlightApp.streamView = next;
        });
        const sv = outgoing();
        MoonlightApp.streamView = sv;
        MoonlightApp._reconnectOf = 'stream-1';

        await MoonlightApp._relaunchTransport(0);

        // No /quit: the worker is gone, and a /quit would end the guests.
        expect(sv.quit).toHaveBeenCalledWith({ silent: true, retire: true, takenOver: true });
        expect(launch.mock.calls[0][2]).toMatchObject({
            transport_index: 0,
            reconnect_of: 'stream-1',
        });
        // The veil: its text, centred, over the frame (none kept here).
        expect(veilAtLaunch.classList.contains('is-reconnecting')).toBe(true);
        expect(veilAtLaunch.querySelector('.relaunch-reconnecting').textContent).toBe(
            t('stream.reconnecting'),
        );
        // Held until the new stream's first frame.
        expect(document.querySelector('#stream-relaunch-loader')).not.toBe(null);
        next.onFirstFrame();
        expect(document.querySelector('#stream-relaunch-loader')).toBe(null);
        // Named until a launch succeeds, never after.
        expect(MoonlightApp._reconnectOf).toBe(null);
        expect(MoonlightApp._lastStreamingSettings.reconnect_of).toBe(undefined);
    });

    it('a plain relaunch is unchanged: a retiring /quit, no veil', async () => {
        vi.spyOn(BackendClient, 'launchApp').mockResolvedValue({ status: 'streaming' });
        vi.spyOn(MoonlightApp, '_startStreamView').mockImplementation(() => {
            MoonlightApp.streamView = { restoreViewState: vi.fn() };
        });
        const sv = outgoing();
        MoonlightApp.streamView = sv;
        MoonlightApp._reconnectOf = null;
        await MoonlightApp._relaunchTransport(1);
        expect(sv.quit).toHaveBeenCalledWith({ silent: true, retire: true });
        expect(BackendClient.launchApp.mock.calls[0][2].reconnect_of).toBe(undefined);
    });

    it('a refusal ends the stream with the banner it always had', async () => {
        const refused = Object.assign(new Error('reconnect_refused'), { statusCode: 409 });
        vi.spyOn(BackendClient, 'launchApp').mockRejectedValue(refused);
        const toast = vi.spyOn(Toast, 'error').mockImplementation(() => {});
        const quit = vi.spyOn(MoonlightApp, '_onStreamingQuit').mockImplementation(() => {});
        MoonlightApp.streamView = outgoing();
        MoonlightApp._reconnectOf = 'stream-1';
        await MoonlightApp._relaunchTransport(0);
        expect(toast).toHaveBeenCalledWith(t('stream.streamDisconnected'));
        expect(quit).toHaveBeenCalledTimes(1);
        expect(MoonlightApp._reconnectOf).toBe(null);
        expect(document.querySelector('#stream-relaunch-loader')).toBe(null);
    });

    it('a failed launch is retried once on the same transport, still named', async () => {
        const launch = vi
            .spyOn(BackendClient, 'launchApp')
            .mockRejectedValueOnce(Object.assign(new Error('Bad gateway'), { statusCode: 502 }))
            .mockResolvedValueOnce({ status: 'streaming' });
        vi.spyOn(MoonlightApp, '_startStreamView').mockImplementation(() => {
            MoonlightApp.streamView = { restoreViewState: vi.fn() };
        });
        MoonlightApp._launchRetried = false;
        MoonlightApp.streamView = outgoing();
        MoonlightApp._reconnectOf = 'stream-1';
        await MoonlightApp._relaunchTransport(0);
        await vi.waitFor(() => expect(launch).toHaveBeenCalledTimes(2));
        expect(launch.mock.calls[1][2]).toMatchObject({
            transport_index: 0,
            reconnect_of: 'stream-1',
        });
    });
});

describe('the veil', () => {
    beforeEach(() => {
        document.body.innerHTML = '<div id="app"></div>';
    });
    afterEach(() => {
        document.body.innerHTML = '';
    });

    it('keeps the last frame under it, with the text over it', () => {
        const frame = document.createElement('img');
        MoonlightApp._showRelaunchLoader(frame, { reconnecting: true });
        const el = document.getElementById('stream-relaunch-loader');
        expect(el.firstChild).toBe(frame);
        expect(frame.className).toBe('relaunch-freeze-frame');
        expect(el.classList.contains('has-freeze-frame')).toBe(false);
        expect(el.querySelector('.startup-loader')).not.toBe(null);
        expect(el.querySelector('.relaunch-reconnecting').textContent).toBe(
            t('stream.reconnecting'),
        );
        MoonlightApp._hideRelaunchLoader();
    });

    it("gives a guest's veil a Leave of their own", () => {
        const onStop = vi.fn();
        MoonlightApp._showRelaunchLoader(null, { reconnecting: true, stopLabel: 'Leave', onStop });
        const stop = document.getElementById('btn-relaunch-quit');
        expect(stop.textContent).toBe('Leave');
        stop.click();
        expect(onStop).toHaveBeenCalledTimes(1);
        MoonlightApp._hideRelaunchLoader();
    });

    it('is said in every language', () => {
        const want = { en: 'Reconnecting…', fr: 'Reconnexion…', zh: '正在重新连接…' };
        for (const [lang, text] of Object.entries(want)) {
            const strings = JSON.parse(readFileSync(join(LOCALES, `${lang}.json`), 'utf8'));
            expect(strings.stream.reconnecting, lang).toBe(text);
        }
    });
});

describe('a standby whose worker crashed', () => {
    beforeEach(() => {
        saveApp();
        vi.spyOn(console, 'warn').mockImplementation(() => {});
        MoonlightApp._lastStreamHost = { uuid: 'host-1' };
        MoonlightApp._lastStreamingSettings = { stream_height: 1080 };
        MoonlightApp._degradeOverrides = {};
        MoonlightApp._noteLiveQuality();
        MoonlightApp._dualSupported = true;
    });
    afterEach(() => {
        vi.restoreAllMocks();
        Object.assign(MoonlightApp, saved);
    });

    it('does not keep the host to one session', async () => {
        vi.spyOn(BackendClient, 'streamState').mockResolvedValue('crashed');
        const standby = { streamId: 'standby-1', quit: vi.fn(async () => {}) };
        MoonlightApp._standbyView = standby;
        await MoonlightApp._failStandby('connect failed: standby transport closed');
        expect(MoonlightApp._dualSupported).toBe(true);
        expect(MoonlightApp._standbyView).toBe(null);
        expect(standby.quit).toHaveBeenCalledWith({
            silent: true,
            retire: true,
            dualFailed: false,
        });
    });

    it('a standby that streamed and showed nothing still does', async () => {
        vi.spyOn(BackendClient, 'streamState').mockResolvedValue('live');
        const standby = { streamId: 'standby-1', quit: vi.fn(async () => {}) };
        MoonlightApp._standbyView = standby;
        await MoonlightApp._failStandby('timeout');
        expect(MoonlightApp._dualSupported).toBe(false);
        expect(standby.quit).toHaveBeenCalledWith({
            silent: true,
            retire: true,
            dualFailed: true,
        });
    });
});
