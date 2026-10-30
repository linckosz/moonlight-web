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

/**
 * True for a television or a TV box: Android TV, Google TV, Fire TV, a set
 * top box. Its browser says "Android" and, as often as not, "Mobile" too —
 * a TV Bro WebView on a Mi TV reads "Android 11; MiTV-MOSR4 … Mobile Safari"
 * — so the platform classes it a phone, and a phone's Auto box (1440 lines,
 * any width) asked a 720p Mi TV for 2148×1208, beyond its hardware decoder
 * (01/10/2026): no picture at all.
 *
 * What tells a TV from a phone is the screen, not the user agent: every
 * Android phone and tablet has a touchscreen and reports its touch points,
 * a TV has none (maxTouchPoints 0, measured on that Mi TV in TV Bro and in a
 * Chromium build alike). A few vendors also say it outright in the agent.
 * The size decisions read this, and so does everything built for a finger
 * (IS_TOUCH_DEVICE): a TV is driven by a remote or a pad, never by touch.
 * For the rest it goes through the same pages a phone does.
 * @param {Navigator} [nav]
 */
export function detectTv(nav) {
    const n = nav || (typeof navigator !== 'undefined' ? navigator : null);
    if (!n) return false;
    const ua = n.userAgent || '';
    if (/\b(android ?tv|google ?tv|smart-?tv|bravia|mitv)\b/i.test(ua)) return true;
    // Fire TV models: "AFTMM", "AFTKA"… — upper case, so "after" is not one.
    if (/\bAFT[A-Z0-9]{1,6}\b/.test(ua)) return true;
    return /android/i.test(ua) && n.maxTouchPoints === 0;
}

/** True on a television (see detectTv). */
export const IS_TV = detectTv();

/**
 * True when the browser takes touch input: a phone, a tablet, a touchscreen
 * laptop. Not a TV, though its user agent reads like a phone's: what a finger
 * would do there (the keyboard toggle, the gesture help, the touch modes) no
 * remote can reach.
 */
export const IS_TOUCH_DEVICE = platform.isTouchDevice && !IS_TV;

/**
 * True when the stream's size should be chosen as for a phone or a tablet —
 * a screen held in the hand, whose Auto box and bitrate estimate are the
 * handheld ones. A TV is neither, whatever its user agent says.
 */
export const IS_HANDHELD = IS_MOBILE_OR_TABLET && !IS_TV;

/**
 * True on a device with a gigabyte of memory or less, as the browser reports
 * it (Chromium only; elsewhere unknown, so false). Chromium rounds DOWN to a
 * power of two, so a 3 GB phone says 2 and stays out of this; a 1.4 GB Mi TV
 * says 1. Its GPU is of the same class (a Mali-G31 there): Auto's frame rate
 * is held lower (RefreshRate.js), and nothing optional is turned on for it
 * behind the viewer's back.
 * @param {Navigator} [nav]
 */
export function isLowMemory(nav) {
    const n = nav || (typeof navigator !== 'undefined' ? navigator : null);
    // Chromium only, absent from the DOM typings.
    const gb = n && /** @type {any} */ (n).deviceMemory;
    return typeof gb === 'number' && gb > 0 && gb <= 1;
}

export const IS_LOW_MEMORY = isLowMemory();

/**
 * True when mouse gaming mode (pointer lock + relative mouse) makes sense: no
 * touch screen at all, or one alongside a mouse or trackpad — a Surface with
 * its type cover, a touchscreen laptop. A touch screen alone is not a reason
 * to take the mode away: `maxTouchPoints > 0` is true on those PCs too, and
 * gating on it hid the option from every one of them (issue #16). Phones stay
 * out. Asked at call time, so a mouse plugged in later counts.
 */
