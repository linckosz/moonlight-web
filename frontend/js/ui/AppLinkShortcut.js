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
 * The banner of a page being made into the home-screen icon of a link straight
 * to one app (issue #63, net/appIcon.js): the app and the host, and where the
 * browser's own button is.
 *
 * No share button of its own: the sheet a page opens (navigator.share) never
 * offers "Add to Home Screen" — only the browser's does — and sending the link
 * elsewhere is what the browser's own sheet does too, the address being the
 * link. Done puts the page back and reloads it; a touch anywhere outside the
 * banner puts it back without a reload, and whatever was touched still happens.
 */

import { t } from '../i18n/i18n.js';
import { escapeHtml } from '../util/escapeHtml.js';

let current = null;

/**
 * Shows the banner. One at a time: a second one first closes the first, which
 * puts its page back.
 *
 * @param {{ title: string, hint: string, onClose: (how: { reload: boolean }) => void }} what
 */
export function showAppLinkShortcut({ title, hint, onClose }) {
    closeAppLinkShortcut({ reload: false });

    const el = document.createElement('div');
    el.className = 'app-link-banner';
    el.setAttribute('role', 'status');
    el.innerHTML = `
        <div class="app-link-banner-text">
            <strong>${escapeHtml(title)}</strong>
            <span>${escapeHtml(hint)}</span>
        </div>
        <div class="app-link-banner-actions">
            <button type="button" class="btn" data-act="done">${escapeHtml(t('appLink.done'))}</button>
        </div>
    `;
    el.addEventListener('click', (e) => {
        const target = /** @type {Element} */ (e.target);
        if (target.closest && target.closest('[data-act="done"]'))
            closeAppLinkShortcut({ reload: true });
    });
    // Captured, and left to go on: the banner goes, what was touched happens.
    const outside = (e) => {
        if (!el.contains(/** @type {Node} */ (e.target))) closeAppLinkShortcut({ reload: false });
    };
    document.addEventListener('pointerdown', outside, true);
    document.body.appendChild(el);
    current = { el, onClose, outside };
}

/**
 * Closes the banner, if one is up, and puts the page back as it was; reloaded
 * too when @p reload (Done).
 */
export function closeAppLinkShortcut({ reload = false } = {}) {
    if (!current) return;
    const { el, onClose, outside } = current;
    current = null;
    document.removeEventListener('pointerdown', outside, true);
    el.remove();
    try {
        const done = onClose({ reload });
        if (done && typeof done.catch === 'function') {
            done.catch((err) =>
                console.warn('[MW] Page not put back after a home-screen link:', err),
            );
        }
    } catch (err) {
        console.warn('[MW] Page not put back after a home-screen link:', err);
    }
}
