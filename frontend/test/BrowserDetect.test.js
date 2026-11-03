/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';
import {
    av1HdrSoftwareDecode,
    decoderRidesOutGaps,
    decoderTakesReferenceRepairs,
    decodesThroughGaps,
    detectPlatform,
    detectTv,
    hdrExtTonemapRoute,
    hdrFramesOpaque,
    hdrLaunchCodec,
    isLowMemory,
    isIphone,
    noteDecoderCannotRideOut,
    noteDecoderRefusesReferenceRepairs,
    noteHdrFramesOpaque,
    OPAQUE_HDR_DAYS,
    physicalScreenSize,
    pickAutoEnhancer,
    PLATFORM_TYPE,
    touchEnabled,
    watchTouchEnabled,
} from '../js/util/BrowserDetect.js';

const UA = {
    iphone: 'Mozilla/5.0 (iPhone; CPU iPhone OS 17_0 like Mac OS X) Mobile',
    androidPhone: 'Mozilla/5.0 (Linux; Android 13; Pixel) AppleWebKit Mobile Safari',
    androidTablet: 'Mozilla/5.0 (Linux; Android 13; Tab) AppleWebKit Safari',
    ipad: 'Mozilla/5.0 (iPad; CPU OS 17_0 like Mac OS X) Safari',
    winTablet: 'Mozilla/5.0 (Windows NT 10.0; Touch) Edge',
    kindle: 'Mozilla/5.0 (Linux; Silk) Safari',
    blackberry: 'Mozilla/5.0 (BlackBerry; BB10) Mobile',
    winPhone: 'Mozilla/5.0 (Windows Phone 10) IEMobile',
    desktop: 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) Chrome/120',
};

function withNavigator(extra) {
    vi.stubGlobal('navigator', { maxTouchPoints: 0, hardwareConcurrency: 4, ...extra });
}

describe('BrowserDetect.detectPlatform', () => {
    // jsdom defines `ontouchstart`, so every case here is a touch device: the
    // no-touch-screen branch cannot be staged and is left to the code.
    afterEach(() => vi.unstubAllGlobals());

    it('classifies phones as mobile', () => {
        withNavigator({ userAgent: UA.iphone });
        expect(detectPlatform().type).toBe('mobile');
        withNavigator({ userAgent: UA.androidPhone });
        expect(detectPlatform().type).toBe('mobile');
        withNavigator({ userAgent: UA.blackberry });
        expect(detectPlatform().type).toBe('mobile');
        withNavigator({ userAgent: UA.winPhone });
        expect(detectPlatform().type).toBe('mobile');
    });

    it('classifies tablets as tablet', () => {
        withNavigator({ userAgent: UA.androidTablet });
        expect(detectPlatform().type).toBe('tablet');
        withNavigator({ userAgent: UA.ipad });
        expect(detectPlatform().type).toBe('tablet');
        withNavigator({ userAgent: UA.winTablet });
        expect(detectPlatform().type).toBe('tablet');
        withNavigator({ userAgent: UA.kindle });
        expect(detectPlatform().type).toBe('tablet');
    });

    it('classifies a plain desktop as desktop, with touch detection', () => {
        withNavigator({ userAgent: UA.desktop, maxTouchPoints: 0 });
        const d = detectPlatform();
        expect(d.type).toBe('desktop');
        expect(typeof d.isTouchDevice).toBe('boolean');
        // A high maxTouchPoints flags a touchscreen laptop as touch-capable.
        withNavigator({ userAgent: UA.desktop, maxTouchPoints: 10 });
        expect(detectPlatform().isTouchDevice).toBe(true);
    });

    it('isIphone reflects the user agent', () => {
        withNavigator({ userAgent: UA.iphone });
        expect(isIphone()).toBe(true);
        withNavigator({ userAgent: UA.desktop });
        expect(isIphone()).toBe(false);
    });
});

// A TV's browser often says "Mobile": the screen tells it apart, not the agent.
describe('BrowserDetect.detectTv', () => {
    const MI_TV_WEBVIEW =
        'Mozilla/5.0 (Linux; Android 11; MiTV-MOSR4 Build/RTM5.220609.003; wv) AppleWebKit/537.36 ' +
        '(KHTML, like Gecko) Version/4.0 Chrome/153.0.8010.36 Mobile Safari/537.36';
    const CROMITE =
        'Mozilla/5.0 (Linux; Android 10; K) AppleWebKit/537.36 Chrome/153.0.0.0 Mobile Safari/537.36';

    it('knows a TV by its missing touchscreen, whatever the agent says', () => {
        expect(detectTv({ userAgent: CROMITE, maxTouchPoints: 0 })).toBe(true);
        expect(detectTv({ userAgent: MI_TV_WEBVIEW, maxTouchPoints: 5 })).toBe(true);
        expect(
            detectTv({
                userAgent: 'Mozilla/5.0 (Linux; Android 9; AFTMM) Silk',
                maxTouchPoints: 5,
            }),
        ).toBe(true);
    });

    it('leaves phones, tablets and desktops alone', () => {
        expect(detectTv({ userAgent: UA.androidPhone, maxTouchPoints: 5 })).toBe(false);
        expect(detectTv({ userAgent: UA.androidTablet, maxTouchPoints: 10 })).toBe(false);
        expect(detectTv({ userAgent: UA.desktop, maxTouchPoints: 0 })).toBe(false);
        expect(detectTv({ userAgent: UA.iphone, maxTouchPoints: 5 })).toBe(false);
        // "After" in an agent is not a Fire TV model.
        expect(
            detectTv({ userAgent: 'Mozilla/5.0 (X11; Linux) After/1.0', maxTouchPoints: 0 }),
        ).toBe(false);
    });
});