export function supportsGamingMode() {
    if (!platform.isTouchDevice) return true;
    if (platform.type === 'mobile') return false;
    try {
        return !!window.matchMedia && window.matchMedia('(any-pointer: fine)').matches;
    } catch {
        return false;
    }
}

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
 * Whether this browser's video decoder decodes a delta whose reference never
 * arrived — patching over the hole — instead of failing on it. The "ride out"
 * bargain rests on that (RIDE_OUT_LOSS in BackendClient): the native host then
 * drops a delta on a saturated link without asking for a keyframe, and its
 * intra-refresh wave repairs what the decoder patched over.
 *
 * Not on an Apple platform. Every WebCodecs H.264 and HEVC stream there is
 * decoded by VideoToolbox, which rejects that delta ("Decoding error"). Seen
 * 28/09/2026 from a Mac (Chrome, over the Internet, an Arc host on oneVPL with
 * intra-refresh): three stalls in three minutes, each one a decoder error, a
 * new decoder and a keyframe, 60 to 110 ms frozen — the keyframe the bargain
 * was meant to spare came anyway, one error later. Windows decoders patch over
 * the hole and carry on, which is what the bargain was measured on.
 *
 * No API tells one decoder from the other, hence the user agent. iOS and
 * iPadOS decode on the same VideoToolbox whatever the browser, and iPadOS's
 * Mac user agent reads as Apple here too. "AppleWebKit", in nearly every user
 * agent, does not.
 *
 * Nor on a device whose decoder was seen going silent under the wave: no
 * error, every chunk taken and not one picture given back, for good. The
 * Freebox Player POP does that (Amlogic, Android TV 10, 01/10/2026) where
 * another Android TV, on MediaTek, rides the same stream out. Nothing in the
 * user agent tells those apart, so the stream finds out (SilentDecoderWatch)
 * and the device keeps the verdict (noteDecoderCannotRideOut).
 * @param {string} [ua]
 * @param {Storage|null} [store] where that verdict is kept
 * @returns {boolean}
 */
export function decoderRidesOutGaps(
    ua = (typeof navigator !== 'undefined' && navigator.userAgent) || '',
    store = rideOutStore(),
) {
    if (/mac|iphone|ipad|ipod/i.test(ua)) return false;
    try {
        return !store || store.getItem(RIDE_OUT_VERDICT_KEY) !== 'off';
    } catch (e) {
        return true;
    }
}

/** This device's verdict on the wave, set once its decoder failed under one. */
const RIDE_OUT_VERDICT_KEY = 'mw_ride_out';

/** localStorage, or null where reading it throws (sandboxed frame, privacy mode). */
function rideOutStore() {
    try {
        return typeof localStorage !== 'undefined' ? localStorage : null;
    } catch (e) {
        return null;
    }
}

/**
 * Remember that this device's decoder cannot ride a gap out: every later
 * launch asks the host for keyframes instead (decoderRidesOutGaps).
 * @param {Storage|null} [store]
 */
export function noteDecoderCannotRideOut(store = rideOutStore()) {
    try {
        if (store) store.setItem(RIDE_OUT_VERDICT_KEY, 'off');
    } catch (e) {
        // Not kept: the next launch finds out again, a relaunch later.
    }
}

/** This device's verdict on the encoder's reference repairs. */
const REF_REPAIRS_VERDICT_KEY = 'mw_ref_repairs';

/**
 * Whether this device's decoder follows the native host's reference repairs:
 * long-term references marked every frame or two (AMF, oneVPL, D3D12 and
 * Vulkan Video), an invalidated DPB (NVENC), all for healing a lost frame by a
 * delta. Every decoder met so far does — but the Freebox Player POP's falls
 * silent under AMF's at 30 fps and under, without a word, as it does under the
 * wave (01/10/2026). Nothing tells it in advance: the stream finds out, the
 * device keeps the verdict, and /start then asks for none (`ref_invalidation`):
 * one reference, a keyframe for a loss.
 *
 * Nor on an Apple platform, for the reason it rides nothing out: a lost frame
 * is healed by the host only once it hears of it, and the deltas already on
 * their way still predict from the frame that never came. VideoToolbox rejects
 * the first of them. Seen 19/10/2026 from a Mac (Chrome, office Wi-Fi, an N95
 * host on QuickSync): every one of seven single-frame gaps was a "Decoding
 * error" on the next frame, a new decoder and a keyframe anyway.
 * @param {string} [ua]
 * @param {Storage|null} [store]
 * @returns {boolean}
 */
export function decoderTakesReferenceRepairs(
    ua = (typeof navigator !== 'undefined' && navigator.userAgent) || '',
    store = rideOutStore(),
) {
    if (/mac|iphone|ipad|ipod/i.test(ua)) return false;
    try {
        return !store || store.getItem(REF_REPAIRS_VERDICT_KEY) !== 'off';
    } catch (e) {
        return true;
    }
}

