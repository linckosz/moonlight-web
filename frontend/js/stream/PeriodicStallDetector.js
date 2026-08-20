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
 * PeriodicStallDetector — spots a *metronomic* disturbance on the link.
 *
 * Congestion is random; a radio being time-shared is not. When something else
 * periodically takes the client's Wi-Fi chip, frames stop arriving for a few
 * tens of milliseconds, then land bunched — at a fixed cadence, forever. That
 * regularity is the whole signal, and it is what separates "your network is
 * busy" (nothing we can advise) from "a process on this machine is stealing the
 * radio" (the user can fix it in one click).
 *
 * The motivating case is Apple's AWDL: AirDrop, Handoff, AirPlay, Sidecar and
 * Continuity share the Wi-Fi radio with the infrastructure link and leave it at
 * periodic availability windows. Measured on a Wi-Fi 5 MacBook against an
 * Ethernet host: `ping -i 0.1` showed a spike every 5th packet without
 * exception — a 500ms period — climbing from 28ms to 74ms against a 4.6ms
 * baseline, while the same host from an iPhone and a Windows PC on the same AP
 * was clean. Moonlight-QT and Parsec stuttered identically, which is what
 * proved it sits below the application. Disabling AirDrop removed it entirely.
 *
 * Detection is deliberately conservative — a false "your machine is misbehaving"
 * is worse than staying quiet:
 *   - keyframes are excluded (they are large, so their transit is long for
 *     reasons that have nothing to do with the link; a periodic IDR cadence
 *     would otherwise be a textbook false positive)
 *   - a warmup ignores the ramp-up (decoder configuration, keyframe burst)
 *   - only the *onset* of each late run counts, since one stall delays several
 *     consecutive frames
 *   - the verdict needs several events, spread over seconds, whose intervals
 *     agree with their own median
 *
 * Pure and clock-injected: `nowMs` is always a parameter, never read from
 * performance.now() here, so the whole thing is deterministic in tests.
 */

const DEFAULTS = {
    WARMUP_MS: 2000, // ignore the stream ramp-up entirely
    MIN_SPAN_MS: 4000, // shortest observation that may produce a verdict
    DRIFT_PER_FRAME: 0.05, // ms the baseline may rise per frame (~3ms/s @60fps)
    RESYNC_MS: 500, // |delay - baseline| past this = discontinuity → rebase
    // Excess over the baseline that counts as "disturbed". Sits above both the
    // legitimate Wi-Fi power-save jitter (~25ms p95) and the client's own
    // scheduling noise (rAF/GC), and is ~2.4 frame intervals at 60fps — so only
    // a stall big enough to actually judder counts. A 12ms blip is imperceptible
    // (we always show the latest frame) yet still periodic: on a Mac, AWDL keeps
    // cycling for Handoff/Continuity even with AirDrop off, which produced these
    // false positives on a stream the user reports as perfectly fluid. The real
    // AWDL case climbed 28→74ms (~46ms of excess), well clear of this floor.
    SPIKE_MS: 40,
    MIN_PERIOD_MS: 250, // below this it is not a radio time-share
    MAX_PERIOD_MS: 1200, // above this the user would call it a freeze, not judder
    MIN_EVENTS: 6, // onsets required (⇒ at least 5 intervals)
    MAX_ONSETS: 16, // ring bound on the onset history
    TOLERANCE: 0.15, // an interval may deviate this much from the median
    AGREEMENT: 0.7, // fraction of intervals that must agree to call it periodic
};

export class PeriodicStallDetector {
    constructor(tunables = {}) {
        this.cfg = { ...DEFAULTS, ...tunables };
        this.reset();
    }

    reset() {
        this._primed = false;
        this._baseline = 0;
        this._startMs = 0;
        this._onsets = [];
        this._wasSpike = false;
        this._verdict = null;
    }

    /** The latched verdict, or null while nothing periodic has been proven. */
    get verdict() {
        return this._verdict;
    }

    /**
     * Feed one arriving frame.
     * @param {number} backendTs capture time (uint32 ms, host steady_clock)
     * @param {number} nowMs arrival time, performance.now() domain
     * @param {boolean} isKeyframe keyframes are excluded from the statistic
     * @returns {{periodMs:number, events:number}|null} the verdict on the tick
     *   it is first reached, null on every other call — so the caller can warn
     *   once without tracking state of its own.
     */
    note(backendTs, nowMs, isKeyframe) {
        const c = this.cfg;
        // A keyframe is 10-50x the size of a delta: its transit time says more
        // about bitrate than about the link, and a periodic IDR cadence would
        // look exactly like the disturbance we are hunting.
        if (isKeyframe) return null;
        // No capture stamp (AV1 bootstrap, WSS fallback, pre-steady_clock
        // backend): there is no clock to measure transit against.
        if (!(backendTs > 0)) return null;

        // Transit delay carries an arbitrary constant (two unrelated clock
        // origins); only its variation matters, so the constant cancels against
        // the baseline below and never has to be known.
        const delay = nowMs - backendTs;

        // Rebase on the first frame and on any discontinuity: a uint32 backendTs
        // wrap, a resolution/session change, or a tab that was throttled in the
        // background. The threshold is far above any stall we care about.
        if (!this._primed || Math.abs(delay - this._baseline) > c.RESYNC_MS) {
            this._primed = true;
            this._baseline = delay;
            this._startMs = nowMs;
            this._onsets.length = 0;
            this._wasSpike = false;
            return null;
        }

        // Baseline = best transit seen lately. Down instantly (a shorter path is
        // a fact), up only by DRIFT_PER_FRAME, so a sustained bad patch cannot
        // be absorbed into the baseline and hide the disturbance.
        this._baseline = Math.min(delay, this._baseline + c.DRIFT_PER_FRAME);
        if (nowMs - this._startMs < c.WARMUP_MS) return null;

        const spike = delay - this._baseline > c.SPIKE_MS;
        // One stall delays a whole run of frames, which then land bunched. The
        // event is the run's ONSET; counting every late frame would turn a
        // single stall into a burst of fake "periods".
        const onset = spike && !this._wasSpike;
        this._wasSpike = spike;
        if (!onset) return null;

        this._onsets.push(nowMs);
        while (this._onsets.length > c.MAX_ONSETS) this._onsets.shift();
        const had = this._verdict;
        this._evaluate(nowMs);
        return had ? null : this._verdict;
    }

    /** Latch a verdict once the onset history looks metronomic. */
    _evaluate(nowMs) {
        const c = this.cfg;
        if (this._verdict) return; // latched: the advice does not change
        if (nowMs - this._startMs < c.MIN_SPAN_MS) return;
        const n = this._onsets.length;
        if (n < c.MIN_EVENTS) return;

        const gaps = [];
        for (let i = 1; i < n; i++) gaps.push(this._onsets[i] - this._onsets[i - 1]);
        const sorted = gaps.slice().sort((a, b) => a - b);
        const median = sorted[Math.floor(sorted.length / 2)];
        if (median < c.MIN_PERIOD_MS || median > c.MAX_PERIOD_MS) return;

        // Agreement rather than a variance test: a missed onset produces one
        // interval of ~2x the period, which should cost confidence but not veto
        // a verdict the other intervals support.
        const agree = gaps.filter((g) => Math.abs(g - median) <= median * c.TOLERANCE).length;
        if (agree / gaps.length < c.AGREEMENT) return;

        this._verdict = { periodMs: median, events: n };
    }
}
