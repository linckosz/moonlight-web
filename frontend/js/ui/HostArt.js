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
 * MoonlightWeb — the machine behind a native host's card.
 *
 * The native host's "apps" are the displays of the machine it runs on, and the
 * host says what each one is (see NvApp::device on the server): the OS, a
 * built-in panel, an external monitor or no screen at all, the monitor's name,
 * whether the machine has a battery, and a key that stays the same for as long
 * as the screen does. This module turns that into a picture of the machine — a
 * Windows laptop, a Linux monitor, a MacBook, a Mac mini — in isometric pixel
 * art. Hosts of any other kind never reach it and keep their own art.
 *
 * ── How the pictures are made ──────────────────────────────────────────────
 *
 * Each device is a handful of boxes (a base, a lid, a stand) and each pixel of
 * the picture casts a ray into them. World axes: x runs right-down, y left-down,
 * z up; a point projects to sx = x − y, sy = (x + y)/2 − z, the 2:1 slope of
 * pixel-art isometry. The first box a ray meets gives the face — top, front or
 * side, lit in that order — and the face's texture (a screen, a keyboard) or
 * its material gives the colour. A lit bevel, a contact shade, a floor shadow
 * and a one-pixel outline come after. The result is an SVG of merged pixel
 * runs, drawn once per distinct picture and kept.
 *
 * ── Which picture, and why it never changes ────────────────────────────────
 *
 * Where there is a choice — which of two monitors, which arcade sprite on a
 * Windows screen — it is drawn from a hash of the display's stable key, not
 * from its position or the clock. So it is decided the first time the display
 * is seen and stays for as long as the display exists, in every browser.
 */

import { SPRITES } from './AppPlaceholder.js';

/* ── Renderer ──────────────────────────────────────────────────────────────── */

const MATS = {
    alu: { top: '#c3ccd3', front: '#a3aeb8', side: '#76828d', edge: '#e2e8ec' },
    gray: { top: '#56616c', front: '#434d58', side: '#2f3740', edge: '#77838e' },
    black: { top: '#3b4046', front: '#2d3136', side: '#1f2226', edge: '#585e66' },
    ice: { top: '#d3dbde', front: '#bcc6ca', side: '#939fa4', edge: '#e8eef0' },
    bondi: { top: '#52b6c6', front: '#3b99aa', side: '#2a7684', edge: '#86d2dc' },
    graph: { top: '#4a5360', front: '#3a424d', side: '#2a3038', edge: '#6a7582' },
};
const OUTLINE = '#080b0f';
/** Every picture shares this canvas, so a pixel is the same size in all of them. */
export const CANVAS_W = 44;
/** Taller than any laptop: the width sets the pixel size, the height is free. */
export const CANVAS_H = 54;

function box(x0, x1, y0, y1, z0, z1, mat, opt = {}) {
    return { x0, x1, y0, y1, z0, z1, mat, r: opt.r || 0, front: opt.front, top: opt.top };
}

function inside(p, x, y, z) {
    if (z < p.z0 || z > p.z1 || x < p.x0 || x > p.x1 || y < p.y0 || y > p.y1) return false;
    if (!p.r) return true;
    const cx = Math.min(Math.max(x, p.x0 + p.r), p.x1 - p.r);
    const cy = Math.min(Math.max(y, p.y0 + p.r), p.y1 - p.r);
    return (x - cx) ** 2 + (y - cy) ** 2 <= p.r * p.r;
}

const RANGE = 44;
const STEP = 0.2;
/** Above the tallest device: a ray starts here and walks down. */
const T_MAX = 40;

