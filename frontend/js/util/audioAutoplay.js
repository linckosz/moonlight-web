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
 * Desktop autoplay safety net for a media element whose play() was rejected.
 *
 * Browsers gate audible playback behind their autoplay policy, and the decision
 * is made when play() is called — several async hops after the launch click
 * (REST launch → StreamView → connect() → SDP/ICE → pc.ontrack). Chrome usually
 * lets it through on sticky activation, but Safari and Firefox block audible
 * media by default, and Chrome does too on a cold Media Engagement profile.
 *
 * Both other audio paths already recover from this — mobile through
 * iosAudioUnlock.armOutputRetry(), the WSS pipeline through
 * AudioPipeline._armGestureResume(). This is the same net for the desktop
 * <audio> element carrying the native RTP Opus track: the next user gesture
 * replays it. In a stream that gesture always comes (the "click to capture the
 * mouse" is unavoidable), so the recovery is invisible.
 *
 * Listeners sit on window in CAPTURE phase, so the stream's input layer cannot
 * swallow the gesture with stopPropagation before we see it.
 */

/** Gesture families that count as a user activation (same list as the two other nets). */
const GESTURE_EVENTS = ['pointerdown', 'mousedown', 'keydown', 'touchstart', 'click'];

/**
 * Elements currently armed → their cleanup function. A WeakMap so an element
 * dropped with its view is never kept alive by this module.
 * @type {WeakMap<HTMLMediaElement, () => void>}
 */
const armed = new WeakMap();

/**
 * Replay `mediaEl.play()` on the next user gesture, until it actually plays.
 *
 * Stays armed while the element is still paused — a first replay can be
 * rejected too — and disarms itself as soon as playback runs. Idempotent:
 * arming an already-armed element returns the existing cleanup instead of
 * stacking a second set of listeners (pc.ontrack can fire again on
 * renegotiation).
 *
 * @param {HTMLMediaElement} mediaEl - the element whose play() was rejected.
 * @returns {() => void} cleanup — removes the listeners. Call it on teardown so
 *   leaving before any gesture does not leak a listener on window.
 */
export function armAudioPlayRetry(mediaEl) {
    if (!mediaEl) return () => {};

    const existing = armed.get(mediaEl);
    if (existing) return existing;

    const cleanup = () => {
        for (const ev of GESTURE_EVENTS) {
            window.removeEventListener(ev, onGesture, true);
        }
        armed.delete(mediaEl);
    };

    const onGesture = () => {
        // Already playing (something else started it) — nothing left to do.
        if (!mediaEl.paused) {
            cleanup();
            return;
        }
        try {
            const p = mediaEl.play();
            if (p && p.then) {
                p.then(() => {
                    console.log('[audioAutoplay] audio resumed on user gesture');
                    cleanup();
                }).catch(() => {
                    /* stay armed — the next gesture gets another try */
                });
            } else {
                // Legacy play() with no promise: trust the paused flag instead.
                if (!mediaEl.paused) cleanup();
            }
        } catch (e) {
            /* stay armed */
        }
    };

    for (const ev of GESTURE_EVENTS) {
        window.addEventListener(ev, onGesture, true);
    }
    armed.set(mediaEl, cleanup);
    return cleanup;
}
