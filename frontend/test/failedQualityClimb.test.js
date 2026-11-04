/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import { MoonlightApp } from '../js/app.js';
import { BackendClient } from '../js/api/BackendClient.js';

/**
 * A quality climb the host cannot take.
 *
 * Issue #36 (Sunshine logs of 31/10/2026): the ladder climbed back to the size
 * the stream had started at, on a host with no standby, so the stream it
 * replaced was already closed. Sunshine was out of GPU memory for the bigger
 * encoder and dropped the session in half a second, and the viewer landed on
 * the host list. Now the page resumes on the rung that worked and climbs no
 * more this session. Nothing else that ends a stream may be taken for this.
 */

const FIELDS = [
    'streamView',
    '_nav',
    '_standbyView',
    '_standbyLaunching',
    '_lastStreamHost',
    '_lastStreamApp',
    '_lastStreamingSettings',
    '_degradeOverrides',
    '_degradeLevel',
    '_originalStreamHeight',
    '_liveQuality',
    '_enhancementAutoForced',
    '_transportIndex',
    '_transportChain',
    '_climbBack',
    '_climbsOver',
    '_launchRetried',
    '_firstTransportRetried',
    '_startupAborted',
    '_relaunching',
    '_crashRelaunches',
    '_lastCongSignal',
    '_lastDegradeTime',
    '_lastUpgradeTime',
    '_upgradeStableMs',
    '_reconnectOf',
    '_dualSupported',
    '_lastStreamNative',
    '_carryViewState',
];

let saved;
let relaunch;

function view(extra = {}) {
    return {
        _firstFrameRendered: false,
        streamId: 's1',
        quit: vi.fn(async () => {}),
        _linkLostDefault: vi.fn(),
        hasActiveShare: () => false,
        ...extra,
    };
}

beforeEach(() => {
    saved = Object.fromEntries(FIELDS.map((f) => [f, MoonlightApp[f]]));
    vi.spyOn(console, 'warn').mockImplementation(() => {});
    vi.spyOn(console, 'log').mockImplementation(() => {});
    vi.spyOn(console, 'error').mockImplementation(() => {});
    relaunch = vi.spyOn(MoonlightApp, '_relaunchTransport').mockImplementation(() => {});
    Object.assign(MoonlightApp, {
        _nav: { ...(MoonlightApp._nav || {}), overlay: 'streaming' },
        _standbyView: null,
        _standbyLaunching: false,
        _dualSupported: false,
        _lastStreamHost: { uuid: 'h1' },
        _lastStreamApp: { id: 1 },
        _transportChain: ['webrtc-dc-udp', 'webrtc-dc-tcp'],
        _transportIndex: 0,
        _launchRetried: false,
        _firstTransportRetried: false,
        _startupAborted: false,
        _relaunching: false,
        _crashRelaunches: [],
        _climbBack: null,
        _climbsOver: false,
        _enhancementAutoForced: true,
    });
});

afterEach(() => {
    vi.restoreAllMocks();
    Object.assign(MoonlightApp, saved);
});

/**
 * The stream on its lower rung, a minute stable, and the page climbing: the
 * classic relaunch has taken the old view down, and a new one is up with no
 * picture yet.
 */
function climbing() {
    MoonlightApp._lastStreamingSettings = { stream_height: 1280, stream_bitrate: 24000 };
    MoonlightApp._degradeOverrides = {
        stream_height: 1080,
        stream_bitrate: 12000,
        video_enhancement: 'on',
        video_enhancement_algo: 'auto',
    };
    MoonlightApp._degradeLevel = 1;
    MoonlightApp._originalStreamHeight = 1280;
    MoonlightApp._noteLiveQuality();
    MoonlightApp.streamView = view({ _firstFrameRendered: true });
    MoonlightApp._lastCongSignal = 1;
    MoonlightApp._lastDegradeTime = 0;
    MoonlightApp._lastUpgradeTime = 0;
    MoonlightApp._upgradeStableMs = 1;
    MoonlightApp._maybeUpgradeQuality();
    expect(relaunch).toHaveBeenCalledTimes(1);
    expect(MoonlightApp._degradeOverrides.stream_height).toBe(undefined);
    relaunch.mockClear();
    const next = view();
    MoonlightApp.streamView = next;
    return next;
}