/** Rasterises a device. Returns its pixels, floor shadow and bounding box. */
export function renderDevice(prims) {
    const hits = new Map();
    const shadows = new Set();
    let minX = Infinity;
    let maxX = -Infinity;
    let minY = Infinity;
    let maxY = -Infinity;

    for (let sy = -RANGE; sy < RANGE; sy++) {
        for (let sx = -RANGE; sx < RANGE; sx++) {
            const SX = sx + 0.5;
            const SY = sy + 0.5;
            const bx = SY + SX / 2;
            const by = SY - SX / 2;

            // Only the stretch of the ray that crosses some box is marched: most
            // pixels of the canvas cross none and cost nothing.
            let tHi = -Infinity;
            let tLo = Infinity;
            for (const p of prims) {
                const a = Math.max(p.x0 - bx, p.y0 - by, p.z0);
                const b = Math.min(p.x1 - bx, p.y1 - by, p.z1);
                if (a <= b) {
                    tHi = Math.max(tHi, b);
                    tLo = Math.min(tLo, a);
                }
            }

            let hit = null;
            if (tHi >= tLo) {
                // On the same lattice for every pixel, so edges step evenly.
                for (let k = Math.max(0, Math.ceil((T_MAX - tHi) / STEP - 1e-9)); ; k++) {
                    const t = T_MAX - k * STEP;
                    if (t < tLo - 1e-9 || t < 0) break;
                    const x = bx + t;
                    const y = by + t;
                    const p = prims.find((pr) => inside(pr, x, y, t));
                    if (!p) continue;
                    const face = !inside(p, x, y, t + STEP)
                        ? 'top'
                        : !inside(p, x, y + STEP, t)
                          ? 'front'
                          : !inside(p, x + STEP, y, t)
                            ? 'side'
                            : 'front';
                    // Textures read the exact point on the face, not the marched
                    // one: a 0.2 step would make a screen's content jitter.
                    const te = face === 'top' ? p.z1 : face === 'front' ? p.y1 - by : p.x1 - bx;
                    let col = null;
                    if (face === 'front' && p.front) {
                        col = p.front(bx + te - p.x0, p.z1 - te, p.x1 - p.x0, p.z1 - p.z0);
                    } else if (face === 'top' && p.top) {
                        col = p.top(bx + te - p.x0, by + te - p.y0, p.x1 - p.x0, p.y1 - p.y0);
                    }
                    hit = { p, face, col: col || MATS[p.mat][face], tex: !!col };
                    break;
                }
            }

            if (!hit) {
                // Contact shadow on the floor, pushed back-right, away from the light.
                if (
                    prims.some(
                        (pr) =>
                            pr.z0 === 0 &&
                            bx > pr.x0 - 0.5 &&
                            bx < pr.x1 + 2.5 &&
                            by > pr.y0 - 2.5 &&
                            by < pr.y1 + 0.5,
                    )
                ) {
                    shadows.add(`${sx},${sy}`);
                }
                continue;
            }
            hits.set(`${sx},${sy}`, hit);
            minX = Math.min(minX, sx);
            maxX = Math.max(maxX, sx);
            minY = Math.min(minY, sy);
            maxY = Math.max(maxY, sy);
        }
    }

    const pixels = new Map();
    for (const [key, h] of hits) {
        const [x, y] = key.split(',').map(Number);
        let col = h.col;
        if (!h.tex) {
            const below = hits.get(`${x},${y + 1}`);
            const above = hits.get(`${x},${y - 1}`);
            const right = hits.get(`${x + 1},${y}`);
            // A lit bevel where a top turns down into its own sides. Not on a
            // thin panel, whose top is a one-pixel staircase the bevel would
            // turn into a zip.
            const thin = h.p.y1 - h.p.y0 < 2;
            if (!thin && h.face === 'top' && below && below.p === h.p && below.face !== 'top') {
                col = MATS[h.p.mat].edge;
            }
            // A lit vertical edge between the front and the side, on sharp corners.
            if (!h.p.r && h.face === 'front' && right && right.p === h.p && right.face === 'side') {
                col = MATS[h.p.mat].edge;
            }
            // A contact shade where a top runs under something standing on it.
            if (h.face === 'top' && above && above.p !== h.p && above.face !== 'top') {
                col = MATS[h.p.mat].side;
            }
        }
        pixels.set(key, col);
    }
    for (const key of [...pixels.keys()]) {
        const [x, y] = key.split(',').map(Number);
        for (const [dx, dy] of [
            [1, 0],
            [-1, 0],
            [0, 1],
            [0, -1],
        ]) {
            const n = `${x + dx},${y + dy}`;
            if (!pixels.has(n)) pixels.set(n, OUTLINE);
        }
    }
    const floor = [...shadows].filter((k) => !pixels.has(k));
    const bottom = floor.reduce((m, k) => Math.max(m, Number(k.split(',')[1])), maxY + 1);
    return {
        pixels,
        shadows: floor,
        bbox: { minX: minX - 1, maxX: maxX + 1, minY: minY - 1, maxY: bottom },
    };
}

