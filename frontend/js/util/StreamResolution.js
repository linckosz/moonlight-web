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
 * The streamed resolution, from the choice in Settings to the size a launch
 * asks for.
 *
 * Four ways to choose it (`stream_resolution`):
 *   - `auto`   — the default. On the native host: the host display's own size
 *                when it fits this screen, else the host's picture scaled DOWN
 *                (on the host, its shape kept) to fit this screen — a 2560×1440
 *                display on a 2000×1600 laptop streams 2000×1125. The box is
 *                never taller than AUTO_MAX_HEIGHT, so a 4K screen asks for
 *                1440p and neither machine spends a GPU on pixels Auto never
 *                promised. On a phone or a tablet the box is not the screen but
 *                1440 lines by any width (MOBILE_AUTO_BOX): the picture is
 *                pinched and zoomed there, and a stream reduced to the screen's
 *                1170 lines blurs the moment it is; so a host of up to 1440
 *                lines streams as it is and a taller one comes down to 1440,
 *                never below 1080 that way. On a TV the box stops at 720 lines
 *                (TV_AUTO_MAX_HEIGHT): latency first. The stream follows both
 *                screens: the host changing its mode, a computer's window
 *                moving to another monitor. On every other host, which cannot
 *                say its display's size: 1080p (720p on a TV), the width from
 *                the Auto ratio (measured from Sunshine's bars).
 *   - `device` — "Match my screen": this screen's size, pixel for pixel. The
 *                native host is asked to put its display in that mode for the
 *                session; when its driver lists no such mode the request falls
 *                back to Auto (`fallback`), which is what the host then gets.
 *                Sunshine and Apollo get the explicit size with GameStream's
 *                optimal-settings flag, which lets them switch the host display
 *                to it. The stream follows this screen's changes.
 *   - `custom` — a width and a height typed in, each within
 *                [CUSTOM_SIZE_MIN, CUSTOM_SIZE_MAX]: the native host fits its
 *                display's shape inside the box, never upscaled.
 *   - `fixed`  — one of the rungs (720p … 2160p) in `stream_height`; the width
 *                follows the host's shape (Auto ratio).
 *
 * Every choice but `fixed` (and `auto` off the native host) fixes the shape
 * itself, so the aspect probe stays out of it — the size IS the request.
 *
 * ── The pixel budget ───────────────────────────────────────────────────────
 *
 * A size and a rate are chosen apart and multiply together: 2560×1440 at
 * 120 fps is 442 million pixels a second to capture, scale, encode, send,
 * decode and paint — more than a machine on either end holds for an hour of
 * play. So whenever ONE of the two was left to us (Auto resolution or Auto
 * frame rate), the pair is held under PIXEL_RATE_BUDGET (fitPixelBudget), and
 * only the half left to us pays. Auto/Auto spends the resolution first: a
 * frame rate is felt on every mouse move, a hundred lines of resolution are
 * not. A half the viewer named is not ours to cut — a 4K rung on a 120 Hz
 * screen streams 4K at 60, not 1080p at 120 (issue #38), and a 144 picked
 * with Auto's resolution stays 144. A rate brought down lands on a divisor of
 * this screen's refresh when one lies close under it, 60 rather than 67 on a
 * 120 Hz one, so its frames still fall on the screen's grid; farther off, the
 * budget's own rate stands. Neither half is ever brought below what
 * the client itself does — BUDGET_MIN_HEIGHT and BUDGET_MIN_FPS, or this
 * screen's own when it is under them. Two explicit choices are the viewer's
 * business and are left alone.
 *
 * ── "MoonlightWeb Virtual Display" ─────────────────────────────────────────
 *
 * A display that exists only for this stream is not something to fit into: it
 * is made to measure, so on that one card every choice names an exact size and
 * the display is created at it (`virtualDisplay` in the context). "Match my
 * screen" means this screen pixel for pixel, a fullscreen with no bars
 * anywhere; Auto means the same screen under Auto's own ceiling, so a 4K panel
 * gets a 1440p display rather than a 4K one nobody looks at. Custom is the
 * pair as typed, portrait included. A rung is that many lines at THIS
 * screen's shape — 1080p on a
 * 2532×1170 phone is 2336×1080, not 1920×1080 — because the shape of a
 * display nobody sees has no reason to be anyone else's. Every one of them is
 * held inside [MODE_SIZE_MIN, MODE_SIZE_MAX], the driver's and the decoders'
 * limits, shrunk at its own shape when it does not fit.
 */

export const RESOLUTION_MODES = ['auto', 'device', 'custom', 'fixed'];

/** Bounds of a custom width or height, in pixels. 360 is the smallest frame a
 *  hardware encoder is guaranteed to take; 4096 is DCI 4K, the widest level
 *  every browser decoder still accepts. */
export const CUSTOM_SIZE_MIN = 360;
export const CUSTOM_SIZE_MAX = 4096;

/** The height streamed under `auto` by a host that cannot say its display's
 *  size (Sunshine, Apollo, Wolf: serverinfo carries no mode). */
export const HOST_FALLBACK_HEIGHT = 1080;

/** The rungs of `fixed` — also the congestion ladder's steps. */
export const FIXED_HEIGHTS = [720, 1080, 1440, 2160];

/** The tallest box Auto ever asks for. A choice made FOR the viewer stops at
 *  1440 lines: above that the gain is a detail seen by leaning in, and the
 *  cost is paid twice, by a host encoding it and by a client decoding it.
 *  A viewer who wants their 4K screen filled says so — "Match my screen" and
 *  Custom are not bound by this. */
export const AUTO_MAX_HEIGHT = 1440;

/** Auto's box on a phone or a tablet: 1440 lines, any width. A host of up to
 *  1440 lines streams as it is (the pinch zoom stays sharp), a 4K one comes
 *  down to 1440 (a phone decoding 4K heats up for pixels it cannot show),
 *  and nothing is ever reduced below 1080 that way. */
export const MOBILE_AUTO_BOX = { width: CUSTOM_SIZE_MAX, height: AUTO_MAX_HEIGHT };

/** Auto's ceiling on a TV, on every host: 720 lines. A TV's decoder pays for
 *  every line in time, and from a couch the frame a pad waits for is worth
 *  more than detail seen across the room — latency first. Measured on a
 *  Freebox Player POP, a 1080p screen at 50 Hz (01/10/2026, AMF host): Auto's
 *  1080p50 showed 46.6 fps at 154 ms, 720p50 all 50 at 68 ms. A viewer who
 *  wants the 1080p names it — "Match my screen", a rung, Custom. */
export const TV_AUTO_MAX_HEIGHT = 720;

/**
 * What a TV is asked beyond what its Auto would pick, or null. A viewer may
 * name 1080p60 on a TV — their choice stands (Bruno, 02/10/2026: the user
 * decides) — but is told first: a Mi TV decodes about 34 frames a second at
 * 720p, and 1080p60 left it frozen on the way out of the stream.
 * @param {{height: number, fps: number, autoFps: number}} asked the launch's
 *   size and rate, and the rate Auto would pick here (0 when unknown)
 * @returns {{height: number, fps: number, autoHeight: number, autoFps: number}|null}
 */
export function tvOverAsk({ height, fps, autoFps }) {
    const tooTall = height > TV_AUTO_MAX_HEIGHT;
    const tooFast = autoFps > 0 && fps > autoFps;
    if (!tooTall && !tooFast) return null;
    return {
        height,
        fps,
        autoHeight: Math.min(height, TV_AUTO_MAX_HEIGHT),
        autoFps: autoFps > 0 ? Math.min(fps, autoFps) : fps,
    };
}

/** The tallest box Auto asks for on this kind of screen.
 *  @param {boolean} [tv]
 *  @param {number} [maxHeight] a computer's own ceiling (autoMaxHeightFor) */
function autoCeiling(tv, maxHeight) {
    if (tv) return TV_AUTO_MAX_HEIGHT;
    return maxHeight > 0 ? maxHeight : AUTO_MAX_HEIGHT;
}

/** How many pixels a second the whole chain is asked to carry, at most, when
 *  either half of the choice was left to us. 1920×1080 at 120 fps is 249
 *  million of them — the reference this number was read off. */
export const PIXEL_RATE_BUDGET = 250000000;

/** The tallest box Auto asks for once a machine holds more than that budget.
 *  The budget alone then decides: at a fixed 1440 lines a 4K 60 Hz screen
 *  would stay at 1440p60 (221 Mpx/s) up to 450 M, then jump to 4K60 (498 M),
 *  more than twice the load on no evidence. */
export const AUTO_RAISED_MAX_HEIGHT = 2160;

/** The tallest box Auto may ask for under this budget.
 *  @param {number} [budget] pixels a second; none is PIXEL_RATE_BUDGET */
export function autoMaxHeightFor(budget) {
    return budget > PIXEL_RATE_BUDGET ? AUTO_RAISED_MAX_HEIGHT : AUTO_MAX_HEIGHT;
}

/** How close under a budget's height a standard one must lie to be preferred
 *  to it: 1448 lines is 1440p with eight lines nobody asked for. */
const STANDARD_HEIGHT_MAX_LOSS = 0.01;

/** The standard height just under `height` when it costs almost nothing. */
function nearStandardHeight(height) {
    for (const standard of FIXED_HEIGHTS) {
        if (standard <= height && height - standard < height * STANDARD_HEIGHT_MAX_LOSS)
            return standard;
    }
    return height;
}

/** How far down the budget may push each half. An Auto resolution goes first
 *  and stops at 1080 lines, an Auto frame rate follows and stops at 60 (a
 *  named half does not move at all) — and neither moves below what this very
 *  screen does, since a stream already smaller than the screen it lands on
 *  has nothing left to give. */
export const BUDGET_MIN_HEIGHT = 1080;
export const BUDGET_MIN_FPS = 60;

/** How much of a budget-cut frame rate may be given up to land it on a
 *  divisor of this screen's refresh: 67 becomes 60 on a 120 Hz screen (a
 *  tenth less, and every frame on a refresh), while 90 stays 90 — a third of
 *  the rate is too much to give for it (Bruno, 24/10/2026). */
export const BUDGET_GRID_MAX_LOSS = 0.15;

/** Bounds of a display mode made on demand — what the virtual display driver
 *  will take, and what the decoder on the other side will. The floor is VGA's
 *  480 lines, the smallest mode Windows itself still offers and one the
 *  driver's own sample lists; the ceiling is DCI 4K. Mirrored in C++ by
 *  VirtualDisplay::kModeMin / kModeMax, which pin the request again. */
export const MODE_SIZE_MIN = 480;
export const MODE_SIZE_MAX = CUSTOM_SIZE_MAX;

/**
 * A size a display can be made at: even, inside the mode bounds, and moved at
 * its own shape rather than squeezed when it is outside them. A 2160p rung at
 * a phone's 2.16 shape asks for 4676×2160 and gets 4096×1892; a screen smaller
 * than the floor grows whole, so an 800×600 desktop stays 800×600 and a
 * 400×300 one becomes 640×480 — never 800×640, which would be a shape nobody
 * asked for.
 */
export function fitModeBounds(width, height) {
    let w = Math.round(width);
    let h = Math.round(height);
    if (!(w > 0) || !(h > 0)) return null;
    const over = Math.max(w / MODE_SIZE_MAX, h / MODE_SIZE_MAX);
    const under = Math.min(w / MODE_SIZE_MIN, h / MODE_SIZE_MIN);
    const scale = over > 1 ? over : under < 1 ? under : 1;
    if (scale !== 1) {
        w = Math.round(w / scale);
        h = Math.round(h / scale);
    }
    // A shape so extreme that one side is still out of bounds: the last word
    // goes to the bounds, not to the shape.
    w = Math.min(MODE_SIZE_MAX, Math.max(MODE_SIZE_MIN, w)) & ~1;
    h = Math.min(MODE_SIZE_MAX, Math.max(MODE_SIZE_MIN, h)) & ~1;
    return { width: w, height: h };
}

/** Pin a typed size into its bounds; anything unreadable becomes `fallback`. */
export function clampCustomSize(value, fallback) {
    const n = typeof value === 'number' ? value : parseInt(value, 10);
    if (!Number.isFinite(n)) return fallback;
    return Math.min(CUSTOM_SIZE_MAX, Math.max(CUSTOM_SIZE_MIN, Math.round(n)));
}

/**
 * Read the resolution choice out of a settings object (localStorage or the
 * server's defaults), normalised: an unknown or missing mode is `auto` (so is
 * the short-lived `host` of the 0.3.1 development builds, which `auto`
 * covers), a custom size out of bounds is pinned, a missing one is 1920x1080.
 */
export function readResolutionChoice(data) {
    const d = data || {};
    const mode = RESOLUTION_MODES.includes(d.stream_resolution) ? d.stream_resolution : 'auto';
    return {
        mode,
        customWidth: clampCustomSize(d.stream_custom_width, 1920),
        customHeight: clampCustomSize(d.stream_custom_height, 1080),
    };
}

/**
 * This device's screen in physical pixels, landscape (the stream is a desktop,
 * shown landscape whatever way the phone is held), even in both dimensions.
 * @param {{screen?: {width:number,height:number}, devicePixelRatio?: number}} [win]
 * @returns {{width:number, height:number}|null} null when the screen is unknown
 */
export function devicePixelSize(win) {
    const w = win || (typeof window !== 'undefined' ? window : null);
    const s = w && w.screen;
    if (!s || !(s.width > 0) || !(s.height > 0)) return null;
    const dpr = w.devicePixelRatio > 0 ? w.devicePixelRatio : 1;
    const a = Math.round(s.width * dpr) & ~1;
    const b = Math.round(s.height * dpr) & ~1;
    if (!(a > 0) || !(b > 0)) return null;
    return { width: Math.max(a, b), height: Math.min(a, b) };
}

/** How far apart two stream widths of the same height may be and still count
 *  as the same size: a screen read a few pixels off is no new screen. */
export const SCREEN_SIZE_TOLERANCE = 0.01;

/**
 * Whether two stream sizes (`{height, aspect}`, aspect "W:H") are the same to
 * the eye: the same height, widths within SCREEN_SIZE_TOLERANCE. A window
 * moved between two monitors of the same size, or a page zoom, can read the
 * screen 4 px wider (2564×1440 for 2560×1440, 15/10/2026): relaunching for
 * that turned the host's virtual display over for nothing — and twice, when
 * the reading came back.
 * @param {{height: number, aspect?: string}} a
 * @param {{height: number, aspect?: string}} b
 */
export function nearlySameStreamSize(a, b) {
    if (!a || !b || a.height !== b.height) return false;
    if ((a.aspect || '') === (b.aspect || '')) return true;
    const width = (s) => {
        const m = /^(\d+):(\d+)$/.exec(s.aspect || '');
        return m ? (Number(m[1]) * s.height) / Number(m[2]) : 0;
    };
    const wa = width(a);
    const wb = width(b);
    if (!(wa > 0) || !(wb > 0)) return false;
    return Math.abs(wa - wb) <= Math.max(wa, wb) * SCREEN_SIZE_TOLERANCE;
}

/**
 * A size brought under Auto's ceiling, its shape kept: a 3840×2160 screen is
 * 2560×1440, a 2560×1080 ultrawide is itself. Even, as every encoder wants.
 * A ceiling above AUTO_MAX_HEIGHT also holds the width to CUSTOM_SIZE_MAX, the
 * widest every decoder takes: 2160 lines on a 5120×2160 screen would pass it.
 * @param {number} [maxHeight] the ceiling: TV_AUTO_MAX_HEIGHT on a TV
 */
export function capAutoHeight(size, maxHeight = AUTO_MAX_HEIGHT) {
    if (!size || !(size.width > 0) || !(size.height > 0)) return null;
    const box =
        size.height <= maxHeight
            ? { width: size.width, height: size.height }
            : {
                  width: Math.round((size.width * maxHeight) / size.height) & ~1,
                  height: maxHeight,
              };
    if (maxHeight <= AUTO_MAX_HEIGHT || box.width <= CUSTOM_SIZE_MAX) return box;
    return {
        width: CUSTOM_SIZE_MAX,
        height: Math.round((box.height * CUSTOM_SIZE_MAX) / box.width) & ~1,
    };
}

/** Auto's box for a native host: this screen under Auto's ceiling on a
 *  computer or a TV, MOBILE_AUTO_BOX on a phone or a tablet, null when
 *  neither is known.
 *  @param {number} [maxHeight] a computer's ceiling (autoMaxHeightFor) */
export function autoBox(device, touch, tv, maxHeight) {
    if (touch) return MOBILE_AUTO_BOX;
    return capAutoHeight(device, autoCeiling(tv, maxHeight));
}

/**
 * The size to make "MoonlightWeb Virtual Display" at for this choice, or null
 * when this screen is unknown and there is nothing to measure against.
 *
 * "Match my screen" is this screen pixel for pixel, because a display made to
 * order has no size of its own to reconcile with. Auto is the same screen
 * under Auto's ceiling (AUTO_MAX_HEIGHT): a display nobody looks at is made
 * at the size the stream wants, and making it 4K to then stream 1440p would
 * only hand the compositor three million pixels to throw away. Custom is the
 * typed pair, portrait included. A rung is its lines at this screen's shape.
 * @param {boolean} [tv] this screen is a TV's: Auto's ceiling is then 720 lines
 * @param {number} [maxHeight] a computer's ceiling (autoMaxHeightFor)
 */
export function virtualDisplaySize(choice, device, tv, maxHeight) {
    const c = choice || {};
    if (c.mode === 'custom')
        return fitModeBounds(
            clampCustomSize(c.customWidth, 1920),
            clampCustomSize(c.customHeight, 1080),
        );
    if (!device || !(device.width > 0) || !(device.height > 0)) return null;
    if (c.mode === 'device') return fitModeBounds(device.width, device.height);
    if (c.mode === 'auto') {
        const box = capAutoHeight(device, autoCeiling(tv, maxHeight));
        return box ? fitModeBounds(box.width, box.height) : null;
    }
    const lines = c.height > 0 ? c.height : HOST_FALLBACK_HEIGHT;
    return fitModeBounds((lines * device.width) / device.height, lines);
}

/**
 * The size a launch asks for, from the choice and the host.
 *
 * @param {{mode:string, height:number, customWidth:number, customHeight:number}} choice
 *        the Settings choice; `height` is the `fixed` rung
 * @param {{nativeHost:boolean, touch?:boolean, tv?:boolean, virtualDisplay?:boolean,
 *          device?: {width:number,height:number}|null, autoMaxHeight?:number}} ctx
 *        whether the host is MoonlightWeb's own native host, whether this is a
 *        phone or a tablet, or a TV (Auto's ceiling: TV_AUTO_MAX_HEIGHT),
 *        whether the app launched is "MoonlightWeb Virtual Display" (every
 *        choice then names an exact size, made to order — see
 *        virtualDisplaySize), this screen (devicePixelSize() when omitted),
 *        and a computer's own ceiling (AUTO_MAX_HEIGHT when omitted)
 * @returns {{height:number, aspect:string|null, fitBox:boolean, allowUpscale:boolean,
 *            matchDisplay:boolean, followsScreen:boolean,
 *            fallback:{width:number,height:number}|null}}
 *        `height` 0 = the host display's own size (native host only);
 *        `aspect` "W:H" fixes the width, null leaves it to the Auto ratio
 *        (probe / memory / the native host's own shape); `fitBox` tells a
 *        native host to fit its display's shape inside W×H rather than keep
 *        the height; `allowUpscale` lets it exceed its display for that;
 *        `matchDisplay` asks it to put its display in that very mode, and
 *        `fallback` is the box it fits instead when it cannot (Auto's);
 *        `followsScreen` says a change of THIS screen changes the request.
 */
export function resolveStreamSize(choice, ctx) {
    const c = choice || {};
    const nativeHost = !!(ctx && ctx.nativeHost);
    const touch = !!(ctx && ctx.touch);
    const tv = !!(ctx && ctx.tv);
    const dev = ctx && ctx.device !== undefined ? ctx.device : devicePixelSize();
    // A phone or a tablet keeps its 1440 lines at any budget.
    const maxHeight = ctx && !touch ? ctx.autoMaxHeight : undefined;
    if (nativeHost && ctx && ctx.virtualDisplay) {
        const made = virtualDisplaySize(c, dev, tv, maxHeight);
        if (made)
            return {
                height: made.height,
                aspect: made.width + ':' + made.height,
                fitBox: true,
                // The display IS that size, so there is nothing to scale; the
                // flag only says the frame may reach the size asked for.
                allowUpscale: true,
                matchDisplay: true,
                // Turned, or moved to another monitor: the display is remade.
                followsScreen: true,
                fallback: null,
            };
    }
    const plain = {
        fitBox: false,
        allowUpscale: false,
        matchDisplay: false,
        followsScreen: false,
        fallback: null,
    };
    const fixed = (height) => ({
        height: height > 0 ? height : HOST_FALLBACK_HEIGHT,
        aspect: null,
        ...plain,
    });
    switch (c.mode) {
        case 'auto': {
            if (!nativeHost) return fixed(tv ? TV_AUTO_MAX_HEIGHT : HOST_FALLBACK_HEIGHT);
            // The host's own size, brought down to fit the box — its shape
            // kept, never upscaled (frameForDisplay's box rule). No screen to
            // read: the host's own size, whatever it is — on a TV, still no
            // more than its ceiling.
            const box = autoBox(dev, touch, tv, maxHeight);
            if (!box) return tv ? fixed(TV_AUTO_MAX_HEIGHT) : { height: 0, aspect: null, ...plain };
            return {
                height: box.height,
                aspect: box.width + ':' + box.height,
                fitBox: true,
                allowUpscale: false,
                matchDisplay: false,
                // A phone's box is the same whichever way it is held.
                followsScreen: !touch,
                fallback: null,
            };
        }
        case 'device':
            if (!dev) return fixed(c.height);
            return {
                height: dev.height,
                aspect: dev.width + ':' + dev.height,
                fitBox: true,
                allowUpscale: true,
                matchDisplay: true,
                followsScreen: true,
                fallback: autoBox(dev, touch, tv, maxHeight),
            };
        case 'custom': {
            // Even sizes: what every encoder takes (4:2:0 chroma is half-size).
            const w = clampCustomSize(c.customWidth, 1920) & ~1;
            const h = clampCustomSize(c.customHeight, 1080) & ~1;
            return {
                height: h,
                aspect: w + ':' + h,
                fitBox: true,
                allowUpscale: false,
                matchDisplay: false,
                followsScreen: false,
                fallback: null,
            };
        }
        default:
            return fixed(c.height);
    }
}

/**
 * What the recommended-bitrate estimate should count for this choice: a
 * height and a "W:H" aspect (see util/AutoBitrate.js). `auto` counts this
 * screen under Auto's ceiling — the most the native host will send under it —
 * 1440p 16:9 on a phone or a tablet, and 1080p 16:9 when the screen is
 * unknown, the estimate's reference (720p on a TV). The pixel budget is
 * applied on top by the caller, which alone knows the frame rate
 * (fitPixelBudget).
 * @param {number} [maxHeight] a computer's ceiling (autoMaxHeightFor)
 */
export function bitrateReference(choice, device, touch, tv, maxHeight) {
    const c = choice || {};
    const dev = device !== undefined ? device : devicePixelSize();
    switch (c.mode) {
        case 'auto': {
            if (touch) return { height: MOBILE_AUTO_BOX.height, aspect: '16:9' };
            const box = capAutoHeight(dev, autoCeiling(tv, maxHeight));
            if (box) return { height: box.height, aspect: box.width + ':' + box.height };
            return { height: tv ? TV_AUTO_MAX_HEIGHT : 1080, aspect: '16:9' };
        }
        case 'device':
            if (dev) return { height: dev.height, aspect: dev.width + ':' + dev.height };
            return { height: c.height > 0 ? c.height : 1080, aspect: '16:9' };
        case 'custom': {
            const w = clampCustomSize(c.customWidth, 1920);
            const h = clampCustomSize(c.customHeight, 1080);
            return { height: h, aspect: w + ':' + h };
        }
        default:
            return { height: c.height > 0 ? c.height : 1080, aspect: '16:9' };
    }
}

/** The ratio behind a "W:H" string, or 0 when there is none to read. */
function aspectRatio(aspect) {
    if (typeof aspect !== 'string') return 0;
    const parts = aspect.split(':');
    const w = parseFloat(parts[0]);
    const h = parseFloat(parts[1]);
    if (!(w > 0) || !(h > 0)) return 0;
    return w / h;
}

/**
 * What a launch size and rate come to, in pixels: all zero when the size is
 * "the host's own", which nothing here can weigh.
 * @param {{height:number, aspect:string|null}} size
 * @param {number} fps 0 (unknown) counts as 60
 * @param {string|null} [aspect] the ratio to assume when the size fixes none
 * @returns {{width:number, height:number, fps:number, pixelRate:number}}
 */
export function streamFootprint(size, fps, aspect) {
    const height = size && size.height > 0 ? size.height : 0;
    const rate = fps > 0 ? fps : 60;
    if (!height) return { width: 0, height: 0, fps: rate, pixelRate: 0 };
    const ratio = aspectRatio(size.aspect) || aspectRatio(aspect) || 16 / 9;
    const width = Math.round(height * ratio) & ~1;
    return { width, height, fps: rate, pixelRate: width * height * rate };
}

/**
 * The rate a budget-cut frame rate lands on: the budget's own, never under
 * `floor` — or the highest whole divisor of this screen's refresh under it,
 * when that gives up no more than BUDGET_GRID_MAX_LOSS of it, so that every
 * streamed frame falls on a refresh. 67 becomes 60 on a 120 Hz screen and 90
 * becomes 80 on a 240 Hz one; 90 on a 120 Hz screen stays 90, and so does 67
 * on a 144 Hz one, whose next divisor down, 48, is under the floor.
 * @param {number} fits the highest rate the budget takes
 * @param {number} floor the lowest rate it may give
 * @param {number} [screenFps] this screen's refresh; 0 or none when unknown
 */
function onScreenGrid(fits, floor, screenFps) {
    const rate = Math.max(floor, fits);
    if (!(screenFps > 0)) return rate;
    for (let k = 1; screenFps / k >= floor; k++) {
        const d = screenFps / k;
        if (Number.isInteger(d) && d <= rate)
            return rate - d <= rate * BUDGET_GRID_MAX_LOSS ? d : rate;
    }
    return rate;
}

/**
 * Hold a size and a frame rate under a budget together: PIXEL_RATE_BUDGET,
 * or the one this machine has earned (`ctx.budget`).
 *
 * Called only when ONE of the two was left to us — Auto resolution or Auto
 * frame rate; a viewer who named both is answered as asked. The resolution
 * goes down first, to whatever fits at the rate asked for, and stops at
 * BUDGET_MIN_HEIGHT (or at this screen's own height, when the screen is under
 * it: there is nothing to gain below what the picture lands on) — unless the
 * viewer named it (`resolutionNamed`), and then it stays exactly as named.
 * Only then is the frame rate brought down, the same way, to BUDGET_MIN_FPS —
 * unless the viewer named that (`rateNamed`) — and onto a divisor of this
 * screen's refresh when one lies close under it (onScreenGrid). A pair that
 * is still over the budget at both floors is let through — the floors are
 * the promise, the budget is the aim.
 *
 * The height that comes back is a multiple of eight, so a 4K screen at 120 fps
 * asks for the 1920×1080 that 249 Mpx/s was read off and not the 1924×1082 the
 * arithmetic alone would give. Under a raised budget a standard height just
 * below is preferred, 1440 to the 1448 that 450 M gives at 120 fps.
 *
 * @param {{height:number, aspect:string|null}} size resolveStreamSize's answer
 * @param {number} fps the rate this launch asks for; 0 (unknown) counts as 60
 * @param {{device?: {width:number,height:number}|null, clientFps?:number,
 *          aspect?:string|null, resolutionNamed?:boolean,
 *          rateNamed?:boolean, budget?:number}} [ctx]
 *        this screen and its rate — the two floors and the divisors are read
 *        off them — the ratio to assume when the size does not fix one (a
 *        `fixed` rung, whose width belongs to the Auto ratio), which half the
 *        viewer named, which leaves the other one alone to pay, and the budget
 *        (PIXEL_RATE_BUDGET when omitted).
 * @returns {{size:object, fps:number, capped:boolean}}
 */
export function fitPixelBudget(size, fps, ctx) {
    const s = /** @type {{height:number, aspect:string|null}} */ (size || {});
    const c = ctx || {};
    const budget = c.budget > 0 ? c.budget : PIXEL_RATE_BUDGET;
    const rate = fps > 0 ? fps : 60;
    const height = s.height;
    const ratio = aspectRatio(s.aspect) || aspectRatio(c.aspect) || 16 / 9;
    // Height 0 is "the host display's own size, whatever it is" — nothing
    // measured here to hold to a budget.
    if (!(height > 0)) return { size: s, fps, capped: false };

    const widthFor = (h) => Math.round(h * ratio) & ~1;
    const pixelsAt = (h) => widthFor(h) * h;
    if (pixelsAt(height) * rate <= budget) return { size: s, fps, capped: false };

    const dev = c.device !== undefined ? c.device : devicePixelSize();
    const floorHeight = Math.min(
        BUDGET_MIN_HEIGHT,
        dev && dev.height > 0 ? dev.height : BUDGET_MIN_HEIGHT,
    );
    const floorFps = Math.min(BUDGET_MIN_FPS, c.clientFps > 0 ? c.clientFps : BUDGET_MIN_FPS);

    // 1. The resolution, down to its floor: h × (h × ratio) × rate ≤ budget.
    //    A named one is the viewer's and is not ours to spend.
    let h = height;
    if (!c.resolutionNamed && h > floorHeight) {
        const room = Math.floor(Math.sqrt(budget / (ratio * rate)));
        const fits = room - (room % 8);
        h = Math.max(
            floorHeight,
            Math.min(h, budget > PIXEL_RATE_BUDGET ? nearStandardHeight(fits) : fits),
        );
    }
    // 2. The frame rate, down to its floor, for what the resolution still
    //    costs — a named one excepted, as above.
    let f = rate;
    const cost = pixelsAt(h);
    if (!c.rateNamed && cost * f > budget && f > floorFps)
        f = onScreenGrid(Math.floor(budget / cost), floorFps, c.clientFps);

    const capped = h !== height || f !== rate;
    if (!capped) return { size: s, fps, capped: false };
    const out = { ...s, height: h };
    // The size fixed the shape: it still does, at the new height.
    if (s.aspect) out.aspect = widthFor(h) + ':' + h;
    return { size: out, fps: f, capped: true };
}
