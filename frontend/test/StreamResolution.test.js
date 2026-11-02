/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 *
 * The resolution choice, from what Settings stores to the size a launch asks
 * for. Each choice has one property that matters to the user and that only a
 * launch reveals: Auto on the native host is the host's own size brought down
 * to this screen and never blown up; "Match my screen" is this screen's size
 * and the host's display asked to take it; a custom pair that reaches a native
 * host as a height is a squeezed picture; and any of them sent as a height of
 * 0 to Sunshine is a refused launch.
 */
import { describe, it, expect } from 'vitest';
import {
    CUSTOM_SIZE_MAX,
    CUSTOM_SIZE_MIN,
    HOST_FALLBACK_HEIGHT,
    AUTO_MAX_HEIGHT,
    TV_AUTO_MAX_HEIGHT,
    PIXEL_RATE_BUDGET,
    BUDGET_MIN_HEIGHT,
    BUDGET_MIN_FPS,
    BUDGET_GRID_MAX_LOSS,
    AUTO_RAISED_MAX_HEIGHT,
    autoMaxHeightFor,
    bitrateReference,
    clampCustomSize,
    devicePixelSize,
    fitPixelBudget,
    nearlySameStreamSize,
    readResolutionChoice,
    resolveStreamSize,
    streamFootprint,
    tvOverAsk,
} from '../js/util/StreamResolution.js';

const phone = { screen: { width: 390, height: 844 }, devicePixelRatio: 3 };
const laptop = { screen: { width: 1536, height: 864 }, devicePixelRatio: 1.25 };
const tablet = { width: 2000, height: 1600 };

describe('readResolutionChoice', () => {
    it('is Auto by default, for anything it does not know, and for the retired "host"', () => {
        expect(readResolutionChoice(undefined).mode).toBe('auto');
        expect(readResolutionChoice({ stream_resolution: 'native' }).mode).toBe('auto');
        expect(readResolutionChoice({ stream_resolution: 'host' }).mode).toBe('auto');
        expect(readResolutionChoice({ stream_resolution: 'fixed' }).mode).toBe('fixed');
        expect(readResolutionChoice({ stream_resolution: 'device' }).mode).toBe('device');
    });

    it('pins a stored custom pair into its bounds and fills a missing one', () => {
        const c = readResolutionChoice({
            stream_resolution: 'custom',
            stream_custom_width: 99999,
            stream_custom_height: 'abc',
        });
        expect(c.customWidth).toBe(CUSTOM_SIZE_MAX);
        expect(c.customHeight).toBe(1080);
        expect(clampCustomSize(10, 500)).toBe(CUSTOM_SIZE_MIN);
        expect(clampCustomSize('1200', 500)).toBe(1200);
    });
});

describe('devicePixelSize', () => {
    it('is the screen in physical pixels, landscape, even', () => {
        // 390×3 = 1170, 844×3 = 2532: held portrait, streamed landscape.
        expect(devicePixelSize(phone)).toEqual({ width: 2532, height: 1170 });
        // 1536×1.25 = 1920, 864×1.25 = 1080.
        expect(devicePixelSize(laptop)).toEqual({ width: 1920, height: 1080 });
    });

    it('rounds an odd result down to even', () => {
        const odd = { screen: { width: 1365, height: 767 }, devicePixelRatio: 1 };
        expect(devicePixelSize(odd)).toEqual({ width: 1364, height: 766 });
    });

    it('is null when there is no screen to read', () => {
        expect(devicePixelSize({})).toBeNull();
        expect(devicePixelSize({ screen: { width: 0, height: 0 } })).toBeNull();
    });
});

