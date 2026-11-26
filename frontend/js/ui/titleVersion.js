/*
 * MoonlightWeb — browser-based Sunshine/GameStream client.
 * Copyright (C) 2026 Bruno Martin
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

import { escapeHtml } from '../util/escapeHtml.js';

/**
 * The version badge beside a settings page's title (Streaming Settings,
 * Server Settings). On a phone the app header has no room for its own badge
 * (layout.css), so the pages where someone looks for it carry a copy; wider,
 * the header shows it and this one stays hidden (views/settings.css).
 *
 * The same text as the header's, which app.js writes from /api/health: nothing
 * until the server has said which version it runs, never a stale number.
 *
 * @returns {string} the badge's markup, or '' while the version is unknown
 */
export function titleVersionHtml() {
    const header = document.querySelector('.app-header .version');
    const version = header && !header.hidden ? (header.textContent || '').trim() : '';
    return version ? ` <span class="version title-version">${escapeHtml(version)}</span>` : '';
}
