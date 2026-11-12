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
 * Recommended-bitrate estimate, shared by the Settings UI (auto-bitrate
 * slider) and the congestion-degradation ladder (bitrate aligned to a
 * reduced resolution).
 */

/** Parse a "W:H" aspect string into a numeric ratio. "auto" / unknown → 16:9
 *  (baseline for the bitrate estimate; the real width is derived from the
 *  host's screen format on the backend at launch). */
export function aspectToNumber(aspect) {
    const m = /^(\d+):(\d+)$/.exec(aspect || '');
    if (m && +m[2] > 0) return +m[1] / +m[2];
    return 16 / 9;
}

/**
 * The Auto rule, measured (docs/wiki/16-Auto-Bitrate-Benchmarks.md): what a
 * stream needs grows with its pixels to the power 0.75, and hardly at all
 * with its frame rate (fps^0.05: 1080p120 needs what 1080p60 needs). 9.5 Mbps
 * at 1080p is VMAF NEG 93 on a scrolling text page, +10 % — the Internet
 * target, for every browser the host does not see on its own network.
 */
const AUTO_MBPS_1080P = 9.5;
const AUTO_PIXEL_EXPONENT = 0.75;

/**
 * The LAN rule: VMAF NEG 95 on the same page, +16 % at 1080p (17.3 Mbps
 * measured), growing with the pixels to the power 0.8 — 20 Mbps at 1080p,
 * the Auto 1080p60 had before v0.4.2. For a browser the host sees on its own
 * network (GET /api/settings/streaming's `viewer_lan`): on a cable the bitrate
 * made no measurable difference to the latency, and the network has the room.
 */
const AUTO_LAN_MBPS_1080P = 20;
const AUTO_LAN_PIXEL_EXPONENT = 0.8;

/** Up to a whole Mbps (the slider's step) to 30, to 5 Mbps above. */
function roundUpMbps(mbps) {
    const x = mbps - 1e-9; // a value already whole stays itself
    return x > 30 ? Math.ceil(x / 5) * 5 : Math.ceil(x);
}

/**
 * HDR's small margin on top. Measured on rendered 3D scenes with highlights up
 * to 3× the SDR white, against the same renders in SDR: a scrolling scene
 * needs ×0.96–1.07 of SDR's bitrate for VMAF NEG 93, a still one up to ×1.32
 * where HDR's score has stopped rising (docs/wiki/16-Auto-Bitrate-Benchmarks.md).
 * ×1.5 until v0.4.3.
 */
const AUTO_HDR_RATIO = 1.2;

/**
 * Recommended bitrate (Mbps):
 *   9.5 × P^0.75 over the Internet, 20 × P^0.8 on the host's own network,
 *   P = the stream's pixels over 1920×1080's
 *   × 1.2 for HDR
 * rounded up (roundUpMbps), clamped to [1, 150] Mbps.
 * "Same as Host" (height 0) uses 1080p. Neither the frame rate, the device,
 * 4:4:4 nor H.264 is a factor: the bench measured no frame-rate term worth
 * one, the Internet target already is what a phone on mobile data needs, and
 * 4:4:4 and H.264 reach the same picture at the same bitrate as HEVC 4:2:0.
 * @param {boolean} [lan] the host sees this browser on its own network
 */
export function computeAutoBitrate(height, aspect, chroma444, hdr, lan = false) {
    const h = height > 0 ? height : 1080;
    // Pixel count = (h × aspectRatio) × h, normalised to 1920×1080.
    // Ultrawide (21:9, 32:9) therefore scales the bitrate up accordingly.
    const pixelRatio = (h * h * aspectToNumber(aspect)) / (1920 * 1080);
    // chroma444 adds nothing, measured: the full chroma of a desktop's text
    // and flat colours costs NVENC no more than 4:2:0, and the host's QP
    // floor stops a 4:4:4 stream short of the 4:2:0 one's bitrate.
    const richRatio = hdr ? AUTO_HDR_RATIO : 1.0;
    const mbps =
        (lan
            ? AUTO_LAN_MBPS_1080P * Math.pow(pixelRatio, AUTO_LAN_PIXEL_EXPONENT)
            : AUTO_MBPS_1080P * Math.pow(pixelRatio, AUTO_PIXEL_EXPONENT)) * richRatio;
    return Math.max(1, Math.min(150, roundUpMbps(mbps)));
}