describe('BrowserDetect.isLowMemory', () => {
    it('reads deviceMemory, and an unknown amount is not low', () => {
        expect(isLowMemory({ deviceMemory: 0.5 })).toBe(true);
        expect(isLowMemory({ deviceMemory: 1 })).toBe(true);
        expect(isLowMemory({ deviceMemory: 2 })).toBe(false);
        expect(isLowMemory({ deviceMemory: 4 })).toBe(false);
        expect(isLowMemory({})).toBe(false);
    });
});

// The ride-out bargain needs a decoder that patches over a missing reference.
// VideoToolbox fails on it instead (seen 28/09/2026 from a Mac): every Apple
// platform decodes there, whatever the browser, and nothing else says so.
describe('BrowserDetect.decoderRidesOutGaps', () => {
    const MAC_CHROME =
        'Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36';
    const MAC_SAFARI =
        'Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/18.0 Safari/605.1.15';
    const MAC_FIREFOX =
        'Mozilla/5.0 (Macintosh; Intel Mac OS X 14.6; rv:131.0) Gecko/20100101 Firefox/131.0';
    const IPHONE_CHROME =
        'Mozilla/5.0 (iPhone; CPU iPhone OS 18_0 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) CriOS/129.0 Mobile/15E148 Safari/604.1';
    const WIN_CHROME =
        'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36';
    const LINUX_CHROME =
        'Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36';
    const CHROMEOS = 'Mozilla/5.0 (X11; CrOS x86_64 14541.0.0) AppleWebKit/537.36 Chrome/126.0.0.0';

    it('says no on every Apple platform, whatever the browser', () => {
        for (const ua of [MAC_CHROME, MAC_SAFARI, MAC_FIREFOX, IPHONE_CHROME, UA.iphone, UA.ipad]) {
            expect(decoderRidesOutGaps(ua)).toBe(false);
        }
    });

    it('says yes everywhere else — "AppleWebKit" alone is not Apple', () => {
        for (const ua of [WIN_CHROME, LINUX_CHROME, CHROMEOS, UA.androidPhone, UA.desktop]) {
            expect(decoderRidesOutGaps(ua)).toBe(true);
        }
    });

    it('reads the browser user agent when given none', () => {
        vi.stubGlobal('navigator', { userAgent: MAC_CHROME });
        expect(decoderRidesOutGaps()).toBe(false);
        vi.stubGlobal('navigator', { userAgent: WIN_CHROME });
        expect(decoderRidesOutGaps()).toBe(true);
        vi.unstubAllGlobals();
    });

    // The Freebox Player POP's decoder goes silent under the wave where the
    // Mi TV's rides it out, and nothing in their user agents says which: the
    // stream finds out, and the device keeps what it found.
    const FREEBOX =
        'Mozilla/5.0 (Linux; Android 10; Freebox Player POP Build/QTT8.201201.002; wv) AppleWebKit/537.36 (KHTML, like Gecko) Version/4.0 Chrome/153.0.8010.39 Mobile Safari/537.36';

    function memoryStore() {
        const m = new Map();
        return { getItem: (k) => (m.has(k) ? m.get(k) : null), setItem: (k, v) => m.set(k, v) };
    }

    it("says no on a device that kept its decoder's verdict, and only there", () => {
        const store = memoryStore();
        expect(decoderRidesOutGaps(FREEBOX, store)).toBe(true);
        noteDecoderCannotRideOut(store);
        expect(decoderRidesOutGaps(FREEBOX, store)).toBe(false);
        expect(decoderRidesOutGaps(FREEBOX, memoryStore())).toBe(true);
    });

    it('keeps the verdict in localStorage by default', () => {
        localStorage.removeItem('mw_ride_out');
        expect(decoderRidesOutGaps(WIN_CHROME)).toBe(true);
        noteDecoderCannotRideOut();
        expect(decoderRidesOutGaps(WIN_CHROME)).toBe(false);
        localStorage.removeItem('mw_ride_out');
    });

    it('rides out when the store cannot be read, and keeps quiet when it cannot be written', () => {
        const broken = {
            getItem() {
                throw new Error('denied');
            },
            setItem() {
                throw new Error('denied');
            },
        };
        expect(decoderRidesOutGaps(FREEBOX, broken)).toBe(true);
        expect(() => noteDecoderCannotRideOut(broken)).not.toThrow();
        expect(decoderRidesOutGaps(FREEBOX, null)).toBe(true);
    });
});