/**
 * Whether this view decodes on through a gap, naming the lost frames to the
 * host: only when the host says its encoder heals them with a delta
 * (`ref_invalidation` in /start's answer) AND this device takes such repairs.
 * The host's answer alone was not enough: told that a Mac takes none, an NVENC
 * host still kept its invalidation (one reference instead of several) and
 * said so, and the Mac decoded on. VideoToolbox then rejected the deltas
 * already on their way: 32 "Decoding error" in eight minutes on the M3
 * (25/10/2026), each a new decoder and a keyframe anyway. Where the device
 * takes none, a gap drops the deltas until a keyframe, asked for at once.
 * @param {boolean} hostHeals
 * @param {string} [ua]
 * @param {Storage|null} [store]
 * @returns {boolean}
 */
export function decodesThroughGaps(hostHeals, ua, store) {
    return hostHeals === true && decoderTakesReferenceRepairs(ua, store);
}

/** @param {Storage|null} [store] */
export function noteDecoderRefusesReferenceRepairs(store = rideOutStore()) {
    try {
        if (store) store.setItem(REF_REPAIRS_VERDICT_KEY, 'off');
    } catch (e) {
        // Not kept: found out again on the next launch.
    }
}

/** The browsers seen handing their HDR frames opaque: user agent → when (ms). */
const OPAQUE_HDR_KEY = 'mw_hdr_opaque';

/** How long such a verdict holds. Past it, the next launch looks again. */
export const OPAQUE_HDR_DAYS = 30;
const OPAQUE_HDR_MS = OPAQUE_HDR_DAYS * 24 * 3600 * 1000;

/** @param {Storage|null} store @returns {Object<string, number>} */
function opaqueHdrVerdicts(store) {
    try {
        const kept = store && JSON.parse(store.getItem(OPAQUE_HDR_KEY) || '{}');
        return kept && typeof kept === 'object' ? kept : {};
    } catch (e) {
        return {};
    }
}

/**
 * Whether this browser was seen handing its AV1 HDR frames opaque: decoded
 * in hardware whatever the decoder was asked (`prefer-software`), with no
 * format and no copyTo. The HDR readback (the linear HDR canvas, the ACES
 * tone-map) then has nothing to read, and each frame goes through the
 * browser's own import instead: washed out in Chrome 126, its chroma broken
 * in Safari 27, both on the same MacBook Air M3 (26/10/2026). Safari's was
 * reproduced on a page with no MoonlightWeb in it: its 2D canvas and its
 * <video> draw the same frame right, its WebGPU import does not.
 *
 * Nothing tells it before a frame is decoded: the stream finds out on its
 * first one and the browser keeps the verdict, under its user agent (a new
 * version is asked again) and for OPAQUE_HDR_DAYS at most.
 * @param {string} [ua]
 * @param {Storage|null} [store]
 * @param {number} [now] ms since the epoch
 * @returns {boolean}
 */
export function hdrFramesOpaque(
    ua = (typeof navigator !== 'undefined' && navigator.userAgent) || '',
    store = rideOutStore(),
    now = Date.now(),
) {
    const seen = opaqueHdrVerdicts(store)[ua];
    // A date ahead of the clock (the clock set back) holds no longer either.
    return typeof seen === 'number' && now >= seen && now - seen < OPAQUE_HDR_MS;
}

/**
 * Remember it (hdrFramesOpaque), dropping the verdicts past their time.
 * @param {string} [ua]
 * @param {Storage|null} [store]
 * @param {number} [now]
 */
export function noteHdrFramesOpaque(
    ua = (typeof navigator !== 'undefined' && navigator.userAgent) || '',
    store = rideOutStore(),
    now = Date.now(),
) {
    if (!store) return;
    try {
        const kept = {};
        for (const [k, seen] of Object.entries(opaqueHdrVerdicts(store))) {
            if (typeof seen === 'number' && now >= seen && now - seen < OPAQUE_HDR_MS)
                kept[k] = seen;
        }
        kept[ua] = now;
        store.setItem(OPAQUE_HDR_KEY, JSON.stringify(kept));
    } catch (e) {
        // Not kept: the next launch finds out again, a relaunch later.
    }
}