/** The SVG of a rendered device, centred on the shared canvas, one path per colour. */
export function deviceSvg({ pixels, shadows, bbox }) {
    const ox = Math.floor((CANVAS_W - (bbox.maxX - bbox.minX + 1)) / 2) - bbox.minX;
    const oy = Math.floor((CANVAS_H - (bbox.maxY - bbox.minY + 1)) / 2) - bbox.minY;
    const rows = new Map(); // colour → y → sorted xs
    const add = (col, key) => {
        const [x, y] = key.split(',').map(Number);
        if (!rows.has(col)) rows.set(col, new Map());
        const byY = rows.get(col);
        if (!byY.has(y)) byY.set(y, []);
        byY.get(y).push(x);
    };
    // The floor shadow runs back-right past a wide device; the canvas cuts it
    // rather than shrinking every picture to fit a shadow.
    for (const k of shadows) {
        const [x, y] = k.split(',').map(Number);
        if (x + ox >= 0 && x + ox < CANVAS_W && y + oy >= 0 && y + oy < CANVAS_H) add('shadow', k);
    }
    for (const [k, col] of pixels) add(col, k);

    let paths = '';
    for (const [col, byY] of rows) {
        let d = '';
        for (const [y, xs] of byY) {
            xs.sort((a, b) => a - b);
            for (let i = 0; i < xs.length; ) {
                let j = i + 1;
                while (j < xs.length && xs[j] === xs[j - 1] + 1) j++;
                const w = j - i;
                d += `M${xs[i] + ox} ${y + oy}h${w}v1h-${w}z`;
                i = j;
            }
        }
        paths +=
            col === 'shadow'
                ? `<path d="${d}" fill="#000" fill-opacity=".32"/>`
                : `<path d="${d}" fill="${col}"/>`;
    }
    return (
        `<svg class="app-icon-sprite" viewBox="0 0 ${CANVAS_W} ${CANVAS_H}" ` +
        `shape-rendering="crispEdges" aria-hidden="true">${paths}</svg>`
    );
}

/* ── Screens and surfaces: (a, b) from the top-left, in world units ────────── */

const inRect = (a, b, x, y, w, h) => a >= x && a < x + w && b >= y && b < y + h;

function winGlyph(a, b, cx, cy, c, s = 2) {
    for (const [qx, qy] of [
        [0, 0],
        [s + 1, 0],
        [0, s + 1],
        [s + 1, s + 1],
    ]) {
        if (inRect(a, b, cx + qx, cy + qy, s, s)) return c;
    }
    return null;
}

const TUX = [
    '..kkk..',
    '.kwkwk.',
    '.kyyyk.',
    'kkwwwkk',
    'kwwwwwk',
    'kwwwwwk',
    '.kwwwk.',
    'yy...yy',
];
const TUX_COLOURS = { k: '#16191d', w: '#e9edf0', y: '#e8b23a' };
function tux(a, b, x, y) {
    const i = Math.floor(b - y);
    const j = Math.floor(a - x);
    if (i < 0 || i >= TUX.length || j < 0 || j >= 7) return null;
    return TUX_COLOURS[TUX[i][j]] || null;
}

