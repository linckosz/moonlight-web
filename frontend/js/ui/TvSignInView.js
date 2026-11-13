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
 * TvSignInView — a TV on plain HTTP signs in with a phone (issue #42).
 *
 * A TV listed in `lan_http_clients` opens this page over http://. Nothing is
 * typed on it: a PIN would travel in clear. It shows a QR code and a code
 * instead. Someone already signed in scans the QR code with a phone or a
 * computer, lands on the approval page over HTTPS, and types the code read on
 * the TV there. Meanwhile the TV asks every couple of seconds where its request
 * stands; once it is approved, the server sets the TV's cookie on that very
 * answer and the page starts again, signed in.
 *
 * A request lasts five minutes. One that runs out, or that the server no longer
 * knows (it restarted), is replaced by a new one without a word: the code on
 * the screen simply changes. A refused one says so and waits for "Ask again".
 */

import { lanHttpPair, lanHttpPoll } from '../api/LanHttpApi.js';
import { t } from '../i18n/i18n.js';
import { escapeHtml } from '../util/escapeHtml.js';

/** How often the TV asks where its request stands. */
const POLL_MS = 2000;
/** The wait before asking again when the server is busy or did not answer. */
const BUSY_RETRY_MS = 10000;
const ERROR_RETRY_MS = 5000;

/** The code as the TV shows it: two groups of three, easier to read across a room. */
export function spacedCode(code) {
    const c = String(code || '').toUpperCase();
    return c.length === 6 ? `${c.slice(0, 3)} ${c.slice(3)}` : c;
}

/** Minutes and seconds left, as "4:52". */
export function clockOf(seconds) {
    const s = Math.max(0, Math.floor(seconds));
    return `${Math.floor(s / 60)}:${String(s % 60).padStart(2, '0')}`;
}

export class TvSignInView {
    /**
     * @param {HTMLElement} container
     * @param {{
     *   onSignedIn: () => void,
     *   qr?: (text: string) => string,
     *   pollMs?: number,
     *   now?: () => number,
     * }} opts  `qr` draws a QR code as SVG markup; without it, the address is
     *   shown as text.
     */
    constructor(container, opts) {
        this.container = container;
        this._onSignedIn = opts.onSignedIn;
        this._qr = opts.qr || null;
        this._pollMs = opts.pollMs ?? POLL_MS;
        this._now = opts.now || (() => Date.now());
        this._timer = null;
        this._stopped = false;
        /** @type {{request: string, claim: string, code: string, approve_url: string, expiresAt: number}|null} */
        this._request = null;
    }

    start() {
        this._stopped = false;
        this._render(this._connecting());
        void this._ask();
    }

    stop() {
        this._stopped = true;
        if (this._timer) clearTimeout(this._timer);
        this._timer = null;
    }

    _later(fn, ms) {
        if (this._stopped) return;
        if (this._timer) clearTimeout(this._timer);
        this._timer = setTimeout(fn, ms);
    }

    /** A new request: its QR code and its code on the screen, then the wait. */
    async _ask() {
        if (this._stopped) return;
        let opened;
        try {
            opened = await lanHttpPair();
        } catch (err) {
            if (this._stopped) return;
            const busy = /** @type {{statusCode?: number}} */ (err)?.statusCode === 429;
            this._render(this._message(t(busy ? 'tvSignIn.busy' : 'tvSignIn.unreachable')));
            this._later(() => this._ask(), busy ? BUSY_RETRY_MS : ERROR_RETRY_MS);
            return;
        }
        if (this._stopped) return;
        if (!opened?.request || !opened?.claim) {
            this._render(this._message(t('tvSignIn.unreachable')));
            this._later(() => this._ask(), ERROR_RETRY_MS);
            return;
        }
        this._request = {
            request: String(opened.request || ''),
            claim: String(opened.claim || ''),
            code: String(opened.code || ''),
            approve_url: String(opened.approve_url || ''),
            expiresAt: this._now() + Math.max(0, Number(opened.expires_in) || 0) * 1000,
        };
        this._render(this._showing(this._request));
        this._later(() => this._poll(), this._pollMs);
    }

    async _poll() {
        const r = this._request;
        if (this._stopped || !r) return;
        const left = (r.expiresAt - this._now()) / 1000;
        if (left <= 0) {
            void this._ask();
            return;
        }
        let state;
        try {
            state = await lanHttpPoll(r.request, r.claim);
        } catch {
            // A moment without the server: the request is still there.
            state = 'pending';
        }
        if (this._stopped || r !== this._request) return;
        if (state === 'approved') {
            this.stop();
            this._onSignedIn();
            return;
        }
        if (state === 'gone') {
            void this._ask();
            return;
        }
        if (state === 'refused') {
            this._request = null;
            this._render(this._refused());
            return;
        }
        this._updateClock(r);
        this._later(() => this._poll(), this._pollMs);
    }

    _updateClock(r) {
        const line = this.container.querySelector('[data-tv-waiting]');
        if (!line) return;
        const left = (r.expiresAt - this._now()) / 1000;
        line.textContent = t('tvSignIn.waiting', { time: clockOf(left) });
    }

    _render(inner) {
        this.container.innerHTML = `
            <div class="login-view tv-sign-in" id="view-login">
                <div class="login-box">
                    <div class="login-logo">
                        <span class="login-logo-img" role="img" aria-label="Moonlight"></span>
                    </div>
                    <h1 class="login-title">${escapeHtml(t('tvSignIn.title'))}</h1>
                    ${inner}
                </div>
            </div>`;
        const again = this.container.querySelector('[data-tv-again]');
        if (again) {
            again.addEventListener('click', () => this.start());
            /** @type {HTMLElement} */ (again).focus();
        }
    }

    _connecting() {
        return `
            <div class="login-loading">
                <div class="login-spinner"></div>
                <p>${escapeHtml(t('login.connecting'))}</p>
            </div>`;
    }

    _message(text) {
        return `<p class="tv-sign-in-message">${escapeHtml(text)}</p>`;
    }

    _showing(r) {
        const qr = this._qr && r.approve_url ? this._qr(r.approve_url) : '';
        const where = qr
            ? `<p class="tv-sign-in-lead">${escapeHtml(t('tvSignIn.scan'))}</p>
               <div class="tv-sign-in-qr" role="img" aria-label="${escapeHtml(t('tvSignIn.qrLabel'))}">${qr}</div>`
            : `<p class="tv-sign-in-lead">${escapeHtml(t('tvSignIn.openAddress'))}</p>
               <p class="tv-sign-in-address">${escapeHtml(r.approve_url)}</p>`;
        const left = (r.expiresAt - this._now()) / 1000;
        return `
            ${where}
            <p class="tv-sign-in-code" aria-label="${escapeHtml(r.code.split('').join(' '))}">${escapeHtml(spacedCode(r.code))}</p>
            <p class="tv-sign-in-waiting" data-tv-waiting>${escapeHtml(t('tvSignIn.waiting', { time: clockOf(left) }))}</p>`;
    }

    _refused() {
        return `
            ${this._message(t('tvSignIn.refused'))}
            <button type="button" class="btn" data-tv-again data-nav-initial>${escapeHtml(t('tvSignIn.askAgain'))}</button>`;
    }
}
