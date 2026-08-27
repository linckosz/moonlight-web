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
 * The seal shown while the connection to the machine is being made.
 *
 * Why the application draws this at all, when the entry page already does: the
 * entry page only runs on a browser's FIRST contact with a machine. Afterwards
 * the interface is cached and every later open or refresh starts here, with the
 * connection still to be made — the same wait, previously spent looking at an
 * empty shell. What is happening during it is the one thing about this product
 * worth understanding, so it is worth showing every time.
 *
 * It never appears on a direct connection. Reached at its own address the
 * browser already has a route to the machine, nothing is being negotiated, and
 * a padlock there would be a picture of security rather than a report of it.
 *
 * The bar is 0.7 real progress + 0.3 elapsed-over-1.2s and never goes backwards.
 * The time term exists because the connect phases have no denominator to report
 * and a frozen bar reads as a hang; with work owning the larger share the bar
 * cannot reach the end before the connection does. Then 300 ms at a full bar so
 * the completion is seen — after the bar has finished easing into it — and 1.6 s
 * on screen in total: a deliberate cost, and the reason the seal is worth
 * drawing.
 */

import { t } from '../i18n/i18n.js';

const WORK_WEIGHT = 0.7;
const TIME_WEIGHT = 0.3;
const TIME_SPAN_MS = 1200;
/* The bar eases into its new value rather than jumping to it (securing.css), so
   it arrives this long after the number does. Waited out before the hold, or
   "300 ms at a full bar" would be spent watching the bar still travelling. */
const BAR_EASE_MS = 260;
const HOLD_FULL_MS = 300;
const MIN_VISIBLE_MS = 1600;
const FADE_MS = 320;

/**
 * How far along each connect stage sits. Guesses, but ordered ones, and the
 * same ones the entry page uses so the two never disagree about what "half way"
 * looks like.
 */
const STAGE_PROGRESS = {
    identity: 0.1,
    calling: 0.25,
    binding: 0.55,
    connecting: 0.8,
    ready: 1,
};

/**
 * The entry page leaves this behind when it hands over, holding the moment it
 * finished. Without it the first visit to a machine would show the seal twice
 * in a row — once on the way in, once again the instant the application starts.
 * Read once and removed, so it only ever covers that single handover: any
 * refresh after it shows the seal, which is the point.
 */
const HANDOVER_KEY = 'mw-seal';
const HANDOVER_GRACE_MS = 10000;

function justHandedOver() {
    try {
        const at = sessionStorage.getItem(HANDOVER_KEY);
        sessionStorage.removeItem(HANDOVER_KEY);
        return at !== null && Date.now() - Number(at) < HANDOVER_GRACE_MS;
    } catch {
        return false;
    }
}

let el = null; // the overlay root, or null when not shown
let bar = null;
let status = null;
let startedAt = 0;
let work = 0;
let shown = 0;
let secured = false;
let done = false;
let heartbeat = null;

function build() {
    const root = document.createElement('div');
    root.className = 'securing';
    root.setAttribute('role', 'status');
    root.setAttribute('aria-live', 'polite');
    root.innerHTML = `
        <div class="securing-inner">
            <p class="securing-brand">MoonlightWeb</p>
            <div class="securing-seal" aria-hidden="true">
                <div class="securing-face">
                    <svg viewBox="0 0 32 32" fill="none" stroke="currentColor" stroke-width="2"
                         stroke-linecap="round" stroke-linejoin="round">
                        <rect x="7" y="14" width="18" height="13" rx="1.5"/>
                        <path class="securing-shackle" d="M11 14v-3.2a5 5 0 0 1 10 0V14"/>
                        <path d="M16 19v3.4"/>
                    </svg>
                </div>
            </div>
            <div class="securing-track"><div class="securing-bar"></div></div>
            <p class="securing-status">${t('securing.working')}</p>
        </div>`;
    return root;
}

/** Compute and paint one frame. Idempotent, so anything may call it. */
function render() {
    if (!el || secured) return;
    const elapsed = performance.now() - startedAt;
    const target = WORK_WEIGHT * work + TIME_WEIGHT * Math.min(elapsed / TIME_SPAN_MS, 1);
    shown = Math.max(shown, target);
    bar.style.width = `${(shown * 100).toFixed(1)}%`;
    if (shown < 0.999) return;

    secured = true;
    el.dataset.state = 'secured';
    status.textContent = t('securing.done');
    // Let the bar finish travelling, hold at full, respect the floor, then lift.
    setTimeout(dismiss, Math.max(BAR_EASE_MS + HOLD_FULL_MS, MIN_VISIBLE_MS - elapsed));
}

/*
 * Two clocks, and the second one is not a nicety: requestAnimationFrame does
 * not run at all in a hidden tab. This seal covers the application, so a frozen
 * bar is a covered application — a link opened in a background tab, or a
 * session restored at startup, would show a dead page the moment it was looked
 * at. The timer is throttled to about a second while hidden, which nobody sees
 * and which is enough to finish. Keep something ticking that a hidden tab gets.
 */
function raf() {
    if (!el || secured) return;
    render();
    requestAnimationFrame(raf);
}

function dismiss() {
    if (!el) return;
    clearInterval(heartbeat);
    heartbeat = null;
    const leaving = el;
    el.dataset.leaving = '';
    el = null;
    bar = null;
    status = null;
    setTimeout(() => leaving.remove(), FADE_MS);
}

export const SecuringOverlay = {
    /**
     * Put the seal up. Does nothing on a direct connection, and nothing right
     * after the entry page has just shown its own.
     *
     * `viaTunnel` is decided by the caller rather than sniffed here, because
     * the caller knows it synchronously and this must be on screen before
     * anything else is.
     */
    show(viaTunnel) {
        if (el || done || !viaTunnel) return;
        if (justHandedOver()) {
            done = true;
            return;
        }
        el = build();
        bar = el.querySelector('.securing-bar');
        status = el.querySelector('.securing-status');
        document.body.appendChild(el);
        startedAt = performance.now();
        requestAnimationFrame(raf);
        heartbeat = setInterval(render, 250);
    },

    /** A connect stage was reported. Ignored when the seal is not up. */
    stage(name) {
        const p = STAGE_PROGRESS[name];
        if (p) work = Math.max(work, p);
    },

    /**
     * The connection is up. The seal closes and lifts on its own from here —
     * never awaited, so the application carries on rendering behind it and the
     * minimum on screen costs the reveal, not the work.
     */
    finish() {
        done = true;
        work = 1;
    },

    /** Take it down now: something failed and the failure has to be readable. */
    abort() {
        done = true;
        dismiss();
    },
};
