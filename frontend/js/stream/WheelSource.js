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
 * Which kind of device a wheel event comes from: a notched wheel, or a device
 * that scrolls continuously.
 *
 * No web API says it (w3c/pointerevents#596 is still open). Chromium on macOS
 * leaves a mark a page can read, because it builds the two kinds differently
 * after asking macOS whether the device is continuous
 * (kCGScrollWheelEventIsContinuous, components/input/web_input_event_builders_mac.mm):
 *  - continuous: whole-pixel deltas, and wheel ticks of delta / 40, so the
 *    legacy wheelDelta reads -3 × delta, give or take the 1 lost when
 *    120 × delta / 40 is rounded (131 for a delta of -44);
 *  - notched: the accelerated Cocoa delta × 40, nearly always fractional, and
 *    the raw notch count, so wheelDelta moves in steps of 120.
 * Chrome 126 on a MacBook Air M3 sorted 167 events of 167 this way, an M590
 * wheel and the trackpad (test/fixtures/wheel-chrome-mac.json, 12/11/2026).
 *
 * Continuous is not "trackpad". A Magic Mouse is continuous too, and so is a
 * wheel that a utility smooths (Logi Options+, Mos): Chromium "cannot
 * distinguish trackpad data from data from any other continuous device".
 *
 * Elsewhere the mark is missing, so the kind is unknown there:
 *  - Safari: WebKit gives both kinds a wheelDelta of 3 × delta
 *    (Source/WebKit/Shared/mac/WebEventFactory.mm);
 *  - Windows and Linux: wheelDelta is the system's raw value, 120 a notch, and
 *    a high-resolution or free-spinning wheel sends less, like a touchpad.
 *    Still to be measured before anything is read there.
 * Pages are not given a gesture's phase or inertia. A decision taken from the
 * tail of a gesture would come after its first notches were already sent.
 */

export const WHEEL_NOTCHED = 'notched';
export const WHEEL_CONTINUOUS = 'continuous';

/**
 * How long without a wheel event ends a gesture. Within one notch burst of the
 * M590, events came at most 110 ms apart; a trackpad's inertia comes every
 * 16 ms until it stops.
 */
export const WHEEL_GESTURE_GAP_MS = 250;

/**
 * Where the kind of a wheel event can be read: 'chromium-mac' (Chrome, Edge,
 * Brave, Arc… on macOS), or null.
 *
 * @param {string} [ua]
 * @param {number} [maxTouchPoints]
 * @returns {'chromium-mac'|null}
 */
export function wheelSourceSupport(
    ua = globalThis.navigator?.userAgent || '',
    maxTouchPoints = globalThis.navigator?.maxTouchPoints || 0,
) {
    if (!/Macintosh/.test(ua) || !/Chrome\//.test(ua)) return null;
    // An iPad asking for the desktop site reads like a Mac, and runs WebKit
    // whatever its browser. A Mac has no touch points.
    if (maxTouchPoints > 1) return null;
    return 'chromium-mac';
}

/** True when v is a whole number of 120-unit notches, 0 included. */
function inNotches(v) {
    return v % 120 === 0;
}

/**
 * The kind of one wheel event, read the way Chromium on macOS builds it.
 * Only meaningful where wheelSourceSupport() says so.
 *
 * A notched event needs both readings: wheelDelta in whole notches on both
 * axes, and not the continuous identity. An event they disagree on stays
 * continuous, the side that is never inverted. Blink divides both the deltas
 * and wheelDelta by devicePixelRatio, so a page zoom other than 100 % may move
 * either reading: it is still to be measured.
 *
 * @param {{deltaX: number, deltaY: number, deltaMode: number,
 *   wheelDeltaX?: number, wheelDeltaY?: number}} e
 * @returns {'notched'|'continuous'|null} null when the event cannot tell
 */
export function wheelEventKind(e) {
    const { deltaX: dx, deltaY: dy, deltaMode, wheelDeltaX: wdx, wheelDeltaY: wdy } = e;
    if (deltaMode !== 0 || typeof wdx !== 'number' || typeof wdy !== 'number') return null;
    if (!dx && !dy) return null;
    if (!inNotches(wdx) || !inNotches(wdy) || (wdx === 0 && wdy === 0)) return WHEEL_CONTINUOUS;
    const whole = Number.isInteger(dx) && Number.isInteger(dy);
    if (whole && Math.abs(wdx + 3 * dx) <= 1 && Math.abs(wdy + 3 * dy) <= 1) {
        return WHEEL_CONTINUOUS;
    }
    return WHEEL_NOTCHED;
}

/**
 * The kind of a whole gesture, so that one never changes kind halfway: the
 * gesture's first readable event decides, and the decision holds until the
 * wheel pauses. A notched burst opens on a small accelerated delta (4 px on the
 * M590), far from the continuous identity, where a later event could land on
 * it by chance (exactly 40 px for one notch).
 */
export class WheelSourceTracker {
    /** @param {number} [gapMs] */
    constructor(gapMs = WHEEL_GESTURE_GAP_MS) {
        this._gapMs = gapMs;
        this._kind = null;
        this._last = -Infinity;
    }

    /**
     * @param {Parameters<typeof wheelEventKind>[0]} e
     * @param {number} now — the event's time in ms (event.timeStamp)
     * @returns {'notched'|'continuous'|null}
     */
    kindOf(e, now) {
        if (now - this._last > this._gapMs) this._kind = null;
        this._last = now;
        if (this._kind === null) this._kind = wheelEventKind(e);
        return this._kind;
    }
}
