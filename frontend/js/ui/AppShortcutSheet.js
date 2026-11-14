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
 * AppShortcutSheet — the sheet a host's ⋮ menu opens to make a link straight to
 * one of its apps (issue #63): the app or display to open, picked in a list, and
 * on a phone or a tablet the name its home-screen icon will take. What it picks
 * goes the way the card's link button used to (app.js _shareAppLink): a link
 * copied on a computer, the page made into the icon on a touch screen.
 */

import { t } from '../i18n/i18n.js';
import { escapeHtml } from '../util/escapeHtml.js';

/** What this PC's own host card adds to the machine's name (NativeHostBackend). */
const OWN_HOST_SUFFIX = ' — MoonlightWeb Host';

/** The machine's name as a person says it: without what this PC's card adds. */
function machineName(host) {
    const name = String((host && host.displayName) || '').trim();
    return name.endsWith(OWN_HOST_SUFFIX) ? name.slice(0, -OWN_HOST_SUFFIX.length).trim() : name;
}

/**
 * The name of a link's home-screen icon: the machine, then the app, then
 * MoonlightWeb — "DualRTX · Virtual Display · MoonlightWeb". The phone shows
 * its first letters under the icon, so the machine comes first; an app named
 * after MoonlightWeb drops it there, since it ends the name anyway.
 *
 * @param {{ displayName?: string }} host
 * @param {{ displayName?: string }} app
 * @returns {string}
 */
export function appIconName(host, app) {
    const full = String((app && app.displayName) || '').trim();
    const shortApp = full.replace(/^MoonlightWeb\s+/i, '').trim() || full;
    return [machineName(host), shortApp, 'MoonlightWeb'].filter(Boolean).join(' · ');
}

/**
 * Asks which app of @p host to link to.
 *
 * @param {{ host: object, apps: Array<{ id: number, displayName: string }>, touch: boolean }} what
 * @returns {Promise<object|null>} the app picked, or null when the sheet was closed
 */
export function pickAppForShortcut({ host, apps, touch }) {
    return new Promise((resolve) => {
        const el = document.createElement('div');
        el.className = 'share-popin-overlay app-shortcut-overlay';
        const title = touch ? t('appLink.menuTouch') : t('appLink.sheetTitle');
        const intro = t(touch ? 'appLink.sheetIntroTouch' : 'appLink.sheetIntroDesktop', {
            host: machineName(host),
        });
        const options = apps
            .map(
                (a) =>
                    `<option value="${escapeHtml(String(a.id))}">${escapeHtml(a.displayName)}</option>`,
            )
            .join('');
        el.innerHTML = `
            <div class="share-popin app-shortcut-sheet" role="dialog" aria-modal="true"
                 aria-labelledby="app-shortcut-title">
                <p id="app-shortcut-title"><strong>${escapeHtml(title)}</strong></p>
                <p>${escapeHtml(intro)}</p>
                <label for="app-shortcut-pick">${escapeHtml(t('appLink.sheetPick'))}</label>
                <select id="app-shortcut-pick">${options}</select>
                ${
                    touch
                        ? `<div class="app-shortcut-name">${escapeHtml(t('appLink.sheetName'))}<strong data-name></strong></div>`
                        : ''
                }
                <div class="share-popin-actions">
                    <button type="button" class="btn btn-secondary" data-answer="no">${escapeHtml(t('common.cancel'))}</button>
                    <button type="button" class="btn" data-answer="yes">${escapeHtml(
                        t(touch ? 'appLink.sheetCreate' : 'appLink.sheetCopy'),
                    )}</button>
                </div>
            </div>
        `;
        const select = /** @type {HTMLSelectElement} */ (el.querySelector('select'));
        const picked = () => apps.find((a) => String(a.id) === select.value) || null;
        const nameEl = el.querySelector('[data-name]');
        const showName = () => {
            if (nameEl) nameEl.textContent = appIconName(host, picked());
        };
        showName();
        select.addEventListener('change', showName);

        const done = (app) => {
            document.removeEventListener('keydown', onKey, true);
            el.remove();
            resolve(app);
        };
        const onKey = (e) => {
            if (e.key !== 'Escape') return;
            e.preventDefault();
            e.stopPropagation();
            done(null);
        };
        el.addEventListener('click', (e) => {
            const target = /** @type {Element} */ (e.target);
            if (target === el) {
                done(null);
                return;
            }
            const btn = target.closest('[data-answer]');
            if (btn) done(btn.getAttribute('data-answer') === 'yes' ? picked() : null);
        });
        document.addEventListener('keydown', onKey, true);
        document.body.appendChild(el);
        select.focus();
    });
}
