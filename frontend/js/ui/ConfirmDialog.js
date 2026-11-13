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
 * ConfirmDialog — an "are you sure?" that a TV browser does not swallow.
 *
 * TV Bro blocks window.confirm(): no dialog shows, a "Popup dialog blocked"
 * notice does, and confirm() answers false within a few milliseconds
 * (measured 01/10/2026). Quitting a running app, stopping a session or
 * logging out could then never be done from a TV. On a TV (RemoteNav) the
 * question is asked in a dialog of the page's own, Cancel focused — the
 * remote's OK must not end anything by accident; everywhere else it is still
 * the browser's own confirm, unchanged.
 */

import { t } from '../i18n/i18n.js';
import { escapeHtml } from '../util/escapeHtml.js';
import * as RemoteNav from './RemoteNav.js';

/**
 * @param {string} message
 * @param {{ confirmLabel?: string, danger?: boolean, inPage?: boolean }} [opts]
 * @returns {Promise<boolean>} whether the user confirmed
 */
export function confirmAction(message, opts = {}) {
    // `inPage` asks in the page's own dialog everywhere: where the answer's
    // words matter ("Launch Bureau" rather than OK), which the browser's own
    // confirm cannot say.
    if (!RemoteNav.isActive() && !opts.inPage) return Promise.resolve(window.confirm(message));
    return new Promise((resolve) => {
        const el = document.createElement('div');
        el.className = 'share-popin-overlay mw-confirm-overlay';
        const label = escapeHtml(opts.confirmLabel || t('common.confirm'));
        el.innerHTML = `
            <div class="share-popin" role="alertdialog" aria-modal="true">
                <p>${escapeHtml(message)}</p>
                <div class="share-popin-actions">
                    <button type="button" class="btn btn-secondary" data-answer="no" data-nav-initial>${escapeHtml(t('common.cancel'))}</button>
                    <button type="button" class="btn${opts.danger ? ' btn-danger' : ''}" data-answer="yes">${label}</button>
                </div>
            </div>
        `;
        const done = (answer) => {
            document.removeEventListener('keydown', onKey, true);
            el.remove();
            resolve(answer);
        };
        const onKey = (e) => {
            if (e.key !== 'Escape') return;
            e.preventDefault();
            e.stopPropagation();
            done(false);
        };
        el.addEventListener('click', (e) => {
            const target = /** @type {Element} */ (e.target);
            if (target === el) {
                done(false);
                return;
            }
            const btn = target.closest('[data-answer]');
            if (btn) done(btn.getAttribute('data-answer') === 'yes');
        });
        document.addEventListener('keydown', onKey, true);
        document.body.appendChild(el);
    });
}