function gnome(a, b, W, H) {
    if (b < 1) return '#0f1215';
    const t = tux(a, b, Math.round(W / 2 - 3.5), Math.round(1 + (H - 1) / 2 - 4));
    if (t) return t;
    const f = (b - 1) / (H - 1);
    return f < 0.35 ? '#4b8f93' : f < 0.7 ? '#3d7a80' : '#30666d';
}

/** Lines of text as seen from afar: every other row, lengths that never repeat in step. */
const TERMINAL_LINES = [9, 6, 11, 4, 13, 7, 10, 5, 12, 8];
const CODE_LINES = [
    [1, 8],
    [3, 11],
    [3, 7],
    [5, 9],
    [5, 13],
    [3, 6],
    [1, 5],
    [1, 10],
    [3, 12],
    [1, 4],
];

function terminal(a, b, W, H) {
    if (b < 1) return '#0f1215';
    const t = tux(a, b, W - 8, 1.5);
    if (t) return t;
    const row = Math.floor(b);
    const n = (row - 3) / 2;
    if (row >= 3 && Number.isInteger(n) && row < H - 1) {
        const last = row + 2 >= H - 1;
        const len = TERMINAL_LINES[n % TERMINAL_LINES.length];
        if (a >= 1.5 && a < 1.5 + (last ? 4 : len)) return last ? '#e8b23a' : '#69c98a';
    }
    return '#161c22';
}

/** The default screen: the Linux monitor's wallpaper with lines of code on it. */
function code(a, b, W, H) {
    if (b < 1) return '#0f1215';
    const row = Math.floor(b);
    const n = (row - 3) / 2;
    if (row >= 3 && Number.isInteger(n) && row < H - 1) {
        const [x, len] = CODE_LINES[n % CODE_LINES.length];
        if (a >= x && a < x + len) return n % CODE_LINES.length === 2 ? '#e8b23a' : '#d3ece6';
    }
    const f = (b - 1) / (H - 1);
    return f < 0.35 ? '#4b8f93' : f < 0.7 ? '#3d7a80' : '#30666d';
}

function macos(a, b, W, H) {
    const d0 = W * 0.2;
    const d1 = W * 0.8;
    if (b >= H - 3 && b < H - 0.6 && a >= d0 && a < d1) {
        const i = Math.floor(a - d0);
        const cols = ['#4a9ee8', '#57c27a', '#e8a93e', '#d9605d', '#a57ad8'];
        return b >= H - 2.6 && b < H - 1 && i % 3 !== 2
            ? cols[Math.floor(i / 3) % cols.length]
            : '#aeb5c0';
    }
    const f = (a / W) * 0.55 + (b / H) * 0.75;
    return f < 0.35 ? '#5b4bb0' : f < 0.6 ? '#8752a8' : f < 0.85 ? '#bd658d' : '#dc8b62';
}

function os9(a, b, W) {
    if (b < 1) return '#eceef0';
    if (inRect(a, b, W - 4, 2, 2, 2)) return '#c9ccd2';
    return (Math.floor(a) + Math.floor(b)) % 2 ? '#7282ad' : '#6a7aa5';
}