/**
 * True on the WebKit engine: Safari on macOS, and every browser on iOS/iPadOS
 * (they all wrap WebKit, whatever their name). Chromium and Gecko on a Mac are
 * excluded by the Chrome/Firefox tokens.
 *
 * Read by the HDR routing: WebKit's importExternalTexture returns the frame's
 * raw PQ code values instead of tone-mapping them into the destination color
 * space, so an HDR stream blitted as-is renders washed-out grey. There is no
 * feature test for that (the call succeeds either way), hence the engine check.
 */
export const IS_WEBKIT = (() => {
    const ua = navigator.userAgent || '';
    if (IS_IOS) return true;
    return /safari/i.test(ua) && !/chrome|chromium|crios|android|firefox|fxios|edg/i.test(ua);
})();

/**
 * Whether an HDR stream without a readback takes the external-texture
 * tone-map ('tonemap-ext' in StreamView's HDR routing): on WebKit, for the
 * reason above. Dev: mw_hdr_ext_tonemap '1' / '0' forces it on or off.
 * @returns {boolean}
 */
export function hdrExtTonemapRoute() {
    let ext = !!navigator.gpu && IS_WEBKIT;
    try {
        const dev = localStorage.getItem('mw_hdr_ext_tonemap');
        if (dev === '1') ext = !!navigator.gpu;
        else if (dev === '0') ext = false;
    } catch (e) {}
    return ext;
}

/** A host's codec mode bit for AV1 Main 10 (moonlight-common-c, SCM_AV1_MAIN10). */
export const SCM_AV1_MAIN10 = 0x20000;

let av1SoftwareProbe = null;

/**
 * Whether this browser decodes 10-bit AV1 in software (dav1d, in Chromium):
 * the frames the HDR readback route reads back. Asked once.
 * @returns {Promise<boolean>}
 */
export function av1HdrSoftwareDecode() {
    if (!av1SoftwareProbe) {
        av1SoftwareProbe = (async () => {
            try {
                if (typeof VideoDecoder === 'undefined' || !VideoDecoder.isConfigSupported)
                    return false;
                const r = await VideoDecoder.isConfigSupported({
                    codec: 'av01.0.08M.10',
                    hardwareAcceleration: 'prefer-software',
                });
                return !!(r && r.supported);
            } catch (e) {
                return false;
            }
        })();
    }
    return av1SoftwareProbe;
}

/**
 * The codec an HDR launch asks for, given the routes each one leads to here.
 * - AV1 in a browser seen handing those frames opaque (hdrFramesOpaque), where
 *   the route left without a readback imports them for the tone-map (WebKit,
 *   which breaks their chroma and takes HEVC's right): HEVC. The relaunch of
 *   the first time is spared (StreamView._relaunchWithoutReadback).
 * - The HEVC of « Auto » (util/CodecAuto.js) with the enhancer asked for: no
 *   browser here reads an HEVC frame back (no software HEVC decoder), so the
 *   stream would end on the <video> sink, its enhancer dropped, or on a pale
 *   import. AV1 takes the readback route and keeps the enhancer, in HDR, where
 *   the host encodes AV1 10-bit, this browser decodes it in software and
 *   WebGPU is there. Not on WebKit, whose AV1 frames are opaque, nor in a
 *   browser already seen so. A HEVC chosen in the menu stays HEVC. Measured
 *   on DualRTX (26/10/2026): AV1 with MWSR no slower click to photon than the
 *   HEVC <video>, for about one CPU core more.
 * @param {string} codec the codec asked for ('av1', 'hevc', 'h264', …)
 * @param {boolean} hdr
 * @param {{auto?: boolean, enhanced?: boolean, hostAv1Hdr?: boolean, av1Software?: boolean}} [opts]
 *        the HEVC is Auto's; the enhancer asked for; the host encodes AV1
 *        Main 10 (SCM_AV1_MAIN10); av1HdrSoftwareDecode's answer
 * @returns {string}
 */
export function hdrLaunchCodec(codec, hdr, opts = {}) {
    if (!hdr) return codec;
    if (codec === 'av1') return hdrExtTonemapRoute() && hdrFramesOpaque() ? 'hevc' : codec;
    const { auto = false, enhanced = false, hostAv1Hdr = false, av1Software = false } = opts;
    if (
        codec === 'hevc' &&
        auto &&
        enhanced &&
        hostAv1Hdr &&
        av1Software &&
        !!navigator.gpu &&
        !hdrExtTonemapRoute() &&
        !hdrFramesOpaque()
    )
        return 'av1';
    return codec;
}