describe('resolveStreamSize', () => {
    const noFlags = {
        fitBox: false,
        allowUpscale: false,
        matchDisplay: false,
        followsScreen: false,
        fallback: null,
    };

    it('leaves a fixed rung to the ratio logic: height only', () => {
        expect(resolveStreamSize({ mode: 'fixed', height: 1440 }, { nativeHost: false })).toEqual({
            height: 1440,
            aspect: null,
            ...noFlags,
        });
    });

    // Auto on the native host: this screen is the box the host's own size
    // must fit in — the host keeps its size when it fits, scales down at its
    // own shape when it does not (2560×1440 → 2000×1125), never up. All of
    // that is the host's box rule; what is sent is the box, and the request
    // follows this screen. The box itself stops at Auto's ceiling, so the
    // 2000×1600 tablet asks for 1800×1440.
    it('asks the native host for its own size within this screen, never upscaled', () => {
        expect(resolveStreamSize({ mode: 'auto' }, { nativeHost: true, device: tablet })).toEqual({
            height: 1440,
            aspect: '1800:1440',
            fitBox: true,
            allowUpscale: false,
            matchDisplay: false,
            followsScreen: true,
            fallback: null,
        });
        // No screen to read: the host's own size, whatever it is.
        expect(resolveStreamSize({ mode: 'auto' }, { nativeHost: true, device: null })).toEqual({
            height: 0,
            aspect: null,
            ...noFlags,
        });
    });

    it("is today's 1080p on a host that cannot say its size", () => {
        expect(resolveStreamSize({ mode: 'auto' }, { nativeHost: false, device: tablet })).toEqual({
            height: HOST_FALLBACK_HEIGHT,
            aspect: null,
            ...noFlags,
        });
    });

    // "Match my screen": this screen's size, pixel for pixel — the one choice
    // that may ask a host for more than its display has, and the one that
    // asks the native host to switch its display to that size.
    it('asks for this screen, the display switched to it, upscaling allowed', () => {
        const size = resolveStreamSize(
            { mode: 'device' },
            { nativeHost: true, device: { width: 2532, height: 1170 } },
        );
        expect(size).toEqual({
            height: 1170,
            aspect: '2532:1170',
            fitBox: true,
            allowUpscale: true,
            matchDisplay: true,
            followsScreen: true,
            fallback: { width: 2532, height: 1170 },
        });
        // The same request reaches every host: the backend turns "W:H" at
        // that height into the explicit width Sunshine needs, and GameStream's
        // optimal-settings flag lets it switch the host display to it.
        expect(
            resolveStreamSize(
                { mode: 'device' },
                { nativeHost: false, device: { width: 1920, height: 1080 } },
            ).aspect,
        ).toBe('1920:1080');
    });

    // A phone or a tablet is pinched and zoomed: Auto's box there is 1440
    // lines by any width, the same whichever way it is held — a 1440p host
    // streams as it is, a 4K one comes down to 1440, nothing goes below 1080.
    it('asks a native host for 1440 lines, any width, on a phone or a tablet', () => {
        const phoneScreen = { width: 2532, height: 1170 };
        const size = resolveStreamSize(
            { mode: 'auto' },
            { nativeHost: true, touch: true, device: phoneScreen },
        );
        expect(size.height).toBe(1440);
        expect(size.aspect).toBe('4096:1440');
        expect(size.allowUpscale).toBe(false);
        expect(size.followsScreen).toBe(false);
        // "Match my screen" on a phone falls back to that same box.
        const match = resolveStreamSize(
            { mode: 'device' },
            { nativeHost: true, touch: true, device: phoneScreen },
        );
        expect(match.aspect).toBe('2532:1170');
        expect(match.fallback).toEqual({ width: 4096, height: 1440 });
        expect(bitrateReference({ mode: 'auto' }, phoneScreen, true)).toEqual({
            height: 1440,
            aspect: '16:9',
        });
    });

    it('falls back to the fixed rung when the screen is unknown', () => {
        expect(
            resolveStreamSize({ mode: 'device', height: 720 }, { nativeHost: true, device: null }),
        ).toEqual({ height: 720, aspect: null, ...noFlags });
    });

    it('sends a custom pair as a box, even, never upscaled, nothing switched', () => {
        expect(
            resolveStreamSize(
                { mode: 'custom', customWidth: 1601, customHeight: 1201 },
                { nativeHost: true },
            ),
        ).toEqual({
            height: 1200,
            aspect: '1600:1200',
            fitBox: true,
            allowUpscale: false,
            matchDisplay: false,
            followsScreen: false,
            fallback: null,
        });
        // Out of bounds is pinned, not refused.
        const pinned = resolveStreamSize(
            { mode: 'custom', customWidth: 100, customHeight: 9000 },
            { nativeHost: false },
        );
        expect(pinned.height).toBe(CUSTOM_SIZE_MAX);
        expect(pinned.aspect).toBe(`${CUSTOM_SIZE_MIN}:${CUSTOM_SIZE_MAX}`);
    });
});

