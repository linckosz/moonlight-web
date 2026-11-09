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
 * PairApproveDialog — a device asks in by the QR code it shows.
 *
 * The Android TV app shows a QR code and a short code beside its PIN pad. The
 * administrator scans the QR code and lands on /pair-<request> (through the
 * rendezvous, the bootstrap keeps that path in `#p=`): this question, over the
 * host list. It names the device and asks for the code the device shows, typed
 * from its screen; the host compares it with the request's
 * (backend/src/server/PhonePairing.h). Yes signs that code with this browser's
 * own pairing key (BackendClient.approvePairRequest); the device then pairs by
 * itself, with the session a PIN would have opened.
 *
 * The administrator answers, and from the server's own network any paired
 * browser with its key: anyone else is told so, and nothing more.
 *
 * A request from outside this server's network says so under its first line,
 * with the address it comes from and, while the host may look one up
 * (`session_location_enabled`), its city.
 *
 * The device sends its half of the code a moment after showing it. Until the
 * host has it, the question waits, a few seconds at most.
 */

import { BackendClient } from '../api/BackendClient.js';
import { t } from '../i18n/i18n.js';
import { escapeHtml } from '../util/escapeHtml.js';

/** The page a pairing link opens: `/pair-<request>`, the id as the host draws it. */
const PAIR_PATH = /^\/pair-([A-Za-z0-9_-]{16,22})$/;

/** How often, and how many times, the question looks for the device's half of the code. */
const WAIT_STEP_MS = 1000;
const WAIT_STEPS = 15;

/** The code a device shows: four characters of Crockford's base32. */
const CODE_LENGTH = 4;

/** How many more looks, a step apart, the question takes for the city of a request from elsewhere. */
const CITY_STEPS = 5;

/** An address as the host names one (IPv4 or IPv6); a line ICE has not named yet has a label instead. */
const IP_ADDRESS = /^(?=.*[.:])[0-9A-Fa-f.:]+$/;

/**
 * Where a request from outside this server's network comes from, as the
 * question says it under its first line: its address, and its city once the
 * host knows it (only while it may look one up: `session_location_enabled`).
 */
export function whereFrom(request) {
    const address = IP_ADDRESS.test(request?.address || '') ? request.address : '';
    return [address, request?.city || ''].filter(Boolean).join(', ');
}

/** The request a pairing link names, or null. */
export function pairRequestFromPath(pathname) {
    const match = PAIR_PATH.exec(pathname || '');
    return match ? match[1] : null;
}

/**
 * A code as a person types it, read as the host reads it
 * (PhonePairing::normalizeCode): spaces and dashes dropped, upper case, O as 0,
 * I and L as 1.
 */
export function normalizeCode(typed) {
    return String(typed || '')
        .replace(/[\s-]/g, '')
        .toUpperCase()
        .replace(/O/g, '0')
        .replace(/[IL]/g, '1');
}

/** What the host's refusal says to the person. */
function problemText(err) {
    const status = err?.statusCode || 0;
    const error = err?.responseBody?.error || '';
    if (status === 404 || status === 410) return t('devicePairing.gone');
    if (error === 'not_allowed') return t('devicePairing.notAllowed');
    if (error === 'temporary_session') return t('devicePairing.temporary');
    if (error === 'bad_signature') return t('devicePairing.noKey');
    if (error === 'too_many_tries') return t('devicePairing.tooManyTries');
    if (error === 'admin_locked') return t('devicePairing.locked');
    if (error === 'decided') return t('devicePairing.decided');
    return t('devicePairing.failed', { message: err?.message || '' });
}

/**
 * Ask whether the device behind request @p id may come in.
 *
 * @param {string} id
 * @param {{ stepMs?: number }} [opts]  the wait between two looks for the device's code
 * @returns {Promise<'approved'|'refused'|'closed'>} once the dialog is closed
 */
