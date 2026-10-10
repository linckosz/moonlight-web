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
 * The video on an RTP track instead of the video DataChannel (POC Ultra U1.4,
 * docs/design/ultra-lan-poc.md). The host offers the track when its
 * `rtp_video` setting names the session's codec for its host type; the frames
 * are taken off it by Encoded Transform (rtpVideoTransformWorker.js), before
 * the browser's decoder, and go to the same decode path as the DataChannel's.
 * A browser without RTCRtpScriptTransform answers the track inactive and the
 * host keeps the video on the DataChannel.
 *
 * Track "video": the stream's codec, frames as the host's encoder made them
 * (Annex B for H.264 / HEVC, a temporal unit for AV1); the RTP timestamp is
 * the frame's backendTs (host steady clock, ms, mod 2^32). Track "ultra" (the
 * bench's synthetic Ultra stream): a 10-byte VP8 key-frame header, then the
 * Ultra DataChannel's chunk format for one chunk.
 */

export const ULTRA_RTP_PREFIX_BYTES = 10;

/** Whether this browser can take an RTP track's frames before its decoder. */
export function rtpVideoSupported() {
    return typeof globalThis.RTCRtpScriptTransform === 'function';
}

/**
 * The bench's other road (U1.4 ter): localStorage `mw_rtp_api=legacy` takes
 * the frames by Chrome's older createEncodedStreams on the page's own thread,
 * no worker. The peer connection must then be made with
 * `encodedInsertableStreams: true`, and every receiver's frames read.
 */
export function rtpLegacyApi() {
    try {
        return (
            globalThis.localStorage?.getItem('mw_rtp_api') === 'legacy' &&
            typeof globalThis.RTCRtpReceiver?.prototype?.createEncodedStreams === 'function'
        );
    } catch {
        return false;
    }
}

/**
 * The audio road's wait for a frame asked again, in ms (bench: localStorage
 * mw_aroad_giveup), or undefined for the worker's default.
 */
export function aroadGiveUpMs() {
    try {
        const v = Number(globalThis.localStorage?.getItem('mw_aroad_giveup'));
        return Number.isFinite(v) && v > 0 && v <= 1000 ? v : undefined;
    } catch {
        return undefined;
    }
}

/**
 * False when the bench's localStorage says mw_aroad_reask=0: the audio road
 * asks neither for whole frames nor twice (the witness of plan « Wi-Fi », W4).
 */
export function aroadReaskOn() {
    try {
        return globalThis.localStorage?.getItem('mw_aroad_reask') !== '0';
    } catch {
        return true;
    }
}

/**
 * The pieces the audio road gives of a PyroWave frame still coming, in bytes
 * (POC Ultra, decode by slices): bench keys mw_ultra=pyrowave and
 * mw_ultra_slices=1, mw_ultra_slice_kb for their size (48 KiB). 0 when off.
 */
export function ultraSliceBytes() {
    try {
        const ls = globalThis.localStorage;
        if (ls?.getItem('mw_ultra') !== 'pyrowave' || ls?.getItem('mw_ultra_slices') !== '1')
            return 0;
        const kb = Number(ls.getItem('mw_ultra_slice_kb'));
        return (Number.isFinite(kb) && kb >= 4 && kb <= 4096 ? kb : 48) * 1024;
    } catch {
        return 0;
    }
}

/** Lets an audio receiver's frames through untouched, on the legacy road. */
export function passEncodedAudio(receiver) {
    const { readable, writable } = receiver.createEncodedStreams();
    readable.pipeTo(writable).catch(() => {});
}

/** One encoded frame as the worker posts it (rtpVideoTransformWorker.js). */
function readEncodedFrame(mid, frame) {
    let ts = frame.timestamp;
    let held = -1;
    try {
        const meta = frame.getMetadata();
        if (meta && typeof meta.rtpTimestamp === 'number') ts = meta.rtpTimestamp;
        if (meta && typeof meta.receiveTime === 'number')
            held = performance.now() - meta.receiveTime;
    } catch {
        // Older engines: the frame's own timestamp is the RTP one.
    }
    const at = performance.timeOrigin + performance.now();
    return { mid, data: frame.data, key: frame.type === 'key', ts: ts >>> 0, at, held };
}

