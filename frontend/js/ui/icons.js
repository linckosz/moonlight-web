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
    // Alert circle — host reachable at the IP level but GameStream server down
    // ("Unavailable" status).
    unavailable: svg(
        '<circle cx="12" cy="12" r="10"/><line x1="12" y1="8" x2="12" y2="12"/>' +
            '<line x1="12" y1="16" x2="12.01" y2="16"/>',
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
    // User-plus — a free player slot in the Share menu, nobody invited yet
    userPlus: svg(
        '<path d="M15 21v-2a4 4 0 0 0-4-4H6a4 4 0 0 0-4 4v2"/><circle cx="8.5" cy="7" r="4"/>' +
            '<line x1="19" y1="8" x2="19" y2="14"/><line x1="22" y1="11" x2="16" y2="11"/>',
    ),
    // Link — an invite link is live, the guest has not joined yet
    link: svg(
        '<path d="M10 13a5 5 0 0 0 7.54.54l3-3a5 5 0 0 0-7.07-7.07l-1.72 1.71"/>' +
            '<path d="M14 11a5 5 0 0 0-7.54-.54l-3 3a5 5 0 0 0 7.07 7.07l1.71-1.71"/>',
    ),
    // Play — the invited player is streaming right now
    play: svg('<polygon points="7 4 20 12 7 20 7 4" fill="currentColor" stroke-width="1.5"/>'),
};
