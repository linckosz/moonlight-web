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
 * GuestLadder — an invited player's congestion ladder (issue #67, step 2).
 *
 * The owner's stream steps down when it keeps choking and back up once the
 * link has been quiet a while (app.js _onStreamCongested, _maybeUpgradeQuality).
 * A guest of a GameStream host (Sunshine, Wolf, Apollo) now has the same: its
 * page relaunches its own leg one step lower, and later one step higher. The
 * other viewers are not touched — each has a session of its own on the host.
 * A native host needs none of it: its rate governor follows each guest's link
 * between two frames, and its shared feed follows the slowest guest.
 *
 * The page never works a picture out. The host sends the whole ladder with
 * the join's answer (`guest_ladder`: the step the stream is on, and the
 * height and bitrate of every step, server/GuestProfile degraded()), and the
 * page only names a step back. This module decides when, and what to say.
 *
 * Its pace is the owner's, slower between two steps: each one restarts the
 * guest's stream, and the host takes one a minute at most from a guest
 * (ShareManager::kLadderStepGapMs); this waits a little longer, so the host
 * never has to refuse an honest page.
 *
 * Pure: the time comes in; nothing here touches the stream.
 */

/** Between two steps, either way: above the host's minute. */
export const STEP_GAP_MS = 65000;
/** Quiet before a step back up — the owner's, doubled on a flap, to 10 min. */
export const STABLE_MS = 75000;
export const MAX_STABLE_MS = 600000;
/** Congestion this soon after a step up: the network had not recovered. The
 *  owner's minute, counted from the earliest a step down may come. */
export const FLAP_MS = STEP_GAP_MS + 60000;

/**
 * @typedef {{height: number, bitrate_kbps: number}} LadderStep
 * @typedef {{level: number, toast: {key: string, params: Record<string, string>}}} LadderMove
 */

export class GuestLadder {
    constructor() {
        this.reset();
    }

    /** A Join of the guest's own starts afresh, as the owner's launch does. */
    reset() {
        /** @type {LadderStep[]} */
        this.steps = [];
        this.level = 0;
        this.lastStepAt = 0;
        this.lastUpAt = 0;
        this.lastSignalAt = 0;
        this.stableMs = STABLE_MS;
    }

    /**
     * The ladder the host sent with a join's answer. Anything malformed leaves
     * the guest with no ladder: nothing is ever stepped then.
     * @param {unknown} guestLadder
     */
    adopt(guestLadder) {
        const g = /** @type {{degrade?: unknown, steps?: unknown}} */ (guestLadder || {});
        const steps = Array.isArray(g.steps) ? g.steps : [];
        const valid = steps.every(
            (s) =>
                s &&
                Number.isInteger(s.height) &&
                s.height > 0 &&
                Number.isInteger(s.bitrate_kbps) &&
                s.bitrate_kbps > 0,
        );
        this.steps = valid
            ? steps.map((s) => ({ height: s.height, bitrate_kbps: s.bitrate_kbps }))
            : [];
        const level = Number.isInteger(g.degrade) ? /** @type {number} */ (g.degrade) : 0;
        this.level = Math.max(0, Math.min(level, Math.max(0, this.steps.length - 1)));
    }

    /** Whether there is a ladder to step at all. */
    get usable() {
        return this.steps.length > 1;
    }

    /** Any congestion hint, which keeps the link from counting as quiet. */
    noteSignal(now) {
        this.lastSignalAt = now;
    }

    /**
     * Sustained congestion: the step to relaunch at, or null — too soon after
     * the last step, or nothing left to lower.
     * @param {number} now
     * @returns {LadderMove|null}
     */
    down(now) {
        if (!this.usable || this._tooSoon(now)) return null;
        const from = this.steps[this.level];
        const next = this.level + 1;
        const to = this.steps[next];
        if (!to || (to.height === from.height && to.bitrate_kbps === from.bitrate_kbps))
            return null;
        // Congestion right after a step up: wait longer before the next one.
        if (this.lastUpAt && now - this.lastUpAt < FLAP_MS)
            this.stableMs = Math.min(MAX_STABLE_MS, this.stableMs * 2);
        this.lastStepAt = now;
        return {
            level: next,
            toast:
                to.height < from.height
                    ? { key: 'stream.degradeResolution', params: { res: String(to.height) } }
                    : {
                          key: 'stream.degradeBitrate',
                          params: { mbps: (to.bitrate_kbps / 1000).toFixed(1) },
                      },
        };
    }

    /**
     * A quiet link: the step back up to relaunch at, or null.
     * @param {number} now
     * @returns {LadderMove|null}
     */
    up(now) {
        if (!this.usable || this.level === 0 || this._tooSoon(now)) return null;
        const quietSince = Math.max(this.lastSignalAt, this.lastStepAt);
        if (now - quietSince < this.stableMs) return null;
        const from = this.steps[this.level];
        const next = this.level - 1;
        const to = this.steps[next];
        this.lastStepAt = now;
        this.lastUpAt = now;
        return {
            level: next,
            toast:
                to.height > from.height
                    ? { key: 'stream.upgradeResolution', params: { res: String(to.height) } }
                    : {
                          key: 'stream.upgradeBitrate',
                          params: { mbps: (to.bitrate_kbps / 1000).toFixed(1) },
                      },
        };
    }

    _tooSoon(now) {
        return this.lastStepAt > 0 && now - this.lastStepAt < STEP_GAP_MS;
    }
}