// "MoonlightWeb Virtual Display": the screen is made for this stream, so
// every choice names an exact size and the display is created at it. No
// letterbox anywhere, and a rung takes THIS screen's shape.
describe('resolveStreamSize on the virtual display', () => {
    const phoneScreen = { width: 2532, height: 1170 };
    const ask = (choice) =>
        resolveStreamSize(choice, {
            nativeHost: true,
            touch: true,
            virtualDisplay: true,
            device: phoneScreen,
        });

    it('makes the display this screen for Auto and for "Match my screen" alike', () => {
        for (const mode of ['auto', 'device']) {
            const size = ask({ mode });
            expect(size.aspect).toBe('2532:1170');
            expect(size.height).toBe(1170);
            expect(size.matchDisplay).toBe(true);
            expect(size.followsScreen).toBe(true);
            expect(size.fallback).toBeNull();
        }
    });

    it('takes a custom pair as typed, portrait and all, even and in bounds', () => {
        expect(ask({ mode: 'custom', customWidth: 1677, customHeight: 2043 }).aspect).toBe(
            '1676:2042',
        );
        // Under the floor a display mode is not usable: the pair is raised to
        // it whole, its shape kept.
        expect(ask({ mode: 'custom', customWidth: 400, customHeight: 400 }).aspect).toBe('480:480');
    });

    it("gives a rung its lines at this screen's shape, shrunk when it overflows", () => {
        // 1080 lines on a 2.165 screen: 2336×1080, not 1920×1080.
        expect(ask({ mode: 'fixed', height: 1080 }).aspect).toBe('2336:1080');
        expect(ask({ mode: 'fixed', height: 720 }).aspect).toBe('1558:720');
        // 2160 lines would be 4676 wide — past what the driver and the
        // decoders take, so the whole thing comes down at its own shape.
        const tall = ask({ mode: 'fixed', height: 2160 });
        expect(tall.aspect).toBe('4096:1892');
        expect(tall.height).toBe(1892);
        // A 16:9 laptop gets exactly the rung.
        expect(
            resolveStreamSize(
                { mode: 'fixed', height: 1440 },
                { nativeHost: true, virtualDisplay: true, device: { width: 1920, height: 1080 } },
            ).aspect,
        ).toBe('2560:1440');
    });

    // A small desktop keeps its own shape: the floor moves the whole size,
    // it never stretches one side into it.
    it('leaves a small screen alone, and grows a tiny one whole', () => {
        const at = (device) =>
            resolveStreamSize({ mode: 'auto' }, { nativeHost: true, virtualDisplay: true, device });
        expect(at({ width: 800, height: 600 }).aspect).toBe('800:600');
        expect(at({ width: 640, height: 480 }).aspect).toBe('640:480');
        expect(at({ width: 400, height: 300 }).aspect).toBe('640:480');
    });

    it('is the ordinary choice again when this screen is unknown', () => {
        const size = resolveStreamSize(
            { mode: 'auto' },
            { nativeHost: true, touch: true, virtualDisplay: true, device: null },
        );
        expect(size.aspect).toBe('4096:1440');
        expect(size.matchDisplay).toBe(false);
    });
});

describe('bitrateReference', () => {
    it('counts the pixels the choice stands for', () => {
        // Auto: this screen under Auto's ceiling, the most the host sends.
        expect(bitrateReference({ mode: 'auto' }, tablet)).toEqual({
            height: 1440,
            aspect: '1800:1440',
        });
        // "Match my screen" is the viewer's own word: no ceiling on it.
        expect(bitrateReference({ mode: 'device' }, tablet)).toEqual({
            height: 1600,
            aspect: '2000:1600',
        });
        expect(bitrateReference({ mode: 'device' }, { width: 2532, height: 1170 })).toEqual({
            height: 1170,
            aspect: '2532:1170',
        });
        expect(bitrateReference({ mode: 'custom', customWidth: 2560, customHeight: 1080 })).toEqual(
            { height: 1080, aspect: '2560:1080' },
        );
        // Unknown screen: the estimate's own 1080p reference.
        expect(bitrateReference({ mode: 'auto', height: 2160 }, null)).toEqual({
            height: 1080,
            aspect: '16:9',
        });
        expect(bitrateReference({ mode: 'fixed', height: 2160 })).toEqual({
            height: 2160,
            aspect: '16:9',
        });
    });
});

