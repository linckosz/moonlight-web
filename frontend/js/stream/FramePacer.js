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
 * FramePacer — adaptive presentation reserve for the DataChannel/WebCodecs path.
 *
 * The DC pipeline presents a frame the moment the decoder emits it and drops
 * whatever is left behind (drop-to-freshest). On a metronomic link that is
 * optimal. Under jitter it is the worst case: a frame that arrives 10ms late
 * leaves the previous one on screen for an extra vsync, then the two frames
 * that arrive together collapse into one — the "repeat + skip" judder, visible
 * as a 2-3 frame hitch on a moving cursor even though the mean RTT is fine.
 *
 * The fix is a reserve: hold each frame until a *presentation clock* says its
 * turn has come, so a late frame eats into the reserve instead of into the
 * cadence. This is the same trade the RTP path gets from the browser's dejitter
 * buffer (JitterController drives it there) — DataChannel has no such buffer, so
 * we build the clock ourselves from the capture timestamps the backend already
 * sends (`backendTs`, monotonic steady_clock ms, see DataChannelRelay.cpp).
 *
 * Control law — the reserve tracks the observed *late tail*, not the mean:
 *   baseline = the minimum transit delay seen lately (drifts up slowly, snaps
 *              down instantly — a faster path is real, a slower one is suspect)
 *   excess   = how much later than the baseline this frame actually landed
 *   target   = p95(excess) * SAFETY, clamped, rising immediately and decaying
 *              slowly so a calm second does not throw the reserve away
 * A frame is then held for `target - excess`: one that took the best path waits
 * the full reserve, one that already lost `target` ms goes straight out. Latency
 * added is therefore the reserve, and *only while the link is actually jittery*.
 *
 * At target = 0 every frame schedules to "now" and the caller's behaviour is
 * bit-for-bit today's drop-to-freshest — that is the intended clean-link state,
 * and the reason DEADBAND snaps small targets to exactly zero.
 *
 * Pure and clock-injected: `now` is always a parameter (never read from
 * performance.now() here), so the whole control law is deterministic in tests.
 */

const DEFAULTS = {
    MIN: 0, // ms — clean-link floor (== today's behavior)
    MAX: 60, // ms — cap on the reserve (~3.5 frames @60fps); past this, the
    // quality ladder is the right lever, not more buffering.
    QUANTILE: 0.95, // cover the late tail, not the mean
    SAFETY: 1.15, // small margin over the measured tail
    WINDOW_MS: 2000, // sliding window backing the tail estimate
    MAX_SAMPLES: 256, // hard bound on that window (240 = 2s @120fps)
    CONTROL_INTERVAL_MS: 100, // re-evaluate the target at most this often
    DECAY_INTERVAL_MS: 250, // ms between decay steps
    DECAY_STEP: 2, // ms shed per decay step (~8ms/s — slow on purpose)
    BUMP_UNDERRUN: 8, // ms — immediate step-up when the queue ran dry
    DEADBAND: 3, // ms — below this the target snaps to MIN (no residual hold)
    DRIFT_PER_FRAME: 0.05, // ms — how fast the baseline may rise (~3ms/s @60fps)
    RESYNC_MS: 500, // |delay - baseline| past this = discontinuity → rebase
    SLACK_MS: 2, // present now rather than arm a timer for less than this
};

export class FramePacer {
    constructor(tunables = {}) {
        this.cfg = { ...DEFAULTS, ...tunables };
        this.reset();
    }

    reset() {
        this._primed = false;
        this._baseline = 0;
        this._targetMs = this.cfg.MIN;
        this._samples = []; // { t, excess } over WINDOW_MS
        this._lastControl = 0;
        this._lastDecay = 0;
        this._lateTail = 0;
        this._lastExcess = 0;
        this._underruns = 0;
        this._held = 0;
    }

    /** Current reserve in ms (0 = present on decode, today's behavior). */
    get targetMs() {
        return this._targetMs;
    }

    /** Live signals for the stats overlay. */
    get stats() {
        return {
            targetMs: this._targetMs,
            lateTailMs: this._lateTail,
            excessMs: this._lastExcess,
            underruns: this._underruns,
            held: this._held,
        };
    }

    /**
     * The queue was empty when a frame was due — the reserve was too short to
     * cover what the network just did. Step up now; the tail estimate would only
     * catch up on the next control tick, a frame or two too late.
     * @param {number} nowMs performance.now()-domain
     */
    noteUnderrun(nowMs) {
        this._underruns++;
        this._targetMs = Math.min(this.cfg.MAX, this._targetMs + this.cfg.BUMP_UNDERRUN);
        this._lastDecay = nowMs;
    }

