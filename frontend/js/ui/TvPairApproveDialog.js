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
 * TvPairApproveDialog — a TV on plain HTTP asks to join (issue #42).
 *
 * The TV's QR code opens /tv-pair-<request> over HTTPS (or through the
 * rendezvous, which keeps that path). This question, over the host list, names
 * the TV by its address, says in its own line that it uses plain HTTP and can
 * stream but never administer, and asks for the code the TV shows. The server
 * compares it and, once it matches, the TV signs itself in. Only the
 * administrator may answer (an admin session, or this server's own computer);
 * anyone else is told so.
 *
 * It looks like PairApproveDialog and shares its styles, but answers on the
 * plain-HTTP routes (/api/lan-http/approve/<id>, /api/lan-http/refuse/<id>).
 */

import { lanHttpApprove, lanHttpRefuse, lanHttpRequest } from '../api/LanHttpApi.js';
import { t } from '../i18n/i18n.js';
import { escapeHtml } from '../util/escapeHtml.js';
import { normalizeCode } from './PairApproveDialog.js';

/** The page a TV's QR code opens: `/tv-pair-<request>`, the id as the host draws it. */
const TV_PAIR_PATH = /^\/tv-pair-([A-Za-z0-9_-]{16,22})$/;

/** The code a TV shows: six characters of Crockford's base32. */
const CODE_LENGTH = 6;

/** The request a TV's QR code names, or null for any other page. */
export function tvPairRequestFromPath(pathname) {
    const match = TV_PAIR_PATH.exec(pathname || '');
    return match ? match[1] : null;
}

/** What the host's refusal says to the person. */
function problemText(err) {
    const status = err?.statusCode || 0;
    const error = err?.responseBody?.error || '';
    if (status === 404 || error === 'gone') return t('tvPairing.gone');
    if (error === 'not_allowed' || status === 403) return t('tvPairing.notAllowed');
    if (error === 'admin_locked') return t('devicePairing.locked');
    return t('devicePairing.failed', { message: err?.message || '' });
}

/**
 * Ask whether the TV behind request @p id may come in.
 *
 * @param {string} id
 * @returns {Promise<'approved'|'refused'|'closed'>} once the dialog is closed
 */
export function openTvPairApprove(id) {
    return new Promise((resolve) => {
        const el = document.createElement('div');
        el.className = 'share-popin-overlay pair-approve-overlay';
        el.innerHTML = `
            <div class="share-popin pair-approve" role="dialog" aria-modal="true" aria-labelledby="tv-pair-approve-title">
                <h3 id="tv-pair-approve-title">${escapeHtml(t('devicePairing.title'))}</h3>
                <div class="pair-approve-body"><p>${escapeHtml(t('devicePairing.checking'))}</p></div>
                <div class="share-popin-actions"></div>
            </div>
        `;
        const body = /** @type {HTMLElement} */ (el.querySelector('.pair-approve-body'));
        const actions = /** @type {HTMLElement} */ (el.querySelector('.share-popin-actions'));
        let outcome = /** @type {'approved'|'refused'|'closed'} */ ('closed');
        let closed = false;
        // While the question is asked, only its own buttons answer it.
        let dismissable = true;

        const done = () => {
            if (closed) return;
            closed = true;
            document.removeEventListener('keydown', onKey, true);
            el.remove();
            resolve(outcome);
        };
        const onKey = (e) => {
            if (e.key !== 'Escape' || !dismissable) return;
            e.preventDefault();
            e.stopPropagation();
            done();
        };
        const closeWith = (text) => {
            dismissable = true;
            body.innerHTML = `<p>${escapeHtml(text)}</p>`;
            const close = escapeHtml(t('common.close'));
            actions.innerHTML = `<button type="button" class="btn" data-close data-nav-initial>${close}</button>`;
            /** @type {HTMLElement|null} */ (actions.querySelector('[data-close]'))?.focus();
        };
        const field = () => /** @type {HTMLInputElement} */ (body.querySelector('[data-code]'));
        const typedInFull = () => normalizeCode(field().value).length === CODE_LENGTH;
        const setButtons = (enabled) => {
            for (const b of actions.querySelectorAll('button')) b.disabled = !enabled;
            const approve = /** @type {HTMLButtonElement} */ (
                actions.querySelector('[data-approve]')
            );
            if (approve && enabled) approve.disabled = !typedInFull();
        };

        const answer = async (yes) => {
            const code = yes ? normalizeCode(field().value) : '';
            setButtons(false);
            try {
                if (yes) await lanHttpApprove(id, code);
                else await lanHttpRefuse(id);
                outcome = yes ? 'approved' : 'refused';
                closeWith(yes ? t('tvPairing.approved') : t('devicePairing.refused'));
            } catch (err) {
                if (yes && err?.responseBody?.error === 'wrong_code') {
                    // The question stays, emptied: the TV still shows its code.
                    const line = /** @type {HTMLElement} */ (body.querySelector('[data-error]'));
                    line.textContent = t('tvPairing.wrongCode');
                    line.hidden = false;
                    field().value = '';
                    setButtons(true);
                    field().focus();
                    return;
                }
                closeWith(problemText(err));
            }
        };

        const ask = (request) => {
            dismissable = false;
            const who = t('tvPairing.browserAt', { address: request.address || '' });
            body.innerHTML = `
                <p class="pair-approve-lead">${escapeHtml(t('devicePairing.asks', { name: who }))}</p>
                <p class="tv-pair-plain-http">${escapeHtml(t('tvPairing.plainHttp'))}</p>
                <label class="pair-approve-label" for="tv-pair-approve-code">${escapeHtml(t('devicePairing.typeCode'))}</label>
                <input id="tv-pair-approve-code" class="share-pin pair-approve-code" type="text" maxlength="10"
                    autocomplete="off" autocapitalize="characters" spellcheck="false" data-code data-nav-initial>
                <p class="pair-approve-error" data-error hidden></p>
                <p class="pair-approve-hint">${escapeHtml(t('tvPairing.hint'))}</p>
            `;
            actions.innerHTML = `
                <button type="button" class="btn btn-secondary" data-refuse>${escapeHtml(t('devicePairing.refuse'))}</button>
                <button type="button" class="btn" data-approve disabled>${escapeHtml(t('devicePairing.approve'))}</button>
            `;
            const approve = /** @type {HTMLButtonElement} */ (
                actions.querySelector('[data-approve]')
            );
            const refuse = /** @type {HTMLElement} */ (actions.querySelector('[data-refuse]'));
            field().addEventListener('input', () => {
                approve.disabled = !typedInFull();
            });
            field().addEventListener('keydown', (e) => {
                if (e.key !== 'Enter' || approve.disabled) return;
                e.preventDefault();
                answer(true);
            });
            approve.addEventListener('click', () => answer(true));
            refuse.addEventListener('click', () => answer(false));
            field().focus();
        };

        const look = async () => {
            try {
                ask(await lanHttpRequest(id));
            } catch (err) {
                closeWith(problemText(err));
            }
        };

        el.addEventListener('click', (e) => {
            const target = /** @type {Element} */ (e.target);
            if (target.closest('[data-close]') || (target === el && dismissable)) done();
        });
        document.addEventListener('keydown', onKey, true);
        document.body.appendChild(el);
        look();
    });
}