/** The arcade sprites a Windows screen shows instead of a wallpaper. */
const ARCADE_ROWS = {
    invader: [
        '..y.....y..',
        '...y...y...',
        '..yyyyyyy..',
        '.yy.yyy.yy.',
        'yyyyyyyyyyy',
        'y.yyyyyyy.y',
        'y.y.....y.y',
        '...yy.yy...',
    ],
    ghost: [
        '....cccc....',
        '..cccccccc..',
        '.cccccccccc.',
        '.ccwwccwwcc.',
        'ccwmmccwmmcc',
        'ccwmmccwmmcc',
        'cccccccccccc',
        'cccccccccccc',
        'cccccccccccc',
        'cccccccccccc',
        'cc.cc..cc.cc',
        'c...c..c...c',
    ],
    joystick: [
        '....mmmm....',
        '...mmmmmm...',
        '...mmwmmm...',
        '....mmmm....',
        '.....cc.....',
        '.....cc.....',
        '.....cc.....',
        '.....cc.....',
        '..yyyyyyyy..',
        '.yyyyyyyyyy.',
        'yyyymmyyyy..',
        'yyyyyyyyyyyy',
    ],
    ship: SPRITES.find((s) => s.name === 'ship').rows,
    coin: [
        '...yyyy...',
        '..yddddy..',
        '.yddddddy.',
        'ydddyydddy',
        'yddyyydddy',
        'ydddyydddy',
        'ydddyydddy',
        'ydddyydddy',
        '.ydyyyydy.',
        '..yddddy..',
        '...yyyy...',
    ],
};
export const WALLPAPERS = Object.keys(ARCADE_ROWS);
const ARCADE_COLOURS = { y: '#fcee0a', c: '#00e5ff', m: '#ff2a6d', w: '#eafdff', d: '#7d7400' };

/**
 * The attract-mode line under each sprite on a monitor, and its colour. Short
 * enough for a 3×5 pixel font on a 38-pixel screen: "PRESS START" needs 40,
 * so the ship says what many cabinets said anyway.
 */
export const ARCADE_LINES = {
    invader: ['INSERT COIN', 'c'],
    ghost: ['READY!', 'y'],
    joystick: ['PLAYER 1', 'c'],
    ship: ['PUSH START', 'y'],
    coin: ['CREDIT 01', 'c'],
};

/** A 3×5 pixel font, proportional where it helps: I, 1 and ! are narrower, N wider. */
export const FONT = {
    A: ['.#.', '#.#', '###', '#.#', '#.#'],
    C: ['###', '#..', '#..', '#..', '###'],
    D: ['##.', '#.#', '#.#', '#.#', '##.'],
    E: ['###', '#..', '##.', '#..', '###'],
    H: ['#.#', '#.#', '###', '#.#', '#.#'],
    I: ['#', '#', '#', '#', '#'],
    L: ['#..', '#..', '#..', '#..', '###'],
    N: ['#..#', '##.#', '#.##', '#..#', '#..#'],
    O: ['###', '#.#', '#.#', '#.#', '###'],
    P: ['###', '#.#', '###', '#..', '#..'],
    R: ['##.', '#.#', '##.', '#.#', '#.#'],
    S: ['###', '#..', '###', '..#', '###'],
    T: ['###', '.#.', '.#.', '.#.', '.#.'],
    U: ['#.#', '#.#', '#.#', '#.#', '###'],
    Y: ['#.#', '#.#', '.#.', '.#.', '.#.'],
    0: ['###', '#.#', '#.#', '#.#', '###'],
    1: ['.#', '##', '.#', '.#', '.#'],
    '!': ['#', '#', '#', '.', '#'],
    ' ': ['', '', '', '', ''],
};

/**
 * The pixels of a line of text: five strings of '#' and '.', one column between
 * glyphs, and for every column the column its glyph starts at.
 */
export function textRows(text) {
    const rows = ['', '', '', '', ''];
    const starts = [];
    [...text].forEach((ch, n) => {
        const glyph = FONT[ch] || FONT[' '];
        if (n) {
            for (let r = 0; r < 5; r++) rows[r] += '.';
            starts.push(starts.length);
        }
        const start = rows[0].length;
        for (let r = 0; r < 5; r++) rows[r] += glyph[r];
        for (let c = 0; c < glyph[0].length; c++) starts.push(start);
    });
    return Object.assign(rows, { starts });
}

/**
 * The row of a pixel-art block drawn upright on a screen seen in isometry.
 *
 * On a front face a screen row is b + a/2, so a block sampled by b alone is
 * sheared: its horizontal strokes break into a staircase in the middle of a
 * letter. Measured from the block's own left column instead, each block stays
 * upright and whole, and only the line of blocks follows the slope.
 */
const uprightRow = (a, b, top, left) => Math.floor(b - top + (a - left) / 2);