// AV1 HDR frames handed opaque (Chrome 126 and Safari 27 on an M3, 26/10/2026):
// a verdict per user agent, for 30 days at most, then asked again.
describe('BrowserDetect.hdrFramesOpaque', () => {
    const SAFARI_27 =
        'Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/27.0 Safari/605.1.15';
    const SAFARI_28 = SAFARI_27.replace('Version/27.0', 'Version/28.0');
    const CHROME_126 =
        'Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36';
    const DAY = 24 * 3600 * 1000;
    const T0 = Date.UTC(2026, 9, 26);

    function memoryStore() {
        const m = new Map();
        return { getItem: (k) => (m.has(k) ? m.get(k) : null), setItem: (k, v) => m.set(k, v) };
    }

    it('says yes for the browser that kept the verdict, in that version only', () => {
        const store = memoryStore();
        expect(hdrFramesOpaque(SAFARI_27, store, T0)).toBe(false);
        noteHdrFramesOpaque(SAFARI_27, store, T0);
        expect(hdrFramesOpaque(SAFARI_27, store, T0 + DAY)).toBe(true);
        expect(hdrFramesOpaque(SAFARI_28, store, T0 + DAY)).toBe(false);
        expect(hdrFramesOpaque(CHROME_126, store, T0 + DAY)).toBe(false);
    });

    it(`holds ${OPAQUE_HDR_DAYS} days, then the next launch looks again`, () => {
        expect(OPAQUE_HDR_DAYS).toBe(30);
        const store = memoryStore();
        noteHdrFramesOpaque(CHROME_126, store, T0);
        expect(hdrFramesOpaque(CHROME_126, store, T0 + 30 * DAY - 1)).toBe(true);
        expect(hdrFramesOpaque(CHROME_126, store, T0 + 30 * DAY)).toBe(false);
        // the clock set back: a verdict dated ahead of it holds no longer
        expect(hdrFramesOpaque(CHROME_126, store, T0 - DAY)).toBe(false);
    });

    it('drops the verdicts past their time when it keeps a new one', () => {
        const store = memoryStore();
        noteHdrFramesOpaque(SAFARI_27, store, T0);
        noteHdrFramesOpaque(CHROME_126, store, T0 + 20 * DAY);
        noteHdrFramesOpaque(SAFARI_28, store, T0 + 31 * DAY);
        expect(Object.keys(JSON.parse(store.getItem('mw_hdr_opaque')))).toEqual([
            CHROME_126,
            SAFARI_28,
        ]);
    });

    it('keeps the verdict in localStorage by default', () => {
        localStorage.removeItem('mw_hdr_opaque');
        expect(hdrFramesOpaque()).toBe(false);
        noteHdrFramesOpaque();
        expect(hdrFramesOpaque()).toBe(true);
        localStorage.removeItem('mw_hdr_opaque');
    });

    it('says no when the store cannot be read or holds garbage, and keeps quiet when it cannot be written', () => {
        const broken = {
            getItem() {
                throw new Error('denied');
            },
            setItem() {
                throw new Error('denied');
            },
        };
        expect(hdrFramesOpaque(SAFARI_27, broken, T0)).toBe(false);
        expect(() => noteHdrFramesOpaque(SAFARI_27, broken, T0)).not.toThrow();
        expect(hdrFramesOpaque(SAFARI_27, null, T0)).toBe(false);
        expect(() => noteHdrFramesOpaque(SAFARI_27, null, T0)).not.toThrow();
        const garbage = memoryStore();
        garbage.setItem('mw_hdr_opaque', 'not json');
        expect(hdrFramesOpaque(SAFARI_27, garbage, T0)).toBe(false);
        noteHdrFramesOpaque(SAFARI_27, garbage, T0);
        expect(hdrFramesOpaque(SAFARI_27, garbage, T0)).toBe(true);
    });
});

// The route an HDR stream takes without a readback, on WebKit.
describe('BrowserDetect.hdrExtTonemapRoute', () => {
    afterEach(() => {
        localStorage.removeItem('mw_hdr_ext_tonemap');
        vi.unstubAllGlobals();
    });

    it('is off outside WebKit, and follows the dev key where WebGPU is', () => {
        expect(hdrExtTonemapRoute()).toBe(false);
        localStorage.setItem('mw_hdr_ext_tonemap', '1');
        expect(hdrExtTonemapRoute()).toBe(false); // no WebGPU here
        vi.stubGlobal('navigator', { userAgent: navigator.userAgent, gpu: {} });
        expect(hdrExtTonemapRoute()).toBe(true);
        localStorage.setItem('mw_hdr_ext_tonemap', '0');
        expect(hdrExtTonemapRoute()).toBe(false);
    });
});

