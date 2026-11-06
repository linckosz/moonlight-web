/**
 * RefreshRate — what this window's screen refreshes at, measured.
 *
 * ── Why the host wants to know ──────────────────────────────────────────────
 *
 * A client that paints on its vsync (tearing off, or a browser that cannot
 * tear) shows one frame per refresh at most. A 60 fps stream on a 144 Hz
 * screen then lands on ticks 2.4 refreshes apart — some frames stay up two
 * refreshes, some three — and the eye reads it as judder although nothing was
 * lost. The native host can run the stream at a divisor of the client's
 * refresh instead (72 on 144 Hz, 55 on 165 Hz), IF it knows the rate. This
 * module is how it does: the rate goes into /start (`client_refresh_mhz`) and,
 * when the window changes screen mid-stream, into a `clientrefresh` message.
 *
 * ── How it is measured ──────────────────────────────────────────────────────
 *
 * No browser exposes the refresh rate directly. requestAnimationFrame fires
 * once per refresh while the tab is visible, with vsync-aligned timestamps in
 * Chromium, so the mean of ~90 deltas is the period to a tenth of a percent
 * (the timestamps are coarsened, so one delta alone is not). A median sorts
 * the frames the browser skipped under load (a doubled delta) out of that
 * mean — as long as fewer than half are skipped. In a hidden tab rAF does not
 * run at all, so the measurement is skipped and the last good value stands.
 *
 * The result is in millihertz, unrounded: the host multiplies the exact
 * measured period, and 164 800 mHz for a 165 Hz panel is a better number for
 * that than a snapped 165 000 would be if the panel really runs at 164.8.
 *
 * ── To the millihertz ───────────────────────────────────────────────────────
 *
 * Ninety refreshes tell 119.872 Hz from 120 only just (±0.035 % on the bench,
 * 14/10/2026), and the host may match its virtual display to this screen
 * (`vddmatch`, plan glissade): at 240.000 Hz for a panel at 119.872, the
 * stream loses a picture every 7.8 s; matched within 0.01 %, one a minute and
 * more. So once the page is visible for a while, rAF is followed for
 * PRECISE_MS more and the period is the SLOPE of its timestamps against the
 * refresh each one falls on — a skipped frame only skips an index, and the
 * coarsened clock's error is spread over twelve seconds instead of one. That
 * reading replaces the quick one when the two agree (the same screen), and is
 * kept in this browser (PRECISE_KEY) for the next page on it: a /start soon
 * after a load carries it too. Nothing of it shows; a TV, whose first reading
 * stands, is not followed.
 *
 * ── On a TV ─────────────────────────────────────────────────────────────────
 *
 * A TV is measured once: its first good reading stands for the page's
 * lifetime. It has one panel, and a stream fills its main thread — rAF then
 * ticks at the page's pace, with more than half the refreshes skipped, which
 * no median sorts out. Under a stream on 01/10/2026 the Mi TV's 60 Hz panel
 * read 34 Hz and the Freebox Player POP's 50 Hz read 29.4; the host took the
 * 34 for a new screen and ran a stream set to 30 at 34 fps, at the ceiling of
 * that TV's decoder.
 *
 * ── Under load ──────────────────────────────────────────────────────────────
 *
 * A phone streaming is loaded too, and its readings come out low the same way:
 * the realme Note 60 (Mali-G57, WebGL2 at 1080p) read 35-39 Hz, the iPhone 13
 * Pro's 120 Hz read 112-116, and the host, which runs the stream at a divisor
 * of that number, went to 56-58 fps and lost its cadence's lock (22/10/2026;
 * on WebGPU, lighter on the main thread, the iPhone read 120). Once more than
 * half the frames are skipped, the median is the doubled period and the
 * mean mixes the two; a frame shown between two refreshes' worth lands in the
 * mean as well. Either way the good deltas are no longer one tight cluster.
 * So a reading LOWER than the last one is dropped when it is not clean — its
 * deltas within SPREAD_MAX of the period, few of them left out: a window that
 * really moved to a slower screen reads clean, a page under load mostly does
 * not. Mostly: Chrome on the same iPhone, held to 60 Hz, read 57.5, 44.0, 50.7
 * and 57.3 clean, and the host followed it down to 51 fps (22/10/2026). A
 * clean lower reading is therefore only a candidate until twelve seconds of
 * rAF sit on its grid (the slope of "To the millihertz", within a percent of
 * it): a slower screen's refreshes do, a loaded page's frames, landing where
 * the page lets them, do not. Until then, and when they do not, the last
 * reading stands; a real move to a slower screen reaches the host twelve
 * seconds late. A higher reading is always believed: no load makes a screen
 * faster.
 */

