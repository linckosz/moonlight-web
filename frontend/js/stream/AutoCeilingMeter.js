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
 * Does this stream prove the machine holds the rung it runs at?
 *
 * Decides nothing about the size and writes nothing: fed by the view's 500 ms
 * tick, it says what it saw (`onEvent`), and util/AutoCeiling.js remembers it.
 * The clock comes in the arguments, like the pipeline's governors.
 *
 * A still desktop sends few frames: no drop, no queue, a stream that looks
 * healthy and proved nothing. So only time at the rate asked for, at the size
 * asked for, counts towards a rung (a loaded window). Some time counts neither
 * for nor against: the start-up traffic, a hidden tab, a transition, a share
 * (`neutral`). A step of the cadence above the stream's own rate counts for and
 * never against (`stepped`): the frames it holds are more than the rung asks,
 * and what goes wrong up there is the step's, which the stepper takes back.
 *
 * One signal is enough to call a stream sick, and it stays sick to its end:
 *   - drops: frames thrown away for want of room (`queueFull`, `backpressure`)
 *     over a share of them. `stale` is left out: it is what "latest frame
 *     first" does when all is well.
 *   - decode_rate: the decode-rate governor asks the host for fewer frames.
 *   - congestion: two sustained congestions in a stream. One proves nothing, a
 *     Wi-Fi produces it with the machine at rest. Counted here because on the
 *     native host the degradation ladder never moves.
 *   - decoder_rebuilt: the decoder was torn down and rebuilt.
 *   - host_behind: the host's display presents fast enough, yet too few frames
 *     arrive: an encoder that does not keep up, seen nowhere else.
 *   - whatever the view reports through noteSick: the enhancer stepping down, a
 *     codec fallback, a silent decoder, a lost GPU context, a crashed worker.
 *
 * "GPU behind" is no signal: it reads falsely at 120 fps on some machines.
 *
 * Every threshold below is a proposal until a bench has read them on the log.
 */

/** The first seconds are start-up traffic, as for the congestion monitor. */
export const GRACE_MS = 10000;
/** A tick later than this (a stall, a throttled timer) counts for no more. */
export const MAX_TICK_MS = 2000;
/** A window is loaded when this share of the rate asked for arrives. */
export const LOADED_ARRIVAL_SHARE = 0.9;
/** Drops over this share of the frames make a stream sick. */
export const DROP_SICK_SHARE = 0.005;
/** The drops are weighed over at least this long, and this many frames: one
 *  frame dropped out of a hundred of a still desktop means nothing. */
export const DROP_WINDOW_MS = 60000;
export const DROP_WINDOW_FRAMES = 600;
/** How many sustained congestions make a stream sick. */
export const CONGESTIONS_SICK = 2;
/** The host's display counts as fast enough from this share of the rate. */
export const HOST_PRESENTS_SHARE = 0.9;
/** Under this share of the rate arriving meanwhile, the host is behind… */
export const HOST_ARRIVAL_SHARE = 0.85;
/** …over at least this long. */
export const HOST_WINDOW_MS = 60000;

/** A frame this share of the size asked for still counts as that size: the
 *  native host fits its own shape inside the box, 2304×1440 in a 2560×1440 one. */
export const FRAME_MIN_SHARE = 0.7;

/**
 * Whether the frame streamed is the stream asked for, as far as a rung goes: its
 * share of the size, and above what the rung below already allows. A 1920×1080
 * host under a 2204×1240 box says nothing of a rung of 330 M.
 * @param {string} resolution the view's "W×H", '' before the first frame
 * @param {{width:number, height:number, fps:number, below:number}|null} asked
 * @returns {boolean|null} null while nothing can be told
 */
export function frameMatchesAsked(resolution, asked) {
    if (!asked || !(asked.height > 0)) return null;
    const m = /^(\d+)[×x](\d+)/.exec(resolution || '');
    if (!m) return null;
    const pixels = Number(m[1]) * Number(m[2]);
    if (pixels < asked.width * asked.height * FRAME_MIN_SHARE) return false;
    return pixels * asked.fps > asked.below;
}

/** How much a cumulative counter grew; a new pipeline starts it over. */
function grown(value, last) {
    return value > last ? value - last : 0;
}

/**
 * @typedef {object} AutoCeilingSample
 * @property {boolean} [neutral] this tick counts neither for nor against
 * @property {boolean} [stepped] the cadence stands on a step above the stream's
 *   own rate: this tick can count for the rung, not against it
 * @property {number} [arrivalFps] the frames arriving, a second
 * @property {boolean|null} [sizeMatches] the frame is the size asked for; null
 *   while no frame has said
 * @property {number} [frames] frames received so far
 * @property {number} [drops] frames dropped so far, `queueFull` + `backpressure`
 * @property {number} [rebuilds] decoder rebuilds so far
 * @property {number} [fpsCap] the rate the decode governor asks of the host
 * @property {number} [presentsFps] the host's display, its presents a second;
 *   0 when it does not say
 */

