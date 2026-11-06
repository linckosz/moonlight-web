/*
 * MoonlightWeb. Copyright (C) 2026 Bruno Martin. GPLv3.
 */

/**
 * The administrator's ceilings (settings.json max_allowed_frame_rate,
 * max_allowed_bitrate, max_allowed_stream_resolution — issue #53), as the
 * server states them in GET /api/settings/streaming. 0 is no limit.
 *
 * The server holds every launch to them whatever the page asks
 * (AppSettings::limitStreamSize, /start); the page holds itself to them too,
 * so that Settings offers nothing above them, and the estimate, the labels
 * and the stats speak of the stream that will really be sent.
 *
 * Held for this page only, read from the server before each launch and
 * whenever Settings opens (no import of the client here: RefreshRate reads
 * these, and the client reads RefreshRate): a ceiling the administrator just wrote reaches the next
 * launch without a reload, and a page that could not read them sends what it
 * would have, which the server brings down.
 */

const NONE = Object.freeze({ fps: 0, bitrateMbps: 0, width: 0, height: 0 });

let current = NONE;

/** A ceiling as the server sends it: a whole number, 0 when absent or bad. */
function whole(v) {
    const n = Number(v);
    return Number.isFinite(n) && n > 0 ? Math.floor(n) : 0;
}

/**
 * Take the ceilings from a GET /api/settings/streaming reply.
 * @param {object} data
 */
export function setStreamLimits(data) {
    if (!data || typeof data !== 'object') return;
    current = Object.freeze({
        fps: whole(data.max_allowed_frame_rate),
        bitrateMbps: whole(data.max_allowed_bitrate),
        width: whole(data.max_allowed_stream_width) & ~1,
        height: whole(data.max_allowed_stream_height) & ~1,
    });
}

/** @returns {{fps:number, bitrateMbps:number, width:number, height:number}} */
export function streamLimits() {
    return current;
}

/** Whether any ceiling is set. */
export function hasStreamLimits() {
    return current.fps > 0 || current.bitrateMbps > 0 || current.width > 0 || current.height > 0;
}

/** A frame rate under the ceiling; 0 (Auto, unknown) stays 0. */
export function limitFps(fps) {
    const cap = current.fps;
    return cap > 0 && fps > cap ? cap : fps;
}

/** A bitrate in Mbps under the ceiling. */
export function limitBitrateMbps(mbps) {
    const cap = current.bitrateMbps;
    return cap > 0 && mbps > cap ? cap : mbps;
}

/** A bitrate in kbps under the ceiling. */
export function limitBitrateKbps(kbps) {
    const cap = current.bitrateMbps * 1000;
    return cap > 0 && kbps > cap ? cap : kbps;
}

/** The highest bitrate the slider offers, in Mbps. */
export function bitrateSliderMax(max = 150) {
    const cap = current.bitrateMbps;
    return cap > 0 ? Math.max(1, Math.min(max, cap)) : max;
}

/**
 * Whether a fixed rung (its height, at 16:9) fits inside the ceiling: under a
 * 1920x1080 ceiling 720p and 1080p, not 1440p nor 4K.
 * @param {number} height
 */
export function rungAllowed(height) {
    const { width: w, height: h } = current;
    if (h > 0 && height > h) return false;
    if (w > 0 && Math.round((height * 16) / 9) > w) return false;
    return true;
}

/** The largest side a custom size may take, per axis. */
export function customSizeMax(max) {
    return {
        width: current.width > 0 ? Math.min(max, current.width) : max,
        height: current.height > 0 ? Math.min(max, current.height) : max,
    };
}

const even = (n) => Math.max(2, Math.round(n) & ~1);

/**
 * A launch size (util/StreamResolution.js: height, "W:H" aspect, a box to fit
 * or not, "Match my screen"'s fallback) held inside the resolution ceiling,
 * the way the server holds it (AppSettings::limitStreamSize): an explicit
 * size keeps its shape, a box to fit is clipped side by side, except "Match
 * my screen"'s, which is a size too. A size the host picks (height 0: the
 * native display's own) is left to the server, which makes it the ceiling's
 * box.
 * @param {object} size
 * @returns {{size: object, capped: boolean}}
 */
export function capSizeToLimits(size) {
    const { width: limW, height: limH } = current;
    if (!size || (!(limW > 0) && !(limH > 0)) || !(size.height > 0)) return { size, capped: false };
    const maxW = limW > 0 ? limW : Infinity;
    const maxH = limH > 0 ? limH : Infinity;
    const parts = typeof size.aspect === 'string' ? size.aspect.split(':').map(Number) : null;
    const known = parts && parts[0] > 0 && parts[1] > 0;
    const width = known ? (size.height * parts[0]) / parts[1] : (size.height * 16) / 9;
    if (width <= maxW && size.height <= maxH) return { size, capped: false };
    let w;
    let h;
    if (size.fitBox && !size.matchDisplay) {
        w = even(Math.min(width, maxW));
        h = even(Math.min(size.height, maxH));
    } else {
        const scale = Math.min(maxW / width, maxH / size.height);
        w = even(width * scale);
        h = even(size.height * scale);
    }
    const fb = size.fallback;
    return {
        size: {
            ...size,
            height: h,
            // A rung's shape is the ratio logic's, later: only its height moves.
            ...(known ? { aspect: w + ':' + h } : {}),
            fallback:
                fb && fb.width > 0 && fb.height > 0
                    ? {
                          width: even(Math.min(fb.width, maxW)),
                          height: even(Math.min(fb.height, maxH)),
                      }
                    : fb,
        },
        capped: true,
    };
}

/** Test hook: no ceiling. */
export function resetStreamLimits() {
    current = NONE;
}
