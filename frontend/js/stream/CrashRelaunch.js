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
 * CrashRelaunch — whether a stream whose link went is started again.
 *
 * The stream's WebRTC connection lives in the host's --stream-worker, so when
 * that process dies the page only sees its link go, a few seconds later. The
 * host remembers how each stream ended (StreamEnds.h) and the page asks:
 *
 *   crashed   the worker died under a healthy stream: start it again, with the
 *             transport that worked, behind the "Reconnecting…" veil
 *   signed_in the stream of a Windows PC's sign-in screen ended because its
 *             user signed in through it: the same, and the same card then
 *             streams the PC's screen, which the session is back on
 *   anything  live (a network cut, seen from here first), ended (the app closed,
 *   else      the link died host-side), stopped (Stop, another device took the
 *             stream over), unknown (no answer in time): the stream ends as
 *             it always has
 *
 * Twice at most in five minutes: a worker that crashes again and again is not
 * started a third time, and the viewer gets the screen they always got. A
 * launch the viewer makes themselves starts the count again.
 */

export const CRASH_RELAUNCH_MAX = 2;
export const CRASH_RELAUNCH_WINDOW_MS = 5 * 60 * 1000;
/** How long the page waits for the host's answer before ending as before. */
export const STREAM_STATE_TIMEOUT_MS = 1500;
/** When the host still says "live", how long before it is asked again. */
export const LIVE_RECHECK_MS = 1000;
/**
 * Nothing at all received for this long (frames, the host's stats every
 * second, pongs) while the link still counts as up: the page asks the host
 * whether the worker crashed. ICE takes some 8 s to call the link dead; the
 * host knows of a crash within a tenth of a second.
 */
export const LINK_STALL_MS = 1500;
/** At most one such question per this long, through a stall that lasts. */
export const LINK_STALL_PROBE_MS = 2000;

/**
 * The host's answer, asked a second time when it is still "live". A link
 * carried by a WebSocket (the wss rung) goes the moment the worker dies, a
 * hair before the server has seen the process exit; a link that went with ICE
 * is noticed seconds after, and the first answer stands.
 * @param {() => Promise<string>} ask
 * @param {number} [waitMs]
 * @returns {Promise<string>}
 */
export async function settledStreamState(ask, waitMs = LIVE_RECHECK_MS) {
    const first = await ask();
    if (first !== 'live') return first;
    await new Promise((resolve) => setTimeout(resolve, waitMs));
    return ask();
}

/**
 * @param {string} state what the host says of the lost stream
 * @param {number[]} history when the previous relaunches happened (ms)
 * @param {number} now
 * @returns {boolean}
 */
export function shouldRelaunchAfterCrash(state, history, now) {
    if (state !== 'crashed' && state !== 'signed_in') return false;
    return recentRelaunches(history, now).length < CRASH_RELAUNCH_MAX;
}

/**
 * The history with this relaunch in it, and without what has aged out.
 * @param {number[]} history
 * @param {number} now
 * @returns {number[]}
 */
export function recordCrashRelaunch(history, now) {
    return [...recentRelaunches(history, now), now];
}

/**
 * @param {number[]} history
 * @param {number} now
 * @returns {number[]}
 */
function recentRelaunches(history, now) {
    return (history || []).filter((at) => now - at < CRASH_RELAUNCH_WINDOW_MS);
}