import { IS_LOW_MEMORY, IS_TV } from './BrowserDetect.js';
import { limitFps } from './StreamLimits.js';

/** @type {number} Last good measurement, millihertz. 0 = none yet. */
let _milliHz = 0;
/** @type {Promise<number> | null} A measurement in flight, shared. */
let _inFlight = null;
/** @type {Set<(mhz: number) => void>} */
const _listeners = new Set();
let _monitorStarted = false;
let _resizeTimer = null;

/** A change under one percent is jitter, not a new screen. */
const CHANGE_THRESHOLD = 0.01;

/** A clean reading ("Under load"): the good deltas' standard deviation within
 *  this share of the period, or SPREAD_FLOOR_MS (a millisecond clock alone
 *  spreads a 240 Hz panel's 4.17 ms over 4 and 5), and no more than
 *  LEFT_OUT_MAX of the deltas outside the median's band. */
const SPREAD_MAX = 0.15;
const SPREAD_FLOOR_MS = 0.6;
const LEFT_OUT_MAX = 0.25;

/** How long the precise reading follows rAF, and the least it keeps. */
const PRECISE_MS = 12000;
const PRECISE_MIN_MS = 10000;
/** A quick reading this close to the precise one is the same screen. */
const PRECISE_AGREES = 0.003;
/** Where the precise reading is kept, for the next page on this screen. */
const PRECISE_KEY = 'mw_refresh_precise_mhz';

/** @type {number} The last precise reading, millihertz. 0 = none. */
let _preciseMilliHz = 0;
/** @type {boolean} A precise reading is being taken. */
let _preciseRunning = false;
/** @type {number} A lower reading waiting for its twelve seconds ("Under
 *  load"), millihertz. 0 = none. */
let _pendingMilliHz = 0;
/** @type {Promise<number> | null} Its confirmation, in flight. */
let _confirming = null;

/** The precise reading this browser kept, 0 when none (or no storage). */
function _storedPrecise() {
    try {
        const v = Number(localStorage.getItem(PRECISE_KEY));
        return Number.isFinite(v) && v > 0 ? Math.round(v) : 0;
    } catch (e) {
        return 0;
    }
}

function _storePrecise(mhz) {
    try {
        localStorage.setItem(PRECISE_KEY, String(mhz));
    } catch (e) {
        /* no storage here: this page keeps it */
    }
}

/** Does a quick reading @p mhz come from the screen @p precise was read on? */
function _agrees(mhz, precise) {
    return precise > 0 && Math.abs(mhz - precise) <= precise * PRECISE_AGREES;
}

/**
 * Measure the refresh rate now. Resolves with millihertz, or with the last
 * good value (possibly 0) when the tab is hidden or the sampling times out —
 * and on a TV, as soon as there is one ("On a TV" above).
 * Concurrent callers share one measurement.
 * @param {{frames?: number, timeoutMs?: number}} [opts]
 * @returns {Promise<number>}
 */
