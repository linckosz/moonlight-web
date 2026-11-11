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
 * A stall of the link, played by the page itself (debug only).
 *
 * Apple's decoder (Chrome on macOS, Safari on iOS) throws "Decoding error"
 * after a stall of the link even when no frame was lost. A real stall takes a
 * Wi-Fi radio going quiet, which no bench can order. What the page sees of one
 * is simple, though: nothing for a while, then every frame the link held, in
 * one burst. This plays exactly that, on any link: every `everyMs` the frames
 * that come in are held for `holdMs`, then handed on together, in order, none
 * lost. Everything downstream (gap check, decode queue, flush at the keyframe)
 * meets them as it would after a real stall.
 *
 * Off unless localStorage `mw_stall_test` is set, as "everyMs,holdMs" — for
 * instance "10000,1500": a 1.5 s stall every 10 s.
 */

/** Shortest and longest stall played, in ms. */
export const STALL_MIN_HOLD_MS = 100;
export const STALL_MAX_HOLD_MS = 10000;

/** Frames held at most; a stall that would hold more ends early. */
export const STALL_MAX_HELD = 1200;

/**
 * The setting, or null when it is absent or makes no sense.
 * @param {string | null | undefined} raw  "everyMs,holdMs"
 * @returns {{ everyMs: number, holdMs: number } | null}
 */
export function parseStallTest(raw) {
    if (typeof raw !== 'string') return null;
    const parts = raw.split(',').map((s) => Number(s.trim()));
    if (parts.length !== 2 || !parts.every((n) => Number.isFinite(n))) return null;
    const [everyMs, holdMs] = parts;
    if (holdMs < STALL_MIN_HOLD_MS || holdMs > STALL_MAX_HOLD_MS) return null;
    // The stream must run between two stalls, or it is one long stall.
    if (everyMs < holdMs * 2) return null;
    return { everyMs, holdMs };
}

export class StallTest {
    /**
     * @param {{ everyMs: number, holdMs: number, maxHeld?: number }} cfg
     */
    constructor(cfg) {
        this.everyMs = cfg.everyMs;
        this.holdMs = cfg.holdMs;
        this.maxHeld = cfg.maxHeld || STALL_MAX_HELD;
        this._nextAt = -1;
        this._holdUntil = -1;
        this._heldSince = -1;
        /** @type {any[]} */
        this._held = [];
        /** Stalls played so far. */
        this.stalls = 0;
        /** The last burst: frames handed on together, and how long they waited. */
        this.lastBurst = { frames: 0, heldMs: 0 };
    }

    /** Whether frames are being held right now. */
    get holding() {
        return this._holdUntil >= 0;
    }

    /**
     * A frame came in at @p nowMs. Returns the frames to hand on now, oldest
     * first: none while a stall holds them, this one alone otherwise, and the
     * whole burst when the stall ends.
     * @template T
     * @param {T} frame
     * @param {number} nowMs
     * @returns {T[]}
     */
    admit(frame, nowMs) {
        if (this._nextAt < 0) this._nextAt = nowMs + this.everyMs;
        if (!this.holding && nowMs >= this._nextAt) {
            this._holdUntil = nowMs + this.holdMs;
            this._heldSince = nowMs;
            this.stalls++;
        }
        if (!this.holding) return [frame];
        if (nowMs < this._holdUntil && this._held.length < this.maxHeld) {
            this._held.push(frame);
            return [];
        }
        // The link comes back: everything it held, then this frame.
        const out = this._held;
        out.push(frame);
        this._held = [];
        this.lastBurst = { frames: out.length, heldMs: nowMs - this._heldSince };
        this._holdUntil = -1;
        this._heldSince = -1;
        this._nextAt = nowMs + this.everyMs;
        return out;
    }
}