// Safari's AV1 HDR, opaque and broken on import: HEVC HDR asked from the start.
describe('BrowserDetect.hdrLaunchCodec', () => {
    afterEach(() => {
        localStorage.removeItem('mw_hdr_ext_tonemap');
        localStorage.removeItem('mw_hdr_opaque');
        vi.unstubAllGlobals();
    });

    function webkit() {
        vi.stubGlobal('navigator', { userAgent: navigator.userAgent, gpu: {} });
        localStorage.setItem('mw_hdr_ext_tonemap', '1');
    }

    it('asks HEVC for AV1 HDR where the frames were seen opaque and the import breaks them', () => {
        webkit();
        expect(hdrLaunchCodec('av1', true)).toBe('av1');
        noteHdrFramesOpaque();
        expect(hdrLaunchCodec('av1', true)).toBe('hevc');
    });

    it('keeps the codec everywhere else', () => {
        webkit();
        noteHdrFramesOpaque();
        expect(hdrLaunchCodec('av1', false)).toBe('av1');
        for (const codec of ['hevc', 'auto', 'h264', 'pyrowave', undefined])
            expect(hdrLaunchCodec(codec, true)).toBe(codec);
        // Chrome: AV1 stays, on the sink or the browser's own tone-map
        localStorage.setItem('mw_hdr_ext_tonemap', '0');
        expect(hdrLaunchCodec('av1', true)).toBe('av1');
    });

    // HEVC frames are read back nowhere: with the enhancer, AV1's readback route.
    const open = { auto: true, enhanced: true, hostAv1Hdr: true, av1Software: true };

    it("asks AV1 for Auto's HEVC in HDR with the enhancer, where the readback route is open", () => {
        vi.stubGlobal('navigator', { userAgent: navigator.userAgent, gpu: {} });
        expect(hdrLaunchCodec('hevc', true, open)).toBe('av1');
    });

    it('keeps a HEVC chosen in the menu', () => {
        vi.stubGlobal('navigator', { userAgent: navigator.userAgent, gpu: {} });
        expect(hdrLaunchCodec('hevc', true, { ...open, auto: false })).toBe('hevc');
    });

    it('keeps HEVC when any of it is missing', () => {
        vi.stubGlobal('navigator', { userAgent: navigator.userAgent, gpu: {} });
        expect(hdrLaunchCodec('hevc', false, open)).toBe('hevc');
        expect(hdrLaunchCodec('hevc', true, { ...open, enhanced: false })).toBe('hevc');
        expect(hdrLaunchCodec('hevc', true, { ...open, hostAv1Hdr: false })).toBe('hevc');
        expect(hdrLaunchCodec('hevc', true, { ...open, av1Software: false })).toBe('hevc');
        expect(hdrLaunchCodec('hevc', true)).toBe('hevc');
        for (const codec of ['h264', 'pyrowave', 'auto'])
            expect(hdrLaunchCodec(codec, true, open)).toBe(codec);
        // WebKit: its AV1 frames are opaque, HEVC goes to its own tone-map
        localStorage.setItem('mw_hdr_ext_tonemap', '1');
        expect(hdrLaunchCodec('hevc', true, open)).toBe('hevc');
        localStorage.removeItem('mw_hdr_ext_tonemap');
        // a browser already seen handing its AV1 HDR frames opaque
        noteHdrFramesOpaque();
        expect(hdrLaunchCodec('hevc', true, open)).toBe('hevc');
    });

    it('keeps HEVC without WebGPU', () => {
        vi.stubGlobal('navigator', { userAgent: navigator.userAgent });
        expect(hdrLaunchCodec('hevc', true, open)).toBe('hevc');
    });
});

describe('BrowserDetect.av1HdrSoftwareDecode', () => {
    afterEach(() => vi.unstubAllGlobals());

    it('asks once whether 10-bit AV1 decodes in software', async () => {
        const isConfigSupported = vi.fn(async () => ({ supported: true }));
        vi.stubGlobal('VideoDecoder', { isConfigSupported });
        expect(await av1HdrSoftwareDecode()).toBe(true);
        expect(await av1HdrSoftwareDecode()).toBe(true);
        expect(isConfigSupported).toHaveBeenCalledOnce();
        expect(isConfigSupported).toHaveBeenCalledWith({
            codec: 'av01.0.08M.10',
            hardwareAcceleration: 'prefer-software',
        });
    });
});