export function measureRefreshRate(opts = {}) {
    if (_inFlight) return _inFlight;
    if (IS_TV && _milliHz > 0) return Promise.resolve(_milliHz);
    const frames = opts.frames || 90;
    const timeoutMs = opts.timeoutMs || 2500;
    if (typeof document !== 'undefined' && document.hidden) return Promise.resolve(_milliHz);
    if (typeof requestAnimationFrame !== 'function') return Promise.resolve(_milliHz);

    _inFlight = new Promise((resolve) => {
        /** @type {number[]} */
        const stamps = [];
        let done = false;
        const finish = () => {
            if (done) return;
            done = true;
            _inFlight = null;
            clearTimeout(timer);
            resolve(_settle(stamps));
        };
        const timer = setTimeout(finish, timeoutMs);
        const tick = (now) => {
            if (done) return;
            stamps.push(now);
            if (stamps.length >= frames) finish();
            else requestAnimationFrame(tick);
        };
        requestAnimationFrame(tick);
    });
    return _inFlight;
}

/**
 * Turn rAF timestamps into millihertz and publish a change. The first three
 * deltas are dropped: the first callbacks after a request often land
 * mid-refresh. The median only sorts the good deltas from the skipped
 * frames; the period itself is the MEAN of the good ones, because the
 * timestamps are coarsened (to a millisecond, in a page without cross-origin
 * isolation) and a single delta of "6.000" cannot tell 165 Hz from 166.7 —
 * ninety of them can, to a tenth of a percent.
 * @param {number[]} stamps
 * @returns {number}
 */
function _settle(stamps) {
    const deltas = [];
    for (let i = 4; i < stamps.length; i++) {
        const d = stamps[i] - stamps[i - 1];
        if (d > 0) deltas.push(d);
    }
    if (deltas.length < 8) return _milliHz;
    const sorted = deltas.slice().sort((a, b) => a - b);
    const median = sorted[sorted.length >> 1];
    // 10 Hz to 1000 Hz: anything else is not a screen.
    if (!(median >= 1 && median <= 100)) return _milliHz;
    let sum = 0;
    let sumSq = 0;
    let count = 0;
    for (const d of deltas) {
        // A delta half again the median is a skipped frame, not a refresh.
        if (d > median * 1.5 || d < median * 0.5) continue;
        sum += d;
        sumSq += d * d;
        count++;
    }
    const period = count > 0 ? sum / count : median;
    let mhz = Math.round(1e6 / period);
    // Lower than the last reading, and not clean: the page under load, not
    // a slower screen ("Under load"). The last reading stands.
    const spread = count > 1 ? Math.sqrt(Math.max(0, sumSq / count - period * period)) : 0;
    const leftOut = 1 - count / deltas.length;
    const clean =
        spread <= Math.max(period * SPREAD_MAX, SPREAD_FLOOR_MS) && leftOut <= LEFT_OUT_MAX;
    if (!clean && _milliHz > 0 && mhz < _milliHz * (1 - CHANGE_THRESHOLD)) {
        console.info(
            `[RefreshRate] ${(mhz / 1000).toFixed(1)} Hz read under load, ` +
                `${(_milliHz / 1000).toFixed(1)} Hz kept (spread ${spread.toFixed(2)} ms ` +
                `on ${period.toFixed(2)}, ${Math.round(leftOut * 100)} % left out)`,
        );
        return _milliHz;
    }
    // Lower and clean: a candidate, until twelve seconds sit on its grid.
    if (_milliHz > 0 && mhz < _milliHz * (1 - CHANGE_THRESHOLD)) {
        if (mhz !== _pendingMilliHz) {
            console.info(
                `[RefreshRate] ${(mhz / 1000).toFixed(1)} Hz read, ` +
                    `${(_milliHz / 1000).toFixed(1)} Hz kept until twelve seconds confirm it`,
            );
        }
        _pendingMilliHz = mhz;
        _confirmPending();
        return _milliHz;
    }
    _pendingMilliHz = 0;
    // The same screen as the precise reading (this page's, else this
    // browser's last): that one stands — it is the better number of the two.
    if (!_preciseMilliHz) {
        const stored = _storedPrecise();
        if (_agrees(mhz, stored)) _preciseMilliHz = stored;
    }
    if (_agrees(mhz, _preciseMilliHz)) mhz = _preciseMilliHz;
    else _preciseMilliHz = 0;
    _publish(mhz);
    return mhz;
}

