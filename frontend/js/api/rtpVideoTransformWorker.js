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
 * The Encoded Transform of the RTP video track (POC Ultra U1.4, RtpVideo.js):
 * each frame the browser has assembled from its RTP packets is posted to the
 * page as is, with its key flag and RTP timestamp (the host puts the frame's
 * backendTs there, in ms), and never handed back — the browser's own decoder
 * never sees it, ours decodes it.
 */

/** How long a complete video frame waits for an older one asked again (the
 * page may set another, bench key mw_aroad_giveup). */
const GIVE_UP_MS = 15;
/** Bytes of the audio road's chunk header. */
const HEAD = 12;
/** The last chunk received is named back at most this often (ms)… */
const ACK_MS = 5;
/** …or after this many bytes, for the host's send window (aroadwin=). */
const ACK_BYTES = 16 * 1024;
/** A chunk asked for and still missing is asked again after this long
 * (ms), at most REASK_TRIES more times: the ask or its answer may be lost
 * too. Past a Wi-Fi round trip and a resend, so that an answer on its way is
 * not asked for twice: at 8 ms and twice, a Mac whose socket overflowed got
 * a storm (06/10/2026: 23,000 chunks resent a pass). */
const REASK_MS = 20;
const REASK_TRIES = 1;
/** The longest hole in the seqs asked for whole, once. One or two small
 * frames lost on the air; a longer hole is a burst of the socket's own drops,
 * which resending into the same socket would only feed: given up on. */
const HOLE_MAX = 2;
/** GIVE_UP_MS while repairing: room for an ask again and its answer. */
const GIVE_UP_REPAIR_MS = 40;

/**
 * The bench's other road (U1.4 ter, tracks "vaudio" / "uaudio"): each frame
 * comes cut in Opus packets — 'M', flags (1 = key), frame seq, index, count
 * (u16, big endian), the host's wire frame id (u32, big endian, the
 * DataChannel's sequence), then the frame's bytes — and is put back together
 * here. The frame id lets the page name a frame given up on to a host that
 * heals by invalidation, as it does on the DataChannel.
 * Chrome asks nothing again on this road: the chunks seen missing (a hole in a
 * frame's indexes, or a newer frame started) are asked for by `nack` messages,
 * which the page sends on the input channel (U1.4 quater). Video frames go out
 * in order, since a delta needs the one before; one that waits more than
 * GIVE_UP_MS for an older one goes anyway, marked `lost`. Ultra frames stand
 * alone and go as they complete.
 * A frame none of whose chunks came (a small delta is one or two chunks: one
 * loss on the air takes it whole) shows only as a hole in the seqs: it is
 * asked for whole (`all`, once, a hole of HOLE_MAX frames at most), the host
 * sending every chunk of it within its resend budget. Chunk asks still
 * unanswered after REASK_MS go again (`reask`). Off with the page's bench key
 * mw_aroad_reask=0 (the witness of plan « Wi-Fi », W4).
 * The last chunk received is named back (`ack`, every ACK_MS or ACK_BYTES): a
 * host with a send window (aroadwin=) keeps at most that much in flight past
 * it, the ack clock SCTP has and this road lacked (plan « Wi-Fi », W4).
 * With `partBytes` (POC Ultra, decode by slices): the front of a frame still
 * coming, in index order and with no hole, goes to the page as `part` pieces
 * of at least that many bytes, so its decoding starts before the frame is
 * whole; the whole frame follows as before.
 */
/** When the browser received the packet of @p frame, on the time origin's
 * clock (ms), or -1. */
function receivedAt(frame) {
    try {
        const meta = frame.getMetadata();
        if (meta && typeof meta.receiveTime === 'number')
            return performance.timeOrigin + meta.receiveTime;
    } catch {
        // No metadata here.
    }
    return -1;
}