// The same box falls silent under AMF's long-term references at 30 fps, on
// keyframes too: a second verdict, kept apart from the first.
describe('BrowserDetect.decoderTakesReferenceRepairs', () => {
    afterEach(() => localStorage.removeItem('mw_ref_repairs'));

    it('says yes until this device kept the verdict, and no after', () => {
        expect(decoderTakesReferenceRepairs()).toBe(true);
        noteDecoderRefusesReferenceRepairs();
        expect(decoderTakesReferenceRepairs()).toBe(false);
        // The two verdicts are apart: refusing the repairs says nothing of the wave.
        expect(decoderRidesOutGaps('Linux; Android 10')).toBe(true);
    });

    it('says yes when the store cannot be read, and keeps quiet when it cannot be written', () => {
        const broken = {
            getItem() {
                throw new Error('denied');
            },
            setItem() {
                throw new Error('denied');
            },
        };
        expect(decoderTakesReferenceRepairs('', broken)).toBe(true);
        expect(() => noteDecoderRefusesReferenceRepairs(broken)).not.toThrow();
        expect(decoderTakesReferenceRepairs('', null)).toBe(true);
    });

    // VideoToolbox fails on the deltas already sent after a lost frame, before
    // the host heard of it: an Apple device asks for keyframes from the start.
    it('says no on an Apple platform, whatever the store says', () => {
        const MAC_CHROME =
            'Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36';
        const IPAD_SAFARI =
            'Mozilla/5.0 (iPad; CPU OS 18_0 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/18.0 Mobile/15E148 Safari/604.1';
        const WIN_CHROME =
            'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36';
        expect(decoderTakesReferenceRepairs(MAC_CHROME, null)).toBe(false);
        expect(decoderTakesReferenceRepairs(IPAD_SAFARI, null)).toBe(false);
        expect(decoderTakesReferenceRepairs(WIN_CHROME, null)).toBe(true);
        vi.stubGlobal('navigator', { userAgent: MAC_CHROME });
        expect(decoderTakesReferenceRepairs()).toBe(false);
        vi.unstubAllGlobals();
    });
});

// The host's answer and this device's verdict, both: an NVENC host told that
// a Mac takes no repairs still said it heals by invalidation, and the M3
// decoded on into 32 "Decoding error" (25/10/2026).
describe('BrowserDetect.decodesThroughGaps', () => {
    const MAC_CHROME =
        'Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/141.0.0.0 Safari/537.36';
    const IPHONE_SAFARI =
        'Mozilla/5.0 (iPhone; CPU iPhone OS 26_0 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/26.0 Mobile/15E148 Safari/604.1';
    const WIN_CHROME =
        'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/141.0.0.0 Safari/537.36';
    afterEach(() => localStorage.removeItem('mw_ref_repairs'));

    it('decodes on where the host heals and the device takes it', () => {
        expect(decodesThroughGaps(true, WIN_CHROME, null)).toBe(true);
    });

    it('never where the host does not heal', () => {
        expect(decodesThroughGaps(false, WIN_CHROME, null)).toBe(false);
        expect(decodesThroughGaps(undefined, WIN_CHROME, null)).toBe(false);
        // Only a real true: a truthy string from a stray answer is not one.
        expect(decodesThroughGaps('true', WIN_CHROME, null)).toBe(false);
    });

    it('never on an Apple device, whatever the host says', () => {
        expect(decodesThroughGaps(true, MAC_CHROME, null)).toBe(false);
        expect(decodesThroughGaps(true, IPHONE_SAFARI, null)).toBe(false);
    });

    it('never on a device that kept the verdict against the repairs', () => {
        noteDecoderRefusesReferenceRepairs();
        expect(decodesThroughGaps(true, WIN_CHROME)).toBe(false);
    });
});

describe('BrowserDetect.physicalScreenSize', () => {
    afterEach(() => vi.unstubAllGlobals());

    it('returns physical pixels scaled by devicePixelRatio', () => {
        vi.stubGlobal('screen', { width: 1280, height: 720 });
        vi.stubGlobal('window', { devicePixelRatio: 2 });
        expect(physicalScreenSize()).toEqual({ short: 1440, long: 2560 });
    });
});

describe('BrowserDetect — enhancer choice + module constants', () => {
    // jsdom has no WebGL: answer "no context" quietly instead of logging its
    // not-implemented notice each time the GPU probe asks.
    beforeEach(() => {
        vi.spyOn(HTMLCanvasElement.prototype, 'getContext').mockReturnValue(null);
    });
    afterEach(() => {
        vi.restoreAllMocks();
        vi.unstubAllGlobals();
    });

    it('PLATFORM_TYPE is a known value and desktop picks MWSR on WebGL2', () => {
        expect(['mobile', 'tablet', 'desktop']).toContain(PLATFORM_TYPE);
        expect(pickAutoEnhancer()).toBe('gl-mwsr'); // jsdom default UA → desktop
    });

    it('HDR picks MWSR on WebGPU, the one renderer with an HDR surface', () => {
        expect(pickAutoEnhancer({ hdr: true })).toBe('mwsr');
    });

    it('a beefy 1080p+ Android phone picks MWSR on WebGL2 (re-imported with a stubbed UA)', async () => {
        // MWSR on every class since v0.4.1 (Bruno, 24/10/2026): it costs less
        // than SGSR1, which phones ran before, on every GPU measured.
        vi.resetModules();
        vi.stubGlobal('navigator', {
            userAgent: UA.androidPhone,
            hardwareConcurrency: 8,
            maxTouchPoints: 5,
        });
        vi.stubGlobal('screen', { width: 1080, height: 2400 });
        const m = await import('../js/util/BrowserDetect.js');
        expect(m.IS_ANDROID).toBe(true);
        expect(m.PLATFORM_TYPE).toBe('mobile');
        expect(m.pickAutoEnhancer()).toBe('gl-mwsr');
    });

    it('a weak Android phone picks MWSR on WebGL2', async () => {
        vi.resetModules();
        vi.stubGlobal('navigator', {
            userAgent: UA.androidPhone,
            hardwareConcurrency: 2,
            maxTouchPoints: 5,
        });
        vi.stubGlobal('screen', { width: 720, height: 1280 });
        const m = await import('../js/util/BrowserDetect.js');
        expect(m.pickAutoEnhancer()).toBe('gl-mwsr');
    });

    it('an iPhone picks MWSR on WebGPU where WebKit has it, on WebGL2 otherwise', async () => {
        // WebKit's texImage2D upload costs milliseconds of main thread per
        // frame; WebGPU drew in a quarter to a half of WebGL2's time (J23-J28).
        for (const [gpu, want] of [
            [{}, 'mwsr'],
            [undefined, 'gl-mwsr'],
        ]) {
            vi.resetModules();
            vi.stubGlobal('navigator', { userAgent: UA.iphone, maxTouchPoints: 5, gpu });
            const m = await import('../js/util/BrowserDetect.js');
            expect(m.IS_IOS).toBe(true);
            expect(m.pickAutoEnhancer()).toBe(want);
            expect(m.pickAutoEnhancer({ hdr: true })).toBe('mwsr');
        }
    });
});

