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
 * What the decoder had just been fed when it said "Decoding error".
 *
 * Apple's decoder (Chrome on macOS, Safari on iOS) breaks after a stall of the
 * link even when no frame was lost: every frame arrives, late and in a burst,
 * and the reference chain is whole (plan A14, 07/11/2026). Reading the code
 * found no orphan delta handed to it; what only happens after a stall is the
 * burst itself — a deep decode queue, and in the worker a flush (reset +
 * configure) at the keyframe. The browser's error says none of that. This
 * keeps the few numbers that tell the cases apart, for one console line:
 *
 *   queue    decode queue depth at the last decode() — a burst or not
 *   key      frames decoded since the last keyframe — an old chain or a new one
 *   gap      the longest wait between two frames in the last 2 s — the stall
 *   last     ms since the last frame came in
 *   flush    ms since the last decoder flush, "none" if there was none
 *
 * Log only: nothing here changes what is decoded or shown.
 */

/** How far back the longest wait between two frames is looked for. */
export const GAP_WINDOW_MS = 2000;

export class DecoderErrorTrail {
    constructor() {
        this.reset();
    }

    /** A new stream: nothing seen yet. */
    reset() {
        this._lastArrival = -1;
        /** @type {{ at: number, gap: number }[]} waits that ended in the window */
        this._gaps = [];
        this._queue = 0;
        this._sinceKey = -1; // -1: no keyframe decoded yet
        this._lastFlush = -1;
    }

    /** A whole frame came in, at @p nowMs. */
    noteArrival(nowMs) {
        if (this._lastArrival >= 0) this._gaps.push({ at: nowMs, gap: nowMs - this._lastArrival });
        this._lastArrival = nowMs;
        while (this._gaps.length > 0 && nowMs - this._gaps[0].at > GAP_WINDOW_MS)
            this._gaps.shift();
    }

    /** A frame handed to decode(), the queue at @p queued before it. */
    noteDecode(isKeyframe, queued) {
        this._queue = queued;
        if (isKeyframe) this._sinceKey = 0;
        else if (this._sinceKey >= 0) this._sinceKey++;
    }

    /** The decoder was flushed (reset + configure) at @p nowMs. */
    noteFlush(nowMs) {
        this._lastFlush = nowMs;
    }

    /** The longest wait between two frames that ended in the window before @p nowMs. */
    longestGap(nowMs) {
        let longest = 0;
        for (const g of this._gaps)
            if (nowMs - g.at <= GAP_WINDOW_MS && g.gap > longest) longest = g.gap;
        return longest;
    }

    /**
     * The console line for @p err at @p nowMs, @p codec named.
     * @param {{ name?: string, code?: number, message?: string } | null} err
     */
    line(err, nowMs, codec) {
        const e = err || {};
        const r = (ms) => Math.round(ms);
        return (
            'Decoder error context:' +
            ' codec=' +
            (codec || '?') +
            ' error=' +
            (e.name || '?') +
            (e.code !== undefined && e.code !== null ? '/' + e.code : '') +
            ' "' +
            String(e.message || '').slice(0, 80) +
            '"' +
            ' queue=' +
            this._queue +
            ' key=' +
            (this._sinceKey < 0 ? 'none' : this._sinceKey) +
            ' gap=' +
            r(this.longestGap(nowMs)) +
            'ms' +
            ' last=' +
            (this._lastArrival < 0 ? 'none' : r(nowMs - this._lastArrival) + 'ms') +
            ' flush=' +
            (this._lastFlush < 0 ? 'none' : r(nowMs - this._lastFlush) + 'ms')
        );
    }
}