/** Frames kept by the audio road's trace (bench key mw_ultra_trace=1). */
const RTP_TRACE_KEEP = 16384;

/** Whether this page keeps the audio road's per-frame trace. */
function rtpTraceOn() {
    try {
        return globalThis.localStorage?.getItem('mw_ultra_trace') === '1';
    } catch {
        return false;
    }
}

/** The bench's figures of one frame (U1.4): kept as running windows. With the
 * trace (POC Ultra P-B), every audio road frame's way through the browser
 * too: [rtp ts, first chunk read, its receive, last chunk's receive, posted
 * by the worker, here, bytes], ms on the time origin's clock. */
function noteFrame(m) {
    const st = (globalThis.__mwRtp ||= { hops: [], held: [], ats: [], rcv: [] });
    const keep = (a, v) => {
        a.push(v);
        if (a.length > 600) a.shift();
    };
    const here = performance.timeOrigin + performance.now();
    if (st.trace && m.w0 !== undefined && st.trace.length < RTP_TRACE_KEEP)
        st.trace.push([m.ts, m.w0, m.rx0, m.rxN, m.at, here, m.data ? m.data.byteLength : 0]);
    keep(st.hops, here - m.at);
    if (m.lost) st.lost = (st.lost || 0) + 1;
    // When the frame reached us, and when its last packet came, on one clock.
    keep(st.ats, m.at);
    if (m.held >= 0) {
        keep(st.held, m.held);
        keep(st.rcv, m.at - m.held);
    }
}

/**
 * Takes the frames of the RTP track @p event announced (an `ontrack` event).
 * @param {RTCTrackEvent} event
 * @param {{onVideo?: function(Uint8Array, boolean, number, boolean, number=): void,
 *          onUltra?: function(ArrayBuffer, number): void,
 *          onVideoPart?: function(number, number, Uint8Array): void,
 *          log?: function(string): void}} sinks
 * @returns {{mid: string, worker: Worker|null, stop: function(): void}}
 */
