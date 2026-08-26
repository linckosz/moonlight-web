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
 * MoonlightWeb — statistics consent bar.
 *
 * A strip at the bottom of the page, shown once, asking whether this machine
 * may report anonymous statistics about its streams. Until it is answered the
 * backend reports nothing at all — silence is not consent.
 *
 * Three properties it must keep:
 *
 *   - It is NOT a cookie wall. Nothing behind it is blocked, no button is
 *     required, and the session cookie the application needs to work is
 *     unaffected by either answer. Neither is the separate Internet Access
 *     agreement, which asks its own question about a different thing.
 *   - "No" costs the user nothing. Streaming, updates and every feature behave
 *     identically; only the counting stops.
 *   - Only the machine's owner is asked. It is a decision about what THIS
 *     server sends, so a remote viewer never sees it, and the backend refuses
 *     the answer from anyone but a host-local session.
 *
 * The exact wording displayed is sent back with the answer and stored beside
 * it: a consent record that does not say what was agreed to is worth nothing.
 */
import { BackendClient } from '../api/BackendClient.js';
import { Toast } from './Toast.js';
import { t } from '../i18n/i18n.js';
import { escapeHtml } from '../util/escapeHtml.js';

/** Bullet keys, in display order. Kept in one place so the visible text and the
 *  stored consent record cannot drift apart. */
const SENT_KEYS = ['statsSent1', 'statsSent2', 'statsSent3', 'statsSent4'];
const NEVER_KEYS = ['statsNever1', 'statsNever2', 'statsNever3'];

export class ConsentBar {
    /**
     * Ask, unless there is nothing to ask about.
     *
     * Silent when: this browser is not the host's own (someone else's machine
     * is not theirs to answer for), the question was already answered, or the
     * build carries no reporting credentials at all — a self-built binary
     * reports nothing whatever the user says, and a question whose answer
     * changes nothing should not be asked.
     *
     * @param {{ isHostLocal: () => boolean }} deps
     */
    static async maybeShow(deps) {
        if (!deps || !deps.isHostLocal()) return;
        if (document.getElementById('consent-bar')) return;

        let state;
        try {
            state = await BackendClient.getMetricsConsent();
        } catch (err) {
            // A backend that will not answer this is a backend that is not
            // reporting anything either. Nothing to ask, nothing to warn about.
            console.warn('[MW] consent state unavailable:', err);
            return;
        }
        if (!state || !state.available || state.decision) return;

        this._render();
    }

    /** The visible text, as one string, for the stored consent record. */
    static _plainText() {
        const lines = [t('stats.title'), t('stats.body'), t('stats.sentIntro')];
        for (const k of SENT_KEYS) lines.push('- ' + t('stats.' + k));
        lines.push(t('stats.neverIntro'));
        for (const k of NEVER_KEYS) lines.push('- ' + t('stats.' + k));
        lines.push(t('stats.necessary'));
        return lines.join('\n');
    }

    static _render() {
        const li = (key) => `<li>${escapeHtml(t('stats.' + key))}</li>`;

        const bar = document.createElement('div');
        bar.className = 'consent-bar';
        bar.id = 'consent-bar';
        // Not an alertdialog: it takes no focus and traps none. A user who
        // came here to start a stream must be able to do exactly that.
        bar.setAttribute('role', 'region');
        bar.setAttribute('aria-label', t('stats.title'));
        bar.innerHTML = `
            <div class="consent-bar-body">
                <strong class="consent-bar-title">${escapeHtml(t('stats.title'))}</strong>
                ${escapeHtml(t('stats.body'))}
                <details class="consent-bar-details">
                    <summary>${escapeHtml(t('stats.detailsSummary'))}</summary>
                    <p>${escapeHtml(t('stats.sentIntro'))}</p>
                    <ul>${SENT_KEYS.map(li).join('')}</ul>
                    <p>${escapeHtml(t('stats.neverIntro'))}</p>
                    <ul>${NEVER_KEYS.map(li).join('')}</ul>
                    <p>${escapeHtml(t('stats.necessary'))}</p>
                </details>
            </div>
            <div class="consent-bar-actions">
                <button type="button" class="btn btn-neutral" id="consent-decline">
                    ${escapeHtml(t('stats.decline'))}
                </button>
                <button type="button" class="btn" id="consent-accept">
                    ${escapeHtml(t('stats.accept'))}
                </button>
            </div>`;
        document.body.appendChild(bar);

        const answer = (granted) => this._answer(bar, granted);
        bar.querySelector('#consent-accept').addEventListener('click', () => answer(true));
        bar.querySelector('#consent-decline').addEventListener('click', () => answer(false));
    }

    /**
     * Record the answer. The bar closes either way and on failure too: a user
     * who has answered must not be asked twice in the same breath, and a
     * backend that did not save the answer is a backend still reporting
     * nothing — the safe side of the failure.
     */
    static async _answer(bar, granted) {
        bar.querySelectorAll('button').forEach((b) => (b.disabled = true));
        try {
            await BackendClient.setMetricsConsent(granted, this._plainText(), 'banner');
            Toast.success(granted ? t('stats.thanks') : t('stats.declined'));
        } catch (err) {
            console.warn('[MW] could not record the statistics answer:', err);
            Toast.error(t('stats.saveFailed'));
        }
        bar.remove();
    }
}
