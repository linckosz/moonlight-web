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
 * MoonlightWeb — the "your gamepad needs a driver" notice.
 *
 * A gamepad is the one input a host cannot fake with an API call: games do not
 * read input events, they enumerate HID devices, so a real device has to exist
 * and only a kernel driver can create one. The installer already puts ViGEmBus
 * down silently — but that can have failed (no network at the time), been
 * bypassed (a build run from sources), or been undone since. In all three cases
 * the controller simply does nothing and NOTHING says why: the log knows, the
 * person holding the pad does not.
 *
 * Three properties this must keep:
 *
 *   - It is not a wall and not a modal. Streaming, keyboard and mouse all work
 *     without the driver; this is a clean degradation being explained, not an
 *     error being raised. It closes, and it never comes back in that tab.
 *   - Only the machine itself is offered the button. The driver would land on
 *     the server's machine, not the browser's, so the server decides — see
 *     GamepadDriver::mayOffer — and hands down `offer_install` already decided.
 *     This file must never re-derive that from `window.location`: `localhost`
 *     in the address bar proves nothing, an SSH tunnel forges it in one command.
 *   - An instance without the rights to install says so and links upstream,
 *     rather than offering a button that dies behind a UAC prompt on a desktop
 *     nobody is looking at.
 */
import { BackendClient } from '../api/BackendClient.js';
import { Toast } from './Toast.js';
import { t } from '../i18n/i18n.js';
import { escapeHtml } from '../util/escapeHtml.js';

/** Closed by hand: honoured for this tab, re-asked in a fresh session. */
const DISMISS_KEY = 'mw-gamepad-driver-dismissed';

export class GamepadDriverNotice {
    /**
     * Render the notice into `parent`, unless there is nothing to say.
     *
     * Silent when: the driver is there, this platform has no virtual pad at
     * all, the caller is not the host machine, or it was closed in this tab.
     * Any failure to ask is silence too — a backend that will not answer this
     * is not a reason to put a warning in front of someone.
     *
     * @param {HTMLElement|null} parent
     * @param {HTMLElement|null} [before] insert ahead of this child (top if omitted)
     */
    static async mount(parent, before = null) {
        if (!parent) return;
        if (parent.querySelector('.gamepad-notice')) return;
        try {
            if (sessionStorage.getItem(DISMISS_KEY)) return;
        } catch {
            // A browser that refuses session storage still gets the notice; it
            // just cannot remember that it was closed.
        }

        let state;
        try {
            state = await BackendClient.getGamepadDriver();
        } catch (err) {
            console.warn('[MW] gamepad driver state unavailable:', err);
            return;
        }
        // The server's verdict, taken as given. It knows where the request came
        // from; this page only knows what its own URL says, which is not the
        // same question.
        if (!state || !state.offer_install) return;

        this._render(parent, before, state);
    }

    /**
     * @param {HTMLElement} parent
     * @param {HTMLElement|null} before
     * @param {{ can_install?: boolean, download_url?: string }} state
     */
    static _render(parent, before, state) {
        const notice = document.createElement('div');
        notice.className = 'gamepad-notice';
        // A region, not an alert: it interrupts nothing and takes no focus.
        notice.setAttribute('role', 'region');
        notice.setAttribute('aria-label', t('gamepad.noticeTitle'));

        const url = state.download_url || '';
        const action = state.can_install
            ? `<button type="button" class="btn" id="gamepad-install">
                   ${escapeHtml(t('gamepad.install'))}
               </button>`
            : `<a class="btn btn-neutral" href="${encodeURI(url)}"
                  target="_blank" rel="noopener">${escapeHtml(t('gamepad.download'))}</a>`;

        notice.innerHTML = `
            <span class="gamepad-notice-icon" aria-hidden="true">\u{1F3AE}</span>
            <div class="gamepad-notice-body">
                <strong class="gamepad-notice-title">${escapeHtml(t('gamepad.noticeTitle'))}</strong>
                <span class="gamepad-notice-text">${escapeHtml(t('gamepad.noticeBody'))}</span>
                ${
                    state.can_install
                        ? ''
                        : `<span class="gamepad-notice-hint">${escapeHtml(
                              t('gamepad.manualHint'),
                          )}</span>`
                }
            </div>
            <div class="gamepad-notice-actions">
                ${action}
                <button type="button" class="gamepad-notice-close" id="gamepad-dismiss"
                        title="${escapeHtml(t('common.close'))}"
                        aria-label="${escapeHtml(t('common.close'))}">&times;</button>
            </div>`;

        parent.insertBefore(notice, before || parent.firstChild);

        const dismiss = notice.querySelector('#gamepad-dismiss');
        if (dismiss) {
            dismiss.addEventListener('click', () => {
                try {
                    sessionStorage.setItem(DISMISS_KEY, '1');
                } catch {
                    // Nothing to remember it with — it will be back next render.
                }
                notice.remove();
            });
        }

        const install = notice.querySelector('#gamepad-install');
        if (install) install.addEventListener('click', () => this._install(notice, install));
    }

    /**
     * Install, and say what happened. The notice stays put on failure — the
     * driver is still missing, which is exactly what it exists to say.
     *
     * @param {HTMLElement} notice
     * @param {HTMLButtonElement} button
     */
    static async _install(notice, button) {
        button.disabled = true;
        const label = button.textContent || t('gamepad.install');
        button.textContent = t('gamepad.installing');

        let result;
        try {
            result = await BackendClient.installGamepadDriver();
        } catch (err) {
            console.warn('[MW] gamepad driver install failed:', err);
            Toast.error(t('gamepad.installFailed'));
            button.disabled = false;
            button.textContent = label;
            return;
        }

        if (result && result.status === 'installed') {
            Toast.success(t('gamepad.installed'));
            notice.remove();
            return;
        }
        if (result && result.status === 'restart_required') {
            // Installed. It just is not running yet, and only a reboot changes
            // that — so the notice goes, and the sentence explains the wait.
            Toast.success(t('gamepad.restartRequired'));
            notice.remove();
            return;
        }

        console.warn('[MW] gamepad driver install failed:', result && result.error);
        Toast.error(t('gamepad.installFailed'));
        button.disabled = false;
        button.textContent = label;
    }
}
