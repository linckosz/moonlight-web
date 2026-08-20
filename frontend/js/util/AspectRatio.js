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
 * Screen aspect ratios — the vocabulary shared by the Settings dropdown, the
 * AspectProbe (which only trusts a measurement that lands on one of them) and
 * the per-host memory that lets the next launch start at the right format.
 *
 * Sunshine never reports its display format (serverinfo carries no DisplayMode,
 * and no REST route exposes it), so the streamed width can only follow the host
 * if the browser works the ratio out for itself — see AspectProbe.js.
 */

/** Market share order, widescreen and ultrawide included. Values double as the
 *  Settings <select> options and as the probe's white list. */
export const SCREEN_ASPECTS = [
    { value: '16:9', ratio: 16 / 9 },
    { value: '16:10', ratio: 16 / 10 },
    { value: '21:9', ratio: 21 / 9 },
    { value: '4:3', ratio: 4 / 3 },
    { value: '3:2', ratio: 3 / 2 },
    { value: '32:9', ratio: 32 / 9 },
    { value: '5:4', ratio: 5 / 4 },
];

/** Every ratio the backend and the Settings PATCH accept, "auto" included. */
export const ASPECT_VALUES = ['auto', ...SCREEN_ASPECTS.map((a) => a.value)];

/** Ratio the stream falls back to when nothing is known about the host. */
export const DEFAULT_ASPECT = '16:9';

// A measurement has to land within 3% of a real screen ratio to be believed.
// Wide enough for the 21:9 family (2560×1080 = 2.370 and 3440×1440 = 2.389 both
// sit ~2.4% off the nominal 2.333), tight enough that neighbours never overlap
// — 3:2 and 16:10, the closest pair, are 6.7% apart.
const PLAUSIBLE_TOLERANCE = 0.03;

// Below this, the measured ratio IS the canonical one (rounding aside) and the
// canonical string is used verbatim. Above it — a 3440×1440 host snapping to
// "21:9" — the exact measurement is sent instead, because 2.4% of width is
// ~60px of residual black bar, which is the very thing this feature removes.
const EXACT_TOLERANCE = 0.005;

/**
 * Validate a measured content rectangle against the white list and turn it into
 * the aspect string to request.
 * @param {number} contentW width of the real picture, in encoded pixels
 * @param {number} contentH height of the real picture, in encoded pixels
 * @returns {string|null} "16:10", or "1440:900" when the exact ratio matters,
 *          or null when the measurement matches no real screen format.
 */
export function resolveMeasuredAspect(contentW, contentH) {
    if (!(contentW > 0) || !(contentH > 0)) return null;
    const ratio = contentW / contentH;
    let best = null;
    let bestErr = Infinity;
    for (const a of SCREEN_ASPECTS) {
        const err = Math.abs(ratio - a.ratio) / a.ratio;
        if (err < bestErr) {
            bestErr = err;
            best = a;
        }
    }
    if (!best || bestErr > PLAUSIBLE_TOLERANCE) return null;
    return bestErr <= EXACT_TOLERANCE
        ? best.value
        : `${Math.round(contentW)}:${Math.round(contentH)}`;
}

/**
 * Read an aspect string back as a number — the inverse of what this module
 * emits, and of what the backend parses out of the launch request.
 * @param {string} value "16:10", or an exact "1728:1080"
 * @returns {number|null} the ratio, or null when the string names none ("auto")
 */
export function parseAspect(value) {
    if (typeof value !== 'string') return null;
    const parts = value.split(':');
    if (parts.length !== 2) return null;
    const w = Number(parts[0]);
    const h = Number(parts[1]);
    if (!(w > 0) || !(h > 0)) return null;
    return w / h;
}

// Per-host memory. Not a source of truth: it only lets a launch start at the
// format the last session measured, instead of spending its first seconds at
// 16:9. Every session re-measures and overwrites it, so a wrong value (a host
// that was showing a letterboxed video at launch) never outlives one session.
const HOST_ASPECT_KEY = 'mw-host-aspect';

function readHostAspects() {
    try {
        const raw = localStorage.getItem(HOST_ASPECT_KEY);
        const map = raw ? JSON.parse(raw) : null;
        return map && typeof map === 'object' ? map : {};
    } catch (e) {
        console.warn('[MW] Could not read the host aspect memory:', e);
        return {};
    }
}

/** Last aspect measured for this host, or null. */
export function loadHostAspect(uuid) {
    if (!uuid) return null;
    const value = readHostAspects()[uuid];
    return typeof value === 'string' && value.includes(':') ? value : null;
}

/** Remember the aspect measured for this host (no-op if unchanged). */
export function saveHostAspect(uuid, aspect) {
    if (!uuid || !aspect) return;
    const map = readHostAspects();
    if (map[uuid] === aspect) return;
    map[uuid] = aspect;
    try {
        localStorage.setItem(HOST_ASPECT_KEY, JSON.stringify(map));
    } catch (e) {
        console.warn('[MW] Could not persist the host aspect memory:', e);
    }
}