const windowsArcade =
    (sprite, { line = false } = {}) =>
    (a, b, W, H) => {
        const bar = 1.2;
        if (b >= H - bar) {
            const c = W / 2;
            if (inRect(a, b, c - 3, H - 1, 1, 1)) return '#5fb0ff';
            if (inRect(a, b, c - 1, H - 1, 1, 1)) return '#d8dfe6';
            if (inRect(a, b, c + 1, H - 1, 1, 1)) return '#e3a53d';
            return '#1c2533';
        }
        const rows = ARCADE_ROWS[sprite];
        const [label, labelColour] = ARCADE_LINES[sprite];
        const text = line ? textRows(label) : null;
        // The sprite and its line are centred as one block.
        const block = rows.length + (text ? 2 + text.length : 0);
        const y = Math.floor((H - bar - block) / 2);
        const x = Math.floor((W - rows[0].length) / 2);
        const j = Math.floor(a - x);
        const i = line ? uprightRow(a, b, y, x) : Math.floor(b - y);
        if (
            i >= 0 &&
            i < rows.length &&
            j >= 0 &&
            j < rows[0].length &&
            ARCADE_COLOURS[rows[i][j]]
        ) {
            return ARCADE_COLOURS[rows[i][j]];
        }
        if (text) {
            const ty = y + rows.length + 2;
            const tx = Math.floor((W - text[0].length) / 2);
            const tj = Math.floor(a - tx);
            if (tj >= 0 && tj < text[0].length) {
                const ti = uprightRow(a, b, ty, tx + text.starts[tj]);
                if (ti >= 0 && ti < 5 && text[ti][tj] === '#') return ARCADE_COLOURS[labelColour];
            }
        }
        return '#111a2b';
    };

const bezel =
    (screen, { notch = false, chin = 0 } = {}) =>
    (a, b, W, H) => {
        if (notch && b < 1.8 && Math.abs(a - W / 2) < 2) return '#0c0e11';
        if (a < 1 || a >= W - 1 || b < 1 || b >= H - 1 - chin) {
            if (chin && b >= H - chin) return null; // the chin is the body's own metal
            return '#101317';
        }
        return screen(a - 1, b - 1, W - 2, H - 2 - chin);
    };

const keyboard = (trackpoint) => (a, b, W, H) => {
    if (trackpoint && inRect(a, b, W / 2 - 0.5, 5.5, 1, 1)) return '#d44a4a';
    if (inRect(a, b, 2, 2, W - 4, 7.5)) {
        return Math.floor(a) % 2 === 0 && Math.floor(b) % 2 === 0 ? '#191c21' : null;
    }
    if (inRect(a, b, W / 2 - 4, H - 5.5, 8, 4)) return trackpoint ? '#23262b' : '#8f9aa4';
    return null;
};

/* ── Devices ───────────────────────────────────────────────────────────────── */

const laptop = (mat, screen, { lid = 17, notch = false, trackpoint = false } = {}) => [
    box(0, 24, 0, 2.2, 1.6, lid, mat, { front: bezel(screen, { notch }) }),
    box(0, 24, 0, 16, 0, 1.6, mat, { top: keyboard(trackpoint) }),
];

/** A monitor on a stand. `z0` is where the panel's bottom edge sits. */
const monitor = (mat, screen, { w = 38, h = 23, z0 = 5 } = {}) => {
    const c = w / 2;
    return [
        box(0, w, 2.2, 3.6, z0, z0 + h, mat, { front: bezel(screen) }),
        box(c - 1.4, c + 1.4, 1, 2.2, 1, z0 + 6, mat),
        box(c - 6, c + 6, 0, 8, 0, 1, mat),
    ];
};

/** The same device, bigger: geometry only, so screen content keeps its pixel size. */
const scaled = (prims, k) =>
    prims.map((p) => ({
        ...p,
        x0: p.x0 * k,
        x1: p.x1 * k,
        y0: p.y0 * k,
        y1: p.y1 * k,
        z0: p.z0 * k,
        z1: p.z1 * k,
        r: p.r * k,
    }));

