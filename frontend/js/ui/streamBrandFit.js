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
 * The mark on the stream bar (title, machine name, version) against the
 * controls centred in the same bar.
 *
 * The mark stops at 40 % of the bar, which is short of the Fullscreen button
 * on its own. It is not short of the group around it: the microphone and the
 * volume dock on that button's left, and on a phone held sideways the keyboard
 * button pushes the mark right as well. The version then ran under the
 * microphone. So the mark gives up what it has to, least useful first:
 *   1. the version — the application's own header and menu still show it;
 *   2. the title — the user knows which application this is, not always
 *      which of their machines;
 *   3. the end of the machine's name, cut with an ellipsis;
 *   4. the whole name, last, once too little of it is left to be read; the
 *      mark keeps its place so the buttons do not move.
 * Without a name, the title goes once it does not fit.
 */

/**
 * Below this many pixels of text, a cut name is not read but guessed: "D…"
 * is no name. About five characters of the bar's font.
 */
export const MIN_NAME_TEXT_PX = 40;

/**
 * What the mark keeps in `room` pixels, from where it starts.
 * @param {{room: number, title: number, name: number, namePad: number,
 *          version: number, gap: number}} m
 *   widths in px: `name` is the full name with its padding, `namePad` that
 *   padding alone; 0 for a part the mark does not carry.
 * @returns {{name: 'full' | 'cut' | 'none', version: boolean, title: boolean}}
 */
export function planStreamBrand(m) {
    const { room, title, name, namePad, version, gap } = m;
    const withName = name > 0 ? title + gap + name : title;
    if (withName + (version > 0 ? gap + version : 0) <= room) {
        return { name: name > 0 ? 'full' : 'none', version: version > 0, title: true };
    }
    if (withName <= room) return { name: name > 0 ? 'full' : 'none', version: false, title: true };
    if (!(name > 0)) return { name: 'none', version: false, title: title <= room };
    if (name <= room) return { name: 'full', version: false, title: false };
    if (room - namePad >= MIN_NAME_TEXT_PX) return { name: 'cut', version: false, title: false };
    return { name: 'none', version: false, title: false };
}

/**
 * Fits the bar's mark within its 40 % and left of `limit` (a viewport x;
 * null when nothing sits in the middle of the bar). Measured from scratch
 * each time, so a part hidden by the last pass is weighed again once there is
 * room for it.
 * @param {HTMLElement | null | undefined} brand  the bar's .stream-brand
 * @param {number | null} limit
 */
export function fitStreamBrand(brand, limit) {
    if (!brand) return;
    const titleEl = /** @type {HTMLElement | null} */ (brand.querySelector('.stream-brand-title'));
    const nameBox = /** @type {HTMLElement | null} */ (brand.querySelector('.instance-menu'));
    const nameEl = /** @type {HTMLElement | null} */ (brand.querySelector('.instance-name'));
    const versionEl = /** @type {HTMLElement | null} */ (brand.querySelector('.version'));
    brand.style.maxWidth = '';
    brand.style.visibility = '';
    if (titleEl) titleEl.style.display = '';
    if (nameBox) nameBox.style.display = '';
    if (versionEl) versionEl.style.display = '';
    const bar = brand.parentElement;
    if (!titleEl || !bar) return;
    const box = brand.getBoundingClientRect();
    // Hidden by the CSS (a narrow bar) or the header gone (fullscreen).
    if (!(box.width > 0)) return;
    // The CSS's own 40 % cap cuts the name too, with or without a limit.
    const bs = getComputedStyle(bar);
    const barWidth =
        bar.clientWidth - (parseFloat(bs.paddingLeft) || 0) - (parseFloat(bs.paddingRight) || 0);
    let room = barWidth * 0.4;
    if (limit != null) room = Math.min(room, limit - box.left);
    const cs = getComputedStyle(brand);
    const gap = parseFloat(cs.columnGap) || 0;
    let namePad = 0;
    if (nameEl) {
        const ns = getComputedStyle(nameEl);
        namePad = (parseFloat(ns.paddingLeft) || 0) + (parseFloat(ns.paddingRight) || 0);
    }
    const plan = planStreamBrand({
        room,
        title: titleEl.getBoundingClientRect().width,
        // scrollWidth: the whole name, even while the bar shows it cut.
        name: nameEl ? nameEl.scrollWidth : 0,
        namePad,
        version: versionEl ? versionEl.getBoundingClientRect().width : 0,
        gap,
    });
    if (versionEl && !plan.version) versionEl.style.display = 'none';
    if (nameBox && plan.name === 'none') nameBox.style.display = 'none';
    if (plan.name === 'cut') brand.style.maxWidth = `${Math.max(0, Math.floor(room))}px`;
    if (!plan.title && plan.name !== 'none') titleEl.style.display = 'none';
    // Nothing left to show. Not display: the mark's margin is what keeps the
    // buttons on the right.
    if (!plan.title && plan.name === 'none') brand.style.visibility = 'hidden';
}