function audioRoad(reader, outMid, giveUpMs = GIVE_UP_MS, repair = true, partBytes = 0) {
    const ordered = outMid === 'video';
    const tag = ordered ? 'v' : 'u';
    const open = new Map(); // seq → frame being put together
    const ready = new Map(); // seq → complete video frame waiting for an older one
    let lastDone = -1;
    let lost = false;
    let gapTimer = 0;
    let hiSeq = -1; // the newest seq a chunk came with
    const holes = new Map(); // seq → {at, tries}: a frame asked for whole
    let reaskTimer = 0;
    // The ack: the last chunk received, not yet named back.
    let ackS = -1;
    let ackI = 0;
    let ackBytes = 0;
    let ackAt = 0;
    let ackTimer = 0;
    const sendAck = () => {
        clearTimeout(ackTimer);
        ackTimer = 0;
        if (ackS < 0) return;
        self.postMessage({ mid: outMid, ack: { t: tag, s: ackS, i: ackI } });
        ackS = -1;
        ackBytes = 0;
        ackAt = performance.now();
    };
    const noteChunk = (seq, idx, bytes) => {
        ackS = seq;
        ackI = idx;
        ackBytes += bytes;
        if (ackBytes >= ACK_BYTES || performance.now() - ackAt >= ACK_MS) sendAck();
        else if (!ackTimer) ackTimer = setTimeout(sendAck, ACK_MS);
    };
    // Whether seq a comes after seq b, on the 16-bit wheel.
    const after = (a, b) => {
        const d = (a - b) & 0xffff;
        return d !== 0 && d < 0x8000;
    };
    const ask = (seq, f, from, to) => {
        const want = [];
        const now = performance.now();
        for (let k = from; k < to; k++)
            if (!f.parts[k] && !f.asked.has(k)) {
                f.asked.set(k, { at: now, tries: 0 });
                want.push(k);
            }
        if (want.length) {
            self.postMessage({ mid: outMid, nack: { t: tag, s: seq, i: want } });
            armReask();
        }
    };
    // Whether seq s is still worth having: newer than the last frame given.
    const wanted = (s) => lastDone < 0 || after(s, lastDone);
    // The asks still unanswered, again; the timer runs while any is left.
    const reask = () => {
        reaskTimer = 0;
        const now = performance.now();
        let pending = false;
        // A frame asked for whole is not asked again: its answer is a whole
        // frame more into a socket that may be full.
        for (const s of holes.keys())
            if (!wanted(s) || open.has(s) || ready.has(s)) holes.delete(s);
        for (const [s, f] of open) {
            const want = [];
            for (const [k, a] of f.asked) {
                if (f.parts[k] || a.tries >= REASK_TRIES) continue;
                pending = true;
                if (now - a.at < REASK_MS) continue;
                a.at = now;
                a.tries++;
                want.push(k);
            }
            if (want.length)
                self.postMessage({ mid: outMid, nack: { t: tag, s, i: want, reask: 1 } });
        }
        if (pending) reaskTimer = setTimeout(reask, REASK_MS);
    };
    function armReask() {
        if (repair && !reaskTimer) reaskTimer = setTimeout(reask, REASK_MS);
    }
    // A hole in the seqs, from the newest seen to @p seq: those frames came
    // with no chunk at all, asked for whole.
    const askHoles = (seq) => {
        if (hiSeq >= 0 && after(seq, hiSeq)) {
            const gap = (seq - hiSeq - 1) & 0xffff;
            if (repair && gap > 0 && gap <= HOLE_MAX) {
                const now = performance.now();
                for (let k = 1; k <= gap; k++) {
                    const m = (hiSeq + k) & 0xffff;
                    if (!wanted(m) || open.has(m) || ready.has(m) || holes.has(m)) continue;
                    holes.set(m, { at: now, tries: 0 });
                    self.postMessage({ mid: outMid, nack: { t: tag, s: m, i: [], all: 1 } });
                }
                armReask();
            }
        }
        if (hiSeq < 0 || after(seq, hiSeq)) hiSeq = seq;
    };
    // The front of frame @p f that came since its last piece, as a piece.
    const postPart = (f) => {
        let bytes = 0;
        let end = f.sentIdx;
        while (end < f.parts.length && f.parts[end]) bytes += f.parts[end++].byteLength;
        if (end === f.parts.length || bytes < partBytes) return;
        const data = new Uint8Array(bytes);
        let at = 0;
        for (let k = f.sentIdx; k < end; k++) {
            data.set(f.parts[k], at);
            at += f.parts[k].byteLength;
        }
        self.postMessage(
            { mid: outMid, part: { fid: f.fid, off: f.sentBytes }, data: data.buffer },
            [data.buffer],
        );
        f.sentIdx = end;
        f.sentBytes += bytes;
    };
    const post = (seq, f) => {
        const data = new Uint8Array(f.bytes);
        let at = 0;
        for (const p of f.parts) {
            data.set(p, at);
            at += p.byteLength;
        }
        lastDone = seq;
        for (const s of open.keys()) if (!after(s, seq)) open.delete(s);
        const now = performance.timeOrigin + performance.now();
        self.postMessage(
            {
                mid: outMid,
                data: data.buffer,
                key: f.key,
                ts: f.ts >>> 0,
                at: now,
                held: f.held,
                w0: f.w0,
                rx0: f.rx0,
                rxN: f.rxN,
                lost,
                fid: f.fid,
            },
            [data.buffer],
        );
        lost = false;
    };
    const flush = () => {
        for (;;) {
            const next = (lastDone + 1) & 0xffff;
            const f = ready.get(next);
            if (!f) break;
            ready.delete(next);
            post(next, f);
        }
        clearTimeout(gapTimer);
        gapTimer = ready.size ? setTimeout(giveUp, giveUpMs) : 0;
    };
    const giveUp = () => {
        gapTimer = 0;
        let oldest = -1;
        for (const s of ready.keys()) if (oldest < 0 || after(oldest, s)) oldest = s;
        if (oldest < 0) return;
        lost = true;
        lastDone = (oldest - 1) & 0xffff;
        flush();
    };
    const pump = () =>
        reader.read().then(({ value: frame, done }) => {
            if (done || !frame) return;
            const buf = frame.data;
            const dv = new DataView(buf);
            if (buf.byteLength < HEAD || dv.getUint8(0) !== 0x4d) return pump();
            const seq = dv.getUint16(2);
            const idx = dv.getUint16(4);
            const count = dv.getUint16(6);
            // Every chunk read counts, a late or doubled one too: it left the network.
            noteChunk(seq, idx, buf.byteLength);
            askHoles(seq);
            holes.delete(seq);
            // Not newer than the last frame given: its time is gone.
            if (lastDone >= 0 && !after(seq, lastDone)) return pump();
            if (ready.has(seq)) return pump();
            let f = open.get(seq);
            if (!f) {
                f = {
                    parts: new Array(count),
                    got: 0,
                    bytes: 0,
                    hi: -1,
                    asked: new Map(), // index → {at, tries}
                    key: (dv.getUint8(1) & 1) === 1,
                    fid: dv.getUint32(8),
                    sentIdx: 0, // chunks given as pieces (partBytes)
                    sentBytes: 0,
                    // The first chunk read here, and when the browser got it
                    // (POC Ultra P-B: the way down, split; -1 if unknown).
                    w0: performance.timeOrigin + performance.now(),
                    rx0: receivedAt(frame),
                };
                open.set(seq, f);
                // A newer frame started: what an older one still lacks is lost.
                for (const [s, o] of open)
                    if (o !== f && after(seq, s)) ask(s, o, 0, o.parts.length);
            }
            if (!f.parts[idx]) {
                f.parts[idx] = new Uint8Array(buf, HEAD);
                f.got++;
                f.bytes += buf.byteLength - HEAD;
            }
            if (idx > f.hi + 1) ask(seq, f, f.hi + 1, idx);
            if (idx > f.hi) f.hi = idx;
            if (f.got < count) {
                if (partBytes > 0) postPart(f);
                return pump();
            }
            open.delete(seq);
            f.held = -1;
            f.rxN = -1;
            f.ts = frame.timestamp;
            try {
                const meta = frame.getMetadata();
                if (meta && typeof meta.rtpTimestamp === 'number') f.ts = meta.rtpTimestamp;
                if (meta && typeof meta.receiveTime === 'number') {
                    f.held = performance.now() - meta.receiveTime;
                    f.rxN = performance.timeOrigin + meta.receiveTime;
                }
            } catch {
                // No metadata: the figures stay empty.
            }
            if (!ordered || lastDone < 0 || seq === ((lastDone + 1) & 0xffff)) {
                post(seq, f);
                if (ordered) flush();
            } else {
                ready.set(seq, f);
                if (!gapTimer) gapTimer = setTimeout(giveUp, giveUpMs);
            }
            return pump();
        });
    return pump();
}