// ── Auto's ceiling and the pixel budget ──────────────────────────────
//
// Two numbers chosen apart multiply into one bill the whole chain pays. What
// is worth pinning down is where the reductions stop and in which order they
// happen: a viewer feels a frame rate on every mouse move and a hundred lines
// of resolution almost never, so the resolution goes first — and neither goes
// below what the client itself does, since a stream smaller than the screen it
// lands on has nothing left to give.
describe("Auto's ceiling", () => {
    const uhd = { width: 3840, height: 2160 };

    it('stops the box at 1440 lines, its shape kept', () => {
        expect(
            resolveStreamSize({ mode: 'auto' }, { nativeHost: true, device: uhd }),
        ).toMatchObject({ height: AUTO_MAX_HEIGHT, aspect: '2560:1440' });
    });

    it('leaves "Match my screen" and Custom alone — the viewer named those', () => {
        expect(
            resolveStreamSize({ mode: 'device' }, { nativeHost: true, device: uhd }),
        ).toMatchObject({ height: 2160, aspect: '3840:2160', matchDisplay: true });
        expect(
            resolveStreamSize(
                { mode: 'custom', customWidth: 3840, customHeight: 2160 },
                { nativeHost: true, device: uhd },
            ),
        ).toMatchObject({ height: 2160, aspect: '3840:2160' });
    });

    it('holds the display it makes to order to the same ceiling under Auto', () => {
        const ctx = { nativeHost: true, device: uhd, virtualDisplay: true };
        expect(resolveStreamSize({ mode: 'auto' }, ctx)).toMatchObject({
            height: AUTO_MAX_HEIGHT,
            aspect: '2560:1440',
            matchDisplay: true,
        });
        expect(resolveStreamSize({ mode: 'device' }, ctx)).toMatchObject({
            height: 2160,
            aspect: '3840:2160',
        });
    });
});

// A TV: latency first. Its Auto stops at 720 lines on every host — a Freebox
// Player POP's 1080p screen streamed 1080p50 at 154 ms and showed 46.6 of its
// 50 frames, 720p50 all 50 at 68 ms. What the viewer names is still theirs.
describe('Auto on a TV', () => {
    // Freebox Player POP in TV Bro: 960×540 at a device pixel ratio of 2.
    const freebox = devicePixelSize({ screen: { width: 960, height: 540 }, devicePixelRatio: 2 });
    // Mi TV: its Android UI runs at 1280×720.
    const miTv = { width: 1280, height: 720 };

    it('asks a native host for 720 lines at most, its shape kept', () => {
        expect(freebox).toEqual({ width: 1920, height: 1080 });
        expect(
            resolveStreamSize({ mode: 'auto' }, { nativeHost: true, tv: true, device: freebox }),
        ).toEqual({
            height: TV_AUTO_MAX_HEIGHT,
            aspect: '1280:720',
            fitBox: true,
            allowUpscale: false,
            matchDisplay: false,
            followsScreen: true,
            fallback: null,
        });
        // A screen already at 720p is the box as it is.
        expect(
            resolveStreamSize({ mode: 'auto' }, { nativeHost: true, tv: true, device: miTv }),
        ).toMatchObject({ height: 720, aspect: '1280:720' });
        // The same Freebox screen on a computer still asks for all of it.
        expect(
            resolveStreamSize({ mode: 'auto' }, { nativeHost: true, device: freebox }),
        ).toMatchObject({ height: 1080, aspect: '1920:1080' });
    });

    it('streams 720p from a host that cannot say its size, and without a screen to read', () => {
        expect(
            resolveStreamSize({ mode: 'auto' }, { nativeHost: false, tv: true, device: freebox }),
        ).toMatchObject({ height: 720, aspect: null });
        expect(
            resolveStreamSize({ mode: 'auto' }, { nativeHost: true, tv: true, device: null }),
        ).toMatchObject({ height: 720, aspect: null });
    });

    it('leaves "Match my screen", a rung and Custom to the viewer', () => {
        const match = resolveStreamSize(
            { mode: 'device' },
            { nativeHost: true, tv: true, device: freebox },
        );
        expect(match).toMatchObject({ height: 1080, aspect: '1920:1080', matchDisplay: true });
        // When the host cannot take that mode, it falls back to Auto's box.
        expect(match.fallback).toEqual({ width: 1280, height: 720 });
        expect(
            resolveStreamSize(
                { mode: 'fixed', height: 1080 },
                { nativeHost: true, tv: true, device: freebox },
            ).height,
        ).toBe(1080);
        expect(
            resolveStreamSize(
                { mode: 'custom', customWidth: 1920, customHeight: 1080 },
                { nativeHost: true, tv: true, device: freebox },
            ).aspect,
        ).toBe('1920:1080');
    });

    it('makes the virtual display at 720p under Auto, and at the screen when asked', () => {
        const ctx = { nativeHost: true, tv: true, virtualDisplay: true, device: freebox };
        expect(resolveStreamSize({ mode: 'auto' }, ctx)).toMatchObject({
            height: 720,
            aspect: '1280:720',
            matchDisplay: true,
        });
        expect(resolveStreamSize({ mode: 'device' }, ctx).aspect).toBe('1920:1080');
    });

    it('estimates the bitrate for the 720p it streams', () => {
        expect(bitrateReference({ mode: 'auto' }, freebox, false, true)).toEqual({
            height: 720,
            aspect: '1280:720',
        });
        expect(bitrateReference({ mode: 'auto' }, null, false, true)).toEqual({
            height: 720,
            aspect: '16:9',
        });
        expect(bitrateReference({ mode: 'device' }, freebox, false, true).height).toBe(1080);
    });
});

