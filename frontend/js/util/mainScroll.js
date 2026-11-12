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

/** The longest a restored position holds against content that arrives late. */
export const SCROLL_HOLD_MS = 2000;

/** What a user does to scroll on purpose: from then on, anchoring is theirs again. */
const USER_SCROLL_EVENTS = ['wheel', 'touchstart', 'pointerdown', 'keydown'];

/** The release of the hold each scroller is under, if any. */
const holds = new WeakMap();

/**
 * Keep the browser's scroll anchoring off @p scroller until the user scrolls,
 * or for SCROLL_HOLD_MS at most. A view put back at a saved position is often
 * not all there yet: a host card above gets its app row a few milliseconds
 * later, and anchoring would then move the view by that row's height to keep
 * what was on screen. Without it, the late content pushes the rest down and
 * the position lands where it was saved.
 * @param {HTMLElement} scroller
 */
function holdPosition(scroller) {
    holds.get(scroller)?.();
    const doc = scroller.ownerDocument;
    const own = scroller.style.getPropertyValue('overflow-anchor');
    scroller.style.setProperty('overflow-anchor', 'none');
    let timer = 0;
    const release = () => {
        if (holds.get(scroller) !== release) return;
        holds.delete(scroller);
        clearTimeout(timer);
        for (const type of USER_SCROLL_EVENTS) doc.removeEventListener(type, release, true);
        if (own) scroller.style.setProperty('overflow-anchor', own);
        else scroller.style.removeProperty('overflow-anchor');
    };
    holds.set(scroller, release);
    for (const type of USER_SCROLL_EVENTS) {
        doc.addEventListener(type, release, { capture: true, passive: true });
    }
    timer = setTimeout(release, SCROLL_HOLD_MS);
}

/**
 * Put @p scroller at @p top: now, and once more on the next frame when that is
 * below the top, for a view whose content is still being laid out when it is
 * mounted (the first attempt is clamped to what is there). Below the top, the
 * position also holds against content that arrives later (holdPosition).
 * @param {HTMLElement | null} scroller
 * @param {number} top
 */
export function restoreScroll(scroller, top) {
    if (!scroller) return;
    if (top > 0) holdPosition(scroller);
    else holds.get(scroller)?.();
    scroller.scrollTop = top;
    if (top > 0) {
        requestAnimationFrame(() => {
            scroller.scrollTop = top;
        });
    }
}
