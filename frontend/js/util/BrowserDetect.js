/*
 * MoonlightWeb — browser-based Sunshine/GameStream client.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * Browser platform detection — lightweight alternative to Bowser.
 *
 * Provides platform type (mobile, tablet, desktop) via User-Agent parsing,
 * matching the semantics of Bowser.parse(navigator.userAgent).platform.type.
 *
 * Used by StreamView to distinguish mobile/tablet from touchscreen laptops
 * (Surface, Chromebooks) for orientation-based fullscreen logic.
 */

/** @returns {{ type: 'mobile'|'tablet'|'desktop', isTouchDevice: boolean }} */
export function detectPlatform() {
    const ua = navigator.userAgent || '';
    const low = ua.toLowerCase();

    // --- Tablet detection (must come before mobile) ---

    // Phones are decided FIRST, even though tablets are matched first, because
    // the iPad test below cannot tell an iPhone from a Mac on its own.
    const isIphone = /iphone|ipod/i.test(ua);

    // iPad: "iPad" in UA, OR (Mac + touch support) for iPadOS 13+, which reports
    // a Mac's user agent and can only be told apart by the touch points.
    //
    // The iPhone has to be excluded explicitly: every iOS user agent says "like
    // Mac OS X", so /mac/ matches an iPhone as readily as a Mac, and an iPhone
    // has touch — the whole heuristic fired, and every iPhone was classified as
    // a tablet. Invisible for a long time because nothing distinguished the two,
    // until the pointer size started asking for phones only.
    const isIpad =
        /ipad/i.test(ua) ||
        (!isIphone && /mac/i.test(ua) && 'ontouchend' in document && navigator.maxTouchPoints > 1);

    // Android tablet: "Android" + no "Mobile" in UA
    const isAndroidTablet = /android/.test(low) && !/mobile/.test(low);

    // Kindle Fire / Silk tablet
    const isKindle = /kindle|silk/i.test(ua);

    // Windows tablet: "Touch" in UA on Windows (Surface in tablet mode)
    const isWindowsTablet = /windows/.test(low) && /touch/.test(low) && !/mobile/.test(low);

    if (isIpad || isAndroidTablet || isKindle || isWindowsTablet) {
        return {
            type: 'tablet',
            isTouchDevice: true,
        };
    }

    // --- Mobile detection ---

    const isAndroidPhone = /android/.test(low) && /mobile/.test(low);
    const isWindowsPhone = /windows phone|iemobile/i.test(ua);
    const isBlackberry = /blackberry|bb10/i.test(ua);

    if (isIphone || isAndroidPhone || isWindowsPhone || isBlackberry) {
        return {
            type: 'mobile',
            isTouchDevice: true,
        };
    }

    // Touchscreen laptop (Surface, Chromebook, etc.) — detected as desktop
    const isTouchDevice =
        'ontouchstart' in window ||
        (typeof navigator.maxTouchPoints !== 'undefined' && navigator.maxTouchPoints > 0);

    return {
        type: 'desktop',
        isTouchDevice: isTouchDevice,
    };
}

/** Singleton result — parsed once at module load time. */
const platform = detectPlatform();

/** True when the user agent is a mobile phone. */
export const IS_MOBILE = platform.type === 'mobile';

/** True when the user agent is a tablet. */
export const IS_TABLET = platform.type === 'tablet';

/** True when the user agent is a mobile phone or tablet. */
export const IS_MOBILE_OR_TABLET = platform.type === 'mobile' || platform.type === 'tablet';

/** True for iPhone / iPod. */
export function isIphone() {
    return /iPhone|iPod/i.test(navigator.userAgent);
}

/** True when the browser supports touch events (any touch-capable device). */
export const IS_TOUCH_DEVICE = platform.isTouchDevice;

/** The raw platform type string: 'mobile', 'tablet', or 'desktop'. */
export const PLATFORM_TYPE = platform.type;

/**
 * True on Apple touch devices (iPhone/iPad), including iPadOS 13+ which reports
 * a Mac UA. Used to surface the "Add to Home Screen" hint, since iOS blocks the
 * Fullscreen API on canvas — a standalone PWA is the only true-fullscreen path.
 */
export const IS_IOS = (() => {
    const ua = navigator.userAgent || '';
    return (
        /iphone|ipad|ipod/i.test(ua) ||
        (/mac/i.test(ua) && 'ontouchend' in document && navigator.maxTouchPoints > 1)
    );
})();

/** True when the user agent is Android (phone or tablet). */
export const IS_ANDROID = /android/i.test(navigator.userAgent || '');

/**
 * True on an Apple platform — macOS, iOS or iPadOS. Every one of them runs AWDL
 * (AirDrop, Handoff, AirPlay, Sidecar, Continuity), which shares the Wi-Fi radio
 * with the infrastructure link and periodically takes it away. That is the only
 * platform family where PeriodicStallDetector's verdict has actionable advice
 * attached, so it gates the hint rather than the detection itself.
 *
 * iPadOS 13+ reports a Mac UA, which IS_IOS already disambiguates by touch —
 * both answers are "Apple" here, so the union needs no such care.
 */
export const IS_APPLE = /mac|iphone|ipad|ipod/i.test(navigator.userAgent || '') || IS_IOS;