export function attachRtpVideo(
    event,
    { onVideo, onUltra, onVideoPart, onNack, onAck, log = console.log } = {},
) {
    const mid = event.transceiver?.mid || (event.track.label === 'ultra' ? 'ultra' : 'video');
    if (!rtpVideoSupported()) {
        // Answered inactive: the host keeps the video on the DataChannel.
        try {
            event.transceiver.direction = 'inactive';
        } catch {
            // An already stopped transceiver: nothing will flow on it.
        }
        log('[MW-RTP] no RTCRtpScriptTransform here: track ' + mid + ' refused');
        return { mid, worker: null, stop() {} };
    }
    const legacy = rtpLegacyApi();
    let frames = 0;
    const onFrame = (m) => {
        if (m.ready) {
            log(
                '[MW-RTP] transform of ' +
                    mid +
                    ' running' +
                    (legacy ? ' (legacy, page thread)' : ''),
            );
            return;
        }
        if (m.error) {
            log('[MW-RTP] transform of ' + mid + ' ended: ' + m.error);
            return;
        }
        if (m.nack) {
            const st = (globalThis.__mwRtp ||= { hops: [], held: [], ats: [], rcv: [] });
            st.nacked = (st.nacked || 0) + m.nack.i.length;
            // Frames asked for whole, and asks repeated (W4's counters).
            if (m.nack.all) st.whole = (st.whole || 0) + 1;
            if (m.nack.reask) st.reasked = (st.reasked || 0) + (m.nack.i.length || 1);
            if (onNack) onNack(m.nack);
            return;
        }
        if (m.ack) {
            if (onAck) onAck(m.ack);
            return;
        }
        // The front of a frame still coming (decode by slices): frame id, offset, bytes.
        if (m.part) {
            if (onVideoPart) onVideoPart(m.part.fid, m.part.off, new Uint8Array(m.data));
            return;
        }
        frames++;
        if (m.at) noteFrame(m);
        if (frames === 1) log('[MW-RTP] first frame on ' + mid + (m.key ? ' (key)' : ''));
        if (m.mid === 'ultraraw') {
            if (onUltra) onUltra(m.data, performance.now());
            return;
        }
        if (m.mid === 'ultra') {
            if (onUltra && m.data.byteLength > ULTRA_RTP_PREFIX_BYTES)
                onUltra(m.data.slice(ULTRA_RTP_PREFIX_BYTES), performance.now());
            return;
        }
        if (onVideo) onVideo(new Uint8Array(m.data), m.key, m.ts, m.lost === true, m.fid);
    };
    // Nothing of the browser's own buffering is wanted before the transform.
    try {
        event.receiver.jitterBufferTarget = 0;
    } catch {
        // Not settable here: the transform sits before it anyway.
    }
    let worker = null;
    let reader = null;
    if (legacy) {
        reader = event.receiver.createEncodedStreams().readable.getReader();
        onFrame({ ready: true });
        const pump = () =>
            reader.read().then(({ value: frame, done }) => {
                if (done || !frame) return;
                onFrame(readEncodedFrame(mid, frame));
                return pump();
            });
        pump().catch((e) => onFrame({ error: String(e && e.message ? e.message : e) }));
    } else {
        if (/^[vu]audio$/.test(mid) && rtpTraceOn())
            (globalThis.__mwRtp ||= { hops: [], held: [], ats: [], rcv: [] }).trace = [];
        worker = new Worker(new URL('./rtpVideoTransformWorker.js', import.meta.url));
        worker.onmessage = (msg) => onFrame(msg.data);
        worker.onerror = (e) =>
            log('[MW-RTP] transform worker of ' + mid + ' failed: ' + e.message);
        event.receiver.transform = new globalThis.RTCRtpScriptTransform(worker, {
            mid,
            giveUpMs: aroadGiveUpMs(),
            // Said only when off: the worker repairs unless told otherwise.
            ...(aroadReaskOn() ? {} : { reask: false }),
            ...(mid === 'vaudio' && ultraSliceBytes() ? { partBytes: ultraSliceBytes() } : {}),
        });
    }
    log('[MW-RTP] taking the frames of RTP track ' + mid + ' by Encoded Transform');
    // The first seconds in figures: packets that came, frames assembled.
    let looks = 0;
    setTimeout(() => {
        try {
            const codecs = event.receiver.getParameters().codecs || [];
            log(
                '[MW-RTP] ' +
                    mid +
                    ' negotiated: ' +
                    codecs
                        .map((c) => c.payloadType + ' ' + c.mimeType + ' ' + (c.sdpFmtpLine || ''))
                        .join(' | '),
            );
        } catch (e) {
            log('[MW-RTP] ' + mid + ' parameters: ' + e.message);
        }
    }, 1000);
    const look = setInterval(() => {
        if (++looks > 5) clearInterval(look);
        event.receiver
            .getStats()
            .then((report) => {
                report.forEach((r) => {
                    if (r.type !== 'inbound-rtp') return;
                    log(
                        '[MW-RTP] ' +
                            mid +
                            ': packets ' +
                            r.packetsReceived +
                            ', lost ' +
                            r.packetsLost +
                            ', frames ' +
                            (r.framesReceived ?? '-') +
                            ', to us ' +
                            frames +
                            ', codec ' +
                            (r.codecId || '-'),
                    );
                });
            })
            .catch(() => {});
    }, 2000);
    return {
        mid,
        worker,
        get frames() {
            return frames;
        },
        stop() {
            clearInterval(look);
            worker?.terminate();
            reader?.cancel().catch(() => {});
        },
    };
}
