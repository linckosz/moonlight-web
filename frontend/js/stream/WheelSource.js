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
 * that scrolls continuously. It lets the stream invert the wheel alone, and
 * leave the trackpad the way it scrolls on this Mac.
 *
 * No web API says it (w3c/pointerevents#596 is still open). Two Mac browsers
 * leave a mark a page can read.
 *
 * Chromium on macOS builds the two kinds differently after asking macOS
 * whether the device is continuous (kCGScrollWheelEventIsContinuous,
 * components/input/web_input_event_builders_mac.mm):
 *  - continuous: whole-pixel deltas, and wheel ticks of delta / 40, so the
 *    legacy wheelDelta reads -3 × delta, give or take the 1 lost when
 *    120 × delta / 40 is rounded (131 for a delta of -44);
 *  - notched: the accelerated Cocoa delta × 40 and the raw notch count, so
 *    wheelDelta moves by notches whatever the delta.
 * A page zoom divides the deltas and wheelDelta alike: at 110 %, a notch reads
 * 109 and a trackpad step of 1 px reads 0.909, wheelDelta -2. The identity
 * holds at any zoom; whole pixels and steps of 120 do not.
 *
 * Safari gives both kinds a wheelDelta of 3 × delta
 * (Source/WebKit/Shared/mac/WebEventFactory.mm), so the identity tells nothing
 * there. The deltas do: a continuous device moves by whole pixels, at any page
 * zoom, while a notched wheel moves by the Cocoa delta × 40, which has a
 * fraction. A turn starts on about 4 px: likely 4.000244, a tenth of a line in
 * macOS's 16.16 fixed point. Should one start on an exact 4 px, which a
 * trackpad could send too, the gesture waits for the next event
 * (WheelInverter).
 *
 * Measured on a MacBook Air M3, an M590 wheel and the trackpad:
 *  - Chrome 126 at 100 % and 110 % (test/fixtures/wheel-chrome-mac.json): all
 *    1,459 trackpad events sit on the identity. Of 201 wheel events, 6 land on
 *    it by chance in the middle of a burst, never the first of a gesture, which
 *    sits about 100 away: hence a decision per gesture (WheelSourceTracker);
 *  - Safari 27 at 100 % and 115 % (test/fixtures/wheel-safari-mac.json): all
 *    1,343 trackpad events are whole pixels, and each gesture opens on 1 px.
 *    All 131 wheel events had a fraction, the recording page counted before
 *    rounding to 3 decimals; rounded, as the fixture keeps them, each gesture
 *    opens on 4 px, once twice in a row, then on fractions.
 * One mouse only, at macOS's default scrolling speed.
 *
 * Continuous is not "trackpad". A Magic Mouse is continuous too, and so is a
 * wheel that a utility smooths (Logi Options+, Mos): Chromium "cannot
 * distinguish trackpad data from data from any other continuous device".
 * A wheel that a utility leaves without acceleration may move by whole pixels
 * in Safari, and read as continuous. Neither is inverted: the safe side.
 *
 * Elsewhere the mark is missing, so the kind is unknown there. On Windows and
 * Linux, wheelDelta is the system's raw value, 120 a notch (Chrome 157 on
 * Windows: deltaY 100, wheelDelta -120), and a high-resolution or
 * free-spinning wheel sends less, like a touchpad. No touchpad has been
 * measured there yet. Pages are not given a gesture's phase or inertia, and a
 * decision taken from the tail of a gesture would come after its first notches
 * were already sent.
 */

export const WHEEL_NOTCHED = 'notched';
export const WHEEL_CONTINUOUS = 'continuous';
/**
 * Not told yet: an opening step of exactly 4 px in Safari, which is how a
 * wheel's first notch reads once rounded, and a whole number, like the
 * trackpad's pixels.
 */
export const WHEEL_PENDING = 'pending';

/**
 * How long without a wheel event ends a gesture. Within one notch burst of the
 * M590, events came at most 164 ms apart; a trackpad's inertia comes every
 * 16 ms until it stops.
 */