/**
 * Physical screen resolution in device pixels, orientation-independent.
 * screen.{width,height} are CSS pixels; multiplying by devicePixelRatio yields
 * physical pixels. We return both edges so callers can reason about the panel
 * class (e.g. the short edge is the panel's "p" rating: 1080 short edge = 1080p).
 * @returns {{ short: number, long: number }}
 */
export function physicalScreenSize() {
    const dpr = window.devicePixelRatio || 1;
    const w = (screen.width || 0) * dpr;
    const h = (screen.height || 0) * dpr;
    return { short: Math.round(Math.min(w, h)), long: Math.round(Math.max(w, h)) };
}

/**
 * Pick the auto Video-Enhancement upscaler for this device: 'fsr1' (sharper,
 * three passes) on a desktop, 'sgsr' (one light pass) on a phone or tablet.
 * Decided 03/09/2026 with the SDR matrix: the platform class is the whole
 * rule — a phone's GPU budget is spent on the decode, whatever its core count.
 * Which API runs it (WebGL2 in SDR, WebGPU in HDR) is StreamView's call.
 * @returns {'fsr1'|'sgsr'}
 */
export function pickAutoEnhancer() {
    return PLATFORM_TYPE === 'desktop' ? 'fsr1' : 'sgsr';
}

/**
 * True when the browser can actually present frames without VSync: a Canvas2D
 * context created with { desynchronized: true } bypasses the compositor swap
 * chain, allowing tearing for lower latency. Only Chromium on desktop
 * implements it (Chrome/Edge); Safari and Firefox ignore the flag, and mobile
 * compositors always re-synchronize. "Chrome/" in the UA covers the Chromium
 * family and is absent from Safari ("CriOS" on iOS) and Firefox.
 */
export const SUPPORTS_CANVAS_TEARING =
    platform.type === 'desktop' && /chrome\/\d+/i.test(navigator.userAgent || '');

/**
 * Resolve the effective "allow tearing" preference from a saved settings blob.
 *
 * Tearing is the DEFAULT wherever the platform can actually tear (Chromium
 * desktop): frames are presented on decode instead of waiting for the next
 * compositor swap. Everywhere else the flag is a no-op, so it is forced off and
 * the render loop keeps its VSync pacing.
 *
 * `tearing_default_v2` marks a value the user actually chose under that default.
 * Anything saved before it — a `tearing_enabled` written while the default was
 * OFF, or the even older inverted `vsync_enabled` — records an old default, not
 * a decision, so the new default wins once. Every save from the Settings overlay
 * carries the marker from now on and is honoured as-is, tearing off included.
 */
export function resolveTearing(settings) {
    if (!SUPPORTS_CANVAS_TEARING) return false;
    const s = settings || {};
    return s.tearing_default_v2 !== true || s.tearing_enabled === true;
}

/**
 * True when the display this window sits on is in an HDR mode right now.
 *
 * A live property, not a capability: Windows with HDR switched off, or a
 * laptop moved to an SDR screen, answers false. Evaluated on demand for that
 * reason (the settings page and the launch both ask at their own moment).
 * Main thread only — matchMedia does not exist in workers.
 */
export function supportsDisplayHdr() {
    try {
        return window.matchMedia('(dynamic-range: high)').matches;
    } catch (e) {
        return false;
    }
}

/** 10-bit profiles an HDR stream arrives in: HEVC Main10, then AV1 10-bit. */
const HDR_DECODE_PROBES = ['hvc1.2.4.L153.B0', 'hev1.2.4.L153.B0', 'av01.0.08M.10'];
let hdrStaticProbe = null;

/**
 * Whether THIS client can show an HDR stream, part by part: the display is in
 * an HDR mode right now, WebGPU has an adapter (the only renderer with an HDR
 * surface), and the browser decodes a 10-bit profile. `ok` is the three
 * together — what the HDR checkbox and the launch gate both key on.
 *
 * The display answer is live (see supportsDisplayHdr); the other two are
 * probed once per page, they do not change under us.
 * @returns {Promise<{display: boolean, webgpu: boolean, decode: boolean, ok: boolean}>}
 */
export async function hdrClientCapability() {
    if (!hdrStaticProbe) {
        hdrStaticProbe = (async () => {
            let webgpu = false;
            try {
                webgpu = !!(navigator.gpu && (await navigator.gpu.requestAdapter()));
            } catch (e) {
                webgpu = false;
            }
            let decode = false;
            if (
                typeof VideoDecoder !== 'undefined' &&
                typeof VideoDecoder.isConfigSupported === 'function'
            ) {
                for (const codec of HDR_DECODE_PROBES) {
                    try {
                        const r = await VideoDecoder.isConfigSupported({ codec });
                        if (r && r.supported) {
                            decode = true;
                            break;
                        }
                    } catch (e) {
                        // This string is refused outright — try the next profile.
                    }
                }
            }
            return { webgpu, decode };
        })();
    }
    const probe = await hdrStaticProbe;
    const display = supportsDisplayHdr();
    return {
        display,
        webgpu: probe.webgpu,
        decode: probe.decode,
        ok: display && probe.webgpu && probe.decode,
    };
}

/** True when the app runs as an installed PWA (no browser chrome). */
export const IS_STANDALONE =
    window.navigator.standalone === true ||
    (window.matchMedia &&
        (window.matchMedia('(display-mode: standalone)').matches ||
            window.matchMedia('(display-mode: fullscreen)').matches));