const miniPc = (mat, badge) => [box(0, 17, 0, 13, 0, 8, mat, { r: 1.2, front: badge })];

const windowsBadge = (a, b, W, H) =>
    winGlyph(a, b, 3, 1.5, '#5fb0ff') ||
    (inRect(a, b, W - 3, H - 3, 1, 1)
        ? '#5fb0ff'
        : inRect(a, b, W - 7, H - 3, 2, 1) || inRect(a, b, W - 10, H - 3, 2, 1)
          ? '#101317'
          : null);

const linuxBadge = (a, b, W, H) =>
    tux(a, b, 2.5, 0.5) ||
    (inRect(a, b, W - 3, H - 3, 1, 1)
        ? '#69c98a'
        : inRect(a, b, W - 7, H - 3, 2, 1)
          ? '#101317'
          : null);

/** Every picture, by id. Windows screens take the arcade sprite as a parameter. */
const DEVICES = {
    'windows-laptop': (w) => laptop('gray', windowsArcade(w), { lid: 18.6 }),
    // Monitors are drawn as big as the canvas allows beside a laptop, which
    // leaves room for the attract-mode line under the sprite.
    'windows-monitor': (w) => monitor('black', windowsArcade(w, { line: true }), { w: 40, h: 25 }),
    'windows-monitor-4x3': (w) =>
        monitor('black', windowsArcade(w, { line: true }), { w: 40, h: 30, z0: 4 }),
    'windows-mini': () => miniPc('black', windowsBadge),
    'linux-laptop': () => laptop('black', gnome, { trackpoint: true }),
    'linux-monitor': () => monitor('graph', gnome),
    'linux-monitor-terminal': () => monitor('black', terminal, { h: 24 }),
    'linux-mini': () => miniPc('graph', linuxBadge),
    macbook: () => laptop('alu', macos, { notch: true }),
    'imac-g3': () =>
        scaled(
            [
                box(1, 21, 7, 17, 4, 21, 'ice', {
                    r: 2.5,
                    front: (a, b, W, H) =>
                        inRect(a, b, 4, 3, W - 8, H - 8)
                            ? os9(a - 4, b - 3, W - 8)
                            : inRect(a, b, 3, 2, W - 6, H - 6)
                              ? '#9aa4aa'
                              : null,
                }),
                box(3, 19, 1, 9, 5, 19, 'bondi', { r: 4 }),
                box(5, 17, 6, 15, 0, 4, 'bondi', { r: 2 }),
            ],
            1.3,
        ),
    'imac-alu': () =>
        scaled(
            [
                box(0, 26, 4, 6.4, 4, 22, 'alu', { front: bezel(macos, { chin: 4.5 }) }),
                box(9, 17, 1, 4, 1, 11, 'alu'),
                box(9, 17, 0, 9, 0, 1, 'alu'),
            ],
            1.4,
        ),
    'studio-display': () =>
        scaled(
            [
                box(0, 26, 4, 6.2, 7, 22, 'alu', { front: bezel(macos) }),
                box(10, 16, 1, 4, 1, 11, 'alu'),
                box(10, 16, 0, 9, 0, 0.9, 'alu'),
            ],
            1.4,
        ),
    'mac-mini': () => [
        box(0, 20, 0, 20, 0, 4.5, 'alu', {
            r: 2,
            front: (a, b, W, H) => (inRect(a, b, W - 7, H - 2.6, 1, 1) ? '#f2f6f8' : null),
        }),
    ],
    'default-monitor': () => monitor('graph', code),
};
export const DEVICE_IDS = Object.keys(DEVICES);

/* ── Choosing the picture ──────────────────────────────────────────────────── */

