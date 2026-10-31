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
 * The "silent carrier": a bench of the way up's priority (plan « le son et la
 * priorité des paquets », 21/10/2026). Off unless the bench key says so.
 *
 * What the browser sends — clicks, keys, pads on the DataChannels, SCTP — leaves
 * unmarked (DSCP 0). Chrome marks only the RTP it sends, after the sender's
 * `networkPriority`, and it does so on the one socket every packet of the
 * connection shares: SCTP packets go out with "no change", so they keep the last
 * mark. The carrier asks for that mark without sending any sound:
 * - `notrack`: the stream's audio transceiver answers sendrecv, with no track,
 *   and its sender takes `networkPriority: 'high'` (EF). Chrome sets the socket
 *   right away; no RTP leaves;
 * - `track`: the same, plus a track of zeros from an AudioContext: a constant
 *   source at 0 into a stream destination (no microphone, no permission, nothing
 *   audible), so RTP packets put the mark back within 20 ms when something else
 *   took it off. The source is needed: a destination with nothing connected
 *   renders no sample at all, and Chrome then sends no RTP (bench, 21/10).
 *
 * The host must offer the sound sendrecv for either (MW_DSCP_CARRIER=1 on the
 * host, a bench key too); against a sendonly offer the answer stays recvonly and
 * nothing changes. The stream's own sound is never touched: it keeps the
 * browser's native track and NetEq.
 */

/** The bench key: `notrack` or `track`; anything else, or nothing, is off. */
export const DSCP_CARRIER_KEY = 'mw_dscp_carrier';

/**
 * The carrier the bench asks for, read once per stream.
 *
 * @param {{getItem: (k: string) => string|null}} [storage]
 * @returns {'notrack'|'track'|null}
 */
export function dscpCarrierMode(storage) {
    let raw = null;
    try {
        const s = storage || globalThis.localStorage;
        raw = s ? s.getItem(DSCP_CARRIER_KEY) : null;
    } catch {
        raw = null;
    }
    const v = String(raw ?? '')
        .trim()
        .toLowerCase();
    return v === 'notrack' || v === 'track' ? v : null;
}

/** The stream's sound transceiver (mid `audio`), or null. */
function soundTransceiver(pc) {
    if (!pc || typeof pc.getTransceivers !== 'function') return null;
    return pc.getTransceivers().find((t) => t && t.mid === 'audio') || null;
}

/**
 * Before createAnswer: let the stream's audio section answer sendrecv.
 *
 * @param {RTCPeerConnection} pc
 * @param {'notrack'|'track'|null} mode
 * @returns {boolean} whether the transceiver was asked to send
 */
export function prepareDscpCarrier(pc, mode) {
    if (!mode) return false;
    const t = soundTransceiver(pc);
    if (!t) return false;
    try {
        t.direction = 'sendrecv';
        return true;
    } catch (e) {
        console.warn('[DscpCarrier] could not ask to send:', e);
        return false;
    }
}

/**
 * An answer SDP with every `a=sendrecv` turned `a=recvonly`, except the stream's
 * audio section when the carrier is on, or the viewer's microphone holds it
 * (`'mic'`, audio/MicUplink.js) — WebRtcMedia answers receive-only otherwise.
 *
 * @param {string} sdp
 * @param {'notrack'|'track'|'mic'|null} mode
 * @returns {string}
 */
export function recvonlyExceptCarrier(sdp, mode) {
    if (!mode) return sdp.replace(/a=sendrecv/g, 'a=recvonly');
    return sdp
        .split(/(?=^m=)/m)
        .map((section) =>
            /^a=mid:audio\r?$/m.test(section)
                ? section
                : section.replace(/a=sendrecv/g, 'a=recvonly'),
        )
        .join('');
}

/**
 * After setLocalDescription: the sender's priority, and the silent track for
 * `track`. Logs what it got; never throws.
 *
 * @param {RTCPeerConnection} pc
 * @param {'notrack'|'track'|null} mode
 * @returns {Promise<{armed: boolean, direction?: string, context?: string}>}
 */
export async function armDscpCarrier(pc, mode) {
    if (!mode) return { armed: false };
    const t = soundTransceiver(pc);
    const direction = t ? t.currentDirection || t.direction : 'none';
    if (!t || !/send/.test(direction || '')) {
        console.info('[DscpCarrier] ' + mode + ': not armed, audio section ' + direction);
        return { armed: false, direction };
    }
    let context;
    try {
        if (mode === 'track') {
            const AC = globalThis.AudioContext || globalThis.webkitAudioContext;
            const ctx = new AC();
            // Zeros into the destination, never into ctx.destination: nothing reaches a speaker.
            const dest = ctx.createMediaStreamDestination();
            if (typeof ctx.createConstantSource === 'function') {
                const zero = ctx.createConstantSource();
                zero.offset.value = 0;
                zero.connect(dest);
                zero.start();
            }
            await t.sender.replaceTrack(dest.stream.getAudioTracks()[0]);
            if (ctx.state !== 'running') await ctx.resume().catch(() => {});
            context = ctx.state;
            const close = () => {
                if (['closed', 'failed'].includes(pc.connectionState)) ctx.close().catch(() => {});
            };
            pc.addEventListener('connectionstatechange', close);
        }
        const params = t.sender.getParameters();
        for (const enc of params.encodings || []) enc.networkPriority = 'high';
        await t.sender.setParameters(params);
        const got = (t.sender.getParameters().encodings || []).map((e) => e.networkPriority);
        console.info(
            '[DscpCarrier] ' +
                mode +
                ': armed, networkPriority ' +
                (got.join(',') || 'none') +
                (context ? ', AudioContext ' + context : ''),
        );
        return { armed: true, direction, context };
    } catch (e) {
        console.warn('[DscpCarrier] ' + mode + ': failed:', e);
        return { armed: false, direction, context };
    }
}
