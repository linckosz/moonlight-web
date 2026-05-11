/**
 * Minimal H.264 NAL unit utilities for WebCodecs-based decoding.
 *
 * Provides Annex B splitting, SPS/PPS extraction, and avcC description
 * building for VideoDecoder.configure(). No fMP4 muxing -- WebCodecs
 * consumes raw Annex B directly.
 */

export class NalParser {
    constructor() {
        this.sps = null;
        this.pps = null;
    }

    /**
     * Feed an Annex B buffer and extract SPS/PPS if not already known.
     * Returns true if we now have both SPS and PPS.
     */
    feed(buffer) {
        const nals = splitNals(buffer);
        for (const n of nals) {
            const type = n[0] & 0x1F;
            if (type === 7) this.sps = n;
            else if (type === 8) this.pps = n;
        }
        return !!(this.sps && this.pps);
    }

    isReady() {
        return !!(this.sps && this.pps);
    }
}

/**
 * Split an Annex B byte stream (with 00 00 01 or 00 00 00 01 start codes)
 * into individual NAL units (without start codes).
 */
export function splitNals(annexB) {
    const nals = [];
    let i = 0, len = annexB.length;
    while (i < len - 3) {
        if (annexB[i] === 0 && annexB[i+1] === 0) {
            let sc = 0;
            if (annexB[i+2] === 1) sc = 3;
            else if (i+3 < len && annexB[i+2] === 0 && annexB[i+3] === 1) sc = 4;
            if (sc) {
                const start = i + sc;
                let end = len;
                for (let j = start; j < len - 3; j++) {
                    if (annexB[j] === 0 && annexB[j+1] === 0) {
                        if (annexB[j+2] === 1 ||
                            (j+3 < len && annexB[j+2] === 0 && annexB[j+3] === 1)) {
                            end = j; break;
                        }
                    }
                }
                nals.push(annexB.slice(start, end));
                i = end;
                continue;
            }
        }
        i++;
    }
    return nals;
}

/**
 * Build an avcC description record for VideoDecoderConfig.description.
 * sps and pps are raw NAL unit data WITHOUT start code prefixes.
 */
export function buildAvccDescription(sps, pps) {
    if (!sps || !pps) return null;
    const len = 7 + 2 + sps.length + 1 + 2 + pps.length;
    const buf = new Uint8Array(len);
    let off = 0;

    buf[off++] = 0x01;            // configurationVersion
    buf[off++] = sps[1];          // AVCProfileIndication
    buf[off++] = sps[2];          // profile_compatibility
    buf[off++] = sps[3];          // AVCLevelIndication
    buf[off++] = 0xFF;            // lengthSizeMinusOne = 3 (4-byte length prefixes)
    buf[off++] = 0xE1;            // 1 SPS

    buf[off++] = (sps.length >> 8) & 0xFF;
    buf[off++] = sps.length & 0xFF;
    buf.set(sps, off);
    off += sps.length;

    buf[off++] = 0x01;            // 1 PPS

    buf[off++] = (pps.length >> 8) & 0xFF;
    buf[off++] = pps.length & 0xFF;
    buf.set(pps, off);
    off += pps.length;

    return buf;
}

/**
 * Build an avc1 codec string from SPS data.
 * Format: avc1.{profile}{constraint}{level}  (each 2 hex digits)
 */
export function getCodecString(sps) {
    if (!sps || sps.length < 4) return 'avc1.64002A'; // fallback High 4.2
    const p = sps[1].toString(16).padStart(2, '0');
    const c = sps[2].toString(16).padStart(2, '0');
    const l = sps[3].toString(16).padStart(2, '0');
    return `avc1.${p}${c}${l}`;
}

/**
 * Convert raw Annex B data (with start codes) to AVCC format
 * (4-byte length prefixes per NAL unit). Useful if the decoder
 * requires AVCC format instead of Annex B.
 */
export function toAvcc(annexB) {
    const nals = splitNals(annexB);
    const parts = [];
    for (const n of nals) {
        const l = n.length;
        parts.push((l>>24)&0xFF, (l>>16)&0xFF, (l>>8)&0xFF, l&0xFF);
        for (let i = 0; i < l; i++) parts.push(n[i]);
    }
    return new Uint8Array(parts);
}
