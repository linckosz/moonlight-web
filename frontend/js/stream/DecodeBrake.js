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
 * The brake on a decoder that does not keep up: when to stop feeding it.
 *
 * A decoder's queue that stays full is pictures waiting, and a delay that only
 * grows. A burst is decoded through (dropping at once made a perpetual
 * keyframe and stutter cycle on mobile WSS); a saturation that lasts drops the
 * deltas until a keyframe comes; one that lasts longer still is a decoder to
 * rebuild.
 *
 * H.264 and HEVC have had this in line since they were written
 * (StreamView.decodeFrame, and the worker's). AV1 had none of it: on a N95
 * decoding 1440p60 in software, 39 pictures came out of the 60 a second and
 * every one of the others waited — a thousand after 50 s, 27 s behind
 * (11/11/2026). The AV1 paths take the same rule from here.
 */

/**
 * @param {{isKeyframe: boolean, queued: number, queueMax: number,
 *   stallStart: number, now: number, stallMs: number, resetMs: number}} o
 *   queued     — VideoDecoder.decodeQueueSize as the picture arrives
 *   queueMax   — the depth from which the queue counts as full
 *   stallStart — when this saturation began, 0 when there is none
 *   stallMs    — how long a full queue is decoded through
 *   resetMs    — how long before the decoder is rebuilt
 * @returns {{verdict: 'decode'|'drop'|'reset', stallStart: number}} what to do
 *   with the picture, and the stallStart to keep for the next one
 */
export function queueBrake(o) {
    // A keyframe always goes in: it is what restores the picture.
    if (o.isKeyframe || o.queued < o.queueMax) return { verdict: 'decode', stallStart: 0 };
    const stallStart = o.stallStart === 0 ? o.now : o.stallStart;
    const saturatedMs = o.now - stallStart;
    if (saturatedMs > o.resetMs) return { verdict: 'reset', stallStart: 0 };
    if (saturatedMs > o.stallMs) return { verdict: 'drop', stallStart };
    return { verdict: 'decode', stallStart };
}