// Chrome on Windows-on-ARM says "Win64; x64" and mobile:false — the platform
// rule reads a Snapdragon laptop as a plain desktop. The GPU string is the one
// thing that tells: it picked SGSR (Qualcomm's upscaler) until v0.4.1, and it
// tells an entry-level Mali, which gets no upscaler, from the rest.
describe('BrowserDetect — the GPU string keys the auto upscaler', () => {
    const ADRENO =
        'ANGLE (Qualcomm, Qualcomm(R) Adreno(TM) 618 GPU (0x41333830) Direct3D11 vs_5_0 ps_5_0, D3D11)';
    const NVIDIA =
        'ANGLE (NVIDIA, NVIDIA GeForce RTX 5060 Ti (0x00002D04) Direct3D11 vs_5_0 ps_5_0, D3D11)';

    /** Fake a WebGL context whose debug extension reports `renderer`. */
    function fakeWebGl(renderer) {
        const UNMASKED = 0x9246;
        const gl = {
            RENDERER: 0x1f01,
            getExtension: (name) =>
                name === 'WEBGL_debug_renderer_info' ? { UNMASKED_RENDERER_WEBGL: UNMASKED } : null,
            getParameter: (p) => (p === UNMASKED ? renderer : 'masked'),
        };
        return vi
            .spyOn(document, 'createElement')
            .mockImplementation(() => ({ getContext: () => (renderer === null ? null : gl) }));
    }

    async function freshModule(renderer, nav = {}) {
        vi.resetModules();
        vi.stubGlobal('navigator', { userAgent: UA.desktop, maxTouchPoints: 0, ...nav });
        const spy = fakeWebGl(renderer);
        const m = await import('../js/util/BrowserDetect.js');
        return { m, spy };
    }

    afterEach(() => {
        vi.restoreAllMocks();
        vi.unstubAllGlobals();
    });

    it('an Adreno under a desktop user agent picks MWSR on WebGL2, in place of SGSR1', async () => {
        const { m } = await freshModule(ADRENO);
        expect(m.PLATFORM_TYPE).toBe('desktop');
        expect(m.isSnapdragonGpu()).toBe(true);
        expect(m.pickAutoEnhancer()).toBe('gl-mwsr');
    });

    it('any other desktop GPU picks MWSR on WebGL2', async () => {
        const { m } = await freshModule(NVIDIA);
        expect(m.isSnapdragonGpu()).toBe(false);
        expect(m.isEntryMaliGpu()).toBe(false);
        expect(m.pickAutoEnhancer()).toBe('gl-mwsr');
    });

    it('without WebGL the platform rule stands', async () => {
        const { m } = await freshModule(null);
        expect(m.gpuRendererString()).toBe('');
        expect(m.isSnapdragonGpu()).toBe(false);
        expect(m.pickAutoEnhancer()).toBe('gl-mwsr');
    });

    it('an entry-level Mali gets no upscaler, in SDR or HDR', async () => {
        for (const r of ['Mali-G31', 'Mali-G52 MC2', 'Mali-G57 MP1', 'Mali-T860', 'Mali-G310']) {
            const { m } = await freshModule(r, { userAgent: UA.androidPhone, maxTouchPoints: 5 });
            expect(m.isEntryMaliGpu()).toBe(true);
            expect(m.pickAutoEnhancer()).toBe('off');
            expect(m.pickAutoEnhancer({ hdr: true })).toBe('off');
        }
    });

    it('a larger Mali takes the handheld upscaler', async () => {
        for (const r of ['Mali-G68 MC4', 'Mali-G78 MP14', 'Mali-G510 MC2', 'Mali-G710 MC10']) {
            const { m } = await freshModule(r, { userAgent: UA.androidPhone, maxTouchPoints: 5 });
            expect(m.isEntryMaliGpu()).toBe(false);
            expect(m.pickAutoEnhancer()).toBe('gl-mwsr');
        }
    });

    it("an entry-level Mali's plain presenter is WebGPU where it exists", async () => {
        const phone = { userAgent: UA.androidPhone, maxTouchPoints: 5 };
        let { m } = await freshModule('Mali-G57 MP1', { ...phone, gpu: {} });
        expect(m.plainPresenterIsWebGpu()).toBe(true);
        ({ m } = await freshModule('Mali-G57 MP1', phone));
        expect(m.plainPresenterIsWebGpu()).toBe(false);
        ({ m } = await freshModule('Mali-G710 MC10', { ...phone, gpu: {} }));
        expect(m.plainPresenterIsWebGpu()).toBe(false);
        ({ m } = await freshModule(NVIDIA, { gpu: {} }));
        expect(m.plainPresenterIsWebGpu()).toBe(false);
    });

    it('asks the GPU once per page', async () => {
        const { m, spy } = await freshModule(ADRENO);
        m.pickAutoEnhancer();
        m.pickAutoEnhancer();
        m.isSnapdragonGpu();
        m.plainPresenterIsWebGpu();
        expect(spy).toHaveBeenCalledTimes(1);
    });
});

