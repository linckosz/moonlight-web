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
 * MoonlightWeb — the stand-in for an app that has no cover.
 *
 * A pixel-art sprite over an arcade line, drawn in the theme's neons. It
 * replaces the 🎮 emoji, which every OS drew in its own colours and none of
 * them in ours.
 *
 * There is a small cast of them, and the app's id picks one, so two apps
 * without a cover never look alike and the library reads as an arcade hall.
 * The id rather than the position: an app keeps its sprite when the list is
 * reordered or grows.
 *
 * Each sprite is kept as the grid it was drawn on, so it can be retouched by
 * editing characters: `c` cyan, `y` yellow, `m` magenta, `w` white. Each colour
 * becomes one path of unit squares. Every grid sits centred on the same canvas,
 * so a pixel is the same size in every sprite.
 */

import { escapeHtml } from '../util/escapeHtml.js';

const CANVAS_W = 15;
const CANVAS_H = 13;

const COLOURS = { c: 'cyan', y: 'yellow', m: 'magenta', w: 'white' };

/**
 * The cast. `glow` is the colour of the halo, `label` the i18n key of the line
 * under the sprite. The first one is the arcade ship the others were drawn to
 * match.
 */
export const SPRITES = [
    {
        name: 'ship',
        glow: 'cyan',
        label: 'apps.arcade.pressStart',
        rows: [
            '......c......',
            '......c......',
            '.....ccc.....',
            '.....cyc.....',
            '.....cyc.....',
            '.c...ccc...c.',
            '.c..ccccc..c.',
            '.c.ccccccc.c.',
            '.ccccccccccc.',
            'ccccccccccccc',
            'cc.ccccccc.cc',
            'c..c.....c..c',
            '...m.....m...',
        ],
    },
    {
        // A netrunner's skull, one eye swapped for an optic.
        name: 'skull',
        glow: 'yellow',
        label: 'apps.arcade.jackIn',
        rows: [
            '..yyyyyyy..',
            '.yyyyyyyyy.',
            'yyyyyyyyyyy',
            'y...yyycccy',
            'y.m.yyycwcy',
            'y...yyycccy',
            'yyyyy.yyyyy',
            '.yyy...yyy.',
            '..yyyyyyy..',
            '..y.y.y.y..',
            '..yyyyyyy..',
        ],
    },
    {
        name: 'katana',
        glow: 'cyan',
        label: 'apps.arcade.wakeUp',
        rows: [
            '...........wc',
            '..........wwc',
            '.........wwc.',
            '........wwc..',
            '.......wwc...',
            '......wwc....',
            '..m..wwc.....',
            '...mwwc......',
            '....m........',
            '..yy.m.......',
            '.yy...m......',
            'yy...........',
            'm............',
        ],
    },
    {
        // Megabuildings, lit windows, one neon sign.
        name: 'city',
        glow: 'yellow',
        label: 'apps.arcade.nightCity',
        rows: [
            '.......m.......',
            '.......y.......',
            '......yyy......',
            '......ycy......',
            '......yyy...y..',
            '..y...ycy..yyy.',
            '.yyy..yyy..ycy.',
            '.ycy.yyyyy.yyy.',
            '.yyy.ycycy.ycy.',
            'myyy.yyyyy.yyym',
            'mycy.ycycy.ycym',
            'yyyyyyyyyyyyyyy',
            'c.c.c.c.c.c.c.c',
        ],
    },
    {
        // A low street racer, headlight on.
        name: 'car',
        glow: 'magenta',
        label: 'apps.arcade.rideOut',
        rows: [
            '.........mmmm..',
            '.......mmmmmmm.',
            '...mmmmmmmmmmmm',
            'mmmyyyyyyyyymmw',
            '.ccc.mmmmm.ccc.',
            'c...c.....c...c',
            'c.y.c.....c.y.c',
            'c...c.....c...c',
            '.ccc.......ccc.',
        ],
    },
    {
        // An optic implant mid-scan, framed by its targeting brackets.
        name: 'optic',
        glow: 'magenta',
        label: 'apps.arcade.scanning',
        rows: [
            'mm.........mm',
            'm...ccccc...m',
            '..cc.....cc..',
            '.c...yyy...c.',
            'c...ymmmy...c',
            'c...ymwmy...c',
            'c...ymmmy...c',
            '.c...yyy...c.',
            '..cc.....cc..',
            'm...ccccc...m',
            'mm.........mm',
        ],
    },
];

/** One path per colour: each run of same-colour cells becomes a rectangle. */
function spritePaths(rows) {
    const ox = Math.floor((CANVAS_W - rows[0].length) / 2);
    const oy = Math.floor((CANVAS_H - rows.length) / 2);
    const d = {};
    rows.forEach((row, y) => {
        let x = 0;
        while (x < row.length) {
            const ch = row[x];
            let end = x + 1;
            while (end < row.length && row[end] === ch) end++;
            if (COLOURS[ch]) {
                const w = end - x;
                d[ch] = (d[ch] || '') + `M${ox + x} ${oy + y}h${w}v1h-${w}z`;
            }
            x = end;
        }
    });
    return Object.entries(d)
        .map(([ch, path]) => `<path class="app-icon-${COLOURS[ch]}" d="${path}"/>`)
        .join('');
}

const SPRITE_SVG = SPRITES.map(
    (s) =>
        `<svg class="app-icon-sprite app-icon-sprite--${s.glow}" ` +
        `viewBox="0 0 ${CANVAS_W} ${CANVAS_H}" shape-rendering="crispEdges" ` +
        `aria-hidden="true">${spritePaths(s.rows)}</svg>`,
);

/** The sprite an app gets. Sequential ids (a host's displays) land on neighbours. */
export function spriteIndexFor(appId) {
    const n = Math.trunc(Number(appId)) || 0;
    return ((n % SPRITES.length) + SPRITES.length) % SPRITES.length;
}

/**
 * The placeholder markup. Decorative only: the card's aria-label already
 * names the app, so nothing here is read out.
 *
 * @param {number|string} appId  picks the sprite
 * @param {(key: string) => string} translate  i18n lookup for the line
 */
export function appPlaceholderHtml(appId, translate) {
    const i = spriteIndexFor(appId);
    return (
        `<span class="app-icon" aria-hidden="true" data-sprite="${SPRITES[i].name}">` +
        `${SPRITE_SVG[i]}<span class="app-icon-label">${escapeHtml(translate(SPRITES[i].label))}</span></span>`
    );
}
