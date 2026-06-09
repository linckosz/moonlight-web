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

    // iPad: "iPad" in UA, OR (Mac + touch support) for iPadOS 13+
    const isIpad = /ipad/i.test(ua) ||
        (/mac/i.test(ua) && 'ontouchend' in document && navigator.maxTouchPoints > 1);

    // Android tablet: "Android" + no "Mobile" in UA
    const isAndroidTablet = /android/.test(low) && !/mobile/.test(low);

    // Kindle Fire / Silk tablet
    const isKindle = /kindle|silk/i.test(ua);

    // Windows tablet: "Touch" in UA on Windows (Surface in tablet mode)
    const isWindowsTablet = /windows/.test(low) && /touch/.test(low) && !/mobile/.test(low);

    if (isIpad || isAndroidTablet || isKindle || isWindowsTablet) {
        return {
            type: 'tablet',
            isTouchDevice: true
        };
    }

    // --- Mobile detection ---

    const isIphone = /iphone|ipod/i.test(ua);
    const isAndroidPhone = /android/.test(low) && /mobile/.test(low);
    const isWindowsPhone = /windows phone|iemobile/i.test(ua);
    const isBlackberry = /blackberry|bb10/i.test(ua);

    if (isIphone || isAndroidPhone || isWindowsPhone || isBlackberry) {
        return {
            type: 'mobile',
            isTouchDevice: true
        };
    }

    // Touchscreen laptop (Surface, Chromebook, etc.) — detected as desktop
    const isTouchDevice = 'ontouchstart' in window ||
        (typeof navigator.maxTouchPoints !== 'undefined' && navigator.maxTouchPoints > 0);

    return {
        type: 'desktop',
        isTouchDevice: isTouchDevice
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

/** True when the browser supports touch events (any touch-capable device). */
export const IS_TOUCH_DEVICE = platform.isTouchDevice;

/** The raw platform type string: 'mobile', 'tablet', or 'desktop'. */
export const PLATFORM_TYPE = platform.type;