export class AutoCeilingMeter {
    /**
     * @param {object} deps
     * @param {number} deps.startedAt when the stream started, on the tick's clock
     * @param {number} deps.fps the rate asked for
     * @param {boolean} deps.limited the ceiling cut this launch: the next rung
     *   would have streamed more (neither the screen nor the decoder stood first)
     * @param {boolean} deps.provesRung this stream asks more than the rung below
     *   allows: only then does it say anything about this rung
     * @param {number} deps.holdMs loaded time that earns the next rung
     * @param {number} deps.confirmMs loaded time that confirms this rung
     * @param {{width:number, height:number, fps:number, below:number}|null} [deps.asked]
     *   what this launch asked for (frameMatchesAsked)
     * @param {(event: {type: 'held'|'earned'|'sick', reason?: string,
     *          loadedMs: number, atRung: boolean}) => void} deps.onEvent
     *   `held`: this rung is proved. `earned`: the next one is. `sick`: the
     *   stream failed, and `atRung` says whether this rung is to blame.
     */
    constructor({ startedAt, fps, limited, provesRung, holdMs, confirmMs, asked, onEvent }) {
        this.asked = asked || null;
        this._startedAt = startedAt;
        this._fps = fps > 0 ? fps : 0;
        this._limited = !!limited;
        this._provesRung = !!provesRung;
        this._holdMs = holdMs;
        this._confirmMs = confirmMs;
        this._onEvent = onEvent;
        this._lastTick = startedAt;
        this._neutral = true;
        this._done = false;
        this._held = false;
        this._earned = false;
        this._sawAskedSize = false;
        this._sawOtherSize = false;
        this._congestions = 0;
        this._loadedMs = 0;
        this._lastFrames = 0;
        this._lastDrops = 0;
        this._lastRebuilds = 0;
        this._dropMs = 0;
        this._dropFrames = 0;
        this._dropCount = 0;
        this._hostMs = 0;
        this._hostArrived = 0;
        /** @type {string} why the stream is sick, '' while it is not */
        this.sickReason = '';
    }

    /** Loaded time so far, ms. */
    get loadedMs() {
        return this._loadedMs;
    }

    /** One tick of the view. @param {number} now @param {AutoCeilingSample} s */
    tick(now, s) {
        const dt = Math.max(0, Math.min(MAX_TICK_MS, now - this._lastTick));
        this._lastTick = now;
        const frames = grown(s.frames, this._lastFrames);
        const drops = grown(s.drops, this._lastDrops);
        const rebuilds = grown(s.rebuilds, this._lastRebuilds);
        this._lastFrames = s.frames > 0 ? s.frames : 0;
        this._lastDrops = s.drops > 0 ? s.drops : 0;
        this._lastRebuilds = s.rebuilds > 0 ? s.rebuilds : 0;
        if (s.sizeMatches === true) this._sawAskedSize = true;
        else if (s.sizeMatches === false) this._sawOtherSize = true;
        const neutral = !!s.neutral || now - this._startedAt <= GRACE_MS;
        // noteCongestion and noteSick go by this: on a step, they say nothing.
        this._neutral = neutral || !!s.stepped;
        if (this._done || neutral) return;

        if (!s.stepped) {
            if (s.fpsCap > 0) return this._sick('decode_rate');
            if (rebuilds > 0) return this._sick('decoder_rebuilt');
            if (this._weighDrops(dt, frames, drops)) return;
            if (this._weighHost(dt, s)) return;
        }
        if (s.sizeMatches === true && s.arrivalFps >= this._fps * LOADED_ARRIVAL_SHARE) {
            this._loadedMs += dt;
            this._checkHold();
        }
    }

    /** A sustained congestion. */
    noteCongestion() {
        if (this._done || this._neutral) return;
        this._congestions++;
        if (this._congestions >= CONGESTIONS_SICK) this._sick('congestion');
    }

    /**
     * The view saw the stream fail.
     * @param {string} reason
     * @param {boolean} [always] it counts even in a neutral window: a crash is a
     *   crash in a hidden tab too, where a step of the enhancer may not be.
     */
    noteSick(reason, always = false) {
        if (this._done || (this._neutral && !always)) return;
        this._sick(reason);
    }

    /** @returns {boolean} the drops have just made the stream sick */
    _weighDrops(dt, frames, drops) {
        this._dropMs += dt;
        this._dropFrames += frames;
        this._dropCount += drops;
        if (this._dropMs < DROP_WINDOW_MS || this._dropFrames < DROP_WINDOW_FRAMES) return false;
        const share = this._dropCount / this._dropFrames;
        this._dropMs = 0;
        this._dropFrames = 0;
        this._dropCount = 0;
        if (share <= DROP_SICK_SHARE) return false;
        this._sick('drops');
        return true;
    }

    /** @returns {boolean} the host has just made the stream sick */
    _weighHost(dt, s) {
        if (!(this._fps > 0) || !(s.presentsFps >= this._fps * HOST_PRESENTS_SHARE)) return false;
        this._hostMs += dt;
        this._hostArrived += (s.arrivalFps > 0 ? s.arrivalFps : 0) * dt;
        if (this._hostMs < HOST_WINDOW_MS) return false;
        const arrived = this._hostArrived / this._hostMs;
        this._hostMs = 0;
        this._hostArrived = 0;
        if (arrived >= this._fps * HOST_ARRIVAL_SHARE) return false;
        this._sick('host_behind');
        return true;
    }

    _checkHold() {
        if (!this._provesRung) return;
        if (!this._held && this._loadedMs >= this._confirmMs) {
            this._held = true;
            this._emit('held');
        }
        if (this._limited && !this._earned && this._loadedMs >= this._holdMs) {
            this._earned = true;
            this._emit('earned');
        }
    }

    _sick(reason) {
        this._done = true;
        this.sickReason = reason;
        this._emit('sick', reason);
    }

    _emit(type, reason) {
        // A stream that never reached the size asked for ran smaller than this
        // rung: a 720p host on a bad Wi-Fi says nothing of it.
        const smaller = this._sawOtherSize && !this._sawAskedSize;
        this._onEvent({
            type,
            reason,
            loadedMs: this._loadedMs,
            atRung: this._provesRung && !smaller,
        });
    }
}