function expectBackOnTheRung() {
    expect(relaunch).toHaveBeenCalledTimes(1);
    expect(relaunch).toHaveBeenCalledWith(0);
    expect(MoonlightApp._degradeOverrides).toEqual({
        stream_height: 1080,
        stream_bitrate: 12000,
        video_enhancement: 'on',
        video_enhancement_algo: 'auto',
    });
    expect(MoonlightApp._enhancementAutoForced).toBe(true);
    expect(MoonlightApp._climbBack).toBe(null);
    expect(MoonlightApp._climbsOver).toBe(true);
}

const flush = () => new Promise((resolve) => setTimeout(resolve, 0));

describe('a climb that fails before its first picture', () => {
    it('resumes on the rung that worked when the host ended the stream', async () => {
        const sv = climbing();
        vi.spyOn(BackendClient, 'streamState').mockResolvedValue('ended');
        await MoonlightApp._onLiveLinkLost(sv);
        expectBackOnTheRung();
        expect(sv._linkLostDefault).not.toHaveBeenCalled();
    });

    it('climbs no more this session', async () => {
        const sv = climbing();
        vi.spyOn(BackendClient, 'streamState').mockResolvedValue('ended');
        await MoonlightApp._onLiveLinkLost(sv);
        relaunch.mockClear();
        MoonlightApp.streamView = view({ _firstFrameRendered: true });
        MoonlightApp._maybeUpgradeQuality();
        expect(relaunch).not.toHaveBeenCalled();
        expect(MoonlightApp._degradeOverrides.stream_height).toBe(1080);
    });

    it('steps back once: the rung below failing ends the stream as usual', async () => {
        const sv = climbing();
        vi.spyOn(BackendClient, 'streamState').mockResolvedValue('ended');
        await MoonlightApp._onLiveLinkLost(sv);
        relaunch.mockClear();
        const below = view();
        MoonlightApp.streamView = below;
        await MoonlightApp._onLiveLinkLost(below);
        expect(relaunch).not.toHaveBeenCalled();
        expect(below._linkLostDefault).toHaveBeenCalledTimes(1);
    });

    it.each(['no_video', 'decoder_unsupported'])('takes %s as the climb', (reason) => {
        climbing();
        MoonlightApp._onTransportFailed(reason);
        expectBackOnTheRung();
    });

    it('gives a refused launch its usual retry first', () => {
        climbing();
        MoonlightApp.streamView = null;
        MoonlightApp._onTransportFailed('launch_failed');
        expect(relaunch).toHaveBeenCalledWith(0);
        expect(MoonlightApp._climbsOver).toBe(false);
        expect(MoonlightApp._launchRetried).toBe(true);
        relaunch.mockClear();

        MoonlightApp._onTransportFailed('launch_failed');
        expectBackOnTheRung();
        expect(MoonlightApp._launchRetried).toBe(false);
    });

    it('asks the host about a link closed before it connected', async () => {
        climbing();
        vi.spyOn(BackendClient, 'streamState').mockResolvedValue('ended');
        MoonlightApp._onTransportFailed('closed before connect');
        expect(relaunch).not.toHaveBeenCalled();
        await flush();
        expectBackOnTheRung();
    });
});

