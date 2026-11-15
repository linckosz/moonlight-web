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
 * The PIN prompt of a booth whose streams have PINs (booth_stream_pin, issue
 * #68): someone else streams this host, and their PIN, shown at the top of
 * their screen, either joins their stream as a guest or starts this app in its
 * place, which ends theirs. One step: the PIN, then one of the two.
 *
 * What the server refused is said in the prompt itself (BoothText), and a
 * prompt held back for too many tries stays shut until it opens again. The
 * PIN only ever leaves through the two callbacks; nothing here keeps it.
 */

import { t } from '../i18n/i18n.js';
import { boothErrorText } from './BoothText.js';

const DIGITS = 6;

/** The digits typed so far, as the field shows them ("481 902"). */
function shown(digits) {
    return digits.length > 3 ? digits.slice(0, 3) + ' ' + digits.slice(3) : digits;
}

/**
 * @param {object} o
 * @param {string} o.hostName     the host in use
 * @param {string} o.ownerName    the machine streaming it
 * @param {string} o.theirApp     what they stream
 * @param {string} o.myApp        what this page was asked to start
 * @param {(pin: string) => Promise<void>} o.onJoin  joins with the PIN; throws what the server refused
 * @param {(pin: string) => void} o.onStartInstead   starts this app with the PIN; called from the click, the prompt closed first
 * @param {() => void} [o.onCancel]
 * @param {{text: string, lockoutSeconds: number}} [o.initial] a refusal to show at once
 * @returns {{ el: HTMLElement, close: () => void }}
 */
export function showBoothPinDialog(o) {
    const overlay = document.createElement('div');
    overlay.className = 'pairing-overlay booth-prompt-overlay';
    const dialog = document.createElement('div');
    dialog.className = 'pairing-dialog booth-prompt';
    dialog.setAttribute('role', 'dialog');
    dialog.setAttribute('aria-modal', 'true');

    const title = document.createElement('h3');
    title.textContent = t('booth.inUseTitle', { host: o.hostName });

    // "LAPTOP-SAM is playing <b>Elden Ring</b>." then the hint: built from
    // text nodes, the app's name set apart where the sentence has it.
    const body = document.createElement('p');
    body.className = 'pairing-instruction';
    const playing = t('booth.inUsePlaying', { owner: o.ownerName, app: o.theirApp });
    const at = o.theirApp ? playing.lastIndexOf(o.theirApp) : -1;
    if (at >= 0) {
        const b = document.createElement('b');
        b.textContent = o.theirApp;
        body.append(playing.slice(0, at), b, playing.slice(at + o.theirApp.length));
    } else {
        body.append(playing);
    }
    body.append(document.createElement('br'), t('booth.inUseHint'));

    const form = document.createElement('form');
    form.className = 'player-pin-form';
    form.setAttribute('autocomplete', 'off');
    const input = document.createElement('input');
    input.className = 'player-pin-input';
    input.type = 'text';
    input.inputMode = 'numeric';
    input.maxLength = DIGITS + 1;
    input.placeholder = '______';
    input.setAttribute('aria-label', t('booth.pinTitle'));
    form.append(input);

    const msg = document.createElement('div');
    msg.className = 'booth-msg';
    msg.setAttribute('role', 'status');

    const actions = document.createElement('div');
    actions.className = 'booth-actions';
    const joinBtn = document.createElement('button');
    joinBtn.type = 'button';
    joinBtn.className = 'btn btn-primary booth-join';
    joinBtn.textContent = t('booth.join');
    const startBtn = document.createElement('button');
    startBtn.type = 'button';
    startBtn.className = 'btn btn-end-theirs booth-start-instead';
    startBtn.textContent = t('booth.startInstead', { app: o.myApp });
    const cancelBtn = document.createElement('button');
    cancelBtn.type = 'button';
    cancelBtn.className = 'btn btn-secondary booth-cancel';
    cancelBtn.textContent = t('common.cancel');
    actions.append(joinBtn, startBtn, cancelBtn);

    dialog.append(title, body, form, msg, actions);
    overlay.append(dialog);

    let busy = false;
    let shutUntil = 0;
    let tick = null;
    const digits = () => input.value.replace(/\D/g, '').slice(0, DIGITS);
    const ready = () => !busy && Date.now() >= shutUntil && digits().length === DIGITS;
    const paint = () => {
        const shut = Date.now() < shutUntil;
        input.disabled = busy || shut;
        joinBtn.disabled = startBtn.disabled = !ready();
    };
    const say = (text, isError) => {
        msg.textContent = text || '';
        msg.classList.toggle('err', !!(text && isError));
    };
    const holdFor = (seconds) => {
        if (tick) clearInterval(tick);
        tick = null;
        if (!(seconds > 0)) return;
        shutUntil = Date.now() + seconds * 1000;
        tick = setInterval(() => {
            const left = Math.ceil((shutUntil - Date.now()) / 1000);
            if (left <= 0) {
                clearInterval(tick);
                tick = null;
                say('', false);
            } else {
                say(t('booth.tooManyTries', { s: left }), true);
            }
            paint();
        }, 1000);
    };
    const refused = (r) => {
        say(r.text, true);
        holdFor(r.lockoutSeconds);
        paint();
    };

    const onKey = (e) => {
        if (e.key === 'Escape' && !busy) cancel();
    };
    const close = () => {
        if (tick) clearInterval(tick);
        tick = null;
        document.removeEventListener('keydown', onKey);
        overlay.remove();
    };
    const cancel = () => {
        close();
        if (o.onCancel) o.onCancel();
    };

    input.addEventListener('input', () => {
        input.value = shown(digits());
        if (msg.classList.contains('err') && Date.now() >= shutUntil) say('', false);
        paint();
    });
    const join = async () => {
        if (!ready()) return;
        const pin = digits();
        busy = true;
        say('', false);
        paint();
        try {
            await o.onJoin(pin);
            close();
        } catch (err) {
            busy = false;
            refused(boothErrorText(err));
        }
    };
    form.addEventListener('submit', (e) => {
        e.preventDefault();
        join();
    });
    joinBtn.addEventListener('click', join);
    startBtn.addEventListener('click', () => {
        if (!ready()) return;
        const pin = digits();
        close();
        o.onStartInstead(pin);
    });
    cancelBtn.addEventListener('click', cancel);
    overlay.addEventListener('click', (e) => {
        if (e.target === overlay && !busy) cancel();
    });
    document.addEventListener('keydown', onKey);

    document.body.appendChild(overlay);
    if (o.initial && o.initial.text) refused(o.initial);
    else paint();
    input.focus();
    return { el: overlay, close };
}