/** Make @p mhz the reading, and tell the listeners when it moved. */
function _publish(mhz) {
    const before = _milliHz;
    _milliHz = mhz;
    if (before === 0 || Math.abs(mhz - before) > before * CHANGE_THRESHOLD) {
        for (const cb of _listeners) {
            try {
                cb(mhz);
            } catch (e) {
                /* a listener's problem */
            }
        }
    }
}

/**
 * Follow rAF for the pending lower reading's twelve seconds ("Under load"),
 * unless they are being followed already or a precise reading holds rAF (it
 * starts this when it ends). Its refreshes on one grid within a percent of
 * the candidate: a slower screen, taken at the slope's value. Anything else —
 * no grid, another grid, a hidden tab — leaves the last reading standing. A
 * candidate replaced meanwhile is followed in turn; one cleared by a reading
 * back at the last one is dropped.
 */
function _confirmPending() {
    if (_confirming || _preciseRunning || !_pendingMilliHz) return;
    const candidate = _pendingMilliHz;
    _confirming = _follow(1e6 / candidate, PRECISE_MS).then((mhz) => {
        _confirming = null;
        if (_pendingMilliHz !== candidate) {
            _confirmPending();
            return _milliHz;
        }
        _pendingMilliHz = 0;
        const seen = (candidate / 1000).toFixed(1);
        if (mhz > 0 && Math.abs(mhz - candidate) <= candidate * CHANGE_THRESHOLD) {
            console.info(`[RefreshRate] ${seen} Hz confirmed, ${(mhz / 1000).toFixed(3)} Hz`);
            _preciseMilliHz = mhz;
            _storePrecise(mhz);
            _publish(mhz);
        } else {
            console.info(
                `[RefreshRate] ${seen} Hz not confirmed, ` +
                    `${(_milliHz / 1000).toFixed(1)} Hz kept`,
            );
        }
        return _milliHz;
    });
}

/**
 * The lower reading being confirmed ("Under load"), resolving with the
 * reading that stands once it is decided; null when none is. For the tests.
 * @returns {Promise<number> | null}
 */
export function refreshConfirmation() {
    return _confirming;
}

/**
 * The period of a run of rAF timestamps, in ms, as the slope of the stamps
 * against the refresh each one falls on: a least-squares line, a skipped
 * frame only leaving its index out. Each stamp's index is counted from the
 * one before (@p approxMs apart, rounded), so that the quick reading's own
 * error does not add up over the run. 0 when the run is too short, too
 * sparse (fewer than half its refreshes) or does not sit on one grid — a
 * screen that changed under it, a page that paused.
 * @param {number[]} stamps
 * @param {number} approxMs
 * @returns {number}
 */
export function slopePeriodMs(stamps, approxMs) {
    if (!(approxMs > 0) || stamps.length < 8) return 0;
    const t0 = stamps[0];
    const span = stamps[stamps.length - 1] - t0;
    if (span < PRECISE_MIN_MS) return 0;
    let n = 0;
    let sx = 0;
    let sy = 0;
    let sxx = 0;
    let sxy = 0;
    /** @type {number[]} */
    const indices = [];
    let index = 0;
    let prev = t0;
    for (const t of stamps) {
        const step = Math.round((t - prev) / approxMs);
        if (indices.length > 0 && step < 1) {
            indices.push(-1); // the same refresh twice: left out
            continue;
        }
        index += indices.length > 0 ? step : 0;
        prev = t;
        indices.push(index);
        const y = t - t0;
        n++;
        sx += index;
        sy += y;
        sxx += index * index;
        sxy += index * y;
    }
    const refreshes = Math.round(span / approxMs);
    if (n < 8 || n * 2 < refreshes) return 0;
    const den = n * sxx - sx * sx;
    if (!(den > 0)) return 0;
    const slope = (n * sxy - sx * sy) / den;
    // Every stamp within a third of a refresh of the line, or it is not one grid.
    const intercept = (sy - slope * sx) / n;
    for (let i = 0; i < stamps.length; i++) {
        if (indices[i] < 0) continue;
        if (Math.abs(stamps[i] - t0 - (intercept + slope * indices[i])) > approxMs / 3) return 0;
    }
    return slope;
}

