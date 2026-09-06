/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';
import {
    detectPlatform,
    isIphone,
    physicalScreenSize,
    pickAutoEnhancer,
    PLATFORM_TYPE,
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

    it('PLATFORM_TYPE is a known value and desktop picks fsr1', () => {
        expect(['mobile', 'tablet', 'desktop']).toContain(PLATFORM_TYPE);
        expect(pickAutoEnhancer()).toBe('fsr1'); // jsdom default UA → desktop
    });

    it('a beefy 1080p+ Android phone still picks sgsr (re-imported with a stubbed UA)', async () => {
        // The platform class is the whole rule since 03/09/2026: a phone's GPU
        // budget goes to the decode, however many cores it has.
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
        expect(m.pickAutoEnhancer()).toBe('sgsr');
    });

    it('a weak Android phone picks sgsr', async () => {
        vi.resetModules();
        vi.stubGlobal('navigator', {
            userAgent: UA.androidPhone,
            hardwareConcurrency: 2,
            maxTouchPoints: 5,
        });
        vi.stubGlobal('screen', { width: 720, height: 1280 });
        const m = await import('../js/util/BrowserDetect.js');
        expect(m.pickAutoEnhancer()).toBe('sgsr');
    });
});

// Chrome on Windows-on-ARM says "Win64; x64" and mobile:false — the platform
// rule reads a Snapdragon laptop as a plain desktop. The GPU string is the one
// thing that tells, so SGSR (Qualcomm's upscaler) is keyed on it.
describe('BrowserDetect — Snapdragon picks SGSR whatever the form factor', () => {
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

    async function freshModule(renderer) {
        vi.resetModules();
        vi.stubGlobal('navigator', { userAgent: UA.desktop, maxTouchPoints: 0 });
        const spy = fakeWebGl(renderer);
        const m = await import('../js/util/BrowserDetect.js');
        return { m, spy };
    }

    afterEach(() => {
        vi.restoreAllMocks();
        vi.unstubAllGlobals();
    });

    it('an Adreno under a desktop user agent picks sgsr', async () => {
        const { m } = await freshModule(ADRENO);
        expect(m.PLATFORM_TYPE).toBe('desktop');
        expect(m.isSnapdragonGpu()).toBe(true);
        expect(m.pickAutoEnhancer()).toBe('sgsr');
    });

    it('any other desktop GPU keeps fsr1', async () => {
        const { m } = await freshModule(NVIDIA);
        expect(m.isSnapdragonGpu()).toBe(false);
        expect(m.pickAutoEnhancer()).toBe('fsr1');
    });

    it('without WebGL the platform rule stands', async () => {
        const { m } = await freshModule(null);
        expect(m.isSnapdragonGpu()).toBe(false);
        expect(m.pickAutoEnhancer()).toBe('fsr1');
    });

    it('asks the GPU once per page', async () => {
        const { m, spy } = await freshModule(ADRENO);
        m.pickAutoEnhancer();
        m.pickAutoEnhancer();
        m.isSnapdragonGpu();
        expect(spy).toHaveBeenCalledTimes(1);
    });
});
