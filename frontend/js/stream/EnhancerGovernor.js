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
 * EnhancerGovernor — steps the enhancer down when its GPU cost stops fitting in
 * a frame, on WebGPU and on WebGL2 alike (the WebGL2 passes carry a 'gl-'
 * prefix, see ladderRung / rendererAlgo).
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
 * The ladder only ever goes DOWN from what the user picked (fsr1 → nis → sgsr → off)
 * and climbs back when the pressure is gone. Recovery is deliberately shy: the
 * cost at the level above cannot be measured from the level below, so a restore
 * is a bet. Each undone bet doubles the wait before the next one, which turns a
 * long call into one failed attempt instead of a flip-flop every 15 seconds.
 *
 * What the ladder settled on is remembered on this device (rememberRung), and
 * the next session starts there instead of at the setting: a phone that could
 * not decode 1440p and run SGSR within a frame last time is not handed the pair
 * again, only to lose its first seconds re-learning it. The setting stays the
 * ceiling, so a session that finds room climbs back — and forgets the verdict.
 */

/**
 * Descending cost order — the only direction this ladder is walked. The menu
 * calls them Quality (FSR1), Balanced (NIS) and Performance (SGSR1).
 */
export const ENHANCER_LADDER = ['fsr1', 'nis', 'sgsr', 'off'];

/** A renderer's pass as a rung of the ladder: 'gl-sgsr' is 'sgsr'. */
export function ladderRung(algo) {
    return typeof algo === 'string' && algo.startsWith('gl-') ? algo.slice(3) : algo;
}

/** A rung as the pass a renderer of this kind runs: 'sgsr' on WebGL is 'gl-sgsr'. */
export function rendererAlgo(rung, kind) {
    return kind === 'webgl' && rung !== 'off' ? 'gl-' + rung : rung;
}

const MEMORY_KEY = 'mw_enhancer_rung';

function readMemory() {
    try {
        const all = JSON.parse(localStorage.getItem(MEMORY_KEY) || '{}');
        return all && typeof all === 'object' ? all : {};
    } catch (e) {
        return {};
    }
}

/**
 * Where the ladder left this renderer kind the last time the setting was the
 * same, or null when it was at the setting (or nothing is known).
 * @param {'webgl'|'webgpu'} kind
 * @param {string} ceiling the setting, as a rung
 * @returns {{rung: string, recoverAfterMs: number}|null}
 */
export function rememberedRung(kind, ceiling) {
    const e = readMemory()[kind];
    if (!e || e.ceiling !== ceiling) return null;
    const top = ENHANCER_LADDER.indexOf(ceiling);
    const at = ENHANCER_LADDER.indexOf(e.rung);
    if (top < 0 || at <= top) return null;
    return { rung: e.rung, recoverAfterMs: e.recoverAfterMs > 0 ? e.recoverAfterMs : 0 };
}

/** Keep the ladder's latest verdict for the next session; back at the
 *  setting, there is nothing to keep. */
export function rememberRung(kind, ceiling, rung, recoverAfterMs) {
    try {
        const all = readMemory();
        if (rung === ceiling) delete all[kind];
        else all[kind] = { ceiling, rung, recoverAfterMs };
        localStorage.setItem(MEMORY_KEY, JSON.stringify(all));
    } catch (e) {}
}

/** Draw wait above this share of the frame budget = it no longer fits. */
const DEGRADE_RATIO = 0.8;
/**
 * Decoded frames waiting at the decoder input, above which the enhancer is
 * judged to be starving it — a second, independent reason to step down.
 *
 * A WebGL or WebGPU draw only QUEUES its work: gl.drawArrays() returns as soon
 * as the commands are recorded, so the draw timing above measures SUBMISSION,
 * not the GPU time the pass really costs. On a contended GPU it stays near
 * 0.2ms while the pass falls further and further behind, and this policy never
 * fires. Where the delay does show up is the decoder: the queued draws hold its
 * output surfaces, and with none free it stalls. Measured on an Arc A380, FSR1
 * at 1440p120 kept serviceMs at 0.2ms for a 9ms budget while decodeQueueSize
 * sat at 12-14 and the decode leg went from 15ms to 477ms — two thirds of the
 * frames lost, with the ladder standing by.
 *
 * StreamView's DECODE_QUEUE_MAX is 8 and a healthy stream reads under 1, so
 * half the cap is a wide margin either way.
 */