/**
 * Follow rAF for PRECISE_MS and read this screen to the millihertz ("To the
 * millihertz" above). Resolves with the reading, or 0 when there was none to
 * take: a hidden tab, a TV, a quick reading still missing, one already
 * running, a screen that changed meanwhile. A reading that agrees with the
 * quick one replaces it, silently: it is the same screen.
 * @param {{durationMs?: number}} [opts]
 * @returns {Promise<number>}
 */
export function measureRefreshRatePrecisely(opts = {}) {
    if (IS_TV || _preciseRunning || !(_milliHz > 0)) return Promise.resolve(0);
    const before = _milliHz;
    return _follow(1e6 / before, opts.durationMs || PRECISE_MS).then((mhz) => {
        if (!mhz || before !== _milliHz || Math.abs(mhz - _milliHz) > _milliHz * PRECISE_AGREES) {
            // A lower reading that came meanwhile waited for rAF ("Under load").
            _confirmPending();
            return 0;
        }
        // Twelve seconds on this screen's grid: a lower reading taken meanwhile
        // was the page, not another screen.
        _pendingMilliHz = 0;
        _preciseMilliHz = mhz;
        _milliHz = mhz;
        _storePrecise(mhz);
        return mhz;
    });
}

/**
 * Follow rAF for @p durationMs and read the period off its stamps' slope on a
 * grid of about @p approxMs (slopePeriodMs), in millihertz. 0 when the tab is
 * or goes hidden, or the stamps sit on no such grid. Holds _preciseRunning.
 * @param {number} approxMs
 * @param {number} durationMs
 * @returns {Promise<number>}
 */
function _follow(approxMs, durationMs) {
    if (typeof document !== 'undefined' && document.hidden) return Promise.resolve(0);
    if (typeof requestAnimationFrame !== 'function') return Promise.resolve(0);
    _preciseRunning = true;
    return new Promise((resolve) => {
        /** @type {number[]} */
        const stamps = [];
        let first = 0;
        const tick = (now) => {
            if (typeof document !== 'undefined' && document.hidden) {
                _preciseRunning = false;
                resolve(0);
                return;
            }
            if (!first) first = now;
            stamps.push(now);
            if (now - first < durationMs) {
                requestAnimationFrame(tick);
                return;
            }
            _preciseRunning = false;
            // The first stamps after a request may land mid-refresh.
            const period = slopePeriodMs(stamps.slice(4), approxMs);
            resolve(period > 0 ? Math.round(1e6 / period) : 0);
        };
        requestAnimationFrame(tick);
    });
}

/** The last measured refresh rate in millihertz, 0 when none was taken yet. */
export function currentRefreshMilliHz() {
    return _milliHz;
}

/** The lowest and highest frame rate "Auto" will ask a host for.
 *
 *  The ceiling is 120 and not the 165 or 240 a panel may run at: past 120 the
 *  smoothness a viewer can feel is bought with a frame every 8 ms across a
 *  capture, an encoder, a network and a decoder, and a choice made FOR the
 *  viewer does not spend that. A viewer who wants their 165 Hz panel fed says
 *  so in the list — a named rate is never held down. The rate that goes to
 *  the host for its vsync alignment is the UNCAPPED measurement
 *  (currentRefreshMilliHz): the grid the frames land on is the panel's, cap
 *  or no cap. */
export const AUTO_FPS_MIN = 24;
export const AUTO_FPS_MAX = 120;

