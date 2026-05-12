/**
 * AV1 OBU utilities for WebCodecs-based decoding.
 *
 * AV1 uses Open Bitstream Units (OBUs) instead of H.264/HEVC NAL units.
 * OBUs have their own framing with built-in length fields, so no start
 * code parsing or AVCC-like conversion is needed.  The VideoDecoder
 * expects raw OBU data directly.
 *
 * Codec string format (per ISO 14496-15 AV1 mapping):
 *   av01.P.LLL.DD
 *     P   = profile (0=Main, 1=High, 2=Professional)
 *     LLL = seq_level_idx as 3-digit tier+M or H
 *     DD  = BitDepth - 8 (08=8-bit, 10=10-bit)
 */

export const CODEC_AV1 = 'av1';

/**
 * Sequence Header OBU type.
 * OBU header type field is the lower 4 bits of the first byte
 * when obu_extension_flag = 0.
 * Type 1 = Sequence Header OBU
 */
const OBU_SEQUENCE_HEADER = 1;

/**
 * AV1 codec strings to try, ordered by likelihood for 1080p60 streaming
 * with Sunshine (Main profile 8-bit, various levels).
 *
 * Each entry is also tried without a description because AV1 decoders
 * can self-configure from the in-band sequence header OBU.
 */
export const AV1_FALLBACK_CODEC_STRINGS = [
    'av01.0.08M.08',   // Profile 0 (Main), Level 4.0,  8-bit — most common 1080p60
    'av01.0.09M.08',   // Profile 0 (Main), Level 4.1,  8-bit
    'av01.0.10M.08',   // Profile 0 (Main), Level 5.0,  8-bit (4K-safe)
    'av01.0.12M.08',   // Profile 0 (Main), Level 5.1,  8-bit
    'av01.0.08M.10',   // Profile 0 (Main), Level 4.0, 10-bit (HDR)
    'av01.0.09M.10',   // Profile 0 (Main), Level 4.1, 10-bit (HDR)
    'av01.0.10M.10',   // Profile 0 (Main), Level 5.0, 10-bit (HDR)
];

/**
 * Extract the OBU type from the first byte of an OBU.
 *
 * AV1 OBU header byte:
 *   bit 0-3: obu_type
 *   bit 4:   obu_extension_flag
 *   bit 5:   obu_has_size_field
 *   bit 6-7: reserved (obu_reserved_1bit)
 */
export function getObuType(firstByte) {
    return firstByte & 0x0F;
}

/**
 * Heuristic: is this buffer likely AV1 OBU data (not Annex B)?
 *
 * AV1 data does NOT start with 0x00 0x00 0x01 or 0x00 0x00 0x00 0x01
 * Annex B start codes.  Instead, the first byte is an OBU header where
 * the lower 4 bits give the OBU type (0 = Reserved, 1 = Sequence Header).
 */
export function isAv1Buffer(data) {
    if (!data || data.length < 4) return false;

    // Annex B start codes indicate H.264/HEVC
    if (data[0] === 0x00 && data[1] === 0x00) {
        if (data[2] === 0x01) return false;          // 3-byte start code
        if (data[2] === 0x00 && data[3] === 0x01) return false; // 4-byte start code
    }

    // If it doesn't have an Annex B start code, it's likely AV1 or raw
    return true;
}

/**
 * Find the first Sequence Header OBU in a buffer.
 *
 * Returns the complete OBU (header + size + payload) or null.
 * AV1 keyframes always contain a Sequence Header OBU at the start.
 */
export function findSequenceHeader(data) {
    if (!data || data.length < 2) return null;

    let offset = 0;
    while (offset < data.length - 1) {
        const obuHeader = data[offset];
        const obuType = obuHeader & 0x0F;
        const hasExtension = (obuHeader & 0x10) !== 0;
        const hasSizeField = (obuHeader & 0x20) !== 0;

        // OBU header length: 1 byte + optional extension (1 byte)
        let hdrLen = 1;
        if (hasExtension) hdrLen++;

        if (offset + hdrLen > data.length) break;

        let obuSize = 0;
        let sizeLen = 0;

        if (hasSizeField) {
            // Read leb128 size field
            let pos = offset + hdrLen;
            let value = 0;
            let leb128Len = 0;
            while (pos < data.length) {
                const lebByte = data[pos];
                value |= (lebByte & 0x7F) << (leb128Len * 7);
                leb128Len++;
                if (!(lebByte & 0x80)) break;
                pos++;
            }
            if (pos >= data.length && (data[data.length - 1] & 0x80)) break;
            obuSize = value;
            sizeLen = leb128Len;
        } else {
            // No size field — OBU extends to end of data or is the last OBU
            // This is uncommon in our pipeline; skip this OBU.
            // Without a size field we can't know where it ends, so abort.
            break;
        }

        const totalObuLen = hdrLen + sizeLen + obuSize;

        if (obuType === OBU_SEQUENCE_HEADER) {
            return data.slice(offset, offset + totalObuLen);
        }

        offset += totalObuLen;
    }

    return null;
}

/**
 * Build a VideoDecoderConfig for AV1.
 *
 * Returns a config object with:
 *   - codec string from the provided list (first supported one)
 *   - optional description (sequence header OBU)
 *   - codedWidth/codedHeight set to 1920x1080
 *
 * If no codec string is given, uses AV1_FALLBACK_CODEC_STRINGS.
 */
export function buildAv1DecoderConfigs(seqHeaderObu = null) {
    // seqHeaderObu from findSequenceHeader() is a data.slice() result,
    // which already has its own standalone ArrayBuffer.
    const desc = seqHeaderObu ? seqHeaderObu.buffer : undefined;

    return AV1_FALLBACK_CODEC_STRINGS.map(codecStr => {
        const cfg = {
            codec: codecStr,
            codedWidth: 1920,
            codedHeight: 1080,
            optimizeForLatency: true,
        };
        if (desc) {
            cfg.description = desc;
        }
        return cfg;
    });
}
