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
 * EnhancerGovernor — steps the WebGPU enhancer down when its GPU cost stops
 * fitting in a frame.
 *
 * The enhancer runs on the same GPU as everything else the machine is doing.
 * Measured on a 4K client (see PipelineDiag): FSR1 costs ~10ms of GPU wait per
 * frame at rest, which fits at 60fps — until a video call contends for the GPU
 * and the same pass costs ~19.5ms. Past that point the render guard (one draw
 * at a time) caps presentation at 1000/19.5 ≈ 51fps: decoded frames queue up,
 * the queue leg inflates and the surplus is dropped. The picture is sharper and
 * a fifth of the frames never reach the screen.
 *
 * So the trigger is not "the GPU is busy" but "the draw no longer fits between
 * two frames": the budget is the frame ARRIVAL interval, because that is the
 * rate the pipeline has to sustain. Costing 10ms is fine at 22fps (45ms apart)
 * and fine at 60fps (16.7ms apart); costing 19.5ms at 60fps is not.
 *
 * The ladder only ever goes DOWN from what the user picked (fsr1 → sgsr → off)
 * and climbs back when the pressure is gone. Recovery is deliberately shy: the
 * cost at the level above cannot be measured from the level below, so a restore
 * is a bet. Each undone bet doubles the wait before the next one, which turns a
 * long call into one failed attempt instead of a flip-flop every 15 seconds.
 */

/** Descending cost order — the only direction this ladder is walked. */
export const ENHANCER_LADDER = ['fsr1', 'sgsr', 'off'];

/** Draw wait above this share of the frame budget = it no longer fits. */
const DEGRADE_RATIO = 0.8;
/** …and back below this share (with margin, so a restore is not a coin flip). */
const RECOVER_RATIO = 0.4;
/** A verdict must hold this long — a single busy window is not a trend. */
const DEGRADE_SUSTAIN_MS = 2000;
/** First restore attempt after this long in the clear; doubles when undone. */
const RECOVER_BASE_MS = 15000;
const RECOVER_MAX_MS = 300000;
/** A restore undone within this delay counts as a failed bet. */
const RECOVER_REGRET_MS = 10000;
/**
 * After a switch, ignore observations for as long as the measurement window
 * (PipelineDiag's 2s): its average still holds draws made at the PREVIOUS
 * level, and acting on them would walk the whole ladder down on the strength of
 * one expensive moment.
 */
const SETTLE_MS = 2000;

/** Arrival intervals outside this range are not a usable frame budget. */
const MIN_BUDGET_MS = 1;
const MAX_BUDGET_MS = 200;

export class EnhancerGovernor {
    /**
     * @param {string} preferred The user's setting — the ceiling, never exceeded.
     * @param {{fixedBudgetMs?: number, warmupMs?: number, noRecovery?: boolean}} [profile]
     *   Overrides for a session that is not the owner's own:
     *   - fixedBudgetMs replaces the frame-arrival budget with a flat cost cap.
     *     An invited player's stream is fixed at 60fps and the guest never chose
     *     any of this, so "does it fit in 8ms" is a promise we can state up
     *     front, where the adaptive budget would depend on their link.
     *   - warmupMs ignores everything before it: the first seconds include
     *     shader compilation and pipeline warm-up, which are not the steady cost.
     *   - noRecovery keeps the enhancer off once it has been dropped —
     *     predictable beats optimal for someone who is only visiting.
     */
    constructor(preferred, profile = {}) {
        const idx = ENHANCER_LADDER.indexOf(preferred);
        this._ceiling = idx >= 0 ? idx : 0;
        this._level = this._ceiling;
        this._degradeSince = 0;
        this._recoverSince = 0;
        this._recoverAfterMs = RECOVER_BASE_MS;
        this._lastRecoveryMs = 0;
        this._settleUntil = 0;
        this._fixedBudgetMs = profile.fixedBudgetMs > 0 ? profile.fixedBudgetMs : 0;
        this._warmupMs = profile.warmupMs > 0 ? profile.warmupMs : 0;
        this._noRecovery = profile.noRecovery === true;
        /** First observation's timestamp — the warm-up is measured from there. */
        this._firstObsMs = 0;
    }

    /** Current algo — what the renderer should be running. */
    get level() {
        return ENHANCER_LADDER[this._level];
    }

    /** True while the governor holds the enhancer below the user's setting. */
    get degraded() {
        return this._level > this._ceiling;
    }

    /**
     * Feed one observation window.
     * @param {{serviceMs: number, arrivalMs: number, now: number}} obs
     *   serviceMs per-frame cost of the render stage — the draw latency divided
     *             by how many draws overlap (PipelineDiag.renderServiceMs), NOT
     *             the raw wait: with two draws in flight each one waits about
     *             twice as long while costing the pipeline the same.
     *   arrivalMs average interval between incoming frames = the frame budget
     *   now       performance.now(), passed in so the policy stays pure
     * @returns {string|null} the new algo when the level changed, else null.
     */
    update(obs) {
        const budget = obs && obs.arrivalMs;
        const wait = obs && obs.serviceMs;
        const now = (obs && obs.now) || 0;
        // No usable budget (stream idle, no sample yet): hold, and let the
        // sustain timers lapse rather than deciding on nothing.
        if (!(budget >= MIN_BUDGET_MS) || budget > MAX_BUDGET_MS || !(wait >= 0)) {
            this._degradeSince = 0;
            this._recoverSince = 0;
            return null;
        }

        // Just switched: the window is still describing the previous level.
        if (now < this._settleUntil) {
            this._degradeSince = 0;
            this._recoverSince = 0;
            return null;
        }

        // Warm-up: shader compilation and a cold pipeline are not the cost this
        // is meant to judge.
        if (this._warmupMs > 0) {
            if (this._firstObsMs === 0) this._firstObsMs = now;
            if (now - this._firstObsMs < this._warmupMs) {
                this._degradeSince = 0;
                this._recoverSince = 0;
                return null;
            }
        }

        // A flat cap replaces the frame-arrival budget when one was set.
        const degradeAbove = this._fixedBudgetMs > 0 ? this._fixedBudgetMs : budget * DEGRADE_RATIO;
        const recoverBelow = this._fixedBudgetMs > 0 ? -1 : budget * RECOVER_RATIO;

        if (wait > degradeAbove) {
            this._recoverSince = 0;
            if (this._degradeSince === 0) this._degradeSince = now;
            if (now - this._degradeSince < DEGRADE_SUSTAIN_MS) return null;
            this._degradeSince = 0;
            if (this._level >= ENHANCER_LADDER.length - 1) return null;
            // Undoing a recent restore: that bet failed, wait longer next time.
            if (this._lastRecoveryMs > 0 && now - this._lastRecoveryMs < RECOVER_REGRET_MS) {
                this._recoverAfterMs = Math.min(RECOVER_MAX_MS, this._recoverAfterMs * 2);
            }
            this._level++;
            this._settleUntil = now + SETTLE_MS;
            return this.level;
        }

        if (!this._noRecovery && wait < recoverBelow && this._level > this._ceiling) {
            this._degradeSince = 0;
            if (this._recoverSince === 0) this._recoverSince = now;
            if (now - this._recoverSince < this._recoverAfterMs) return null;
            this._recoverSince = 0;
            this._level--;
            this._lastRecoveryMs = now;
            this._settleUntil = now + SETTLE_MS;
            return this.level;
        }

        // In between the two thresholds: stable, let both timers lapse.
        this._degradeSince = 0;
        this._recoverSince = 0;
        return null;
    }
}
