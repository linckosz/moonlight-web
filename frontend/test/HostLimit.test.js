/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect } from 'vitest';
import { readFileSync } from 'node:fs';
import { URL, fileURLToPath } from 'node:url';
import { askedOf, hostScreenCaps, hostScreenOf } from '../js/util/HostLimit.js';
import { StreamView } from '../js/ui/StreamView.js';

/**
 * "Host limit" beside the resolution in the stats card (Bruno, 12/11/2026):
 * the native host never streams more than its own screen, and the card says
 * so only when that screen is what held the stream down.
 */

const FHD = { width: 1920, height: 1080 };
const QHD = { width: 2560, height: 1440 };
const UHD = { width: 3840, height: 2160 };

// A launch's settings as app.js _applyResolvedSize writes them: a rung keeps
// its height, Auto, Custom and "Match my screen" are boxes.
const rung = (height, aspect = '16:9') => askedOf({ stream_height: height, stream_aspect: aspect });
const box = (width, height, over = {}) =>
    askedOf({
        stream_height: height,
        stream_aspect: width + ':' + height,
        stream_fit_box: true,
        ...over,
    });
const match = (width, height, fallback) =>
    box(width, height, {
        stream_allow_upscale: true,
        stream_match_display: true,
        stream_fallback_width: fallback ? fallback.width : 0,
        stream_fallback_height: fallback ? fallback.height : 0,
    });

describe('the host limit — a rung', () => {
    it('is the screen that capped a rung above it', () => {
        expect(hostScreenCaps(rung(1440), FHD, FHD)).toBe(true);
        expect(hostScreenCaps(rung(2160), QHD, QHD)).toBe(true);
        // "auto" aspect and a ratio chosen by hand: the host takes its screen's
        // shape either way, and the lines are what it cannot give.
        expect(hostScreenCaps(rung(1440, 'auto'), FHD, FHD)).toBe(true);
        expect(hostScreenCaps(rung(1440, '21:9'), FHD, FHD)).toBe(true);
    });

    it('says nothing of a rung the screen has', () => {
        expect(hostScreenCaps(rung(1080), FHD, FHD)).toBe(false);
        expect(hostScreenCaps(rung(1080), QHD, FHD)).toBe(false);
        // An ultrawide screen at its own lines streams its own width.
        expect(
            hostScreenCaps(
                rung(1440),
                { width: 3440, height: 1440 },
                { width: 3440, height: 1440 },
            ),
        ).toBe(false);
    });
});

describe('the host limit — a box (Auto, Custom)', () => {
    it("is the screen when the box, at the screen's shape, is larger", () => {
        // Auto on a 1440p screen, a 1080p host.
        expect(hostScreenCaps(box(2560, 1440), FHD, FHD)).toBe(true);
        // A 16:10 box whose width alone is more than the screen.
        expect(hostScreenCaps(box(2560, 1600), FHD, FHD)).toBe(true);
    });

    it("is the box's shape, not the screen, when the box is what limits", () => {
        // This screen's own size, smaller than the host's.
        expect(hostScreenCaps(box(1920, 1080), QHD, FHD)).toBe(false);
        // 1920×1200 holds a 16:9 screen to 1920×1080 by its width.
        expect(hostScreenCaps(box(1920, 1200), FHD, FHD)).toBe(false);
        // A 2560-wide box holds an ultrawide 2560×1080 screen to its own size.
        const ultrawide = { width: 2560, height: 1080 };
        expect(hostScreenCaps(box(2560, 1440), ultrawide, ultrawide)).toBe(false);
    });
});

describe('the host limit — "Match my screen" and a display made to order', () => {
    it('says nothing when the screen took the size asked', () => {
        expect(hostScreenCaps(match(2560, 1440, QHD), QHD, QHD)).toBe(false);
        // The virtual display, made at that size.
        expect(hostScreenCaps(match(3840, 2160, null), UHD, UHD)).toBe(false);
    });

    it("is the screen when it could not, and Auto's box is larger than it", () => {
        expect(hostScreenCaps(match(2560, 1440, QHD), FHD, FHD)).toBe(true);
        // A display made to order stops at its bounds.
        const bound = { width: 4096, height: 2304 };
        expect(hostScreenCaps(match(5120, 2880, null), bound, bound)).toBe(true);
    });

    it("says nothing when Auto's box fits the screen that could not switch", () => {
        const wuxga = { width: 1920, height: 1200 };
        expect(hostScreenCaps(match(1920, 1200, wuxga), QHD, FHD)).toBe(false);
    });

    it('never calls an upscale allowed without the mode a cap', () => {
        expect(hostScreenCaps(box(2560, 1440, { stream_allow_upscale: true }), FHD, FHD)).toBe(
            false,
        );
    });
});

