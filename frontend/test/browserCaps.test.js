/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';

/**
 * The `[Caps]` probe of the client log (issue #48: a Tesla nobody here can sit
 * in). It reports every group even where the APIs are missing, once per page,
 * and never names a device: a gamepad is its mapping, not its id.
 */

let lines;

beforeEach(() => {
    vi.resetModules();
    lines = [];
    vi.spyOn(console, 'log').mockImplementation((...a) => lines.push(a.join(' ')));
    // jsdom has no canvas: the probe reads that as no WebGL, quietly.
    vi.spyOn(HTMLCanvasElement.prototype, 'getContext').mockReturnValue(null);
});

afterEach(() => {
    vi.restoreAllMocks();
    vi.unstubAllGlobals();
    delete navigator.getGamepads;
});

describe('logBrowserCaps', () => {
    it('prints every group once, even without WebCodecs, WebRTC or WebGPU', async () => {
        const { logBrowserCaps } = await import('../js/util/BrowserCaps.js');
        await logBrowserCaps();
        await logBrowserCaps();
        const caps = lines.filter((l) => l.startsWith('[Caps] '));
        const groups = caps.map((l) => l.split(' ')[1]);
        expect(groups).toEqual([
            'browser',
            'display',
            'page',
            'webcodecs',
            'rtp',
            'webgl',
            'webgpu',
            'audio',
            'input',
        ]);
        const webcodecs = JSON.parse(caps[3].slice('[Caps] webcodecs '.length));
        expect(webcodecs.videoDecoder).toBe(false);
    });

    it('asks every codec of the decoder, and names no gamepad', async () => {
        vi.stubGlobal('VideoDecoder', {
            isConfigSupported: async (c) => ({ supported: !c.codec.startsWith('av01') }),
        });
        Object.defineProperty(navigator, 'getGamepads', {
            configurable: true,
            value: () => [
                {
                    id: 'Xbox Wireless Controller (STANDARD GAMEPAD Vendor: 045e Product: 0b13)',
                    mapping: 'standard',
                },
                null,
            ],
        });
        const { logBrowserCaps } = await import('../js/util/BrowserCaps.js');
        await logBrowserCaps();
        const webcodecs = lines.find((l) => l.startsWith('[Caps] webcodecs '));
        const video = JSON.parse(webcodecs.slice('[Caps] webcodecs '.length)).video;
        expect(video['hevc-main@1080p-hw']).toBe(true);
        expect(video['av1-10bit@2160p']).toBe(false);
        expect(Object.keys(video)).toHaveLength(24);
        const input = lines.find((l) => l.startsWith('[Caps] input '));
        expect(input).toContain('"gamepads":["standard"]');
        expect(lines.join('\n')).not.toMatch(/Xbox|045e/);
    });

    it('reports a probe that never answers, and goes on', async () => {
        vi.useFakeTimers();
        vi.stubGlobal('VideoDecoder', { isConfigSupported: () => new Promise(() => {}) });
        const { logBrowserCaps } = await import('../js/util/BrowserCaps.js');
        const done = logBrowserCaps();
        await vi.runAllTimersAsync();
        await done;
        vi.useRealTimers();
        const webcodecs = lines.find((l) => l.startsWith('[Caps] webcodecs '));
        expect(webcodecs).toContain('"h264-baseline@1080p-hw":"timeout"');
        expect(lines.some((l) => l.startsWith('[Caps] input '))).toBe(true);
    });
});
