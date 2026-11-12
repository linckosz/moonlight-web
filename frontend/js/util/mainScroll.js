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
 * The hosts page, Settings and Admin all render into one scroller, which
 * stays put while its content is swapped. Without its own position, the hosts
 * page came back from Settings at the depth Settings was left at.
 */

/**
 * The element the main views scroll in: #main-content, or the document itself
 * on a TV, where RemoteNav lets the page grow instead (layout.css,
 * html.remote-nav).
 * @param {HTMLElement | null} main  #main-content
 * @returns {HTMLElement | null}
 */
export function mainScroller(main, doc = document) {
    if (doc.documentElement.classList.contains('remote-nav')) {
        return /** @type {HTMLElement} */ (doc.scrollingElement || doc.documentElement);
    }
    return main;
}

/**
 * Put @p scroller at @p top: now, and once more on the next frame when that is
 * below the top, for a view whose content is still being laid out when it is
 * mounted (the first attempt is clamped to what is there).
 * @param {HTMLElement | null} scroller
 * @param {number} top
 */
export function restoreScroll(scroller, top) {
    if (!scroller) return;
    scroller.scrollTop = top;
    if (top > 0) {
        requestAnimationFrame(() => {
            scroller.scrollTop = top;
        });
    }
}