    /**
     * Decide when a just-decoded frame should be presented.
     * @param {number} backendTs capture time (uint32 ms, steady_clock domain)
     * @param {number} nowMs performance.now()-domain
     * @returns {number} presentation deadline in the performance.now() domain
     *                   (== nowMs when the frame should go out immediately)
     */
    schedule(backendTs, nowMs) {
        const c = this.cfg;
        // No capture stamp (AV1 bootstrap, WSS fallback, backend pre-steady_clock):
        // there is no clock to pace against — present now, exactly as before.
        if (!(backendTs > 0)) return nowMs;

        // Transit delay carries an arbitrary constant (two unrelated clock
        // origins); only its *variation* matters, so the constant cancels out
        // against the baseline below and never has to be known.
        const delay = nowMs - backendTs;

        // Rebase on the first frame and on any discontinuity: a uint32 backendTs
        // wrap (~49 days of host uptime), a resolution/session change, or a tab
        // that was throttled in the background. Adapting instead of rebasing
        // would peg the reserve at MAX for the whole window.
        if (!this._primed || Math.abs(delay - this._baseline) > c.RESYNC_MS) {
            this._primed = true;
            this._baseline = delay;
            this._samples.length = 0;
            this._lastControl = nowMs;
            this._lastDecay = nowMs;
            this._lateTail = 0;
            this._lastExcess = 0;
            return nowMs;
        }

        // Baseline = best transit seen lately. Down instantly (a shorter path is
        // a fact), up only by DRIFT_PER_FRAME (otherwise a sustained bad patch
        // would be absorbed into the baseline and the jitter would go unseen).
        this._baseline = Math.min(delay, this._baseline + c.DRIFT_PER_FRAME);

        const excess = delay - this._baseline; // >= 0 by construction
        this._lastExcess = excess;

        this._samples.push({ t: nowMs, excess });
        const cutoff = nowMs - c.WINDOW_MS;
        while (this._samples.length > 0 && this._samples[0].t < cutoff) this._samples.shift();
        while (this._samples.length > c.MAX_SAMPLES) this._samples.shift();

        this._control(nowMs);

        // Hold for whatever of the reserve this frame has not already spent in
        // the network. SLACK_MS avoids arming a timer for a sub-millisecond wait
        // that would cost more in wakeup jitter than it buys in smoothness.
        const wait = this._targetMs - excess;
        let deadline = nowMs;
        if (wait > c.SLACK_MS) {
            deadline = nowMs + wait;
            this._held++;
        }

        // This frame blew through the entire reserve — the buffer was shorter
        // than what the link just did, so the judder happened. Step up for the
        // frames behind it (never for this one: it is already late, holding it
        // longer would add to the very hitch we are trying to remove).
        if (this._targetMs > 0 && excess > this._targetMs) this.noteUnderrun(nowMs);

        return deadline;
    }

    /** Re-evaluate the reserve (rate-limited to CONTROL_INTERVAL_MS). */
    _control(nowMs) {
        const c = this.cfg;
        if (nowMs - this._lastControl < c.CONTROL_INTERVAL_MS) return;
        this._lastControl = nowMs;

        this._lateTail = this._tail();
        const desired = Math.min(c.MAX, this._lateTail * c.SAFETY);

        if (desired > this._targetMs) {
            // Rise immediately: the jitter has already happened, waiting a decay
            // interval to react means judder for that whole interval.
            this._targetMs = desired;
            this._lastDecay = nowMs;
        } else if (nowMs - this._lastDecay >= c.DECAY_INTERVAL_MS) {
            this._lastDecay = nowMs;
            this._targetMs = Math.max(c.MIN, desired, this._targetMs - c.DECAY_STEP);
        }

        // Snap a negligible reserve to exactly MIN so a clean link is bit-for-bit
        // the old immediate path rather than a permanent 1-2ms hold.
        if (this._targetMs < c.DEADBAND) this._targetMs = c.MIN;
    }

    /** QUANTILE of the excess samples in the window (0 when empty). */
    _tail() {
        const n = this._samples.length;
        if (n === 0) return 0;
        const v = this._samples.map((s) => s.excess).sort((a, b) => a - b);
        return v[Math.min(n - 1, Math.floor(n * this.cfg.QUANTILE))];
    }
}
