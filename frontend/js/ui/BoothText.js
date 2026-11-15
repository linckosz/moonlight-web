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
 * What a booth whose streams have PINs (booth_stream_pin, issue #68) says in
 * words: a PIN the server refused, and why a stream was taken over. Plain
 * strings, never markup: whoever shows them escapes them, since a machine's
 * name comes from somebody else's device.
 */

import { t } from '../i18n/i18n.js';

/** The longest machine name a notice shows (TakeoverNotice::kMaxNameLength). */
const MAX_NAME = 64;

/**
 * What to tell the person who entered a PIN the server refused, and how long
 * the prompt stays shut.
 * @param {any} err a BackendError (statusCode, message, responseBody)
 * @returns {{text: string, lockoutSeconds: number}}
 */
export function boothErrorText(err) {
    const body = (err && err.responseBody) || {};
    const code = body.error || (err && err.message) || '';
    const lockout = Number(body.lockout_seconds) > 0 ? Math.ceil(Number(body.lockout_seconds)) : 0;
    switch (code) {
        case 'booth_pin_invalid': {
            if (lockout > 0)
                return { text: t('booth.tooManyTries', { s: lockout }), lockoutSeconds: lockout };
            const n = Number(body.remaining);
            return {
                text: n === 1 ? t('booth.wrongPinOne') : t('booth.wrongPin', { n: n > 0 ? n : 0 }),
                lockoutSeconds: 0,
            };
        }
        case 'rate_limited':
            return {
                text: t('booth.tooManyTries', { s: lockout || 30 }),
                lockoutSeconds: lockout || 30,
            };
        case 'room_full':
            return { text: t('booth.roomFull'), lockoutSeconds: 0 };
        case 'no_stream':
            return { text: t('booth.streamEnded'), lockoutSeconds: 0 };
        case 'pairing_key_required':
        case 'owner_proof_required':
        case 'owner_proof_refused':
            return { text: t('booth.needsPairing'), lockoutSeconds: 0 };
        case 'booth_owner_only':
            return { text: t('booth.ownerOnly'), lockoutSeconds: 0 };
        default:
            return { text: (err && err.message) || t('launch.failed'), lockoutSeconds: 0 };
    }
}

/**
 * Why a stream ended, as its take-over notice tells it: with the PIN, or
 * because nobody used it for the idle time, and by which machine. The plain
 * take-over text for a notice that says nothing more.
 * @param {any} notice the exit notice ({type: 'takeover', reason?, by?, idle_minutes?})
 * @returns {string}
 */
export function boothTakeoverBody(notice) {
    const reason = notice && notice.reason;
    const by = notice && typeof notice.by === 'string' ? notice.by.trim().slice(0, MAX_NAME) : '';
    const name = by || t('booth.someone');
    if (reason === 'pin') return t('booth.takenOverPin', { name });
    const minutes = notice && notice.idle_minutes;
    if (reason === 'idle' && Number.isInteger(minutes) && minutes > 0)
        return t('booth.takenOverIdle', { name, minutes });
    return t('stream.takenOverBody');
}
