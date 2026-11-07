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
 * target, used everywhere: the page cannot tell at launch whether the stream
 * will cross the LAN or the Internet, and on a cable the bitrate made no
 * measurable difference to the latency.
 */
const AUTO_MBPS_1080P = 9.5;
const AUTO_PIXEL_EXPONENT = 0.75;

/** Up to a whole Mbps (the slider's step) to 30, to 5 Mbps above. */
function roundUpMbps(mbps) {
    const x = mbps - 1e-9; // a value already whole stays itself
    return x > 30 ? Math.ceil(x / 5) * 5 : Math.ceil(x);
}

/**
 * Recommended bitrate (Mbps):
 *   9.5 × P^0.75, P = the stream's pixels over 1920×1080's
 *   × 1.5 when HDR and/or YUV 4:4:4 is enabled (not cumulative — single 1.5×)
 * rounded up (roundUpMbps), clamped to [1, 150] Mbps.
 * "Same as Host" (height 0) uses 1080p. Neither the frame rate nor the device
 * is a factor: the bench measured no frame-rate term worth one, and the
 * Internet target already is what a phone on mobile data needs.
 */
export function computeAutoBitrate(height, aspect, chroma444, hdr) {
    const h = height > 0 ? height : 1080;
    // Pixel count = (h × aspectRatio) × h, normalised to 1920×1080.
    // Ultrawide (21:9, 32:9) therefore scales the bitrate up accordingly.
    const pixelRatio = (h * h * aspectToNumber(aspect)) / (1920 * 1080);
    // HDR (10-bit) and 4:4:4 (4× chroma samples) each justify ~1.5× bitrate;
    // not cumulative — a single 1.5× when either is enabled.
    const richRatio = chroma444 || hdr ? 1.5 : 1.0;
    const mbps = AUTO_MBPS_1080P * Math.pow(pixelRatio, AUTO_PIXEL_EXPONENT) * richRatio;
    return Math.max(1, Math.min(150, roundUpMbps(mbps)));
}