const DECODE_QUEUE_DEGRADE = 4;
/** …and the queue has to be back to about empty before climbing again. */
const DECODE_QUEUE_RECOVER = 1;
/**
 * Share of draws whose GPU work was still running when the next frame came to
 * be drawn (WebGlRenderer's fences), above which the pass is judged too slow
 * for this GPU — the third signal, and the one that sees a GPU that keeps UP
 * but runs BEHIND. Measured 25/09/2026 on a 2-CU Radeon iGPU: FSR1 at 1440p
 * held 60 fps with the decoder queue empty and the submission at 0.3 ms, while
 * every picture reached the screen ~29 ms late, two frames queued on the GPU
 * (click-to-photon 59.6 ms against 30.8 without the enhancer, 32.6 with SGSR).
 * Throughput hid it; latency is what the player pays.
 */
const GPU_BEHIND_DEGRADE = 0.5;
/** …and about none of them before climbing again. */
const GPU_BEHIND_RECOVER = 0.1;
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
     * @param {{fixedBudgetMs?: number, warmupMs?: number, noRecovery?: boolean,
     *          startRung?: string, recoverAfterMs?: number}} [profile]
     *   Overrides for a session that is not the owner's own:
     *   - fixedBudgetMs replaces the frame-arrival budget with a flat cost cap.
     *     An invited player's stream is fixed at 60fps and the guest never chose
     *     any of this, so "does it fit in 8ms" is a promise we can state up
     *     front, where the adaptive budget would depend on their link.
     *   - warmupMs ignores everything before it: the first seconds include
     *     shader compilation and pipeline warm-up, which are not the steady cost.
     *   - noRecovery keeps the enhancer off once it has been dropped —
     *     predictable beats optimal for someone who is only visiting.
     *   - startRung / recoverAfterMs resume where a previous session left the
     *     ladder (rememberedRung): below the ceiling, with that session's
     *     wait before the next bet.
     */
    constructor(preferred, profile = {}) {
        const idx = ENHANCER_LADDER.indexOf(preferred);
        this._ceiling = idx >= 0 ? idx : 0;
        const start = ENHANCER_LADDER.indexOf(profile.startRung);
        this._level = start > this._ceiling ? start : this._ceiling;
        this._degradeSince = 0;
        this._recoverSince = 0;
        this._recoverAfterMs = Math.min(
            RECOVER_MAX_MS,
            Math.max(RECOVER_BASE_MS, profile.recoverAfterMs > 0 ? profile.recoverAfterMs : 0),
        );
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

    /** The user's setting, as a rung. */
    get ceiling() {
        return ENHANCER_LADDER[this._ceiling];
    }

    /** How long the ladder waits in the clear before its next bet upward. */
    get recoverAfterMs() {
        return this._recoverAfterMs;
    }

    /**
     * Feed one observation window.
     * @param {{serviceMs: number, arrivalMs: number, now: number,
     *           decodeQueue?: number, gpuBehind?: number|null}} obs
     *   serviceMs per-frame cost of the render stage — the draw latency divided
     *             by how many draws overlap (PipelineDiag.renderServiceMs), NOT
     *             the raw wait: with two draws in flight each one waits about
     *             twice as long while costing the pipeline the same.
     *   arrivalMs average interval between incoming frames = the frame budget
     *   now       performance.now(), passed in so the policy stays pure
     *   decodeQueue average decodeQueueSize (PipelineDiag.decodeQueueAvg) —
     *             the cost a queued GPU pass does not report about itself, see
     *             DECODE_QUEUE_DEGRADE. Absent counts as an empty queue.
     *   gpuBehind share (0..1) of draws still on the GPU when the next one
     *             came (see GPU_BEHIND_DEGRADE). Absent or null — a renderer
     *             that cannot tell — counts as never behind.
     * @returns {string|null} the new algo when the level changed, else null.
     */
    update(obs) {
        const budget = obs && obs.arrivalMs;
        const wait = obs && obs.serviceMs;
        const now = (obs && obs.now) || 0;
        const queue = obs && obs.decodeQueue > 0 ? obs.decodeQueue : 0;
        const behind = obs && obs.gpuBehind > 0 ? obs.gpuBehind : 0;
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

        // Either the draw no longer fits, or it says it does while the decoder
        // starves behind it, or while the GPU finishes each picture after the
        // next one is due. The last two are the only signals a queued GPU pass
        // gives about its real cost.
        if (wait > degradeAbove || queue > DECODE_QUEUE_DEGRADE || behind > GPU_BEHIND_DEGRADE) {
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

        if (
            !this._noRecovery &&
            wait < recoverBelow &&
            queue <= DECODE_QUEUE_RECOVER &&
            behind <= GPU_BEHIND_RECOVER &&
            this._level > this._ceiling
        ) {
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