describe('fitPixelBudget', () => {
    const uhd = { width: 3840, height: 2160 };

    it('lets a pair that fits through untouched', () => {
        // 1920×1080 at 120 is 249 Mpx/s — the reference the budget was read
        // off, and the one pair that must never be touched.
        const size = { height: 1080, aspect: '1920:1080' };
        const out = fitPixelBudget(size, 120, { device: uhd, clientFps: 120 });
        expect(out.capped).toBe(false);
        expect(out.size).toBe(size);
        expect(out.fps).toBe(120);
        expect(1920 * 1080 * 120).toBeLessThanOrEqual(PIXEL_RATE_BUDGET);
    });

    it('spends an Auto resolution before the frame rate', () => {
        // Auto's 1440p120 on a 4K screen is 442 Mpx/s. The rate is what the
        // viewer feels, so it stands and the lines come down to 1080.
        const out = fitPixelBudget({ height: 1440, aspect: '2560:1440' }, 120, {
            device: uhd,
            clientFps: 120,
        });
        expect(out.capped).toBe(true);
        expect(out.fps).toBe(120);
        expect(out.size).toEqual({ height: 1080, aspect: '1920:1080' });
    });

    it('takes the frame rate down once the resolution is at its floor', () => {
        // An ultrawide at its own shape: 2560×1080 is already at the floor's
        // lines, so the rate pays the rest — 90 fits, and stays 90 on a
        // 120 Hz screen: its divisor under it, 60, would cost a third.
        const out = fitPixelBudget({ height: 1080, aspect: '2560:1080' }, 120, {
            device: { width: 2560, height: 1080 },
            clientFps: 120,
        });
        expect(out.size.height).toBe(BUDGET_MIN_HEIGHT);
        expect(out.fps).toBe(90);
        expect(2560 * out.size.height * out.fps).toBeLessThanOrEqual(PIXEL_RATE_BUDGET);
        expect((90 - 60) / 90).toBeGreaterThan(BUDGET_GRID_MAX_LOSS);
    });

    it("lands a rate it brings down on the screen's grid when that costs little", () => {
        // A 1440p rung fits at 67: on a 120 Hz screen, 60 is a tenth less and
        // every frame on a refresh. The ultrawide's 90 on a 240 Hz screen
        // becomes 80 the same way.
        const rung = (clientFps) =>
            fitPixelBudget({ height: 1440, aspect: null }, 120, {
                device: uhd,
                clientFps,
                aspect: '16:9',
                resolutionNamed: true,
            }).fps;
        expect(rung(120)).toBe(60);
        expect((67 - 60) / 67).toBeLessThanOrEqual(BUDGET_GRID_MAX_LOSS);
        const ultrawide = fitPixelBudget({ height: 1080, aspect: '2560:1080' }, 120, {
            device: { width: 2560, height: 1080 },
            clientFps: 240,
        });
        expect(ultrawide.fps).toBe(80);
        // On a 144 Hz screen, the divisor under 67 is 48, under the floor:
        // the budget's 67 stands.
        expect(rung(144)).toBe(67);
    });

    it('keeps a frame rate the viewer named, and spends the Auto resolution alone', () => {
        // Auto's 1440 box with 144 picked: the lines come down to their floor
        // and the 144 stands, over the budget as it may be.
        const out = fitPixelBudget({ height: 1440, aspect: '2560:1440' }, 144, {
            device: uhd,
            clientFps: 144,
            rateNamed: true,
        });
        expect(out.capped).toBe(true);
        expect(out.size).toEqual({ height: 1080, aspect: '1920:1080' });
        expect(out.fps).toBe(144);
    });

    it('never pushes either half below what this very screen does', () => {
        // A 4K120 ask from a 720p60 screen: both floors are the screen's own,
        // and the budget stops there even when it is still over.
        const out = fitPixelBudget({ height: 2160, aspect: '3840:2160' }, 120, {
            device: { width: 1280, height: 720 },
            clientFps: 60,
        });
        expect(out.size.height).toBeGreaterThanOrEqual(720);
        expect(out.fps).toBeGreaterThanOrEqual(60);
    });

    it('keeps a resolution the viewer named, and lowers the Auto rate instead', () => {
        // Issue #38: 2160p picked, the rate on Auto at a 120 Hz screen. The
        // lines used to come down to 1080 for the 120 to stand; the 2160 is
        // the viewer's, so the rate pays — to its floor, which 4K is over
        // anyway.
        const size = { height: 2160, aspect: null };
        const out = fitPixelBudget(size, 120, {
            device: uhd,
            clientFps: 120,
            aspect: '16:9',
            resolutionNamed: true,
        });
        expect(out.capped).toBe(true);
        expect(out.size).toEqual(size);
        expect(out.fps).toBe(BUDGET_MIN_FPS);
    });

    it('weighs a rung whose width still belongs to the Auto ratio', () => {
        // `fixed` fixes no shape: the rung is weighed at the ratio the caller
        // knows, and comes back with its width still unstated. On a 240 Hz
        // screen, 2560×1440 fits at 67 and gets 60, 1920×1440 fits at 90 and
        // gets 80 — the rate comes down only as far as the budget asks.
        const at = (aspect) =>
            fitPixelBudget({ height: 1440, aspect: null }, 120, {
                device: uhd,
                clientFps: 240,
                aspect,
                resolutionNamed: true,
            });
        const wide = at('16:9');
        expect(wide.size).toEqual({ height: 1440, aspect: null });
        expect(wide.fps).toBe(60);
        expect(2560 * 1440 * wide.fps).toBeLessThanOrEqual(PIXEL_RATE_BUDGET);
        expect(at('4:3').fps).toBe(80);
    });

    it('holds its peace when there is no size to weigh', () => {
        // Height 0 is "the host display's own size, whatever it is".
        const out = fitPixelBudget({ height: 0, aspect: null }, 120, { device: uhd });
        expect(out.capped).toBe(false);
        expect(out.fps).toBe(120);
    });
});