export const WHEEL_GESTURE_GAP_MS = 250;

/** Safari's opening step of a notched wheel, in pixels, at any page zoom. */
const SAFARI_STEP_PX = 4;

/**
 * How many opening steps a gesture holds back before it calls itself notched:
 * 8 px. The trackpad never opened on one; a wheel sent two in a row once.
 */
const STEPS_HELD = 2;

/**
 * How far wheelDelta may sit from -3 × delta on a continuous event: Chromium
 * rounds 120 × delta / 40 toward zero, after the zoom.
 */
const IDENTITY_SLACK = 1.01;

/**
 * Where the kind of a wheel event can be read: 'chromium-mac' (Chrome, Edge,
 * Brave, Arc… on macOS), 'safari-mac', or null.
 *
 * @param {string} [ua]
 * @param {number} [maxTouchPoints]
 * @returns {'chromium-mac'|'safari-mac'|null}
 */
export function wheelSourceSupport(
    ua = globalThis.navigator?.userAgent || '',
    maxTouchPoints = globalThis.navigator?.maxTouchPoints || 0,
) {
    if (!/Macintosh/.test(ua)) return null;
    // An iPad asking for the desktop site reads like a Mac, and runs WebKit
    // whatever its browser. A Mac has no touch points.
    if (maxTouchPoints > 1) return null;
    if (/Chrome\//.test(ua)) return 'chromium-mac';
    // Safari itself: Firefox has no AppleWebKit token, and the other Mac
    // browsers on WebKit were not measured.
    if (/AppleWebKit\//.test(ua) && /Version\/[\d.]+ Safari\//.test(ua)) return 'safari-mac';
    return null;
}

/**
 * The kind of one wheel event. Only meaningful where wheelSourceSupport() says
 * so, for that support.
 *  - Chromium on macOS: continuous when both axes sit on the identity
 *    wheelDelta = -3 × delta, notched otherwise.
 *  - Safari: notched when a delta has a fraction; pending on an exact 4 px
 *    step along one axis; continuous for any other whole number.
 *
 * @param {{deltaX: number, deltaY: number, deltaMode: number,
 *   wheelDeltaX?: number, wheelDeltaY?: number}} e
 * @param {'chromium-mac'|'safari-mac'} [support]
 * @returns {'notched'|'continuous'|'pending'|null} null when the event cannot tell
 */
export function wheelEventKind(e, support = 'chromium-mac') {
    const { deltaX: dx, deltaY: dy, deltaMode, wheelDeltaX: wdx, wheelDeltaY: wdy } = e;
    if (deltaMode !== 0 || (!dx && !dy)) return null;
    if (support === 'safari-mac') {
        if (!Number.isInteger(dx) || !Number.isInteger(dy)) return WHEEL_NOTCHED;
        const step =
            (Math.abs(dy) === SAFARI_STEP_PX && dx === 0) ||
            (Math.abs(dx) === SAFARI_STEP_PX && dy === 0);
        return step ? WHEEL_PENDING : WHEEL_CONTINUOUS;
    }
    if (typeof wdx !== 'number' || typeof wdy !== 'number') return null;
    const onIdentity =
        Math.abs(wdx + 3 * dx) <= IDENTITY_SLACK && Math.abs(wdy + 3 * dy) <= IDENTITY_SLACK;
    return onIdentity ? WHEEL_CONTINUOUS : WHEEL_NOTCHED;
}

/**
 * The kind of a whole gesture, so that one never changes kind halfway: the
 * gesture's first readable event decides, and the decision holds until the
 * wheel pauses. A notched burst in Chrome opens on a small accelerated delta
 * (4 px on the M590), far from the continuous identity, where a later event
 * can land on it by chance (39.976 px for one notch).
 *
 * In Safari, a gesture that opens on the 4 px step stays pending: the next
 * event decides, or a third step in a row, which is a wheel turned slowly,
 * makes it notched. Events that cannot tell stay pending with the steps.
 *
 * It gives a kind, never a direction. A notch that bounces back during a fast
 * turn (seen on a Logitech M331) keeps its own sign and its gesture's kind, so
 * whatever is done to the gesture is done to it too.
 */
export class WheelSourceTracker {
    /**
     * @param {'chromium-mac'|'safari-mac'} [support]
     * @param {number} [gapMs]
     */
    constructor(support = 'chromium-mac', gapMs = WHEEL_GESTURE_GAP_MS) {
        this._support = support;
        this._gapMs = gapMs;
        this._kind = null;
        this._steps = 0;
        this._last = -Infinity;
    }

    /**
     * Whether an event at this time opens a new gesture.
     * @param {number} now
     */
    opens(now) {
        return now - this._last > this._gapMs;
    }

    /**
     * @param {Parameters<typeof wheelEventKind>[0]} e
     * @param {number} now — the event's time in ms (event.timeStamp)
     * @returns {'notched'|'continuous'|'pending'|null}
     */
    kindOf(e, now) {
        if (this.opens(now)) {
            this._kind = null;
            this._steps = 0;
        }
        this._last = now;
        if (this._kind !== null) return this._kind;
        const kind = wheelEventKind(e, this._support);
        if (kind === null) return this._steps ? WHEEL_PENDING : null;
        if (kind === WHEEL_PENDING) {
            this._steps++;
            if (this._steps <= STEPS_HELD) return WHEEL_PENDING;
            this._kind = WHEEL_NOTCHED;
        } else {
            this._kind = kind;
        }
        return this._kind;
    }

    /**
     * Ends a gesture left pending, with nothing after its steps: a lone step is
     * a wheel turned by one notch, so the gesture is notched until it pauses.
     */
    settle() {
        if (this._kind === null && this._steps) this._kind = WHEEL_NOTCHED;
    }
}

/** @typedef {{deltaX: number, deltaY: number, deltaMode: number}} WheelDelta */

/** The same travel the other way, on both axes. */
function reversed({ deltaX, deltaY, deltaMode }) {
    return { deltaX: -deltaX || 0, deltaY: -deltaY || 0, deltaMode };
}

/**
 * Inverts the notched wheel, and only it: a continuous gesture, or one whose
 * kind cannot be read, goes on unchanged.
 *
 * What a pending Safari gesture sends is held back until its kind is known,
 * then sent with the event that told it. A gesture that ends while pending
 * sends its steps as notches: call flush() once the gap has passed with no
 * event, which the next event does too if the timer comes late.
 */
export class WheelInverter {
    /**
     * @param {'chromium-mac'|'safari-mac'} support
     * @param {number} [gapMs]
     */
    constructor(support, gapMs = WHEEL_GESTURE_GAP_MS) {
        this._tracker = new WheelSourceTracker(support, gapMs);
        /** @type {WheelDelta[]} */
        this._held = [];
    }

    /** Whether some travel waits for the gesture's kind: arm a flush. */
    get holding() {
        return this._held.length > 0;
    }

    /**
     * @param {Parameters<typeof wheelEventKind>[0]} e
     * @param {number} now — the event's time in ms (event.timeStamp)
     * @returns {WheelDelta[]} the travel to send now, in order
     */
    push(e, now) {
        const out = this._tracker.opens(now) ? this.flush() : [];
        const kind = this._tracker.kindOf(e, now);
        const d = { deltaX: e.deltaX, deltaY: e.deltaY, deltaMode: e.deltaMode };
        if (kind === WHEEL_PENDING) {
            this._held.push(d);
            return out;
        }
        for (const h of this._held.concat([d])) out.push(kind === WHEEL_NOTCHED ? reversed(h) : h);
        this._held = [];
        return out;
    }

    /**
     * Sends a pending gesture's steps as notches, inverted.
     * @returns {WheelDelta[]}
     */
    flush() {
        if (!this._held.length) return [];
        this._tracker.settle();
        const out = this._held.map(reversed);
        this._held = [];
        return out;
    }
}
