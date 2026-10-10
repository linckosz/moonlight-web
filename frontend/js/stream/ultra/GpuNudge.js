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
 * GpuNudge — POC Ultra U3.7, B2.0 (design ultra-lan-poc §6.25). Chrome's GPU
 * process looks at its fences only now and then: work the GPU has finished is
 * seen 2 to 3 ms late. Any command wakes it up, so while a frame waits, an
 * empty submit every EVERY ms, on a loop of messages (a timer comes 1.5 to
 * 3 ms late, too late to help).
 *
 * The loop holds the main thread as long as it runs. auto (the default,
 * mw_ultra_nudge=0 turns it off) starts it only a little before the end expected: the
 * shortest submit → done of the last frames (the nudges bring it to ~0.35 ms
 * after the GPU's end), less LEAD; a timer waits until then, when that is
 * further away than a timer can be late. Nearer, and with all
 * (mw_ultra_nudge=all, the lab's way), the loop runs from the submit: the
 * nudges then also get the GPU started sooner on some (the AMD iGPU).
 */

// ms between two nudges; the lab found 1 ms leaves 0.6 ms of the delay.
const EVERY = 0.25;
// The loop starts this long before the end expected.
const LEAD = 1;
// How late a timer may come; a start nearer than this, and the loop runs at once.
const TIMER_LATE = 2.5;
// Frames whose submit → done the end expected is taken from (the 10th percentile).
const HISTORY = 32;
// A wait this long (a lost device) stops the loop.
const MAX_SPIN = 100;

/**
 * The key mw_ultra_nudge: 'auto' by default (no key, 1 or auto; §6.38 found
 * it 2.5 ms sooner at the client's screen), 'all', or null (off: 0, off, or
 * the empty value the bench recipes have always meant off by).
 */
export function nudgeMode() {
    let v = null;
    try {
        v = globalThis.localStorage?.getItem('mw_ultra_nudge') ?? null;
    } catch {
        v = null;
    }
    if (v === 'all') return 'all';
    if (v === null || v === '1' || v === 'auto') return 'auto';
    return null;
}

export class GpuNudge {
    /**
     * @param {GPUDevice} device the device the frames are submitted on
     * @param {'auto'|'all'} [mode]
     */
    constructor(device, mode = 'auto') {
        this.device = device;
        this.mode = mode;
        this.stats = { waits: 0, nudges: 0, timers: 0 };
        this._recent = [];
        this._wait = null;
        this._timer = 0;
        // Each wait's messages carry its number: one still on its way when
        // the wait ends must not start the next wait's loop at its submit.
        this._waits = 0;
        this._channel = new MessageChannel();
        this._channel.port1.onmessage = (e) => this._spin(e.data);
    }

    /** ms after the submit the loop starts at: 0 with all, or until enough frames are known. */
    lead() {
        if (this.mode === 'all' || this._recent.length < 4) return 0;
        const s = Float64Array.from(this._recent).sort();
        return Math.max(0, s[Math.floor(0.1 * s.length)] - LEAD);
    }

    /**
     * A submission at @p t1 now waits for its work done: the nudges until
     * end(). Returns the wait, whose record end() completes: {t1, from (the
     * start planned), first (the loop's real start, 0 if it never ran),
     * nudges, spin (ms the loop ran)}.
     */
    begin(t1) {
        this.end();
        const w = {
            id: ++this._waits,
            t1,
            from: t1 + this.lead(),
            first: 0,
            next: 0,
            nudges: 0,
            spin: 0,
            over: false,
        };
        this._wait = w;
        const ahead = w.from - performance.now();
        if (ahead > TIMER_LATE) {
            this.stats.timers++;
            this._timer = setTimeout(() => {
                this._timer = 0;
                if (!w.over) this._channel.port2.postMessage(w.id);
            }, ahead - TIMER_LATE);
        } else {
            this._channel.port2.postMessage(w.id);
        }
        return w;
    }

    _spin(id) {
        const w = this._wait;
        if (!w || w.over || w.id !== id) return;
        const now = performance.now();
        if (!w.first) w.first = now;
        if (now - w.t1 > MAX_SPIN) {
            w.over = true;
            return;
        }
        if (now >= w.next) {
            this.device.queue.submit([]);
            w.nudges++;
            w.next = now + EVERY;
        }
        this._channel.port2.postMessage(id);
    }

    /**
     * The wait is over: @p t2 when its work is done (learned for the next
     * start), nothing when it failed. Returns the wait, or null.
     */
    end(t2) {
        const w = this._wait;
        if (!w) return null;
        w.over = true;
        this._wait = null;
        if (this._timer) {
            clearTimeout(this._timer);
            this._timer = 0;
        }
        if (t2 === undefined) return w;
        w.spin = w.first ? Math.max(0, t2 - w.first) : 0;
        this.stats.waits++;
        this.stats.nudges += w.nudges;
        if (this._recent.length >= HISTORY) this._recent.shift();
        this._recent.push(t2 - w.t1);
        return w;
    }

    destroy() {
        this.end();
        this._channel.port1.close();
    }
}
