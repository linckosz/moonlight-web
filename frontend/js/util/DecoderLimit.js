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
 * The largest frame this browser's hardware decoder takes, and a launch size
 * brought under it.
 *
 * A stream larger than the hardware decoder does not degrade, it does not
 * start: a Mi TV whose decoders stop at 1920×1088 was asked 2148×1208 by Auto
 * (01/10/2026) and got "Decoding error" on HEVC, then on H.264, then nothing on
 * the <video> transport either — forty-five seconds of fallbacks ending on a
 * failure. The size is ours to choose, so it is chosen inside what decodes.
 *
 * The question is put to `VideoDecoder.isConfigSupported` with
 * `prefer-hardware`: the only form that answers for the hardware. Without the
 * hint the same TV says yes to 2560×1440 and fails on the first frame. A
 * browser that answers no at every size (no hardware path for that codec, or
 * no answer at all) is left alone: the limit is unknown, not small.
 */

/** Sizes asked, largest first. The first one taken is the limit; 4K taken
 *  means no limit worth applying. */
const PROBE_SIZES = [
    [3840, 2160],
    [2560, 1440],
    [1920, 1080],
    [1280, 720],
];

/** Codec strings at a level high enough for every probe size, so that a
 *  refusal is about the decoder and not about the level named. */
const PROBE_CODECS = {
    h264: 'avc1.640033',
    hevc: 'hev1.1.6.L153.90',
    av1: 'av01.0.13M.08',
};

/** codec → Promise<{width, height}|null>, once per page. */
const probes = new Map();
/** codec → {width, height}|null, filled when a probe settles. */
const settled = new Map();

async function probe(codec) {
    const string = PROBE_CODECS[codec];
    if (!string || typeof VideoDecoder === 'undefined' || !VideoDecoder.isConfigSupported)
        return null;
    for (let i = 0; i < PROBE_SIZES.length; i++) {
        const [width, height] = PROBE_SIZES[i];
        let ok = false;
        try {
            const r = await VideoDecoder.isConfigSupported({
                codec: string,
                codedWidth: width,
                codedHeight: height,
                hardwareAcceleration: 'prefer-hardware',
            });
            ok = !!(r && r.supported);
        } catch (e) {
            ok = false;
        }
        if (ok) return i === 0 ? null : { width, height };
    }
    return null;
}

/**
 * The largest frame the hardware decodes in `codec` ('h264', 'hevc', 'av1'),
 * or null when there is no limit below 4K or it cannot be told.
 * @param {string} codec
 * @returns {Promise<{width:number, height:number}|null>}
 */
export function hardwareDecodeLimit(codec) {
    const key = codec || 'hevc';
    if (!probes.has(key)) {
        probes.set(
            key,
            probe(key).then((limit) => {
                settled.set(key, limit);
                return limit;
            }),
        );
    }
    return probes.get(key);
}

/** The same answer, synchronously, once a probe has settled (else null). */
export function knownHardwareDecodeLimit(codec) {
    return settled.get(codec || 'hevc') || null;
}

/** Test hook: forget every answer. */
export function resetHardwareDecodeLimits() {
    probes.clear();
    settled.clear();
}

/**
 * Whether this browser says outright that it will not decode a stream smoothly:
 * the guard on a rung of Auto's learned ceiling (stream/AutoCeilingSession.js).
 * A "yes" proves little, Chrome often says "smooth" without measuring, so only a
 * "no" is heard. No API, an error, or no answer in time: no "no".
 * @param {string} [codec] 'h264', 'hevc', 'av1'; none stored is HEVC, as above
 * @param {{width:number, height:number, fps:number, bitrate:number}} stream bitrate in bit/s
 * @param {number} timeoutMs how long a launch may wait for the answer
 * @param {any} [capabilities] navigator.mediaCapabilities by default
 * @returns {Promise<boolean>}
 */
export async function decoderRefuses(codec, stream, timeoutMs, capabilities) {
    const string = PROBE_CODECS[codec || 'hevc'];
    const mc =
        capabilities ||
        (typeof navigator !== 'undefined'
            ? /** @type {any} */ (navigator).mediaCapabilities
            : null);
    if (!string || !mc || typeof mc.decodingInfo !== 'function') return false;
    let timer = null;
    const late = new Promise((resolve) => {
        timer = setTimeout(() => resolve(null), timeoutMs);
    });
    try {
        const info = await Promise.race([
            mc.decodingInfo({
                type: 'media-source',
                video: {
                    contentType: 'video/mp4; codecs="' + string + '"',
                    width: stream.width,
                    height: stream.height,
                    bitrate: stream.bitrate,
                    framerate: stream.fps,
                },
            }),
            late,
        ]);
        return !!info && (info.supported === false || info.smooth === false);
    } catch (e) {
        return false;
    } finally {
        clearTimeout(timer);
    }
}

const even = (n) => Math.max(2, Math.round(n) & ~1);

/**
 * A launch size (util/StreamResolution.js resolveStreamSize) brought under a
 * decode limit.
 *
 * - A box the host fits its display into is clamped side by side: the host
 *   keeps its own shape inside it, so 4096:1440 under 1920×1080 is a
 *   1920:1080 box and a 16:9 display streams 1920×1080.
 * - An exact size (a display switched or made to it) keeps its shape and is
 *   scaled as a whole; the box the host falls back to when it has no such
 *   mode is clamped like any box.
 * - A rung (a height, the width left to the ratio) comes down to the height
 *   whose 16:9 width still fits.
 * - "The host's own size" (height 0) becomes the limit as a box.
 *
 * @param {object} size
 * @param {{width:number, height:number}|null} limit
 * @returns {{size: object, capped: boolean}}
 */
export function capSizeToDecoder(size, limit) {
    if (!size || !limit || !(limit.width > 0) || !(limit.height > 0))
        return { size, capped: false };
    if (!(size.height > 0)) {
        return {
            size: {
                ...size,
                height: limit.height,
                aspect: limit.width + ':' + limit.height,
                fitBox: true,
                allowUpscale: false,
                matchDisplay: false,
                fallback: null,
            },
            capped: true,
        };
    }
    const parts = typeof size.aspect === 'string' ? size.aspect.split(':').map(Number) : null;
    if (!parts || !(parts[0] > 0) || !(parts[1] > 0)) {
        const most = Math.min(limit.height, Math.floor((limit.width * 9) / 16));
        if (size.height <= most) return { size, capped: false };
        return { size: { ...size, height: even(most) }, capped: true };
    }
    const width = (size.height * parts[0]) / parts[1];
    if (width <= limit.width && size.height <= limit.height) return { size, capped: false };
    let w;
    let h;
    if (size.matchDisplay) {
        const scale = Math.min(limit.width / width, limit.height / size.height);
        w = even(width * scale);
        h = even(size.height * scale);
    } else {
        w = even(Math.min(width, limit.width));
        h = even(Math.min(size.height, limit.height));
    }
    const fb = size.fallback;
    return {
        size: {
            ...size,
            height: h,
            aspect: w + ':' + h,
            fallback:
                fb && fb.width > 0 && fb.height > 0
                    ? {
                          width: even(Math.min(fb.width, limit.width)),
                          height: even(Math.min(fb.height, limit.height)),
                      }
                    : fb,
        },
        capped: true,
    };
}
