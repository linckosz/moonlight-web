/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import { MoonlightApp } from '../js/app.js';
import { WebRtcDataChannel } from '../js/api/WebRtcDataChannel.js';
import {
    ICE_LOST_BEFORE_CHANNELS,
    UDP_MEMORY_MAX_AGE_MS,
    UDP_MEMORY_SKIPS,
    forgetUdpFailed,
    iceLostBeforeChannels,
    networkKind,
    pathFailure,
    rememberUdpFailed,
    skipUdpFor,
} from '../js/util/UdpMemory.js';

/**
 * Issue #48: a car on LTE whose UDP answers the ICE checks, then carries
 * nothing. Each launch spent two UDP attempts, about fourteen seconds, before
 * ICE-TCP streamed. The lost path is not tried twice in a launch, and the
 * launches after one that streamed past it start past UDP, for a while.
 */

function memoryStore() {
    const m = new Map();
    return {
        getItem: (k) => (m.has(k) ? m.get(k) : null),
        setItem: (k, v) => m.set(k, String(v)),
        removeItem: (k) => m.delete(k),
        raw: () => m.get('mw_udp_failed'),
    };
}

describe('UdpMemory', () => {
    const T0 = 1_000_000;

    it('tells a path failure from the rest', () => {
        expect(pathFailure('ICE timeout')).toBe(true);
        expect(pathFailure('ICE failed')).toBe(true);
        expect(pathFailure('ICE disconnected' + ICE_LOST_BEFORE_CHANNELS)).toBe(true);
        expect(pathFailure('launch_failed')).toBe(false);
        expect(pathFailure('decoder_unsupported')).toBe(false);
        expect(iceLostBeforeChannels('ICE disconnected' + ICE_LOST_BEFORE_CHANNELS)).toBe(true);
        expect(iceLostBeforeChannels('ICE disconnected')).toBe(false);
    });

    it('skips UDP for the next launches, then tries it again', () => {
        const s = memoryStore();
        rememberUdpFailed('h1', { now: T0, net: '', s });
        for (let i = 1; i <= UDP_MEMORY_SKIPS; i++)
            expect(skipUdpFor('h1', { now: T0 + i, net: '', s })).toEqual({
                skip: i,
                of: UDP_MEMORY_SKIPS,
            });
        expect(skipUdpFor('h1', { now: T0 + 99, net: '', s })).toBe(null);
        expect(s.raw()).toBe(undefined); // forgotten: UDP is tried
    });

    it('tries UDP again after a day', () => {
        const s = memoryStore();
        rememberUdpFailed('h1', { now: T0, net: '', s });
        expect(skipUdpFor('h1', { now: T0 + UDP_MEMORY_MAX_AGE_MS, net: '', s })).toBe(null);
        expect(s.raw()).toBe(undefined);
    });

    it('holds nothing against another host or another kind of network', () => {
        const s = memoryStore();
        rememberUdpFailed('h1', { now: T0, net: 'cellular', s });
        expect(skipUdpFor('h2', { now: T0, net: 'cellular', s })).toBe(null);
        expect(skipUdpFor('h1', { now: T0, net: 'wifi', s })).toBe(null);
        // kept for the network it failed on
        expect(skipUdpFor('h1', { now: T0, net: 'cellular', s })).toEqual({
            skip: 1,
            of: UDP_MEMORY_SKIPS,
        });
    });

    it('is forgotten when UDP carries a stream on the same kind of network', () => {
        const s = memoryStore();
        rememberUdpFailed('h1', { now: T0, net: 'cellular', s });
        forgetUdpFailed('h1', { net: 'wifi', s });
        expect(s.raw()).not.toBe(undefined);
        forgetUdpFailed('h1', { net: 'cellular', s });
        expect(s.raw()).toBe(undefined);
    });

    it('keeps no address: a host, a time, a count and a word', () => {
        const s = memoryStore();
        rememberUdpFailed('h1', { now: T0, net: 'cellular', s });
        expect(JSON.parse(s.raw())).toEqual({ h1: { at: T0, skips: 0, net: 'cellular' } });
        rememberUdpFailed('not a host id', { now: T0, s });
        rememberUdpFailed(undefined, { now: T0, s });
        expect(Object.keys(JSON.parse(s.raw()))).toEqual(['h1']);
        expect(networkKind({ connection: { type: 'wifi' } })).toBe('wifi');
        expect(networkKind({ connection: { type: '10.0.0.1' } })).toBe('');
        expect(networkKind({})).toBe('');
    });

    it('remembers sixteen hosts at most, the latest', () => {
        const s = memoryStore();
        for (let i = 0; i < 20; i++) rememberUdpFailed('h' + i, { now: T0 + i, net: '', s });
        const ids = Object.keys(JSON.parse(s.raw()));
        expect(ids.length).toBe(16);
        expect(ids).not.toContain('h0');
        expect(ids).toContain('h19');
    });

    it('tries UDP where the storage cannot be read or holds garbage', () => {
        const broken = {
            getItem() {
                throw new Error('denied');
            },
            setItem() {
                throw new Error('denied');
            },
            removeItem() {},
        };
        expect(() => rememberUdpFailed('h1', { s: broken })).not.toThrow();
        expect(skipUdpFor('h1', { s: broken })).toBe(null);
        const s = memoryStore();
        s.setItem('mw_udp_failed', '{"h1":{"at":"x"}}');
        expect(skipUdpFor('h1', { now: T0, net: '', s })).toBe(null);
        expect(s.raw()).toBe(undefined);
    });
});

