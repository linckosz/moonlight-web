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
 * The banner that turns this page into a home-screen icon for one app (issue
 * #63): "Tap Share, then Add to Home Screen".
 *
 * A phone makes its icon from the page it shows — the address, the title, the
 * manifest — and not from a link handed to its share sheet. So the page that
 * asks is made into the link for as long as the banner stays: the caller has
 * already put the link in the address and the manifest, and the title here; the
 * banner says what to tap, offers the system's own share sheet for sending the
 * link elsewhere, and Done puts the page back as it was.
 */

import { t } from '../i18n/i18n.js';
import { escapeHtml } from '../util/escapeHtml.js';

let current = null;

/**
 * Shows the banner. One at a time: a second one first closes the first, which
 * puts its page back.
 *
 * @param {{ title: string, url: string, onDone: () => void }} what
 */
export function showAppLinkShortcut({ title, url, onDone }) {
    closeAppLinkShortcut();

    const el = document.createElement('div');
    el.className = 'app-link-banner';
    el.setAttribute('role', 'status');
    const canShare = typeof navigator.share === 'function';
    el.innerHTML = `
        <div class="app-link-banner-text">
            <strong>${escapeHtml(title)}</strong>
            <span>${escapeHtml(t('appLink.homeScreenHint'))}</span>
        </div>
        <div class="app-link-banner-actions">
            ${canShare ? `<button type="button" class="btn btn-secondary" data-act="share">${escapeHtml(t('appLink.shareNow'))}</button>` : ''}
            <button type="button" class="btn" data-act="done">${escapeHtml(t('appLink.done'))}</button>
        </div>
    `;
    el.addEventListener('click', (e) => {
        const btn = /** @type {Element} */ (e.target).closest('[data-act]');
        if (!btn) return;
        if (btn.getAttribute('data-act') === 'share') {
            navigator.share({ title, url }).catch(() => {
                // Closed without sending: nothing to say.
            });
            return;
        }
        closeAppLinkShortcut();
    });
    document.body.appendChild(el);
    current = { el, onDone };
}

/** Closes the banner, if one is up, and puts the page back as it was. */
export function closeAppLinkShortcut() {
    if (!current) return;
    const { el, onDone } = current;
    current = null;
    el.remove();
    try {
        onDone();
    } catch (err) {
        console.warn('[MW] Page not put back after a home-screen link:', err);
    }
}
