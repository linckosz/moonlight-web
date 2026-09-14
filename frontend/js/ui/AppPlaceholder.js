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
 * A pixel-art arcade ship over a "PRESS START" line, drawn in the theme's
 * neons. It replaces the 🎮 emoji, which every OS drew in its own colours and
 * none of them in ours.
 *
 * The sprite is kept as the grid it was drawn on, so it can be retouched by
 * editing characters: `c` cyan hull, `y` yellow cockpit, `m` magenta exhaust.
 * Each colour becomes one path of unit squares, so the SVG stays three
 * elements whatever the drawing.
 */

import { escapeHtml } from '../util/escapeHtml.js';

const SHIP = [
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
];

const COLOURS = { c: 'cyan', y: 'yellow', m: 'magenta' };

/** One path per colour: each run of same-colour cells becomes a rectangle. */
function spritePaths(rows) {
    const d = {};
    rows.forEach((row, y) => {
        let x = 0;
        while (x < row.length) {
            const ch = row[x];
            let end = x + 1;
            while (end < row.length && row[end] === ch) end++;
            if (COLOURS[ch]) d[ch] = (d[ch] || '') + `M${x} ${y}h${end - x}v1h-${end - x}z`;
            x = end;
        }
    });
    return Object.entries(d)
        .map(([ch, path]) => `<path class="app-icon-${COLOURS[ch]}" d="${path}"/>`)
        .join('');
}

const SHIP_SVG =
    `<svg class="app-icon-sprite" viewBox="0 0 ${SHIP[0].length} ${SHIP.length}" ` +
    `shape-rendering="crispEdges" aria-hidden="true">${spritePaths(SHIP)}</svg>`;

/**
 * The placeholder markup. Decorative only: the card's aria-label already
 * names the app, so nothing here is read out.
 *
 * @param {string} label  the line under the ship, translated
 */
export function appPlaceholderHtml(label) {
    return (
        `<span class="app-icon" aria-hidden="true">${SHIP_SVG}` +
        `<span class="app-icon-label">${escapeHtml(label)}</span></span>`
    );
}