/** Auto's ceiling on a device of a gigabyte of memory or less.
 *
 *  Measured on a Mi TV (MT5867, Mali-G31, 1.4 GB, 01/10/2026) streaming a
 *  moving 720p desktop: asked 60, its browser decoded 34 frames a second
 *  with 112 ms in the decoder and a quarter of the frames thrown away, for
 *  146 ms end to end; asked 30, 25 ms in the decoder and 54 ms end to end.
 *  The decoder alone does 60 and more (127 fps fed flat out); what gives way
 *  is the page around it — every decoded frame waits for the next refresh to
 *  be drawn, and MediaCodec does not hand out the next one meanwhile. The
 *  decode-rate governor finds the same answer, seconds later and after a
 *  burst of keyframes; a choice made FOR the viewer starts where it ends. */
export const LOW_MEMORY_AUTO_FPS_MAX = 30;

/** What a panel was actually measured at, whole frames per second, with no
 *  ceiling on it: what the Settings line says about the screen, and the floor
 *  the pixel budget reads (fitPixelBudget). 0 when nothing was measured. */
export function measuredFps(milliHz) {
    const mhz = milliHz === undefined ? _milliHz : milliHz;
    if (!(mhz > 0)) return 0;
    return Math.round(mhz / 1000);
}

/**
 * The frame rate "Auto" asks for: this screen's own, in whole frames per
 * second, up to AUTO_FPS_MAX. A stream at the screen's rate shows one frame
 * per refresh — no frame encoded for nothing, and no refresh showing the
 * previous one again.
 *
 * The measurement is unrounded on purpose (a 165 Hz panel may run at 164.8),
 * so it is rounded here and nowhere else; 0 — nothing measured yet, a hidden
 * tab at launch, a browser without rAF — answers 0 and the caller's default
 * stands.
 *
 * @param {number} [milliHz] defaults to the last measurement
 * @param {boolean} [lowMemory] a device of a gigabyte or less (BrowserDetect):
 *        the ceiling is then LOW_MEMORY_AUTO_FPS_MAX
 * @returns {number} frames per second, or 0 when unknown
 */
export function autoFps(milliHz, lowMemory = IS_LOW_MEMORY) {
    const fps = measuredFps(milliHz);
    if (!fps) return 0;
    const ceiling = lowMemory ? LOW_MEMORY_AUTO_FPS_MAX : AUTO_FPS_MAX;
    // And never above the administrator's (util/StreamLimits.js).
    return limitFps(Math.min(ceiling, Math.max(AUTO_FPS_MIN, fps)));
}

/**
 * Be told when the rate changes by more than a percent — the window moved to
 * another screen, typically. Returns the unsubscribe function.
 * @param {(mhz: number) => void} cb
 * @returns {() => void}
 */
export function onRefreshRateChange(cb) {
    _listeners.add(cb);
    return () => {
        _listeners.delete(cb);
    };
}

/**
 * Measure once now and again whenever the window may have changed screen: a
 * resize (a move between monitors of different geometry always resizes; one
 * between identical monitors is caught by the screen change event where the
 * browser has it), a `Screen.change`, and the tab becoming visible again —
 * except on a TV, whose first good reading stands (measureRefreshRate).
 * Idempotent; the first call is enough for the page's lifetime.
 */
export function startRefreshRateMonitor() {
    if (_monitorStarted || typeof window === 'undefined') return;
    _monitorStarted = true;
    // The quick reading, then the precise one on the screen it read.
    const measureBoth = () =>
        measureRefreshRate()
            .then((mhz) => (mhz > 0 ? measureRefreshRatePrecisely() : 0))
            .catch(() => {});
    const remeasure = () => {
        clearTimeout(_resizeTimer);
        _resizeTimer = setTimeout(measureBoth, 400);
    };
    window.addEventListener('resize', remeasure);
    document.addEventListener('visibilitychange', () => {
        if (!document.hidden) remeasure();
    });
    try {
        // Window Management API (Chromium): fires when the window's current
        // screen changes its properties — including which screen it is.
        const scr = /** @type {any} */ (window.screen);
        if (scr && typeof scr.addEventListener === 'function')
            scr.addEventListener('change', remeasure);
    } catch (e) {
        /* no screen events here */
    }
    measureBoth();
}