/**
 * True on the Gecko engine: Firefox on a desktop or on Android. Firefox on iOS
 * ("FxiOS") wraps WebKit and is not Gecko.
 *
 * Read by the renderer choice: Gecko draws a decoded VideoFrame onto a 2D
 * canvas through a synchronous copy, and WebGL takes the same frame for a
 * fraction of it (see geckoPresentsOnPlainWebGl).
 */
export const IS_GECKO = (() => {
    const ua = navigator.userAgent || '';
    return !IS_IOS && /\bfirefox\//i.test(ua);
})();

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

let rendererProbe = null;

/**
 * The WebGL renderer string of the GPU drawing this page, '' when WebGL is
 * missing or the string is masked.
 *
 * Asked of WebGL, not of the user agent: Chrome on Windows-on-ARM presents
 * itself as "Windows NT 10.0; Win64; x64" and `mobile: false` (measured on the
 * ARM bench, 06/09/2026), so nothing the platform detection reads tells a
 * Snapdragon laptop from an x64 one. The renderer string does — "ANGLE
 * (Qualcomm, Qualcomm(R) Adreno(TM) 618 GPU …)" on that bench, "Mali-G31" on
 * the Mi TV — and the GPU is the thing the answers below are about anyway.
 * Probed once per page: the GPU does not change under us.
 * @returns {string}
 */
export function gpuRendererString() {
    if (rendererProbe === null) {
        rendererProbe = '';
        try {
            const canvas = document.createElement('canvas');
            const gl = canvas.getContext('webgl2') || canvas.getContext('webgl');
            if (gl) {
                const ext = gl.getExtension('WEBGL_debug_renderer_info');
                rendererProbe = String(
                    gl.getParameter(ext ? ext.UNMASKED_RENDERER_WEBGL : gl.RENDERER) || '',
                );
                const lose = gl.getExtension('WEBGL_lose_context');
                if (lose) lose.loseContext();
            }
        } catch (e) {
            rendererProbe = '';
        }
    }
    return rendererProbe;
}

/** True when the GPU drawing this page is a Qualcomm Adreno — a Snapdragon. */
export function isSnapdragonGpu() {
    return /adreno|qualcomm/i.test(gpuRendererString());
}

/**
 * True on an entry-level Mali: the Bifrost G31, G51 and G52, the Valhall G57
 * and G310, and the older Midgard T series. The realme Note 60's Mali-G57 MP1
 * is the class's measured member: the ladder cut the pass within 2 s at ×1.33
 * in both flavours (journal J28), and its plain presenter is WebGPU's
 * (plainPresenterIsWebGpu). The larger Malis (G68, G7x, G610 and up,
 * Immortalis) are not in it: no bench has seen one, and they take the
 * handheld's upscaler. A masked string answers false.
 */
export function isEntryMaliGpu() {
    return /\bmali-(t\d+|g(31|51|52|57|310)\b)/i.test(gpuRendererString());
}

/**
 * Lab key, no UI, main thread only: localStorage.mw_mwsr_gaze = '1' turns on
 * the mwsr kernel's gaze zone (docs/design/upscaler-mwsr.md §15.2: a sharper
 * centre, the table's values at the border) on a phone or a tablet, where its
 * weight follows the picture's motion (stream/GazeZone.js); never on a
 * desktop client, which keeps one quality over the whole screen (Bruno,
 * 23/10/2026). Off by default until his eye on his iPhone settles the zone's
 * values and its default; where storage is refused, nothing changes.
 * @returns {boolean}
 */
export function mwsrGazeWanted() {
    if (PLATFORM_TYPE === 'desktop') return false;
    try {
        return localStorage.getItem('mw_mwsr_gaze') === '1';
    } catch (e) {
        return false;
    }
}

/**
 * Lab key, no UI: localStorage.mw_mwsr_map = '1' asks the native host, at
 * /start, for MWSR's sharpness by zone (docs/design/upscaler-mwsr-b.md §12 c,
 * Bruno's decision of 30/10/2026): every frame then carries its grid
 * (stream/MwsrZoneBlock.js). Off by default until the host's cost is measured
 * and Bruno says yes; other hosts ignore the request. Where storage is
 * refused, nothing is asked.
 * @returns {boolean}
 */