describe('WebRtcDataChannel: ICE that goes', () => {
    class FakePc {
        constructor() {
            this.iceConnectionState = 'new';
        }
        createDataChannel(label) {
            return { label, readyState: 'connecting', close() {} };
        }
        close() {}
    }

    function transport() {
        vi.stubGlobal('RTCPeerConnection', FakePc);
        const t = new WebRtcDataChannel('wss://host.invalid/ws');
        t._chainFallback = true;
        t._createPeerConnection();
        const reasons = [];
        t.onError = (e) => reasons.push(e.message);
        const to = (state) => {
            t.pc.iceConnectionState = state;
            t.pc.oniceconnectionstatechange();
        };
        return { t, reasons, to };
    }

    beforeEach(() => {
        vi.spyOn(console, 'log').mockImplementation(() => {});
        vi.spyOn(console, 'warn').mockImplementation(() => {});
        vi.spyOn(console, 'error').mockImplementation(() => {});
    });
    afterEach(() => {
        vi.unstubAllGlobals();
        vi.restoreAllMocks();
    });

    it('says when it went before a single channel opened', () => {
        const { reasons, to } = transport();
        to('connected');
        to('disconnected');
        expect(reasons).toEqual(['ICE disconnected' + ICE_LOST_BEFORE_CHANNELS]);
    });

    it('says it plainly once the channels were open', () => {
        const { t, reasons, to } = transport();
        to('connected');
        t.connected = true;
        to('failed');
        expect(reasons).toEqual(['ICE failed']);
    });
});

const FIELDS = [
    'streamView',
    '_nav',
    '_lastStreamHost',
    '_transportIndex',
    '_transportChain',
    '_launchRetried',
    '_firstTransportRetried',
    '_startupAborted',
    '_standbyView',
    '_climbBack',
    '_viewerOnLan',
    '_udpFailedHere',
];

describe('the transport chain on a path that connects and carries nothing', () => {
    let saved;
    let relaunch;

    beforeEach(() => {
        saved = Object.fromEntries(FIELDS.map((f) => [f, MoonlightApp[f]]));
        localStorage.removeItem('mw_udp_failed');
        vi.spyOn(console, 'warn').mockImplementation(() => {});
        vi.spyOn(console, 'log').mockImplementation(() => {});
        vi.spyOn(console, 'error').mockImplementation(() => {});
        relaunch = vi.spyOn(MoonlightApp, '_relaunchTransport').mockImplementation(() => {});
        Object.assign(MoonlightApp, {
            streamView: { _firstFrameRendered: false, quit: vi.fn(async () => {}) },
            _nav: { ...(MoonlightApp._nav || {}), overlay: 'streaming' },
            _lastStreamHost: { uuid: 'h1' },
            _transportChain: ['webrtc-dc-udp', 'webrtc-dc-tcp'],
            _transportIndex: 0,
            _launchRetried: false,
            _firstTransportRetried: false,
            _startupAborted: false,
            _standbyView: null,
            _climbBack: null,
            _viewerOnLan: false,
            _udpFailedHere: false,
        });
    });

    afterEach(() => {
        vi.restoreAllMocks();
        Object.assign(MoonlightApp, saved);
        localStorage.removeItem('mw_udp_failed');
    });

    it('ICE lost before the channels opened goes straight to the next rung', () => {
        MoonlightApp._onTransportFailed('ICE disconnected' + ICE_LOST_BEFORE_CHANNELS);
        expect(relaunch).toHaveBeenCalledWith(1);
        expect(relaunch).not.toHaveBeenCalledWith(0);
        expect(MoonlightApp._udpFailedHere).toBe(true);
    });

    it('a failure that never connected still gets its silent retry', () => {
        MoonlightApp._onTransportFailed('ICE failed');
        expect(relaunch).toHaveBeenCalledWith(0);
    });

    it('the rung that carried the stream past UDP is where the next launch starts', () => {
        MoonlightApp._onTransportFailed('ICE disconnected' + ICE_LOST_BEFORE_CHANNELS);
        MoonlightApp._transportIndex = 1;
        MoonlightApp._noteRungCarried();
        expect(skipUdpFor('h1')).toEqual({ skip: 1, of: UDP_MEMORY_SKIPS });
    });

    it('nothing is remembered without a UDP path failure, or on the host network', () => {
        MoonlightApp._onTransportFailed('launch_failed');
        MoonlightApp._transportIndex = 1;
        MoonlightApp._noteRungCarried();
        expect(localStorage.getItem('mw_udp_failed')).toBe(null);

        MoonlightApp._udpFailedHere = true;
        MoonlightApp._viewerOnLan = true;
        MoonlightApp._noteRungCarried();
        expect(localStorage.getItem('mw_udp_failed')).toBe(null);
    });

    it('a stream over UDP clears what was held against it', () => {
        rememberUdpFailed('h1');
        MoonlightApp._transportIndex = 0;
        MoonlightApp._noteRungCarried();
        expect(localStorage.getItem('mw_udp_failed')).toBe(null);
    });
});
