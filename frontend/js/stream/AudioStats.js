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
 * What the browser does with the stream's sound, once a second (plan « le son
 * et la priorité des paquets », A0).
 *
 * The sound rides an RTP track on either WebRTC transport, so the browser's
 * NetEq owns its buffer and its repairs. Its getStats() counters are all
 * cumulative; each sample turns them into the last interval's values:
 * - the buffer (jitterBufferDelay / jitterBufferEmittedCount), what the
 *   overlay has shown as "Audio buffer" since September;
 * - the target and the minimum NetEq was holding to — `jitterBufferTarget` is
 *   a floor in libwebrtc, so a buffer sitting on its minimum is the floor's
 *   doing, not the link's;
 * - the arrival jitter (RFC 3550, the receiver's own estimate);
 * - the sound NetEq had to make up: concealed samples (the crackle), the
 *   silent part of them, concealment events, and the samples it inserted or
 *   removed to slow down or catch up;
 * - what never reached the speaker although it arrived: packets NetEq
 *   discarded (late, or thrown out with a flushed buffer), the flushes
 *   themselves (Chrome only), and the energy of the sound actually played
 *   (`totalAudioEnergy`), which tells a sound lost inside the browser from one
 *   lost after it. A1 (06/10): the host sent all 60 beeps of a pass, the
 *   client's output held 12-19, with no packet lost;
 * - what the audio device took from the browser (`media-playout`, Chrome): the
 *   sound it played, the part the browser had to make up because nothing was
 *   ready when the device asked (a gap at the output, not in NetEq), and the
 *   output's own delay. A device that stopped asking (an output suspended on
 *   silence) shows as less than a second played in a second.
 *
 * Only the stream's sound: in POC Ultra's audio road mode (`vaudio`, `uaudio`)
 * other audio tracks carry video, and the last `inbound-rtp` of kind audio may
 * be one of them. The bench fetches the rows over CDP: `mwAudio.csv()`.
 */

/** The audio road's mids (RtpVideo), which are not the stream's sound. */
const AUDIO_ROAD_MID = /^[vu]audio$/;

/**
 * The `inbound-rtp` stats of the stream's sound in @p report, or null.
 *
 * Chrome puts the transceiver's `mid` on the stats; where it does not, the
 * track id of each transceiver tells them apart.
 *
 * @param {RTCStatsReport|Map<string, object>} report
 * @param {RTCPeerConnection} [pc]
 */
export function findStreamAudioInbound(report, pc) {
    const audio = [];
    report.forEach((s) => {
        if (s && s.type === 'inbound-rtp' && s.kind === 'audio') audio.push(s);
    });
    if (audio.length === 0) return null;
    const byMid = audio.filter((s) => typeof s.mid === 'string' && s.mid !== '');
    if (byMid.length > 0) return byMid.find((s) => !AUDIO_ROAD_MID.test(s.mid)) || null;
    if (audio.length === 1) return audio[0];
    const roadTracks = new Set();
    try {
        for (const t of pc && pc.getTransceivers ? pc.getTransceivers() : []) {
            if (t && AUDIO_ROAD_MID.test(t.mid || '') && t.receiver && t.receiver.track)
                roadTracks.add(t.receiver.track.id);
        }
    } catch {
        // a closing connection: fall through to the first one
    }
    return audio.find((s) => !roadTracks.has(s.trackIdentifier)) || audio[0];
}

/**
 * The `media-playout` stats that @p inbound plays through (its `playoutId`),
 * or the only one in @p report, or null (Firefox, Safari).
 *
 * @param {RTCStatsReport|Map<string, object>} report
 * @param {object} [inbound]
 */
export function findAudioPlayout(report, inbound) {
    const playouts = [];
    report.forEach((s) => {
        if (s && s.type === 'media-playout') playouts.push(s);
    });
    if (inbound && inbound.playoutId) {
        const own = playouts.find((s) => s.id === inbound.playoutId);
        if (own) return own;
    }
    return playouts.length === 1 ? playouts[0] : null;
}

/** The CSV columns, in order. */
export const AUDIO_STATS_COLUMNS = [
    't',
    'bufferMs',
    'targetMs',
    'minimumMs',
    'jitterMs',
    'packets',
    'lost',
    'samples',
    'concealed',
    'silentConcealed',
    'concealmentEvents',
    'inserted',
    'removed',
    'discarded',
    'flushes',
    'energy',
    'playedMs',
    'synthMs',
    'synthEvents',
    'outputMs',
];

/** The columns that hold milliseconds. */
const MS_COLUMNS = new Set([
    'bufferMs',
    'targetMs',
    'minimumMs',
    'jitterMs',
    'playedMs',
    'synthMs',
    'outputMs',
]);

const delta = (now, before) =>
    typeof now === 'number' && typeof before === 'number' && now >= before ? now - before : 0;

/** The interval's value of a counter some browsers lack: -1 when absent. */
const optionalDelta = (now, before) =>
    typeof now === 'number' && typeof before === 'number' ? delta(now, before) : -1;