describe('streamFootprint', () => {
    it('weighs a size that fixes its shape, a rung at the ratio given, and an unknown rate', () => {
        expect(streamFootprint({ height: 1080, aspect: '1920:1080' }, 120)).toEqual({
            width: 1920,
            height: 1080,
            fps: 120,
            pixelRate: 1920 * 1080 * 120,
        });
        expect(streamFootprint({ height: 1440, aspect: null }, 60, '4:3').width).toBe(1920);
        expect(streamFootprint({ height: 1080, aspect: null }, 0).fps).toBe(60);
    });

    it("says nothing of the host's own size", () => {
        expect(streamFootprint({ height: 0, aspect: null }, 120).pixelRate).toBe(0);
    });
});

describe('a raised pixel budget', () => {
    /** What Auto/Auto streams on a screen at this budget, as "<height>p<fps>". */
    const autoAuto = (device, clientFps, budget) => {
        const size = resolveStreamSize(
            { mode: 'auto' },
            { nativeHost: true, device, autoMaxHeight: autoMaxHeightFor(budget) },
        );
        const out = fitPixelBudget(size, clientFps, { device, clientFps, budget });
        return `${out.size.height}p${out.fps}`;
    };
    const uhd = { width: 3840, height: 2160 };
    const qhd = { width: 2560, height: 1440 };
    const rungs = [250e6, 330e6, 450e6, 500e6, 700e6, 1000e6];

    it('keeps 1440 lines at the base budget, and lets the budget alone cut above it', () => {
        expect(autoMaxHeightFor()).toBe(AUTO_MAX_HEIGHT);
        expect(autoMaxHeightFor(PIXEL_RATE_BUDGET)).toBe(AUTO_MAX_HEIGHT);
        expect(autoMaxHeightFor(330e6)).toBe(AUTO_RAISED_MAX_HEIGHT);
    });

    it('walks a 4K 120 Hz screen from 1080p120 to 2160p120', () => {
        expect(rungs.map((b) => autoAuto(uhd, 120, b))).toEqual([
            '1080p120',
            '1240p120',
            '1440p120',
            '1528p120',
            '1808p120',
            '2160p120',
        ]);
    });

    it('walks a 4K 60 Hz screen from 1440p60 to 2160p60', () => {
        expect(rungs.map((b) => autoAuto(uhd, 60, b))).toEqual([
            '1440p60',
            '1752p60',
            '2048p60',
            '2160p60',
            '2160p60',
            '2160p60',
        ]);
    });

    it('stops a 1440p 120 Hz screen at its own size', () => {
        expect(rungs.map((b) => autoAuto(qhd, 120, b))).toEqual([
            '1080p120',
            '1240p120',
            '1440p120',
            '1440p120',
            '1440p120',
            '1440p120',
        ]);
    });

    it('prefers a standard height just under what fits, only under a raised budget', () => {
        // 1448 lines at 450 M: 1440p costs under one percent of them. 1088
        // lines at the base budget (a rate of 118) keep the exact answer.
        const raised = fitPixelBudget({ height: 2160, aspect: '3840:2160' }, 120, {
            device: uhd,
            clientFps: 120,
            budget: 450e6,
        });
        expect(raised.size).toEqual({ height: 1440, aspect: '2560:1440' });
        const base = fitPixelBudget({ height: 1440, aspect: '2560:1440' }, 118, {
            device: uhd,
            clientFps: 118,
        });
        expect(base.size.height).toBe(1088);
    });

    it("still leaves a named resolution alone, and only moves the rate on the budget's word", () => {
        const rate = (budget) =>
            fitPixelBudget({ height: 2160, aspect: null }, 120, {
                device: uhd,
                clientFps: 120,
                aspect: '16:9',
                resolutionNamed: true,
                budget,
            }).fps;
        expect(rungs.map(rate)).toEqual([60, 60, 60, 60, 84, 120]);
    });

    it('holds a wide screen to the width every decoder takes', () => {
        const wide = { width: 5120, height: 2160 };
        const ctx = { nativeHost: true, device: wide, autoMaxHeight: AUTO_RAISED_MAX_HEIGHT };
        expect(resolveStreamSize({ mode: 'auto' }, ctx)).toMatchObject({
            height: 1728,
            aspect: '4096:1728',
        });
        expect(resolveStreamSize({ mode: 'auto' }, { ...ctx, virtualDisplay: true })).toMatchObject(
            { height: 1728, aspect: '4096:1728' },
        );
        // Without a raised ceiling, nothing moves.
        expect(resolveStreamSize({ mode: 'auto' }, { nativeHost: true, device: wide })).toMatchObject(
            { height: 1440, aspect: '3412:1440' },
        );
    });

    it('raises every computer path together, and neither a phone nor a TV', () => {
        const ctx = { nativeHost: true, device: uhd, autoMaxHeight: AUTO_RAISED_MAX_HEIGHT };
        expect(resolveStreamSize({ mode: 'auto' }, ctx)).toMatchObject({
            height: 2160,
            aspect: '3840:2160',
        });
        expect(resolveStreamSize({ mode: 'auto' }, { ...ctx, virtualDisplay: true })).toMatchObject(
            { height: 2160, aspect: '3840:2160' },
        );
        expect(resolveStreamSize({ mode: 'device' }, { ...ctx, device: uhd }).fallback).toEqual({
            width: 3840,
            height: 2160,
        });
        expect(bitrateReference({ mode: 'auto' }, uhd, false, false, 2160)).toEqual({
            height: 2160,
            aspect: '3840:2160',
        });
        // A phone keeps its 1440-line box, a TV its 720 lines.
        expect(resolveStreamSize({ mode: 'auto' }, { ...ctx, touch: true }).height).toBe(1440);
        expect(
            resolveStreamSize(
                { mode: 'auto' },
                { ...ctx, touch: true, virtualDisplay: true, device: { width: 2732, height: 2048 } },
            ).height,
        ).toBe(1440);
        expect(resolveStreamSize({ mode: 'auto' }, { ...ctx, tv: true }).height).toBe(720);
        expect(bitrateReference({ mode: 'auto' }, uhd, false, true, 2160).height).toBe(720);
        // A host that cannot say its size stays at 1080.
        expect(resolveStreamSize({ mode: 'auto' }, { ...ctx, nativeHost: false }).height).toBe(1080);
    });
});