describe('the host limit — the frame', () => {
    it("takes a frame an encoder's grid shaved as the screen's size", () => {
        // AV1's 64×16 blocks make 1920×1080 1792×1008.
        expect(hostScreenCaps(rung(1440), FHD, { width: 1792, height: 1008 })).toBe(true);
    });

    it('says nothing of a frame well under the screen, or of none yet', () => {
        expect(hostScreenCaps(rung(1440), FHD, { width: 1280, height: 720 })).toBe(false);
        expect(hostScreenCaps(rung(1440), FHD, null)).toBe(false);
    });

    it("says nothing without a screen, or for the host's own size", () => {
        expect(hostScreenCaps(rung(1440), { width: 0, height: 0 }, FHD)).toBe(false);
        expect(hostScreenCaps(rung(1440), null, FHD)).toBe(false);
        expect(hostScreenCaps(askedOf({ stream_height: 0 }), FHD, FHD)).toBe(false);
        expect(hostScreenCaps(null, FHD, FHD)).toBe(false);
    });
});

describe('what a launch asked, read as the server reads it', () => {
    it('takes the upscale, the mode and the fallback only with a box', () => {
        const over = {
            stream_height: 1440,
            stream_aspect: '2560:1440',
            stream_allow_upscale: true,
            stream_match_display: true,
            stream_fallback_width: 1921,
            stream_fallback_height: 1081,
        };
        expect(askedOf(over)).toEqual({
            height: 1440,
            aspect: '2560:1440',
            fitBox: false,
            allowUpscale: false,
            matchDisplay: false,
            fallback: null,
        });
        expect(askedOf({ ...over, stream_fit_box: true })).toEqual({
            height: 1440,
            aspect: '2560:1440',
            fitBox: true,
            allowUpscale: true,
            matchDisplay: true,
            fallback: { width: 1920, height: 1080 },
        });
        expect(askedOf(undefined)).toEqual({
            height: 0,
            aspect: null,
            fitBox: false,
            allowUpscale: false,
            matchDisplay: false,
            fallback: null,
        });
    });

    it('knows the screen of the native host only', () => {
        const settings = { stream_height: 1440, stream_aspect: '16:9' };
        expect(
            hostScreenOf({ native: false, display_width: 1920, display_height: 1080 }, settings),
        ).toBe(null);
        expect(hostScreenOf({ native: true }, settings)).toBe(null);
        expect(hostScreenOf(null, settings)).toBe(null);
        expect(
            hostScreenOf({ native: true, display_width: 1920, display_height: 1080 }, settings),
        ).toEqual({
            display: FHD,
            asked: askedOf(settings),
        });
    });
});

describe("StreamView — the stats card's resolution", () => {
    const stat = (view) => StreamView.prototype._resolutionStat.call(view);

    it('adds the host limit only when the native host capped the stream', () => {
        const capped = { display: { ...FHD }, asked: rung(1440) };
        expect(stat({ _resolution: '1920×1080', _hostScreen: capped })).toBe(
            'stream.statResolutionHostLimit',
        );
        expect(
            stat({
                _resolution: '1920×1080',
                _hostScreen: { display: { ...QHD }, asked: rung(1080) },
            }),
        ).toBe('1920×1080');
        expect(stat({ _resolution: '1920×1080', _hostScreen: null })).toBe('1920×1080');
        expect(stat({ _resolution: '', _hostScreen: capped })).toBe('?');
    });
});

describe('the words for the host limit, as Bruno chose them', () => {
    const words = (code) => {
        const path = fileURLToPath(new URL('../locales/' + code + '.json', import.meta.url));
        const s = JSON.parse(readFileSync(path, 'utf8')).stream;
        return s.statResolution + ' ' + s.statResolutionHostLimit.replace('{{size}}', '1920×1080');
    };

    it('reads the same in the three languages', () => {
        expect(words('fr')).toBe("Résolution : 1920×1080 (limite de l'hôte)");
        expect(words('en')).toBe('Resolution: 1920×1080 (host limit)');
        expect(words('zh').replace(' ', '')).toBe('分辨率：1920×1080（主机上限）');
    });
});
