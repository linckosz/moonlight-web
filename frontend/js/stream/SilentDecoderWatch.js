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
 * A decoder that takes every chunk and gives nothing back.
 *
 * WebCodecs reports a decoder that fails. It says nothing of one that simply
 * stops: decode() keeps returning, the decode queue stays short, and no output
 * callback ever comes again. Seen on a Freebox Player POP (Amlogic, Android
 * TV 10, WebView 153, 01/10/2026) under an intra-refresh stream: a few seconds
 * in, the picture froze for good — 13 000 chunks taken, 71 pictures out — and
 * nothing on the page knew. The same box decodes the same host at 50 fps once
 * the stream carries no refresh wave.
 *
 * Fed the session's two counters on a timer, it answers once the decoder has
 * gone `minMs` without a picture while at least `minChunks` went in. Both: a
 * still host sends a couple of frames a second, and a short hiccup is not a
 * decoder gone. Any picture starts the count again.
 *
 * The time counts only while chunks go in. A stretch in which none came is
 * the link holding the frames, not the decoder keeping them: counted, it made
 * a silence of every stall that ended in a burst. On a MacBook Air M3 (Chrome,
 * HEVC, 13/11/2026), 1.5 s held and 91 frames let go at once, the tick 9 ms
 * later read "91 chunks in, no picture for 1507 ms", and the page threw away
 * a decoder that was about to answer — once in 65 such stalls.
 */
export class SilentDecoderWatch {
    /** @param {{minChunks?: number, minMs?: number}} [opts] */
    constructor({ minChunks = 30, minMs = 1500 } = {}) {
        this.minChunks = minChunks;
        this.minMs = minMs;
        this.reset();
    }

    /** Forget the count: the next observation starts it again. */
    reset() {
        this._received = -1;
        this._decoded = -1;
        this._faults = 0;
        this._since = 0;
        this._seen = -1;
        this._waitMs = 0;
    }

    /**
     * @param {number} received chunks handed to the decoder so far
     * @param {number} decoded pictures it gave back so far
     * @param {number} now ms, any monotonic clock
     * @param {number} [faults] the decoder's failures so far (recoveries): a
     *   silence with one in it is a decoder failing, which the answer says
     * @returns {{chunks: number, ms: number, faults: number} | null} the
     *   silence, when it is one — chunks taken, and how long they went in
     *   without a picture — once per stretch: the next answer needs as much
     *   again
     */
    observe(received, decoded, now, faults = 0) {
        // A picture came out, or the counters started over (a new decoder).
        if (decoded !== this._decoded || received < this._received) {
            this._received = received;
            this._decoded = decoded;
            this._faults = faults;
            this._since = now;
            this._seen = received;
            this._waitMs = 0;
            return null;
        }
        // Since the last look: chunks went in, or the link held them all.
        if (received > this._seen) this._waitMs += now - this._since;
        this._seen = received;
        this._since = now;
        const chunks = received - this._received;
        const ms = this._waitMs;
        if (chunks < this.minChunks || ms < this.minMs) return null;
        const failed = Math.max(0, faults - this._faults);
        this._received = received;
        this._faults = faults;
        this._waitMs = 0;
        return { chunks, ms, faults: failed };
    }
}