export function mwsrMapWanted() {
    try {
        return localStorage.getItem('mw_mwsr_map') === '1';
    } catch (e) {
        return false;
    }
}

function hasWebGpu() {
    return typeof navigator !== 'undefined' && !!navigator.gpu;
}

/**
 * Pick the auto Video-Enhancement pass for this device, its flavour included:
 * MoonlightWeb's own upscaler, `mwsr` (docs/design/upscaler-mwsr.md), the
 * default on every platform for v0.4.1 (Bruno, 24/10/2026), by the device
 * classes he decided on 22/10 (plan §9):
 *  - an entry-level Mali (isEntryMaliGpu) gets none;
 *  - HDR runs it on WebGPU, the one renderer with an HDR surface;
 *  - an iPhone or an iPad runs it on WebGPU where WebKit has it: WebKit's
 *    texImage2D upload costs 3-24 ms of main thread per frame, WebGPU drew in
 *    0.25-0.53 × WebGL2's time and held 60 fps where WebGL2 dropped to 56-58
 *    (journals J23-J28);
 *  - everything else runs it on WebGL2: PC and Mac (WebGL2 won the
 *    click-to-photon runs in a window, 03/09/2026), a Snapdragon in place of
 *    SGSR1 (0.63-0.77 × SGSR1's time on the Adreno 618), and the mobile GPUs
 *    no bench has seen, which ran SGSR1 before: mwsr cost less than SGSR1 on
 *    every GPU measured.
 * The low-memory and TV rules come first, in StreamView.
 * @param {{hdr?: boolean}} [opts]
 * @returns {'off'|'mwsr'|'gl-mwsr'}
 */
export function pickAutoEnhancer({ hdr = false } = {}) {
    if (isEntryMaliGpu()) return 'off';
    if (hdr) return 'mwsr';
    if (IS_IOS && hasWebGpu()) return 'mwsr';
    return 'gl-mwsr';
}

/**
 * True when this device's plain presenter — no upscaler, the Enhancer on or
 * off — is WebGPU rather than Canvas2D: an entry-level Mali (Bruno,
 * 22/10/2026, plan §9), the one exception to the Canvas2D default of
 * 03/09/2026. At the realme Note 60's photon WebGPU presented 53-55 frames a
 * second of a 1080p stream where Canvas2D gave 30, equal at 960×540, with the
 * same response to a frame (journal J28). Without WebGPU, Canvas2D stays.
 * @returns {boolean}
 */
