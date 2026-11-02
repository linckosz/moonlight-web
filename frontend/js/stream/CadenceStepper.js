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
 * CadenceStepper — "Auto" with detection, the client's half (design §33.10).
 *
 * ── Why ──────────────────────────────────────────────────────────────────────
 *
 * "Auto" streams at this screen's refresh rate. When what the host shows
 * changes faster — a page scrolling at 240 Hz on the virtual display, a game at
 * 80 frames a second for a 60 Hz screen — the host's gate carries one present
 * per refresh of this screen, and what a skipped present showed waits for the
 * next one. A canvas that tears paints each frame the moment it is decoded, so
 * a faster stream shows it younger pictures: 3 to 6 ms on the bench, on a
 * client that keeps up (the UM790Pro on Ethernet, the Mac). On one that does
 * not — the N95 on Wi-Fi, a weak iGPU — the frames queue somewhere between the
 * host and the screen and the picture gets OLDER, by tens or hundreds of
 * milliseconds. Which of the two a client is cannot be known in advance: this
 * measures it, a step at a time, and gives up the moment it costs.
 *
 * ── The ladder ───────────────────────────────────────────────────────────────
 *
 * The stream's own rate (Auto's), twice it, then the captured display's
 * refresh (240 on the product's virtual display), each capped at that refresh
 * (ladder()). A step is tried only while the host says its display presents
 * clearly faster than the current one — `stats.cadence.presents` above FASTER
 * times it — in FASTER_STATS of its reports in a row: a game at 50 frames a
 * second on a 60 Hz client never starts one, nor do the few faster seconds of
 * a window opening over it (a kiosk's launch on the bench, 01/10/2026). A
 * trial during which the content slows is taken back without blame, and a
 * kept step the content stops using, CONTENT_STABLE reports in a row, is let
 * go down to the one it uses.
 *
 * ── A trial ──────────────────────────────────────────────────────────────────
 *
 * BASE_MS at the current step, then the next one is asked for (`fpsstep`) and
 * the host answers at once: applied, capped, refused. SETTLE_MS after the
 * answer, TRIAL_MS are measured. A window's age is what the viewer looks at:
 * the median of capture → painted on the host's clock (ClockEstimator, fed by
 * the pongs), plus half the interval between two painted frames — a picture
 * stays up until the next one. The step is kept when the age drops by GAIN_MS
 * or more and the client paints PAINTED_MIN of what it receives; otherwise the
 * stream goes back to the step before, and the next trial waits BACKOFF_MS —
 * 30 s, 1, 2, 4 min… — reset when the content or the link changes. A kept step
 * is the base of the next trial, if there is one and the content asks for it.
 *
 * ── The net ──────────────────────────────────────────────────────────────────
 *
 * Armed whenever the stream runs above this screen's rate, trials included:
 * capture → painted over the last FILET_WINDOW_MS above its reference — the
 * same, measured at the client's own rate — by half a refresh for
 * FILET_HOLD_MS, or QUEUE_FULL frames at the decoder for QUEUE_HOLD_MS, and
 * the stream is back at the client's rate in one round trip, whatever the
 * cause (Wi-Fi, main thread, decoder). The hold lets a spike of the link pass
 * — a Mac on Wi-Fi has them at any rate — where a client that drowns stays
 * above. NET_STRIKES trips in a row on one link, and the next trial waits the
 * longest backoff, whatever the content does meanwhile.
 *
 * A Mac on Wi-Fi rises above that bound a third of the time at its own rate,
 * for 1 to 2.7 s at a stretch, and fares no worse at 240 — where an N95 on
 * Wi-Fi drowns (02/10/2026). Nothing tells the two apart within the first
 * second, so the net widens on trust. When the link, at the client's rate,
 * has stood above the bound longer than FILET_HOLD_MS — in the frames kept
 * before the trial, or over LINK_HISTORY_MS since — the trial holds through
 * its longest rise × FILET_WIDE_FACTOR, FILET_WIDE_MIN_MS at least and
 * FILET_WIDE_MAX_MS at most, and is
 * judged on the lower quartile of capture → painted, which a rise does not
 * move. A widened trial that fails, given up or tripped, counts as a trip and
 * makes the next one narrow; a narrow net that trips on a rise, or a widened
 * one under a step already kept, lets the next one widen again.
 *
 * ── What this device remembers ───────────────────────────────────────────────
 *
 * A step that cost this device MEMO_FAILS times in one stream — a trial the
 * net took back, or one given up on an older picture or frames left unpainted
 * — and was never kept, is remembered for this host at this resolution
 * (stepMemory, localStorage). The next streams do not try it for their first
 * MEMO_WAIT_MS: a client that cannot carry it (the N95 on Wi-Fi: its p99 211 →
 * 350 ms on the trials alone, 02/10/2026) no longer pays for it at each
 * launch. A step kept is forgotten, a remembered one expires after
 * MEMO_TTL_MS, and a link that changes under the stream tries it again. A
 * trial given up for no gain costs nothing and is not remembered.
 *
 * ── Where it runs ────────────────────────────────────────────────────────────
 *
 * StreamView decides: a native host, a rate left to Auto, a canvas that tears
 * (Chromium on a desktop, its default), frames decoded and drawn on the main
 * thread. A client on vsync shows one frame per refresh and keeps its rate.
 * Pure apart from its two timers: the caller feeds the frames, the pongs, the
 * stats and the host's answers.
 */

import { ClockEstimator } from '../util/ClockEstimator.js';

/** After the first painted frame: a stream's first second is start-up traffic. */
export const WARMUP_MS = 1500;
/** The window a trial is compared with, at the current step. */
export const BASE_MS = 2500;
/** After the host's answer, before the trial is measured. */
export const SETTLE_MS = 400;
/** The trial's measured window. */
export const TRIAL_MS = 2500;
/** No answer by then: the step is taken back. */
export const REPLY_TIMEOUT_MS = 2000;
/** The age must drop at least this much for a step to be kept, ms. */
export const GAIN_MS = 1;
/** …and the client must paint at least this share of what it receives. */
export const PAINTED_MIN = 0.9;
/** Content counts as faster than a step above this many times its rate. */
export const FASTER = 1.15;
/** Fewest painted frames a window is judged on. */
export const MIN_FRAMES = 20;
/** The net looks at this much of the latest capture → painted… */
export const FILET_WINDOW_MS = 250;
/** …once it holds this many frames. */
export const FILET_MIN_FRAMES = 4;
/**
 * …and above its bound this long before it trips. Without it, the Mac on
 * Wi-Fi lost each 240 kept, within 0.1 s, to spikes it has at 120 as well
 * (p99 of 140 to 240 ms), while cadence=host-guarded showed it 15 ms younger
 * pictures at 240 (02/10/2026).
 */
export const FILET_HOLD_MS = 500;
/** How far back the link's own rises, at the client's rate, are remembered. */
export const LINK_HISTORY_MS = 60000;
/** The widened net holds through the link's longest rise times this… */
export const FILET_WIDE_FACTOR = 1.2;
/**
 * …at least this long: the rises seen before a trial are not the longest the
 * link has. The Mac on Wi-Fi rose up to 1.8 s at its own rate and 2.8 s at
 * 240 later on, and lost its kept 240 in 5 passes of 7 (02/10/2026).
 */
export const FILET_WIDE_MIN_MS = 3000;
/** …at most this long: past it, a rise is not the link's own. */
export const FILET_WIDE_MAX_MS = 4000;
/** Frames waiting at the decoder that make a queue… */
export const QUEUE_FULL = 2;
/** …once it has stood this long. */
export const QUEUE_HOLD_MS = 150;
/** Waits before the next trial, after each failed one. */
export const BACKOFF_MS = [30000, 60000, 120000, 240000, 480000, 960000];
/**
 * Trips of the net in a row on one link before the next trial waits the
 * longest backoff, whatever the content does meanwhile. A link that drops
 * every step (the N95 on Wi-Fi: 8 trials, 8 trips, 02/10/2026) is then not
 * tried again every 30 s, nor each time the content changes.
 */
export const NET_STRIKES = 2;
/** A kept step that holds this long clears the trips counted. */
export const HELD_MS = 30000;
/** Stats in a row before the content counts as changed. */
export const CONTENT_STABLE = 2;
/**
 * Stats in a row with the content faster before a step is asked: about five
 * seconds. A kiosk opening over a 50-frame page on the bench presented 84 to
 * 169 times a second for four of them, and was tried (01/10/2026).
 */
export const FASTER_STATS = 5;
/** Failures of a step in one stream, never kept, before this device remembers it. */
export const MEMO_FAILS = 2;
/** A remembered step is not tried in a stream's first… */
export const MEMO_WAIT_MS = BACKOFF_MS[BACKOFF_MS.length - 1];
/** …and is forgotten after this long: a browser, a driver, a network change. */
export const MEMO_TTL_MS = 7 * 24 * 3600 * 1000;
/** The localStorage key, and how many host × resolution entries it keeps. */
export const MEMO_KEY = 'mw_autostep_failed';
const MEMO_ENTRIES = 32;
/** How many of the host's reports are kept. */
const SAMPLES_KEPT = 12;
/** Stats older than this say nothing about the content. */
export const STATS_STALE_MS = 3000;
/** Stats in a row before a step the host no longer runs is let go. */
export const HOST_MISMATCH = 2;
/** The state machine's clock, and the pings for the host's clock. */
export const TICK_MS = 100;
export const PING_EVERY_MS = 500;
/** How long frames are kept: a base window, an answer, a settle, a trial. */
export const KEEP_MS = 9000;
/** How many decisions the bench can read back. */
const EVENTS_KEPT = 200;

const WRAP_MS = 2 ** 32;

/**
 * Whether the detection runs. On by default since the bench judged it (porte
 * UA, 02/10/2026); the switch `mw_autostep` in localStorage turns it off ('0')
 * or on ('1'). A TV keeps it off unless switched on: what it calls painted is
 * the frame handed to its <video>, not what its screen shows, and a TV that
 * shows 30 frames a second would keep 60.
 * @param {boolean} [tv] this screen is a TV's
 * @returns {boolean}
 */
export function autostepEnabled(tv = false) {
    let v = null;
    try {
        v = localStorage.getItem('mw_autostep');
    } catch (e) {
        // Storage refused: the default.
    }
    if (v === '0') return false;
    if (v === '1') return true;
    return !tv;
}

/**
 * The stream's steps: its own rate, twice it, then the display's refresh,
 * each capped at that refresh and above the one before. [] without a rate.
 * @param {number} baseFps
 * @param {number} displayHz 0 when unknown: no cap, no display step
 * @param {number} [maxFps] no step above this; 0 or none: no bound
 * @returns {number[]}
 */
export function ladder(baseFps, displayHz, maxFps = 0) {
    if (!(baseFps > 0)) return [];
    const levels = [Math.round(baseFps)];
    const add = (fps) => {
        const capped = displayHz > 0 ? Math.min(fps, displayHz) : fps;
        if (maxFps > 0 && capped > maxFps) return;
        if (capped > levels[levels.length - 1]) levels.push(capped);
    };
    add(levels[0] * 2);
    if (displayHz > 0) add(Math.round(displayHz));
    return levels;
}

/**
 * The steps this device failed at, for one host at one resolution (@p key),
 * kept in localStorage as { key: { fps: when } }. Storage refused or broken:
 * nothing is remembered.
 * @param {string} key e.g. the host's uuid and "1920×1080"
 * @param {Storage|null} [storage] localStorage by default
 * @param {() => number} [clock] Date.now by default
 */
export function stepMemory(key, storage, clock) {
    let store = storage;
    if (store === undefined) {
        try {
            store = localStorage;
        } catch (e) {
            store = null;
        }
    }
    const nowMs = clock || (() => Date.now());
    const read = () => {
        try {
            const all = JSON.parse(store.getItem(MEMO_KEY) || '{}');
            return all && typeof all === 'object' ? all : {};
        } catch (e) {
            return {};
        }
    };
    const write = (all) => {
        try {
            store.setItem(MEMO_KEY, JSON.stringify(all));
        } catch (e) {
            // Storage refused: nothing remembered.
        }
    };
    const fresh = (entry, now) =>
        Object.keys(entry || {}).filter((fps) => now - entry[fps] < MEMO_TTL_MS);
    return {
        /** @returns {number[]} the steps remembered as failed, not expired */
        failed() {
            const entry = read()[key];
            return fresh(entry, nowMs()).map(Number);
        },
        /** Remember @p fps as failed here. */
        fail(fps) {
            const now = nowMs();
            const all = read();
            for (const k of Object.keys(all)) {
                const kept = {};
                for (const f of fresh(all[k], now)) kept[f] = all[k][f];
                if (Object.keys(kept).length) all[k] = kept;
                else delete all[k];
            }
            all[key] = { ...(all[key] || {}), [fps]: now };
            // The oldest hosts and resolutions go first.
            const last = (k) => Math.max(...Object.values(all[k]));
            const keys = Object.keys(all).sort((a, b) => last(b) - last(a));
            for (const k of keys.slice(MEMO_ENTRIES)) delete all[k];
            write(all);
        },
        /** Forget @p fps: it was kept here. */
        clear(fps) {
            const all = read();
            if (!all[key] || !(fps in all[key])) return;
            delete all[key][fps];
            if (!Object.keys(all[key]).length) delete all[key];
            write(all);
        },
    };
}

/** The median of an ascending array. */
function median(sorted) {
    const n = sorted.length;
    if (!n) return NaN;
    return n % 2 ? sorted[n >> 1] : (sorted[n / 2 - 1] + sorted[n / 2]) / 2;
}

export class CadenceStepper {
    /**
     * @param {object} deps
     * @param {number} deps.baseFps the stream's own rate, Auto's choice
     * @param {(msg: object) => void} deps.send a control message to the host
     * @param {(seq: number, ts: number) => void} [deps.sendPing] a `ping` the
     *        host answers with a `pong` carrying `host` (its steady µs)
     * @param {() => number} [deps.now] this client's clock, ms
     * @param {(line: string) => void} [deps.log]
     * @param {{every: (fn: () => void, ms: number) => any, stop: (id: any) => void}} [deps.timers]
     * @param {ReturnType<typeof stepMemory>} [deps.memory] the steps this
     *        device failed at, for this host at this resolution
     * @param {number} [deps.maxFps] no step above this (Auto's learned ceiling)
     */
    constructor({ baseFps, send, sendPing, now, log, timers, memory, maxFps }) {
        this._maxFps = maxFps > 0 ? maxFps : 0;
        this._send = send;
        this._sendPing = sendPing || (() => {});
        this._now = now || (() => performance.now());
        this._log = log || ((line) => console.log(line));
        this._timers = timers || {
            every: (fn, ms) => setInterval(fn, ms),
            stop: (id) => clearInterval(id),
        };
        this._clock = new ClockEstimator();
        this._base = baseFps > 0 ? Math.round(baseFps) : 0;
        this._display = 0;
        this._levels = ladder(this._base, 0, this._maxFps);
        this._presents = 0;
        /** @type {number[]} the display's presents a second, the host's latest reports last */
        this._samples = [];
        this._statsAt = -Infinity;
        this._hostStep = 0;
        this._mismatch = 0;
        this._level = 0; // the step kept: an index into _levels
        this._phase = 'idle'; // idle | base | asking | settling | trial
        this._try = 0; // the step being tried
        this._trialFps = 0; // …and the rate the host granted for it
        this._baseFrom = 0;
        this._baseTo = 0;
        this._askedAt = 0;
        this._trialFrom = 0;
        /** @type {{at: number, ts: number}[]} painted frames: when, and their capture (host ms) */
        this._painted = [];
        /** @type {number[]} when frames arrived */
        this._received = [];
        this._firstPaintAt = 0;
        this._reference = null; // capture → painted at the client's own rate, ms
        this._overSince = 0; // since when capture → painted has stood above the net's bound
        this._hold = FILET_HOLD_MS; // the net's hold for the trial or the step in force
        this._wide = false; // …widened, and the trial judged on the lower quartile
        this._widen = true; // the next trial may widen: no widened trial has failed here
        /** @type {{end: number, ms: number}[]} the link's rises at the client's rate */
        this._baseRises = [];
        this._baseRiseSince = 0;
        this._queueSince = 0;
        this._backoff = 0;
        this._strikes = 0; // trips of the net in a row on this link
        this._keptAt = 0;
        this._nextTrialAt = 0;
        this._band = 0;
        this._bandSeen = 0;
        this._bandCount = 0;
        this._capped = false;
        this._running = false;
        this._tickTimer = null;
        this._pingTimer = null;
        this._seq = 1 << 22; // apart from the view's, the grid's and the probe's pings
        this._memory = memory || null;
        /** @type {Set<number>} steps failed here in earlier streams */
        this._remembered = new Set();
        this._memoLifted = false; // the link changed: the remembered steps are tried
        this._memoSaid = false;
        this._startedAt = 0;
        /** @type {Map<number, number>} failures that cost, per step, in this stream */
        this._fails = new Map();
        /** @type {Set<number>} steps kept in this stream */
        this._keptSteps = new Set();
        this.trials = 0;
        this.kept = 0;
        this.rejected = 0;
        this.refused = 0;
        this.trips = 0;
        /** @type {object[]} what was decided, and on what — for the bench */
        this.events = [];
    }

    get running() {
        return this._running;
    }

    /** The rate the stream is asked to run at: 0 for its own. */
    get stepFps() {
        return this._level > 0 ? this._levels[this._level] : 0;
    }

    start() {
        if (this._running) return;
        this._running = true;
        this._startedAt = this._now();
        if (this._memory) this._remembered = new Set(this._memory.failed());
        this._pingTimer = this._timers.every(
            () => this._sendPing(this._seq++, this._now()),
            PING_EVERY_MS,
        );
        this._tickTimer = this._timers.every(() => this.tick(this._now()), TICK_MS);
    }

    /** Stops measuring; a step in force is taken back. */
    stop() {
        if (!this._running) return;
        this._running = false;
        this._timers.stop(this._pingTimer);
        this._timers.stop(this._tickTimer);
        this._pingTimer = null;
        this._tickTimer = null;
        if (this._stepped()) this._ask(0);
        this._level = 0;
        this._phase = 'idle';
    }

    /** A pong: the host's clock. */
    notePong(msg, recvMs) {
        if (msg && typeof msg.host === 'number' && typeof msg.ts === 'number')
            this._clock.note(msg.ts, msg.host, recvMs);
    }

    /**
     * The host's word on its cadence, once a second (`stats.cadence`): its
     * display's presents per second, the stream's own rate, the step it
     * runs, its display's refresh.
     */
    noteStats(cadence, now) {
        if (!cadence || typeof cadence !== 'object') return;
        this._statsAt = now;
        this._presents = cadence.presents > 0 ? cadence.presents : 0;
        this._samples.push(this._presents);
        if (this._samples.length > SAMPLES_KEPT) this._samples.shift();
        this._hostStep = cadence.step > 0 ? cadence.step : 0;
        const base = cadence.base > 0 ? Math.round(cadence.base) : 0;
        const display = cadence.display > 0 ? Math.round(cadence.display) : 0;
        if (base > 0 && (base !== this._base || display !== this._display)) {
            const moved = this._base > 0 && base !== this._base;
            this._base = base;
            this._display = display;
            this._levels = ladder(base, display, this._maxFps);
            if (moved) this._restart("the stream's own rate moved", now);
            else if (this._level >= this._levels.length) this._fallBack('no such step', now);
        }
        this._noteBand(now);
        this._stepDown(now);
        this._reconcile(now);
    }

    /** The host's answer to an `fpsstep`. */
    noteReply(msg, now) {
        if (this._phase !== 'asking' || !msg) return;
        const fps = msg.fps > 0 ? msg.fps : 0;
        const ok = (msg.verdict === 'applied' || msg.verdict === 'capped') && fps > this._rate();
        if (!ok) {
            // The step in force before, if any, stays (the host keeps it).
            this.refused++;
            this._event('refused', now, {
                asked: msg.asked,
                why: msg.why || msg.verdict,
                fps,
            });
            this._log(
                `[CadenceStepper] ${msg.asked} fps refused by the host` +
                    (msg.why ? ': ' + msg.why : '') +
                    ` — staying at ${this._rate()} fps`,
            );
            this._phase = 'idle';
            this._delay(now);
            return;
        }
        this._phase = 'settling';
        this._trialFrom = now + SETTLE_MS;
        this._trialFps = fps;
        this._event('trial', now, {
            from: this._rate(),
            to: fps,
            verdict: msg.verdict,
            netMs: Math.round(this._hold),
        });
    }

    /** A frame arrived, whole. */
    noteReceived(at) {
        this._received.push(at);
    }

    /** A frame was painted at @p paintedMs; @p backendTs its capture, host ms (32 bits). */
    notePainted(backendTs, paintedMs) {
        if (!(backendTs > 0)) return;
        if (!this._firstPaintAt) this._firstPaintAt = paintedMs;
        this._painted.push({ at: paintedMs, ts: backendTs, base: !this._stepped() });
        this._checkNet(paintedMs);
    }

    /** VideoDecoder.decodeQueueSize, as it stands at @p now. */
    noteDecodeQueue(depth, now) {
        if (depth >= QUEUE_FULL) {
            if (!this._queueSince) this._queueSince = now;
            this._checkNet(now);
        } else {
            this._queueSince = 0;
        }
    }

    /** The decoder asked the host for fewer frames (`clientfpscap`): no step meanwhile. */
    setDecoderCapped(capped, now) {
        this._capped = capped === true;
        if (this._capped && (this._stepped() || this._phase !== 'idle'))
            this._fallBack('the decoder asked for fewer frames', now);
    }

    /** The link or the screen changed under the stream: start over. */
    linkChanged(why, now) {
        // Another link: what failed on the last one is tried again.
        this._memoLifted = true;
        this._restart(why || 'the link changed', now);
    }

    /** The state machine; on its own timer once started. */
    tick(now) {
        if (!this._running) return;
        this._prune(now);
        // A kept step that holds: the link carries it, its trips are forgotten.
        if (this._strikes && this._level > 0 && now - this._keptAt >= HELD_MS) this._strikes = 0;
        switch (this._phase) {
            case 'idle':
                if (this._mayTry(now)) {
                    this._phase = 'base';
                    this._baseFrom = now;
                }
                break;
            case 'base':
                if (!this._fasterRun(this._rate()) || now - this._statsAt > STATS_STALE_MS) {
                    // The content slowed before a step was asked: nothing
                    // tried, nothing to blame.
                    this._phase = 'idle';
                    break;
                }
                if (
                    now - this._baseFrom >= BASE_MS &&
                    this._fasterRun(this._rate()) >= FASTER_STATS
                )
                    this._askNext(now);
                break;
            case 'asking':
                if (now - this._askedAt >= REPLY_TIMEOUT_MS) {
                    this._log('[CadenceStepper] no answer from the host — step taken back');
                    this._event('timeout', now, {});
                    this._ask(this.stepFps);
                    this._phase = 'idle';
                    this._delay(now);
                }
                break;
            case 'settling':
            case 'trial':
                if (this._statsAt > this._askedAt && !this._fasterRun(this._rate())) {
                    // The content slowed under the trial: what it would
                    // measure is the content, not the step. Taken back, no
                    // blame.
                    this._event('inconclusive', now, { why: 'the content slowed' });
                    this._ask(this.stepFps);
                    this._phase = 'idle';
                    this._nextTrialAt = now + 1000;
                    break;
                }
                if (now >= this._trialFrom) this._phase = 'trial';
                if (now >= this._trialFrom + TRIAL_MS) this._judge(now);
                break;
        }
    }

    // ── The steps ───────────────────────────────────────────────────────────

    /** The rate the stream runs at now: the kept step's. */
    _rate() {
        return this._levels.length ? this._levels[this._level] : this._base;
    }

    /** Whether the host may be running above this screen's rate right now. */
    _stepped() {
        return (
            this._level > 0 ||
            this._phase === 'asking' ||
            this._phase === 'settling' ||
            this._phase === 'trial'
        );
    }

    _mayTry(now) {
        if (this._capped || this._level + 1 >= this._levels.length) return false;
        if (now < this._nextTrialAt) return false;
        if (!this._clock.ready || !this._firstPaintAt) return false;
        if (now - this._firstPaintAt < WARMUP_MS) return false;
        if (now - this._statsAt > STATS_STALE_MS) return false;
        // The base window opens on the first faster report; the step is asked
        // once FASTER_STATS of them are in (tick, 'base').
        if (this._fasterRun(this._rate()) <= 0) return false;
        // A step this device failed at before waits, early in the stream.
        const next = this._levels[this._level + 1];
        if (
            this._remembered.has(next) &&
            !this._memoLifted &&
            now - this._startedAt < MEMO_WAIT_MS
        ) {
            if (!this._memoSaid) {
                this._memoSaid = true;
                this._event('remembered', now, { fps: next });
                this._log(
                    `[CadenceStepper] ${next} fps failed on this device before — not tried ` +
                        `in the first ${Math.round(MEMO_WAIT_MS / 60000)} min`,
                );
            }
            return false;
        }
        return true;
    }

    /** How many of the host's latest reports, in a row, show content faster than @p fps. */
    _fasterRun(fps) {
        let n = 0;
        for (let i = this._samples.length - 1; i >= 0 && this._samples[i] > fps * FASTER; i--) n++;
        return n;
    }

    /** …and how many, in a row, show it no faster. */
    _slowerRun(fps) {
        let n = 0;
        for (let i = this._samples.length - 1; i >= 0 && this._samples[i] <= fps * FASTER; i--) n++;
        return n;
    }

    /**
     * A kept step the content no longer uses, CONTENT_STABLE reports in a
     * row: down to the step it does use — the content's doing, nothing to
     * blame, and the next trial waits for it to be faster again.
     */
    _stepDown(now) {
        if (this._phase !== 'idle' || this._level === 0) return;
        let level = this._level;
        while (level > 0 && this._slowerRun(this._levels[level - 1]) >= CONTENT_STABLE) level--;
        if (level === this._level) return;
        const from = this.stepFps;
        this._level = level;
        this._ask(this.stepFps);
        this._event('stepdown', now, { from, to: this._rate(), presents: this._presents });
        this._log(
            `[CadenceStepper] back to ${this._rate()} fps: the content presents ` +
                `${this._presents} times a second, ${from} fps no longer used`,
        );
    }

    /** Close the base window and ask for the next step. */
    _askNext(now) {
        const base = this._window(this._baseFrom, now);
        if (!base) {
            // Too few frames to judge (the content stopped): look again soon.
            this._phase = 'idle';
            this._nextTrialAt = now + 1000;
            return;
        }
        if (this._level === 0) this._reference = base.latency;
        // The net of this trial: widened when the link, at the client's rate,
        // rises on its own longer than the net holds — in the frames kept, or
        // since the reference was taken — unless a widened trial failed here.
        const rise = Math.max(this._longestBaseRise(now), this._scanBaseRises());
        this._wide = this._widen && rise >= FILET_HOLD_MS;
        this._hold = this._wide
            ? Math.min(FILET_WIDE_MAX_MS, Math.max(FILET_WIDE_MIN_MS, rise * FILET_WIDE_FACTOR))
            : FILET_HOLD_MS;
        this._baseTo = now;
        this._try = this._level + 1;
        this._phase = 'asking';
        this._askedAt = now;
        this.trials++;
        this._ask(this._levels[this._try]);
        this._log(
            `[CadenceStepper] trying ${this._levels[this._try]} fps (from ${this._rate()}): ` +
                `content at ${this._presents}/s, age ${base.age.toFixed(1)} ms here` +
                (this._wide
                    ? `, a net of ${Math.round(this._hold)} ms (the link rises ` +
                      `${Math.round(rise)} ms at its own rate)`
                    : ''),
        );
    }

    /** The trial's window is in: keep the step, or go back. */
    _judge(now) {
        const base = this._window(this._baseFrom, this._baseTo);
        const trial = this._window(this._trialFrom, now);
        if (!base || !trial) {
            // The content stopped meanwhile: nothing to judge, no blame.
            this._event('inconclusive', now, {});
            this._ask(this.stepFps);
            this._phase = 'idle';
            this._nextTrialAt = now + 1000;
            return;
        }
        // A widened trial is judged on the lower quartile: a rise of the link,
        // there at any rate, moves the median of a window it falls in.
        const baseAge = this._wide ? base.lowAge : base.age;
        const trialAge = this._wide ? trial.lowAge : trial.age;
        const gain = baseAge - trialAge;
        const keep = gain >= GAIN_MS && trial.ratio >= PAINTED_MIN;
        const facts = {
            from: this._rate(),
            to: this._trialFps,
            baseAge,
            trialAge,
            gain,
            ratio: trial.ratio,
            baseLatency: base.latency,
            trialLatency: trial.latency,
            measure: this._wide ? 'lower quartile' : 'median',
        };
        this._log(
            `[CadenceStepper] ${this._trialFps} fps ${keep ? 'kept' : 'given up'}: age ` +
                `${baseAge.toFixed(1)} → ${trialAge.toFixed(1)} ms` +
                (this._wide ? ' (lower quartile)' : '') +
                `, ${Math.round(trial.ratio * 100)} % of the frames painted`,
        );
        if (!keep) {
            this.rejected++;
            this._event('rejected', now, facts);
            // Given up on an older picture, or frames left unpainted: the
            // step cost. Given up for no gain, it did not.
            if (trial.ratio < PAINTED_MIN || -gain > 500 / this._base)
                this._failed(this._levels[this._try], now);
            this._ask(this.stepFps);
            this._phase = 'idle';
            // A widened trial given up: the link had its chance — a trip.
            if (this._wide) {
                this._strike();
                this._widen = false;
            }
            this._delay(now);
            return;
        }
        this.kept++;
        this._event('kept', now, facts);
        // Kept here: whatever this device remembered of it is forgotten.
        const asked = this._levels[this._try];
        this._keptSteps.add(asked);
        if (this._remembered.delete(asked) && this._memory) this._memory.clear(asked);
        // The step is what the host granted: capped below the rate asked when
        // the ladder was built before the display's refresh was known.
        if (this._trialFps !== this._levels[this._try]) {
            const above = this._levels.slice(this._try + 1).filter((f) => f > this._trialFps);
            this._levels = this._levels.slice(0, this._try).concat([this._trialFps], above);
        }
        this._level = this._try;
        this._backoff = 0;
        this._keptAt = now;
        this._phase = 'idle';
        // The next step from this window — at once when the content has asked
        // for it long enough, otherwise once it has (tick, 'base').
        if (this._mayTry(now)) {
            this._phase = 'base';
            this._baseFrom = this._trialFrom;
            if (this._fasterRun(this._rate()) >= FASTER_STATS) this._askNext(now);
        }
    }

    /** Tell the host: the stream at @p fps, 0 for its own rate. */
    _ask(fps) {
        this._send({ type: 'fpsstep', fps: fps > 0 ? fps : 0 });
    }

    /** The next trial waits, longer each time it is called. */
    _delay(now) {
        this._nextTrialAt = now + BACKOFF_MS[Math.min(this._backoff, BACKOFF_MS.length - 1)];
        this._backoff++;
    }

    /** Back to the client's own rate at once, and wait before trying again. */
    _fallBack(why, now) {
        if (this._stepped()) this._ask(0);
        this._event('fallback', now, { why, from: this.stepFps || this._trialFps || 0 });
        this._log(`[CadenceStepper] back to ${this._base} fps: ${why}`);
        this._level = 0;
        this._phase = 'idle';
        this._queueSince = 0;
        this._overSince = 0;
        this._delay(now);
    }

    /** Start over: back to the client's rate, the backoff forgotten. */
    _restart(why, now) {
        if (this._stepped()) this._ask(0);
        this._event('restart', now, { why });
        this._level = 0;
        this._phase = 'idle';
        this._overSince = 0;
        this._reference = null;
        this._backoff = 0;
        this._strikes = 0;
        this._widen = true;
        this._wide = false;
        this._hold = FILET_HOLD_MS;
        this._baseRises = [];
        this._baseRiseSince = 0;
        this._nextTrialAt = now + WARMUP_MS;
    }

    // ── The net ─────────────────────────────────────────────────────────────

    /** A failed trial counted against the link: NET_STRIKES in a row, the longest wait. */
    _strike() {
        this._strikes++;
        if (this._strikes >= NET_STRIKES) this._backoff = BACKOFF_MS.length - 1;
    }

    /**
     * The net tripped: back at once. The next trial may widen after a trip on
     * a rise (@p onRise) — of the narrow net, or of a widened one under a step
     * already kept — not after a widened trial that did not hold, nor after a
     * decode queue.
     */
    _netTrip(why, now, onRise) {
        const trial =
            this._phase === 'asking' || this._phase === 'settling' || this._phase === 'trial';
        this.trips++;
        this._strike();
        if (trial) this._failed(this._levels[this._try], now);
        this._widen = onRise === true && !(this._wide && trial);
        this._fallBack(why, now);
    }

    /**
     * A trial of @p fps cost this device: MEMO_FAILS of them in this stream,
     * the step never kept, and it is remembered for the streams to come.
     */
    _failed(fps, now) {
        const n = (this._fails.get(fps) || 0) + 1;
        this._fails.set(fps, n);
        if (n !== MEMO_FAILS || this._keptSteps.has(fps) || !this._memory) return;
        this._memory.fail(fps);
        this._event('remember', now, { fps, fails: n });
        this._log(
            `[CadenceStepper] ${fps} fps failed ${n} times, never kept — remembered on this ` +
                'device for this host at this resolution',
        );
    }

    _checkNet(now) {
        if (!this._stepped()) {
            this._overSince = 0;
            this._noteBaseRise(now);
            return;
        }
        if (this._queueSince && now - this._queueSince >= QUEUE_HOLD_MS) {
            this._netTrip('a decode queue that holds', now, false);
            return;
        }
        if (this._reference === null) return;
        const latency = this._recentLatency(now);
        if (latency === null) return;
        const half = 500 / this._base;
        if (latency <= this._reference + half) {
            this._overSince = 0;
            return;
        }
        if (!this._overSince) this._overSince = now;
        if (now - this._overSince < this._hold) return;
        this._netTrip(
            `capture → painted ${latency.toFixed(1)} ms, over ` +
                `${this._reference.toFixed(1)} + ${half.toFixed(1)} at the client's rate ` +
                `for ${Math.round(now - this._overSince)} ms`,
            now,
            true,
        );
    }

    /** The median of capture → painted over the last FILET_WINDOW_MS; null when too few. */
    _recentLatency(now) {
        if (!this._clock.ready) return null;
        const recent = [];
        for (let i = this._painted.length - 1; i >= 0; i--) {
            const s = this._painted[i];
            if (s.at < now - FILET_WINDOW_MS) break;
            const ms = this._latency(s);
            if (ms !== null) recent.push(ms);
        }
        if (recent.length < FILET_MIN_FRAMES) return null;
        return median(recent.sort((a, b) => a - b));
    }

    /**
     * At the client's own rate: how long the link stays above the net's
     * bound on its own — what a widened net has to hold through.
     */
    _noteBaseRise(now) {
        if (this._level > 0 || this._reference === null) {
            this._baseRiseSince = 0;
            return;
        }
        const latency = this._recentLatency(now);
        if (latency === null) return;
        if (latency > this._reference + 500 / this._base) {
            if (!this._baseRiseSince) this._baseRiseSince = now;
            return;
        }
        if (this._baseRiseSince) {
            this._baseRises.push({ end: now, ms: now - this._baseRiseSince });
            this._baseRiseSince = 0;
        }
        while (this._baseRises.length && this._baseRises[0].end < now - LINK_HISTORY_MS)
            this._baseRises.shift();
    }

    /** The link's longest rise at the client's rate over LINK_HISTORY_MS, ms. */
    _longestBaseRise(now) {
        let ms = this._baseRiseSince ? now - this._baseRiseSince : 0;
        for (const r of this._baseRises)
            if (r.end >= now - LINK_HISTORY_MS) ms = Math.max(ms, r.ms);
        return ms;
    }

    /**
     * The longest rise of the link in the frames kept that were painted at
     * the client's rate, against the reference just taken: what the first
     * trial knows of the link before any step, ms.
     */
    _scanBaseRises() {
        if (this._reference === null || !this._clock.ready) return 0;
        const bound = this._reference + 500 / this._base;
        const recent = [];
        let since = 0;
        let longest = 0;
        for (const s of this._painted) {
            if (!s.base) {
                // A step was in force: what it measured is not the link's own.
                recent.length = 0;
                since = 0;
                continue;
            }
            const ms = this._latency(s);
            if (ms === null) continue;
            recent.push({ at: s.at, ms });
            while (recent[0].at < s.at - FILET_WINDOW_MS) recent.shift();
            if (recent.length < FILET_MIN_FRAMES) continue;
            if (median(recent.map((r) => r.ms).sort((a, b) => a - b)) > bound) {
                if (!since) since = s.at;
                longest = Math.max(longest, s.at - since);
            } else {
                since = 0;
            }
        }
        return longest;
    }

    // ── The content ─────────────────────────────────────────────────────────

    /**
     * How many steps the content could use; when that changes and holds, the
     * content changed and the backoff is forgotten.
     */
    _noteBand(now) {
        let band = 0;
        for (let i = 1; i < this._levels.length; i++)
            if (this._presents > this._levels[i - 1] * FASTER) band = i;
        if (band === this._band) {
            this._bandCount = 0;
            return;
        }
        if (band === this._bandSeen) this._bandCount++;
        else {
            this._bandSeen = band;
            this._bandCount = 1;
        }
        if (this._bandCount < CONTENT_STABLE) return;
        this._band = band;
        this._bandCount = 0;
        // A net that keeps tripping says the link, not the content: its wait stays.
        if (this._strikes < NET_STRIKES) {
            this._backoff = 0;
            if (this._phase === 'idle') this._nextTrialAt = Math.min(this._nextTrialAt, now);
        }
        this._event('content', now, { presents: this._presents, band });
    }

    /** The host's step against ours: a step it dropped (a decoder cap) is let go. */
    _reconcile(now) {
        if (this._phase !== 'idle') {
            this._mismatch = 0;
            return;
        }
        const ours = this.stepFps;
        if (ours > 0 === this._hostStep > 0) {
            this._mismatch = 0;
            return;
        }
        if (++this._mismatch < HOST_MISMATCH) return;
        this._mismatch = 0;
        if (ours > 0) this._fallBack('the host no longer runs the step', now);
        else this._ask(0);
    }

    // ── The measure ─────────────────────────────────────────────────────────

    /** Capture → painted of one frame on the host's clock, ms; null when unknown. */
    _latency(s) {
        if (!this._clock.ready) return null;
        const paintedMs = this._clock.toHostUs(s.at) / 1000;
        const captureMs = s.ts + Math.round((paintedMs - s.ts) / WRAP_MS) * WRAP_MS;
        const ms = paintedMs - captureMs;
        return ms > 0 && ms < 1000 ? ms : null;
    }

    /**
     * What a viewer looked at between @p from and @p to: capture → painted
     * (median), the interval between painted frames, their sum as the age of
     * what is shown (and the same on the lower quartile), and the share of the
     * frames received that were painted. Null under MIN_FRAMES.
     */
    _window(from, to) {
        const latencies = [];
        let first = Infinity;
        let last = -Infinity;
        for (const s of this._painted) {
            if (s.at < from || s.at > to) continue;
            const ms = this._latency(s);
            if (ms === null) continue;
            latencies.push(ms);
            if (s.at < first) first = s.at;
            if (s.at > last) last = s.at;
        }
        const painted = latencies.length;
        if (painted < MIN_FRAMES) return null;
        latencies.sort((a, b) => a - b);
        const latency = median(latencies);
        const low = latencies[Math.floor((painted - 1) / 4)];
        const interval = (last - first) / (painted - 1);
        let received = 0;
        for (const at of this._received) if (at >= from && at <= to) received++;
        return {
            latency,
            interval,
            age: latency + interval / 2,
            lowAge: low + interval / 2,
            painted,
            received,
            ratio: received > 0 ? Math.min(1, painted / received) : 1,
        };
    }

    _prune(now) {
        const horizon = now - KEEP_MS;
        let i = 0;
        while (i < this._painted.length && this._painted[i].at < horizon) i++;
        if (i) this._painted.splice(0, i);
        let j = 0;
        while (j < this._received.length && this._received[j] < horizon) j++;
        if (j) this._received.splice(0, j);
    }

    _event(what, now, facts) {
        this.events.push({ at: Math.round(now), what, ...facts });
        if (this.events.length > EVENTS_KEPT) this.events.shift();
    }

    /** Where the detection stands, for the bench and the console. */
    get summary() {
        return {
            running: this._running,
            base: this._base,
            display: this._display,
            presents: this._presents,
            fasterReports: this._fasterRun(this._rate()),
            levels: this._levels.slice(),
            stepFps: this.stepFps,
            hostStep: this._hostStep,
            phase: this._phase,
            referenceMs: this._reference,
            trials: this.trials,
            kept: this.kept,
            rejected: this.rejected,
            refused: this.refused,
            trips: this.trips,
            strikes: this._strikes,
            netHoldMs: Math.round(this._hold),
            wideNet: this._wide,
            linkRiseMs: Math.round(this._longestBaseRise(this._now())),
            backoff: this._backoff,
            nextTrialInMs: Math.max(0, Math.round(this._nextTrialAt - this._now())),
            remembered: [...this._remembered],
            clock: this._clock.summary,
        };
    }
}
