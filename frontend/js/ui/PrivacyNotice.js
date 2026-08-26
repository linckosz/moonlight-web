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
 * MoonlightWeb — the one description of what the census covers.
 *
 * Four places show it: the first-launch bar, the Settings page, the guest's
 * join page, and the consent record stored on disk. They must say the same
 * thing — a record that does not match what was on screen is worthless, and a
 * disclosure that drifts between two pages is worse than one page having none.
 * So the bullet list lives here once, and every caller renders it from these
 * keys rather than from its own copy.
 */
import { t } from '../i18n/i18n.js';
import { escapeHtml } from '../util/escapeHtml.js';

/** Bullet keys, in display order. */
export const SENT_KEYS = ['statsSent1', 'statsSent2', 'statsSent3', 'statsSent4'];
export const NEVER_KEYS = ['statsNever1', 'statsNever2', 'statsNever3'];

/**
 * The two lists, as HTML. `necessary` is the sentence that keeps this from
 * being read as a cookie notice, so it is part of the block and not optional.
 */
export function noticeHtml() {
    const li = (key) => `<li>${escapeHtml(t('stats.' + key))}</li>`;
    return `
        <p>${escapeHtml(t('stats.sentIntro'))}</p>
        <ul>${SENT_KEYS.map(li).join('')}</ul>
        <p>${escapeHtml(t('stats.neverIntro'))}</p>
        <ul>${NEVER_KEYS.map(li).join('')}</ul>
        <p>${escapeHtml(t('stats.necessary'))}</p>`;
}

/** The same thing folded shut, for a page that has other business first. */
export function noticeDetailsHtml() {
    return `
        <details class="privacy-details">
            <summary>${escapeHtml(t('stats.detailsSummary'))}</summary>
            ${noticeHtml()}
        </details>`;
}

/**
 * The visible text as one plain string, stored with the answer. Built from the
 * same keys the reader saw, in the language they read it in.
 *
 * @param {string[]} leadKeys the caller's own headline/description keys, which
 *   differ between the bar ("title", "body") and the Settings switch.
 */
export function plainText(leadKeys) {
    const lines = leadKeys.map((k) => t('stats.' + k));
    lines.push(t('stats.sentIntro'));
    for (const k of SENT_KEYS) lines.push('- ' + t('stats.' + k));
    lines.push(t('stats.neverIntro'));
    for (const k of NEVER_KEYS) lines.push('- ' + t('stats.' + k));
    lines.push(t('stats.necessary'));
    return lines.join('\n');
}