export function plainPresenterIsWebGpu() {
    return isEntryMaliGpu() && hasWebGpu();
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
 * True when tearing, once on, is real: a frame drawn is on the screen at once,
 * mid-refresh. Chromium on Windows. Chromium on macOS asks for the same canvas
 * and its compositor still shows each frame at the next vsync; on Linux it
 * depends on the desktop and was never measured. The native host opens its
 * admission window after a press only for such a client (bench §8w.9: a
 * client on its vsync gained nothing from it and stepped over pictures).
 */
export const CANVAS_TEARING_IS_REAL =
    SUPPORTS_CANVAS_TEARING && /windows nt/i.test(navigator.userAgent || '');

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

/**
 * 8-bit 4:4:4 profiles a chroma 4:4:4 stream arrives in, per codec: HEVC Range
 * Extensions (general_profile_idc 4 — the same profile a 10-bit 4:4:4 stream
 * uses, the SPS tells them apart), and H.264 High 4:4:4 Predictive (profile
 * 244, 0xF4).
 */
export const CHROMA444_DECODE_PROBES = {
    hevc: ['hvc1.4.10.L153.B0', 'hev1.4.10.L153.B0', 'hvc1.4.10.L123.B0'],
    h264: ['avc1.F4002A', 'avc1.F40028'],
};
let chroma444Probe = null;

/**
 * Which codecs THIS browser decodes a 4:4:4 stream in.
 *
 * Measured 03/09/2026: Chrome 152 on Windows answers false for every RExt
 * string (no 4:4:4 DXVA GUID on Chromium's D3D11 path) while Chrome on macOS
 * answers true and shows the picture. Asked for the host anyway, the stream
 * came up, failed three decodes and fell back to 4:2:0 H.264 three seconds
 * later — so the settings page greys the box out when neither codec can, and
 * the launch asks the one that can (see launchApp).
 *
 * Per codec, because one "yes" said nothing about the other: until 25/09/2026
 * an H.264 4:4:4 answer let an HEVC 4:4:4 stream be asked of a browser that
 * decodes no HEVC 4:4:4, and it ended in H.264 4:2:0 two seconds later. At the
 * size the decoder is configured at (StreamView), since a level is a size.
 * Probed once per page: a decoder does not change under us. A browser without
 * isConfigSupported is trusted, as for HDR.
 * @returns {Promise<{decode: boolean, hevc: boolean, h264: boolean}>}
 */
export async function chroma444ClientCapability() {
    if (!chroma444Probe) {
        chroma444Probe = (async () => {
            if (
                typeof VideoDecoder === 'undefined' ||
                typeof VideoDecoder.isConfigSupported !== 'function'
            ) {
                return { decode: true, hevc: true, h264: true };
            }
            const decodes = async (strings) => {
                for (const codec of strings) {
                    try {
                        const r = await VideoDecoder.isConfigSupported({
                            codec,
                            codedWidth: 1920,
                            codedHeight: 1080,
                        });
                        if (r && r.supported) return true;
                    } catch (e) {
                        // This string is refused outright — try the next one.
                    }
                }
                return false;
            };
            const hevc = await decodes(CHROMA444_DECODE_PROBES.hevc);
            const h264 = await decodes(CHROMA444_DECODE_PROBES.h264);
            return { decode: hevc || h264, hevc, h264 };
        })();
    }
    return chroma444Probe;
}

/**
 * The codec a 4:4:4 launch should ask for, given the one chosen and what this
 * browser decodes in 4:4:4 (chroma444ClientCapability).
 *
 * The chosen codec when it can; H.264 instead of HEVC when only H.264 can,
 * because that keeps the 4:4:4 the viewer asked for (the HEVC stream would end
 * in H.264 4:2:0 anyway); null when 4:4:4 cannot be had in the chosen codec. AV1
 * is left to the host, which says whether it encodes it — asked, as before, of
 * a browser that decodes 4:4:4 at all.
 * @param {string|undefined} codec 'hevc' (the default), 'h264' or 'av1'
 * @param {{decode: boolean, hevc: boolean, h264: boolean}} cap
 * @returns {string|null}
 */
export function chroma444Codec(codec, cap) {
    const c = codec || 'hevc';
    if (c === 'hevc') return cap.hevc ? 'hevc' : cap.h264 ? 'h264' : null;
    if (c === 'h264') return cap.h264 ? 'h264' : null;
    return cap.decode ? c : null;
}

/** 8-bit HEVC (Main) at the sizes a guest's picture comes in. */
const HEVC_DECODE_PROBES = ['hev1.1.6.L123.B0', 'hvc1.1.6.L123.B0', 'hvc1.1.6.L153.B0'];
let hevcProbe = null;

/**
 * Whether THIS browser decodes HEVC. A guest of a native host watches the one
 * stream its guests share, HEVC unless one of them cannot decode it — then the
 * whole stream goes H.264. Asked before joining, so a browser without HEVC
 * (Firefox, Chrome on Linux) asks for H.264 at once rather than failing its
 * first picture and coming back. Probed once per page; a browser that cannot be
 * asked is trusted, as for HDR and 4:4:4.
 * @returns {Promise<boolean>}
 */
export async function hevcClientDecodes() {
    if (!hevcProbe) {
        hevcProbe = (async () => {
            if (
                typeof VideoDecoder === 'undefined' ||
                typeof VideoDecoder.isConfigSupported !== 'function'
            ) {
                return true;
            }
            for (const codec of HEVC_DECODE_PROBES) {
                try {
                    const r = await VideoDecoder.isConfigSupported({
                        codec,
                        codedWidth: 1920,
                        codedHeight: 1080,
                    });
                    if (r && r.supported) return true;
                } catch (e) {
                    // This string is refused outright — try the next one.
                }
            }
            return false;
        })();
    }
    return hevcProbe;
}

/** True when the app runs as an installed PWA (no browser chrome). */
export const IS_STANDALONE =
    window.navigator.standalone === true ||
    (window.matchMedia &&
        (window.matchMedia('(display-mode: standalone)').matches ||
            window.matchMedia('(display-mode: fullscreen)').matches));