self.onrtctransform = (event) => {
    const transformer = event.transformer;
    const mid = (transformer.options && transformer.options.mid) || 'video';
    const reader = transformer.readable.getReader();
    self.postMessage({ mid, ready: true });
    // "uaudio": the bench's Ultra stream on the same road, posted without
    // the VP8 header of its video track.
    if (mid === 'vaudio' || mid === 'uaudio') {
        let giveUpMs = (transformer.options && transformer.options.giveUpMs) || GIVE_UP_MS;
        const repair = !(transformer.options && transformer.options.reask === false);
        if (!(transformer.options && transformer.options.giveUpMs) && repair)
            giveUpMs = GIVE_UP_REPAIR_MS;
        const partBytes = (transformer.options && transformer.options.partBytes) || 0;
        audioRoad(
            reader,
            mid === 'vaudio' ? 'video' : 'ultraraw',
            giveUpMs,
            repair,
            partBytes,
        ).catch((e) => self.postMessage({ mid, error: String(e && e.message ? e.message : e) }));
        return;
    }
    const pump = () =>
        reader.read().then(({ value: frame, done }) => {
            if (done || !frame) return;
            let ts = frame.timestamp;
            let held = -1;
            try {
                const meta = frame.getMetadata();
                if (meta && typeof meta.rtpTimestamp === 'number') ts = meta.rtpTimestamp;
                // The browser's own wait, last packet in to this transform.
                if (meta && typeof meta.receiveTime === 'number')
                    held = performance.now() - meta.receiveTime;
            } catch {
                // Older engines: the frame's own timestamp is the RTP one.
            }
            const data = frame.data;
            const at = performance.timeOrigin + performance.now();
            self.postMessage({ mid, data, key: frame.type === 'key', ts: ts >>> 0, at, held }, [
                data,
            ]);
            return pump();
        });
    pump().catch((e) => self.postMessage({ mid, error: String(e && e.message ? e.message : e) }));
};
