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
 * JitterController — adaptive jitter-buffer target controller (webrtc-media).
 *
 * Drives RTCRtpReceiver.jitterBufferTarget from measured network signals so the
 * browser's dejitter buffer absorbs jitter ONLY when the link degrades, sitting
 * at 0 (current low-latency behavior) on a clean link. A non-zero target also
 * reactivates the backend NACK retransmission (a packet retransmitted within the
 * buffer window arrives before its playout deadline → loss recovered without IDR).
 *
 * Control law — AIMD-asymmetric: rise fast (grab safety + enable NACK), decay
 * slowly after sustained calm (release latency cautiously). A predictive baseline
 * from interarrival jitter pre-empts freezes; an RTT term sizes the window for
 * NACK; a deadband avoids thrashing the browser buffer.
 *
 * Pure/stateful and transport-agnostic: it owns all adaptation state (counter
 * deltas, EWMA, target). Feed it raw cumulative RTCStats fields each tick; it
 * returns the target to command (ms) or null when no change is warranted.
 */

const DEFAULTS = {
    MIN: 0, // ms — clean-link floor (== today's behavior)
    MAX: 200, // ms — playable cap for a game stream
    JITTER_K: 2.5, // cover the jitter peak, not the mean
    RTT_K: 1.5, // window so a NACK retransmission lands in time
    BUMP_FREEZE: 50, // ms — immediate step-up on an underrun
    BUMP_LOSS: 30, // ms — immediate step-up on a loss spike
    LOSS_HI: 0.02, // loss rate triggering a bump
    LOSS_LO: 0.003, // loss rate below which decay is allowed
    JITTER_LO: 8, // ms — jitter below which decay is allowed
    EWMA_ALPHA: 0.3, // jitter smoothing
    CLEAN_HOLD: 5, // ticks of sustained calm before decaying
    DECAY_STEP: 10, // ms removed per decay tick
    DEADBAND: 15, // ms — minimum change worth pushing to the receiver
};

export class JitterController {
    constructor(tunables = {}) {
        this.cfg = { ...DEFAULTS, ...tunables };
        this.reset();
    }

    reset() {
        this._targetMs = this.cfg.MIN;
        this._lastCommanded = this.cfg.MIN;
        this._jitterEwma = 0;
        this._cleanTicks = 0;
        this._prevPacketsLost = 0;
        this._prevPacketsReceived = 0;
        this._prevFreezeCount = 0;
        this._primed = false;
    }

    /** Current target in ms (the controller's internal state, always available). */
    get targetMs() {
        return this._targetMs;
    }

    /**
     * Advance one tick.
     * @param {{packetsLost:number, packetsReceived:number, freezeCount:number,
     *          jitterMs:number, rttMs:number}} raw cumulative stats + derived ms.
     * @returns {number|null} target (ms) to command, or null if within the deadband.
     */
    update(raw) {
        const c = this.cfg;
        const lost = raw.packetsLost || 0;
        const recv = raw.packetsReceived || 0;
        const freezeCount = raw.freezeCount || 0;

        // Prime deltas on the first tick (no baseline yet).
        if (!this._primed) {
            this._prevPacketsLost = lost;
            this._prevPacketsReceived = recv;
            this._prevFreezeCount = freezeCount;
            this._primed = true;
            return null;
        }

        const dLost = Math.max(0, lost - this._prevPacketsLost);
        const dRecv = Math.max(0, recv - this._prevPacketsReceived);
        const freezes = Math.max(0, freezeCount - this._prevFreezeCount);
        this._prevPacketsLost = lost;
        this._prevPacketsReceived = recv;
        this._prevFreezeCount = freezeCount;

        const lossRate = dLost + dRecv > 0 ? dLost / (dLost + dRecv) : 0;
        const jitterMs = raw.jitterMs || 0;
        const rttMs = raw.rttMs || 0;

        this._jitterEwma = c.EWMA_ALPHA * jitterMs + (1 - c.EWMA_ALPHA) * this._jitterEwma;

        let desired = c.JITTER_K * this._jitterEwma;
        if (lossRate > c.LOSS_LO) desired = Math.max(desired, c.RTT_K * rttMs); // make NACK effective
        if (freezes > 0) desired = Math.max(desired, this._targetMs + c.BUMP_FREEZE);
        else if (lossRate > c.LOSS_HI) desired = Math.max(desired, this._targetMs + c.BUMP_LOSS);
        desired = Math.max(c.MIN, Math.min(c.MAX, desired));

        if (desired > this._targetMs) {
            // Rise immediately — grab the safety margin while the link is bad.
            this._targetMs = desired;
            this._cleanTicks = 0;
        } else {
            // Decay only after sustained calm — release latency cautiously.
            if (freezes === 0 && lossRate < c.LOSS_LO && this._jitterEwma < c.JITTER_LO)
                this._cleanTicks++;
            else this._cleanTicks = 0;
            if (this._cleanTicks >= c.CLEAN_HOLD) {
                this._targetMs = Math.max(c.MIN, this._targetMs - c.DECAY_STEP);
            }
        }

        // Deadband: only command on a significant change, or when settling back to
        // the minimum so we don't leave a small residual buffer pinned.
        const settledToMin = this._targetMs === c.MIN && this._lastCommanded > c.MIN;
        if (Math.abs(this._targetMs - this._lastCommanded) >= c.DEADBAND || settledToMin) {
            this._lastCommanded = this._targetMs;
            this._lastSignals = { lossRate, jitterEwma: this._jitterEwma, rttMs, freezes };
            return this._targetMs;
        }
        return null;
    }

    /** Signals at the last commanded change (for logging/overlay). */
    get lastSignals() {
        return this._lastSignals || null;
    }
}
