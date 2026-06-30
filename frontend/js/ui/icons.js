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

/* Inline SVG icon set — replaces font-dependent glyphs (⏻ ✔ 🔒 ⌨ ⛶) that
 * render inconsistently across OS/browsers. Each icon is a stroke icon using
 * currentColor, sized by CSS (.icon = 1em). Drop straight into innerHTML. */

const svg = (body) =>
    `<svg class="icon" viewBox="0 0 24 24" fill="none" stroke="currentColor" ` +
    `stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">${body}</svg>`;

export const Icons = {
    // Power (was ⏻) — offline status + Wake-on-LAN button
    power: svg('<path d="M18.36 6.64a9 9 0 1 1-12.73 0"/><line x1="12" y1="2" x2="12" y2="12"/>'),
    // Check (was ✔) — paired / ready status
    check: svg('<polyline points="20 6 9 17 4 12"/>'),
    // Lock (was 🔒) — online but not paired
    lock: svg(
        '<rect x="3" y="11" width="18" height="11" rx="2" ry="2"/><path d="M7 11V7a5 5 0 0 1 10 0v4"/>',
    ),
    // Keyboard (was ⌨) — on-screen keyboard toggle
    keyboard: svg(
        '<rect x="2" y="6" width="20" height="12" rx="2"/><path d="M6 10h0M10 10h0M14 10h0M18 10h0M6 14h0M9 14h6M18 14h0"/>',
    ),
    // Maximize (was ⛶) — fullscreen button
    fullscreen: svg(
        '<path d="M8 3H5a2 2 0 0 0-2 2v3M21 8V5a2 2 0 0 0-2-2h-3M3 16v3a2 2 0 0 0 2 2h3M16 21h3a2 2 0 0 0 2-2v-3"/>',
    ),
    // Kebab (⋮) — per-host options menu (remove, …)
    menu: svg(
        '<circle cx="12" cy="5" r="1.4" fill="currentColor"/>' +
            '<circle cx="12" cy="12" r="1.4" fill="currentColor"/>' +
            '<circle cx="12" cy="19" r="1.4" fill="currentColor"/>',
    ),
};