/**
 * A stable, well-spread 32-bit number from a string: FNV-1a, then MurmurHash3's
 * finaliser. FNV-1a alone is not enough here — its low bit is only the parity
 * of the characters, and the two monitors of one machine have keys that differ
 * in a few characters in the middle, so they drew the same picture every time.
 */
export function hashKey(text) {
    let h = 0x811c9dc5;
    for (let i = 0; i < text.length; i++) {
        h ^= text.charCodeAt(i);
        h = Math.imul(h, 0x01000193);
    }
    h ^= h >>> 16;
    h = Math.imul(h, 0x85ebca6b);
    h ^= h >>> 13;
    h = Math.imul(h, 0xc2b2ae35);
    h ^= h >>> 16;
    return h >>> 0;
}

/**
 * The picture for one display of a native host.
 *
 * @param {object} device  the app's `device` object from the server
 * @param {object[]} siblings  the `device` of every display of the same host
 * @returns {{ id: string, wallpaper: string|null, os: string }}
 */
export function hostArtFor(device, siblings = []) {
    const d = device || {};
    const os = d.os || '';
    const kind = d.display || 'unknown';
    const seed = hashKey(String(d.key || ''));
    const pick = (a, b) => (seed & 1 ? b : a);
    const wallpaper = WALLPAPERS[(seed >>> 8) % WALLPAPERS.length];

    if (os === 'windows') {
        if (kind === 'builtin') return { id: 'windows-laptop', wallpaper, os };
        if (kind === 'external')
            return { id: pick('windows-monitor', 'windows-monitor-4x3'), wallpaper, os };
        if (kind === 'virtual') return { id: 'windows-mini', wallpaper: null, os };
    } else if (os === 'linux') {
        if (kind === 'builtin') return { id: 'linux-laptop', wallpaper: null, os };
        if (kind === 'external')
            return { id: pick('linux-monitor', 'linux-monitor-terminal'), wallpaper: null, os };
        if (kind === 'virtual') return { id: 'linux-mini', wallpaper: null, os };
    } else if (os === 'macos') {
        const iMac = pick('imac-g3', 'imac-alu');
        if (kind === 'builtin') return { id: d.battery ? 'macbook' : iMac, wallpaper: null, os };
        if (kind === 'external') {
            if (/studio display/i.test(String(d.model || ''))) {
                return { id: 'studio-display', wallpaper: null, os };
            }
            // A monitor on a Mac with a screen of its own (a MacBook, an iMac)
            // is "a normal screen". On a Mac with none — a mini, a Studio, a
            // Pro — it is the box the picture should show.
            const ownScreen = d.battery || siblings.some((s) => s && s.display === 'builtin');
            return { id: ownScreen ? iMac : 'mac-mini', wallpaper: null, os };
        }
        if (kind === 'virtual') return { id: 'mac-mini', wallpaper: null, os };
    }
    return { id: 'default-monitor', wallpaper: null, os };
}

const svgCache = new Map();

/** The SVG for a picture, drawn the first time it is asked for and kept. */
export function hostArtSvg(id, wallpaper = null) {
    const cacheKey = `${id}:${wallpaper || ''}`;
    let svg = svgCache.get(cacheKey);
    if (!svg) {
        const build = DEVICES[id] || DEVICES['default-monitor'];
        svg = deviceSvg(renderDevice(build(wallpaper || WALLPAPERS[0])));
        svgCache.set(cacheKey, svg);
    }
    return svg;
}

/**
 * The markup that stands in a native host card's frame. Decorative: the card's
 * aria-label already names the display.
 */
export function hostArtHtml(device, siblings = []) {
    const art = hostArtFor(device, siblings);
    const glow =
        art.os === 'windows' || art.os === 'linux' || art.os === 'macos' ? art.os : 'default';
    return (
        `<span class="app-icon app-icon--host app-icon--${glow}" aria-hidden="true" ` +
        `data-art="${art.id}${art.wallpaper ? `:${art.wallpaper}` : ''}">` +
        `${hostArtSvg(art.id, art.wallpaper)}</span>`
    );
}
