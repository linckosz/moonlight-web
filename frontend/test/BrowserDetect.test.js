/*
 * Moonlight-Web — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect, afterEach, vi } from 'vitest';
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
    afterEach(() => vi.unstubAllGlobals());

    it('PLATFORM_TYPE is a known value and desktop picks fsr1', () => {
        expect(['mobile', 'tablet', 'desktop']).toContain(PLATFORM_TYPE);
        expect(pickAutoEnhancer()).toBe('fsr1'); // jsdom default UA → desktop
    });

    it('a beefy 1080p+ Android phone picks fsr1 (re-imported with a stubbed UA)', async () => {
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
        expect(m.pickAutoEnhancer()).toBe('fsr1');
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