describe('BrowserDetect.supportsGamingMode', () => {
    async function fresh(userAgent, maxTouchPoints, finePointer) {
        vi.resetModules();
        vi.stubGlobal('navigator', { userAgent, maxTouchPoints });
        vi.stubGlobal('matchMedia', (q) => ({
            matches: q === '(any-pointer: fine)' && finePointer,
        }));
        return import('../js/util/BrowserDetect.js');
    }

    afterEach(() => vi.unstubAllGlobals());

    it('a TV is no touch device, nor handheld, and keeps gaming mode out', async () => {
        const tv =
            'Mozilla/5.0 (Linux; Android 10; Freebox Player POP Build/QTT8.201201.002; wv) ' +
            'AppleWebKit/537.36 (KHTML, like Gecko) Version/4.0 Chrome/153.0.8010.39 Mobile Safari/537.36';
        const m = await fresh(tv, 0, false);
        expect(m.IS_TV).toBe(true);
        expect(m.IS_TOUCH_DEVICE).toBe(false);
        expect(m.IS_HANDHELD).toBe(false);
        expect(m.supportsGamingMode()).toBe(false);
        // A phone is still one.
        const phone = await fresh(UA.androidPhone, 5, false);
        expect(phone.IS_TOUCH_DEVICE).toBe(true);
    });

    it('a touchscreen PC with a mouse has it (Surface, issue #16)', async () => {
        const m = await fresh(UA.desktop, 10, true);
        expect(m.IS_TOUCH_DEVICE).toBe(true);
        expect(m.supportsGamingMode()).toBe(true);
    });

    it('a Windows touchscreen PC has it even when no fine pointer is reported (Surface Pro, issue #46)', async () => {
        const m = await fresh(UA.desktop, 10, false);
        expect(m.supportsGamingMode()).toBe(true);
    });

    it('another touchscreen device with no mouse or trackpad does not', async () => {
        const m = await fresh('Mozilla/5.0 (X11; Linux x86_64) Chrome/120', 10, false);
        expect(m.supportsGamingMode()).toBe(false);
        const tablet = await fresh(UA.androidTablet, 10, false);
        expect(tablet.supportsGamingMode()).toBe(false);
    });

    it('a Windows tablet user agent still asks for a fine pointer', async () => {
        const ua = 'Mozilla/5.0 (Windows NT 10.0; Win64; x64; Touch) Chrome/120';
        expect((await fresh(ua, 10, false)).supportsGamingMode()).toBe(false);
        expect((await fresh(ua, 10, true)).supportsGamingMode()).toBe(true);
    });

    it('a phone never has it, even with a mouse paired', async () => {
        const m = await fresh(UA.androidPhone, 5, true);
        expect(m.supportsGamingMode()).toBe(false);
    });
});

// The native host's admission window after a press serves a client whose
// tearing is real: Chromium on Windows. Chromium on macOS tears into a
// compositor that still waits for vsync; Linux was never measured.
describe('BrowserDetect.CANVAS_TEARING_IS_REAL', () => {
    afterEach(() => vi.unstubAllGlobals());

    async function realFor(userAgent) {
        vi.resetModules();
        vi.stubGlobal('navigator', { userAgent, maxTouchPoints: 0 });
        const m = await import('../js/util/BrowserDetect.js');
        return m.CANVAS_TEARING_IS_REAL;
    }

    it('Chrome and Edge on Windows', async () => {
        expect(await realFor(UA.desktop)).toBe(true);
        expect(
            await realFor(
                'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 Chrome/141.0 Safari/537.36 Edg/141.0',
            ),
        ).toBe(true);
    });

    it('not Chrome on macOS or Linux, nor Firefox on Windows, nor a Windows tablet', async () => {
        expect(
            await realFor(
                'Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) Chrome/141.0 Safari/537.36',
            ),
        ).toBe(false);
        expect(await realFor('Mozilla/5.0 (X11; Linux x86_64) Chrome/141.0 Safari/537.36')).toBe(
            false,
        );
        expect(
            await realFor(
                'Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:140.0) Gecko Firefox/140.0',
            ),
        ).toBe(false);
        expect(await realFor(UA.winTablet)).toBe(false);
    });
});