/**
 * The interval's playout values from two `media-playout` stats (seconds,
 * cumulative): -1 each without both, or when the device's count went back.
 */
function playoutDeltas(now, before) {
    const none = { playedMs: -1, synthMs: -1, synthEvents: -1, outputMs: -1 };
    if (!now || !before) return none;
    const count = optionalDelta(now.totalSamplesCount, before.totalSamplesCount);
    if ((now.totalSamplesCount || 0) < (before.totalSamplesCount || 0)) return none;
    const ms = (key) => {
        const d = optionalDelta(now[key], before[key]);
        return d >= 0 ? d * 1000 : -1;
    };
    const delay = optionalDelta(now.totalPlayoutDelay, before.totalPlayoutDelay);
    return {
        playedMs: ms('totalSamplesDuration'),
        synthMs: ms('synthesizedSamplesDuration'),
        synthEvents: optionalDelta(now.synthesizedSamplesEvents, before.synthesizedSamplesEvents),
        outputMs: count > 0 && delay >= 0 ? (delay / count) * 1000 : -1,
    };
}

export class AudioStatsSampler {
    /** @param {{maxRows?: number}} [opts] */
    constructor({ maxRows = 7200 } = {}) {
        this.maxRows = maxRows;
        /** @type {object[]} */
        this.rows = [];
        /** The newest row, or null. */
        this.last = null;
        this._prev = null;
        this._prevPlayout = null;
        this._t0 = -1;
    }

    /**
     * One interval from cumulative stats @p s, taken at @p nowMs, with the
     * `media-playout` stats @p playout where the browser has them. The first
     * call only anchors; a counter that went backwards (a new track) anchors
     * again.
     *
     * @returns {object|null} the row, or null when there is none yet
     */
    sample(s, nowMs, playout = null) {
        if (!s) return null;
        const prev = this._prev;
        const prevPlayout = this._prevPlayout;
        this._prev = s;
        this._prevPlayout = playout;
        if (this._t0 < 0) this._t0 = nowMs;
        if (!prev) return null;
        const emitted = delta(s.jitterBufferEmittedCount, prev.jitterBufferEmittedCount);
        if ((s.jitterBufferEmittedCount || 0) < (prev.jitterBufferEmittedCount || 0)) return null;
        const per = (key) => (emitted > 0 ? (delta(s[key], prev[key]) / emitted) * 1000 : -1);
        const row = {
            t: Math.round(nowMs - this._t0),
            bufferMs: per('jitterBufferDelay'),
            targetMs:
                typeof s.jitterBufferTargetDelay === 'number' ? per('jitterBufferTargetDelay') : -1,
            minimumMs:
                typeof s.jitterBufferMinimumDelay === 'number'
                    ? per('jitterBufferMinimumDelay')
                    : -1,
            jitterMs: typeof s.jitter === 'number' ? s.jitter * 1000 : -1,
            packets: delta(s.packetsReceived, prev.packetsReceived),
            lost: delta(s.packetsLost, prev.packetsLost),
            samples: delta(s.totalSamplesReceived, prev.totalSamplesReceived),
            concealed: delta(s.concealedSamples, prev.concealedSamples),
            silentConcealed: delta(s.silentConcealedSamples, prev.silentConcealedSamples),
            concealmentEvents: delta(s.concealmentEvents, prev.concealmentEvents),
            inserted: delta(s.insertedSamplesForDeceleration, prev.insertedSamplesForDeceleration),
            removed: delta(s.removedSamplesForAcceleration, prev.removedSamplesForAcceleration),
            discarded: optionalDelta(s.packetsDiscarded, prev.packetsDiscarded),
            flushes: optionalDelta(s.jitterBufferFlushes, prev.jitterBufferFlushes),
            energy: optionalDelta(s.totalAudioEnergy, prev.totalAudioEnergy),
            ...playoutDeltas(playout, prevPlayout),
        };
        this.last = row;
        this.rows.push(row);
        if (this.rows.length > this.maxRows) this.rows.splice(0, this.rows.length - this.maxRows);
        return row;
    }

    /** The share of the last interval's samples NetEq made up, in percent. */
    static concealedPercent(row) {
        if (!row || !(row.samples > 0)) return 0;
        return (100 * Math.max(0, row.concealed - row.silentConcealed)) / row.samples;
    }

    /**
     * The rows as CSV, header first: the times in ms to two decimals, the
     * energy to eight (a 20 ms beep at half scale is ~0.0025), -1 kept.
     */
    csv() {
        const lines = [AUDIO_STATS_COLUMNS.join(',')];
        for (const r of this.rows)
            lines.push(
                AUDIO_STATS_COLUMNS.map((c) => {
                    const v = r[c];
                    if (v >= 0 && MS_COLUMNS.has(c)) return v.toFixed(2);
                    if (v >= 0 && c === 'energy') return v.toFixed(8);
                    return v;
                }).join(','),
            );
        return lines.join('\n') + '\n';
    }
}
