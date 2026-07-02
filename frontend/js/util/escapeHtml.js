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
 * Escape a value for safe interpolation into an HTML template literal.
 *
 * Unlike the old per-view `div.textContent` trick, this also escapes the quote
 * characters (`"` and `'`). Templates interpolate values inside quoted
 * attributes (`title="${esc(x)}"`, `src="${esc(url)}"`, `data-token="${esc(t)}"`),
 * where an unescaped quote lets a semi-trusted value (Sunshine app name, box-art
 * URL, client-supplied machine name) break out of the attribute — an XSS vector.
 *
 * Pure string replacement: no DOM, so it also works in Workers and is trivially
 * testable. Non-string/number inputs return "" (matches the old behaviour).
 */
const HTML_ESCAPES = {
    '&': '&amp;',
    '<': '&lt;',
    '>': '&gt;',
    '"': '&quot;',
    "'": '&#39;',
};

export function escapeHtml(text) {
    if (typeof text !== 'string' && typeof text !== 'number') return '';
    return String(text).replace(/[&<>"']/g, (ch) => HTML_ESCAPES[ch]);
}