describe('touchEnabled', () => {
    /** A window whose (any-pointer: coarse) is what `coarse` says, and can change. */
    function fakeWin(coarse) {
        const win = new EventTarget();
        const listeners = new Set();
        const query = {
            matches: coarse,
            addEventListener: (_type, fn) => listeners.add(fn),
            removeEventListener: (_type, fn) => listeners.delete(fn),
        };
        win.matchMedia = vi.fn((q) => (q === '(any-pointer: coarse)' ? query : { matches: false }));
        win.setCoarse = (value) => {
            query.matches = value;
            for (const fn of listeners) fn();
        };
        win.listeners = listeners;
        return win;
    }
    const touchNav = { userAgent: UA.winTablet, maxTouchPoints: 10 };

    it('says yes for a touch screen the OS has switched on', () => {
        expect(touchEnabled({ nav: touchNav, win: fakeWin(true) })).toBe(true);
    });

    it('goes by the screen, not by the kind of device', () => {
        // A phone whose browser reports no touch at all, and a desktop agent with one.
        const phone = { userAgent: UA.iphone, maxTouchPoints: 0 };
        const desktop = { userAgent: UA.desktop, maxTouchPoints: 5 };
        expect(touchEnabled({ nav: phone, win: fakeWin(true) })).toBe(false);
        expect(touchEnabled({ nav: desktop, win: fakeWin(true) })).toBe(true);
    });

    it('says no when the OS has the touch screen off, whichever way that shows', () => {
        const noPoints = { userAgent: UA.winTablet, maxTouchPoints: 0 };
        expect(touchEnabled({ nav: noPoints, win: fakeWin(true) })).toBe(false);
        expect(touchEnabled({ nav: touchNav, win: fakeWin(false) })).toBe(false);
    });

    it('says no without the media query', () => {
        const win = new EventTarget();
        expect(touchEnabled({ nav: touchNav, win })).toBe(false);
        win.matchMedia = () => {
            throw new Error('unsupported');
        };
        expect(touchEnabled({ nav: touchNav, win })).toBe(false);
    });

    it('never says yes on a TV, even after a touch', () => {
        const tv = { userAgent: 'Mozilla/5.0 (Linux; Android 11; MiTV) Chrome', maxTouchPoints: 5 };
        const win = fakeWin(true);
        expect(touchEnabled({ nav: tv, win })).toBe(false);
        const onChange = vi.fn();
        const stop = watchTouchEnabled(onChange, { nav: tv, win });
        win.dispatchEvent(new Event('touchstart'));
        expect(touchEnabled({ nav: tv, win })).toBe(false);
        expect(onChange).not.toHaveBeenCalled();
        stop();
    });

    it('tells when the OS switches the screen off and back on', () => {
        const win = fakeWin(true);
        const onChange = vi.fn();
        const stop = watchTouchEnabled(onChange, { nav: touchNav, win });
        win.setCoarse(false);
        expect(onChange).toHaveBeenLastCalledWith(false);
        win.setCoarse(true);
        expect(onChange).toHaveBeenLastCalledWith(true);
        expect(onChange).toHaveBeenCalledTimes(2);
        stop();
    });

    it('takes a real touch as proof when the browser still says no', () => {
        const win = fakeWin(false);
        const onChange = vi.fn();
        const stop = watchTouchEnabled(onChange, { nav: touchNav, win });
        win.dispatchEvent(new Event('touchstart'));
        expect(onChange.mock.calls).toEqual([[true]]);
        expect(touchEnabled({ nav: touchNav, win })).toBe(true);
        // A second touch changes nothing.
        win.dispatchEvent(new Event('touchstart'));
        expect(onChange).toHaveBeenCalledTimes(1);
        stop();
    });

    it('forgets the proof when the OS takes the screen away', () => {
        const win = fakeWin(true);
        const onChange = vi.fn();
        const stop = watchTouchEnabled(onChange, { nav: touchNav, win });
        win.dispatchEvent(new Event('touchstart'));
        win.setCoarse(false);
        expect(touchEnabled({ nav: touchNav, win })).toBe(false);
        expect(onChange.mock.calls).toEqual([[false]]);
        stop();
    });

    it('stops listening when told to', () => {
        const win = fakeWin(true);
        const onChange = vi.fn();
        watchTouchEnabled(onChange, { nav: touchNav, win })();
        expect(win.listeners.size).toBe(0);
        win.setCoarse(false);
        win.dispatchEvent(new Event('touchstart'));
        expect(onChange).not.toHaveBeenCalled();
    });
});
