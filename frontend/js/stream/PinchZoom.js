/*
 * MoonlightWeb — a trackpad pinch as the host's Ctrl+wheel.
 * Copyright (C) 2026 Bruno Martin. GPLv3 — see repository LICENSE.
 *
 * A pinch on a trackpad zooms on the host the way a Windows precision
 * touchpad does it: Ctrl held, and the wheel turned by as much as the fingers
 * spread or close (issue #52, v0.4.4).
 *
 * What the browsers give (M3, Chrome 126 and Safari 27, 13/11/2026):
 * - Chrome sends a pinch as wheel events with ctrlKey set, while no Control
 *   key is down. getModifierState('Control') says true as well, so only the
 *   keys this view saw go down tell a pinch from a real Ctrl+wheel. deltaY is
 *   negative as the fingers spread, and fractional. Read as
 *   deltaY = -100 × ln(scale), Chromium's convention: our gestures matched
 *   Safari's scales for the same moves, without a reference to prove it.
 * - Safari sends no wheel at all: gesturestart, gesturechange and gestureend
 *   carry the scale since the gesture began (1 at its start). They reach an
 *   element although 'ongesturechange' is not in window.
 *
 * The zoom goes one notch (120 host units) for every 10 % of scale, the step
 * of a Ctrl+wheel in Windows browsers, Office and Explorer, in high-resolution
 * units: a fraction of a notch is carried, never rounded away. A slow pinch is
 * some 15 notches.
 */

/** One notch of the host's wheel. */
export const PINCH_NOTCH = 120;
/** The scale that one notch stands for: 10 %. */
export const PINCH_STEP = Math.log(1.1);
/** A Chrome pinch is over once no event came for this long. */
export const PINCH_GAP_MS = 250;

/** The log of the scale a Chrome pinch event stands for. */
export function chromePinchLog(deltaY) {
    return -deltaY / 100;
}

/** Turns the log of a scale into whole host wheel units, carrying the rest. */
export class PinchToWheel {
    constructor() {
        this._carry = 0;
    }

    /**
     * @param {number} lnScale how much the scale grew (positive: zoom in)
     * @returns {number} whole units, positive for a zoom in (the wheel turned up)
     */
    push(lnScale) {
        if (!Number.isFinite(lnScale)) return 0;
        const units = this._carry + (PINCH_NOTCH * lnScale) / PINCH_STEP;
        const whole = Math.trunc(units);
        this._carry = units - whole;
        return whole;
    }

    reset() {
        this._carry = 0;
    }
}
