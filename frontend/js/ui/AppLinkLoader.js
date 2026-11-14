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
 * AppLinkLoader — what a link straight to one app shows while it opens
 * (issue #63): the icon of an app opens on that app's launch, not on the list
 * of hosts it goes through (Bruno, 14/11). Over the page, until the stream
 * takes over, or the link ends anywhere else — then the page shows, with its
 * message. Never left up for good: it goes by itself after a while.
 */

import { t } from '../i18n/i18n.js';

/** Longest the loader stays up, whatever happens behind it. */
const LONGEST_MS = 30000;

let el = null;
let timer = null;

/** Shows the loader, naming @p name once it is known. */
export function showAppLinkLoader(name = '') {
    if (!el) {
        el = document.createElement('div');
        el.className = 'app-link-loader';
        el.setAttribute('role', 'status');
        el.setAttribute('aria-live', 'polite');
        el.innerHTML = '<span class="app-link-loader-spin" aria-hidden="true"></span><p></p>';
        document.body.appendChild(el);
        timer = setTimeout(hideAppLinkLoader, LONGEST_MS);
    }
    el.querySelector('p').textContent = name
        ? t('appLink.opening', { name })
        : t('appLink.openingAny');
}

/** Whether the loader is up. */
export function appLinkLoaderShown() {
    return !!el;
}

/** The page shows again. */
export function hideAppLinkLoader() {
    clearTimeout(timer);
    timer = null;
    if (el) el.remove();
    el = null;
}