describe('what is not a failed climb', () => {
    it('a picture came: an ended stream ends as usual', async () => {
        const sv = climbing();
        sv._firstFrameRendered = true;
        vi.spyOn(BackendClient, 'streamState').mockResolvedValue('ended');
        await MoonlightApp._onLiveLinkLost(sv);
        expect(relaunch).not.toHaveBeenCalled();
        expect(sv._linkLostDefault).toHaveBeenCalledTimes(1);
        expect(MoonlightApp._climbBack).toBe(null);
        expect(MoonlightApp._climbsOver).toBe(false);
    });

    it('a picture settles the climb at the next poll', () => {
        const sv = climbing();
        sv._firstFrameRendered = true;
        MoonlightApp._upgradeStableMs = 1e12;
        MoonlightApp._maybeUpgradeQuality();
        expect(MoonlightApp._climbBack).toBe(null);
        expect(MoonlightApp._climbsOver).toBe(false);
    });

    it.each(['stopped', 'unknown', 'live'])('the host says %s', async (state) => {
        const sv = climbing();
        vi.spyOn(BackendClient, 'streamState').mockResolvedValue(state);
        await MoonlightApp._onLiveLinkLost(sv);
        expect(relaunch).not.toHaveBeenCalled();
        expect(sv._linkLostDefault).toHaveBeenCalledTimes(1);
        expect(MoonlightApp._climbsOver).toBe(false);
    });

    it('a worker crash starts the stream again at the wished quality', async () => {
        const sv = climbing();
        vi.spyOn(BackendClient, 'streamState').mockResolvedValue('crashed');
        await MoonlightApp._onLiveLinkLost(sv);
        expect(relaunch).toHaveBeenCalledTimes(1);
        expect(MoonlightApp._reconnectOf).toBe('s1');
        expect(MoonlightApp._degradeOverrides.stream_height).toBe(undefined);
        expect(MoonlightApp._climbsOver).toBe(false);
    });

    it('Stop pressed: nothing relaunches', async () => {
        const sv = climbing();
        sv._manualQuitting = true;
        vi.spyOn(BackendClient, 'streamState').mockResolvedValue('ended');
        await MoonlightApp._onLiveLinkLost(sv);
        MoonlightApp._onTransportFailed('no_video');
        expect(relaunch).not.toHaveBeenCalled();
        expect(MoonlightApp._climbsOver).toBe(false);
    });

    it('Stop pressed while the host answers: nothing relaunches', async () => {
        const sv = climbing();
        vi.spyOn(BackendClient, 'streamState').mockResolvedValue('ended');
        MoonlightApp._onTransportFailed('closed before connect');
        sv._manualQuitting = true;
        await flush();
        expect(relaunch).not.toHaveBeenCalled();
        expect(MoonlightApp._climbsOver).toBe(false);
    });

    it('an ICE timeout goes down the transport chain', () => {
        climbing();
        MoonlightApp._onTransportFailed('ICE timeout');
        expect(relaunch).toHaveBeenCalledWith(1);
        expect(MoonlightApp._climbBack).toBe(null);
        expect(MoonlightApp._climbsOver).toBe(false);
    });

    it('a link closed with the host still live goes down the chain', async () => {
        climbing();
        vi.useFakeTimers();
        try {
            vi.spyOn(BackendClient, 'streamState').mockResolvedValue('live');
            MoonlightApp._onTransportFailed('closed before connect');
            await vi.runAllTimersAsync();
        } finally {
            vi.useRealTimers();
        }
        expect(relaunch).toHaveBeenCalledWith(0); // the chain's silent first retry
        expect(MoonlightApp._climbsOver).toBe(false);
        expect(MoonlightApp._climbBack).toBe(null);
    });

    it('a step down settles the climb', () => {
        climbing();
        MoonlightApp._climbBack = { quality: MoonlightApp._liveQuality };
        MoonlightApp._lastDegradeTime = 0;
        MoonlightApp._lastUpgradeTime = 0;
        MoonlightApp._lastStreamNative = false;
        MoonlightApp.streamView = view({ _firstFrameRendered: true });
        vi.spyOn(MoonlightApp, '_qualityRelaunch').mockImplementation(() => {});
        MoonlightApp._onStreamCongested();
        expect(MoonlightApp._climbBack).toBe(null);
    });

    it('the stream ending clears it', () => {
        climbing();
        MoonlightApp._nav.overlay = null;
        MoonlightApp.streamView = null;
        MoonlightApp._onStreamingQuit();
        expect(MoonlightApp._climbBack).toBe(null);
    });
});
