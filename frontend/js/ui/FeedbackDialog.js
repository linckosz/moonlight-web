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
 * "Send feedback" (Admin → Feedback, owner only): a message, an optional
 * e-mail address and, if the box is ticked, the version, edition and OS
 * family, which the server passes to the moderators' private Discord channel.
 *
 * The box shows exactly what would leave, as the server reported it
 * (GET /api/feedback). What the owner types leaves through `send` alone: it is
 * never written to the console, a log or storage, and goes with the dialog.
 * The hidden "website" field is the bots' trap: a person never sees it, so
 * the server drops whatever arrives with it filled.
 */

import { t } from '../i18n/i18n.js';

/** Below this many characters (trimmed), Send stays off: the server's own floor. */
export const MIN_MESSAGE = 10;
const MAX_EMAIL = 254;

/**
 * The words the dialog says when the server did not send it: too many
 * messages within the hour, otherwise "unavailable". A refusal never names
 * what was typed.
 * @param {unknown} err
 */
export function feedbackErrorText(err) {
    const status = err && /** @type {any} */ (err).statusCode;
    return status === 429 ? t('feedback.rateLimited') : t('feedback.unavailable');
}

/**
 * What the box would attach, in the order it leaves: "0.4.4, release, windows".
 * @param {{version?: string, edition?: string, os?: string}|null|undefined} a
 */
export function attachmentText(a) {
    if (!a) return '';
    return [a.version, a.edition, a.os].filter((v) => typeof v === 'string' && v).join(', ');
}

/**
 * @param {object} o
 * @param {number} o.maxMessage  the server's bound, in UTF-16 units (what maxlength counts)
 * @param {{version?: string, edition?: string, os?: string}|null} o.attachment
 * @param {(body: {message: string, email: string, include_version: boolean, website: string}) => Promise<unknown>} o.send
 * @param {() => void} [o.onSent]
 * @returns {{ el: HTMLElement, close: () => void }}
 */
export function showFeedbackDialog(o) {
    const overlay = document.createElement('div');
    overlay.className = 'pairing-overlay feedback-overlay';
    const dialog = document.createElement('div');
    dialog.className = 'pairing-dialog backend-dialog feedback-dialog';
    dialog.setAttribute('role', 'dialog');
    dialog.setAttribute('aria-modal', 'true');
    dialog.setAttribute('aria-labelledby', 'feedback-title');

    const title = document.createElement('h3');
    title.id = 'feedback-title';
    title.textContent = t('feedback.dialogTitle');

    const form = document.createElement('form');
    form.setAttribute('autocomplete', 'off');
    form.noValidate = true;

    const messageField = document.createElement('label');
    messageField.className = 'backend-field feedback-field';
    const messageLabel = document.createElement('span');
    messageLabel.textContent = t('feedback.message') + ' *';
    const message = document.createElement('textarea');
    message.id = 'feedback-message';
    message.rows = 6;
    message.maxLength = o.maxMessage;
    message.required = true;
    const count = document.createElement('span');
    count.className = 'feedback-count';
    messageField.append(messageLabel, message, count);

    const emailField = document.createElement('label');
    emailField.className = 'backend-field feedback-field';
    const emailLabel = document.createElement('span');
    emailLabel.textContent = t('feedback.email');
    const email = document.createElement('input');
    email.id = 'feedback-email';
    email.type = 'email';
    email.maxLength = MAX_EMAIL;
    email.setAttribute('autocomplete', 'email');
    emailField.append(emailLabel, email);

    // The trap: out of sight, out of the tab order, skipped by screen readers.
    const trap = document.createElement('div');
    trap.className = 'feedback-trap';
    trap.setAttribute('aria-hidden', 'true');
    const website = document.createElement('input');
    website.type = 'text';
    website.name = 'website';
    website.tabIndex = -1;
    website.setAttribute('autocomplete', 'off');
    trap.append(website);

    // No box at all when the server reported nothing to attach.
    const details = attachmentText(o.attachment);
    const include = document.createElement('input');
    include.type = 'checkbox';
    include.id = 'feedback-include-version';
    include.checked = !!details;
    const includeRow = document.createElement('label');
    includeRow.className = 'settings-checkbox-label feedback-include';
    const includeText = document.createElement('span');
    includeText.className = 'settings-checkbox-text';
    includeText.textContent = t('feedback.includeVersion', { details });
    includeRow.append(include, includeText);

    const notice = document.createElement('p');
    notice.className = 'settings-hint feedback-notice';
    notice.textContent = t('feedback.notice');

    const status = document.createElement('p');
    status.className = 'feedback-status';
    status.setAttribute('role', 'status');

    const actions = document.createElement('div');
    actions.className = 'pairing-actions';
    const cancelBtn = document.createElement('button');
    cancelBtn.type = 'button';
    cancelBtn.className = 'btn btn-secondary feedback-cancel';
    cancelBtn.textContent = t('common.cancel');
    const sendBtn = document.createElement('button');
    sendBtn.type = 'submit';
    sendBtn.className = 'btn btn-primary feedback-send';
    sendBtn.textContent = t('feedback.send');
    actions.append(cancelBtn, sendBtn);

    form.append(messageField, emailField, trap);
    if (details) form.append(includeRow);
    form.append(notice, status, actions);
    dialog.append(title, form);
    overlay.append(dialog);

    let busy = false;
    const ready = () =>
        !busy && message.value.trim().length >= MIN_MESSAGE && email.checkValidity();
    const paint = () => {
        count.textContent = `${message.value.length} / ${o.maxMessage}`;
        message.disabled = email.disabled = include.disabled = busy;
        cancelBtn.disabled = busy;
        sendBtn.disabled = !ready();
    };
    const say = (text, isError) => {
        status.textContent = text || '';
        status.classList.toggle('err', !!(text && isError));
    };

    const onKey = (e) => {
        if (e.key === 'Escape' && !busy) close();
    };
    const close = () => {
        document.removeEventListener('keydown', onKey);
        overlay.remove();
    };

    const submit = async () => {
        if (!ready()) return;
        busy = true;
        say(t('feedback.sending'), false);
        paint();
        try {
            await o.send({
                message: message.value,
                email: email.value.trim(),
                include_version: !!details && include.checked,
                website: website.value,
            });
            close();
            if (o.onSent) o.onSent();
        } catch (err) {
            busy = false;
            say(feedbackErrorText(err), true);
            paint();
        }
    };

    message.addEventListener('input', () => {
        if (status.classList.contains('err')) say('', false);
        paint();
    });
    email.addEventListener('input', paint);
    form.addEventListener('submit', (e) => {
        e.preventDefault();
        void submit();
    });
    cancelBtn.addEventListener('click', () => {
        if (!busy) close();
    });
    overlay.addEventListener('click', (e) => {
        if (e.target === overlay && !busy) close();
    });
    document.addEventListener('keydown', onKey);

    document.body.appendChild(overlay);
    paint();
    message.focus();
    return { el: overlay, close };
}