export function openPairApprove(id, opts = {}) {
    const stepMs = opts.stepMs ?? WAIT_STEP_MS;
    return new Promise((resolve) => {
        const el = document.createElement('div');
        el.className = 'share-popin-overlay pair-approve-overlay';
        el.innerHTML = `
            <div class="share-popin pair-approve" role="dialog" aria-modal="true" aria-labelledby="pair-approve-title">
                <h3 id="pair-approve-title">${escapeHtml(t('devicePairing.title'))}</h3>
                <div class="pair-approve-body"><p>${escapeHtml(t('devicePairing.checking'))}</p></div>
                <div class="share-popin-actions"></div>
            </div>
        `;
        const body = /** @type {HTMLElement} */ (el.querySelector('.pair-approve-body'));
        const actions = /** @type {HTMLElement} */ (el.querySelector('.share-popin-actions'));
        let outcome = /** @type {'approved'|'refused'|'closed'} */ ('closed');
        let closed = false;
        // While the question is asked, only its own buttons answer it: a tap beside
        // the box must not leave a device waiting for an answer that never comes.
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
        const nameOf = (request) => request.machine_name || t('devicePairing.unnamed');
        const field = () => /** @type {HTMLInputElement} */ (body.querySelector('[data-code]'));
        const typedInFull = () => normalizeCode(field().value).length === CODE_LENGTH;
        const setButtons = (enabled) => {
            for (const b of actions.querySelectorAll('button')) b.disabled = !enabled;
            const approve = /** @type {HTMLButtonElement} */ (
                actions.querySelector('[data-approve]')
            );
            if (approve && enabled) approve.disabled = !typedInFull();
        };

        // Not the device's code: the question stays, emptied, with what is left.
        const wrongCode = (triesLeft) => {
            const line = /** @type {HTMLElement} */ (body.querySelector('[data-error]'));
            line.textContent = t('devicePairing.wrongCode', { count: triesLeft });
            line.hidden = false;
            field().value = '';
            setButtons(true);
            field().focus();
        };

        const answer = async (request, yes) => {
            const code = yes ? normalizeCode(field().value) : '';
            setButtons(false);
            try {
                if (yes) await BackendClient.approvePairRequest(id, code, request.host_id);
                else await BackendClient.refusePairRequest(id, request.host_id);
                outcome = yes ? 'approved' : 'refused';
                closeWith(
                    yes
                        ? t('devicePairing.approved', { name: nameOf(request) })
                        : t('devicePairing.refused'),
                );
            } catch (err) {
                if (yes && err?.responseBody?.error === 'code_mismatch') {
                    wrongCode(err.responseBody.tries_left);
                    return;
                }
                closeWith(problemText(err));
            }
        };

        // A request from elsewhere says so under the first line, with where it
        // comes from; nothing more for one from this server's network.
        const fromElsewhere = (request) => {
            if (request.origin !== 'internet') return '';
            const where = whereFrom(request);
            return `
                <div class="pair-approve-from">
                    <p class="pair-approve-origin">${escapeHtml(t('devicePairing.fromElsewhere'))}</p>
                    <p class="pair-approve-where" data-where ${where ? '' : 'hidden'}>${escapeHtml(where)}</p>
                </div>`;
        };

        // The host looks the city up at the first look (only while it may), so
        // it comes a moment later: a few more looks fill it in, and change
        // nothing else in the question.
        const fillCity = async (request) => {
            if (request.origin !== 'internet' || request.city) return;
            for (let i = 0; i < CITY_STEPS; i++) {
                await new Promise((r) => setTimeout(r, stepMs));
                const line = /** @type {HTMLElement|null} */ (body.querySelector('[data-where]'));
                if (closed || !line?.isConnected) return;
                let again;
                try {
                    again = await BackendClient.getPairRequest(id);
                } catch {
                    return;
                }
                if (again.state !== 'waiting') return;
                if (again.city) {
                    if (!line.isConnected) return;
                    line.textContent = whereFrom(again);
                    line.hidden = false;
                    return;
                }
            }
        };

        const ask = (request) => {
            dismissable = false;
            const lead = escapeHtml(t('devicePairing.asks', { name: nameOf(request) }));
            body.innerHTML = `
                <p class="pair-approve-lead">${lead}</p>${fromElsewhere(request)}
                <label class="pair-approve-label" for="pair-approve-code">${escapeHtml(t('devicePairing.typeCode'))}</label>
                <input id="pair-approve-code" class="share-pin pair-approve-code" type="text" maxlength="8"
                    autocomplete="off" autocapitalize="characters" spellcheck="false" data-code data-nav-initial>
                <p class="pair-approve-error" data-error hidden></p>
                <p class="pair-approve-hint">${escapeHtml(t('devicePairing.hint'))}</p>
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
                answer(request, true);
            });
            approve.addEventListener('click', () => answer(request, true));
            refuse.addEventListener('click', () => answer(request, false));
            field().focus();
            fillCity(request);
        };

        const look = async () => {
            for (let i = 0; i < WAIT_STEPS && !closed; i++) {
                let request;
                try {
                    request = await BackendClient.getPairRequest(id);
                } catch (err) {
                    closeWith(problemText(err));
                    return;
                }
                if (request.state !== 'waiting') {
                    closeWith(t('devicePairing.decided'));
                    return;
                }
                if (request.ready) {
                    ask(request);
                    return;
                }
                if (i === 0) body.innerHTML = `<p>${escapeHtml(t('devicePairing.waiting'))}</p>`;
                await new Promise((r) => setTimeout(r, stepMs));
            }
            if (!closed) closeWith(t('devicePairing.deviceSilent'));
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