describe('tvOverAsk — a TV asked more than its Auto would take', () => {
    it("is null within Auto's 720p and rate", () => {
        expect(tvOverAsk({ height: 720, fps: 30, autoFps: 30 })).toBeNull();
        expect(tvOverAsk({ height: 540, fps: 50, autoFps: 50 })).toBeNull();
    });

    it("names the size and the rate past Auto's, and what Auto takes (Mi TV, 1080p60)", () => {
        expect(tvOverAsk({ height: 1080, fps: 60, autoFps: 30 })).toEqual({
            height: 1080,
            fps: 60,
            autoHeight: TV_AUTO_MAX_HEIGHT,
            autoFps: 30,
        });
    });

    it('a size alone past 720 is enough', () => {
        expect(tvOverAsk({ height: 1080, fps: 50, autoFps: 50 })).toEqual({
            height: 1080,
            fps: 50,
            autoHeight: 720,
            autoFps: 50,
        });
    });

    it('a rate alone past Auto is enough; an unknown Auto rate is no reason', () => {
        expect(tvOverAsk({ height: 720, fps: 60, autoFps: 30 })?.autoFps).toBe(30);
        expect(tvOverAsk({ height: 720, fps: 60, autoFps: 0 })).toBeNull();
    });
});

describe('nearlySameStreamSize', () => {
    it('a screen read 4 px wider is the same screen (15/10/2026)', () => {
        expect(
            nearlySameStreamSize(
                { height: 1080, aspect: '1922:1080' },
                { height: 1080, aspect: '1920:1080' },
            ),
        ).toBe(true);
    });

    it('a real change of shape or height is not', () => {
        const fhd = { height: 1080, aspect: '1920:1080' };
        expect(nearlySameStreamSize({ height: 1080, aspect: '2560:1080' }, fhd)).toBe(false);
        expect(nearlySameStreamSize({ height: 1080, aspect: '1728:1080' }, fhd)).toBe(false);
        expect(nearlySameStreamSize({ height: 1440, aspect: '2560:1440' }, fhd)).toBe(false);
    });

    it('the tolerance is 1 % of the width', () => {
        const fhd = { height: 1080, aspect: '1920:1080' };
        expect(nearlySameStreamSize({ height: 1080, aspect: '1938:1080' }, fhd)).toBe(true);
        expect(nearlySameStreamSize({ height: 1080, aspect: '1942:1080' }, fhd)).toBe(false);
    });

    it('equal or missing aspects compare as they are', () => {
        expect(nearlySameStreamSize({ height: 720 }, { height: 720 })).toBe(true);
        expect(nearlySameStreamSize({ height: 720, aspect: '16:9' }, { height: 720 })).toBe(false);
        expect(nearlySameStreamSize(null, { height: 720 })).toBe(false);
    });
});
