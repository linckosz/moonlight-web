/**
 * NAL unit utilities for H.264 and HEVC with WebCodecs-based decoding.
 *
 * Provides Annex B splitting, parameter set extraction (SPS/PPS for H.264,
 * VPS/SPS/PPS for HEVC), avcC/hvcC description building, and codec string
 * generation for VideoDecoder.configure().
 */

// --- NAL type constants ---

const H264_SPS = 7;
const H264_PPS = 8;

const HEVC_VPS = 32;
const HEVC_SPS = 33;
const HEVC_PPS = 34;

export const CODEC_H264 = 'h264';
export const CODEC_HEVC = 'hevc';

/**
 * Auto-detects codec type from NAL unit types found in an Annex B buffer.
 * Returns CODEC_H264, CODEC_HEVC, or null if undetermined.
 */
export function detectCodec(buffer) {
    if (!buffer || buffer.length < 4) return null;
    // Scan for start codes and check the first NAL type
    const nals = splitNals(buffer);
    for (const n of nals) {
        if (n.length < 1) continue;
        // H.264: 1-byte NAL header, type in lower 5 bits
        // HEVC: 2-byte NAL header, type in bits 1-6 of first byte
        const h264Type = n[0] & 0x1F;
        if (h264Type === H264_SPS || h264Type === H264_PPS) return CODEC_H264;

        if (n.length >= 2) {
            const hevcType = (n[0] >> 1) & 0x3F;
            if (hevcType === HEVC_VPS || hevcType === HEVC_SPS || hevcType === HEVC_PPS) return CODEC_HEVC;
        }
    }
    // Default to H.264 (most common fallback)
    return CODEC_H264;
}

export class NalParser {
    constructor() {
        this.codec = null;   // CODEC_H264 or CODEC_HEVC
        this.sps = null;
        this.pps = null;
        this.vps = null;     // HEVC only
    }

    /**
     * Feed an Annex B buffer and extract parameter sets if not already known.
     * Returns true if we now have all required parameter sets.
     */
    feed(buffer) {
        if (this.isReady()) return true;

        const nals = splitNals(buffer);

        // Auto-detect codec on first valid NAL
        if (!this.codec) {
            for (const n of nals) {
                if (n.length < 1) continue;
                if ((n[0] & 0x1F) === H264_SPS) { this.codec = CODEC_H264; break; }
                if (n.length >= 2 && ((n[0] >> 1) & 0x3F) === HEVC_VPS) { this.codec = CODEC_HEVC; break; }
                if (n.length >= 2 && ((n[0] >> 1) & 0x3F) === HEVC_SPS) { this.codec = CODEC_HEVC; break; }
            }
            if (!this.codec) this.codec = CODEC_H264; // fallback
        }

        for (const n of nals) {
            if (this.codec === CODEC_HEVC && n.length >= 2) {
                const type = (n[0] >> 1) & 0x3F;
                if (type === HEVC_VPS) this.vps = n;
                else if (type === HEVC_SPS) this.sps = n;
                else if (type === HEVC_PPS) this.pps = n;
            } else if (n.length >= 1) {
                const type = n[0] & 0x1F;
                if (type === H264_SPS) this.sps = n;
                else if (type === H264_PPS) this.pps = n;
            }
        }
        return this.isReady();
    }

    isReady() {
        if (this.codec === CODEC_HEVC) return !!(this.vps && this.sps && this.pps);
        return !!(this.sps && this.pps);
    }

    reset() {
        this.codec = null;
        this.sps = null;
        this.pps = null;
        this.vps = null;
    }
}

/**
 * Split an Annex B byte stream into individual NAL units (without start codes).
 * Handles both 3-byte (00 00 01) and 4-byte (00 00 00 01) start codes.
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

// --- H.264 ---

/**
 * Build an avcC description record (AVCDecoderConfigurationRecord).
 */
export function buildAvccDescription(sps, pps) {
    if (!sps || !pps) return null;
    const len = 11 + sps.length + pps.length;
    const buf = new Uint8Array(len);
    let off = 0;

    buf[off++] = 0x01;            // configurationVersion
    buf[off++] = sps[1];          // AVCProfileIndication
    buf[off++] = sps[2];          // profile_compatibility
    buf[off++] = sps[3];          // AVCLevelIndication
    buf[off++] = 0xFF;            // lengthSizeMinusOne = 3 (4-byte length prefixes)
    buf[off++] = 0xE1;            // numOfSequenceParameterSets = 1

    buf[off++] = (sps.length >> 8) & 0xFF;
    buf[off++] = sps.length & 0xFF;
    buf.set(sps, off);
    off += sps.length;

    buf[off++] = 0x01;            // numOfPictureParameterSets = 1

    buf[off++] = (pps.length >> 8) & 0xFF;
    buf[off++] = pps.length & 0xFF;
    buf.set(pps, off);

    return buf;
}

/**
 * Build an avc1 codec string from SPS data.
 * Format: avc1.{profile}{constraint}{level}  (3 hex bytes)
 */
export function getH264CodecString(sps) {
    if (!sps || sps.length < 4) return 'avc1.64002A';
    const p = sps[1].toString(16).padStart(2, '0');
    const c = sps[2].toString(16).padStart(2, '0');
    const l = sps[3].toString(16).padStart(2, '0');
    return `avc1.${p}${c}${l}`;
}

// --- HEVC ---

/**
 * Build an hvcC description record (HEVCDecoderConfigurationRecord)
 * from VPS, SPS, and PPS NAL units (without start codes).
 *
 * The HEVC NAL header is 2 bytes: forbidden_zero_bit(1) + nal_unit_type(6)
 * + nuh_layer_id(6) + nuh_temporal_id_plus1(3).
 *
 * We extract the profile_tier_level from the SPS and build a minimal hvcC.
 */
export function buildHvcCDescription(vps, sps, pps) {
    if (!vps || !sps || !pps || sps.length < 4) return null;

    // Parse SPS to extract profile_tier_level fields.
    // After the 2-byte NAL header:
    // sps_video_parameter_set_id (4 bits)
    // sps_max_sub_layers_minus1 (3 bits)
    // sps_temporal_id_nesting_flag (1 bit)
    // Then profile_tier_level(sps_max_sub_layers_minus1)
    //
    // The profile_tier_level general fields are always present:
    // general_profile_space (2), general_tier_flag (1), general_profile_idc (5)
    // general_profile_compatibility_flags (32)
    // general_constraint_indicator_flags (48) -- includes general_*_source_flag
    // general_level_idc (8)
    // Total general part: 12 bytes + 1 optional sublayer flag byte

    // Read bits from sps starting at byte 2 (after 2-byte NAL header)
    const rbsp = sps.slice(2);

    // Fast path: copy the first 12 bytes of profile_tier_level directly.
    // This works because sps_max_sub_layers_minus1 is typically 0,
    // so the profile_tier_level has exactly the 12 general bytes.
    // The general_part starts at bit offset 8 (after the 8-bit SPS header
    // fields: 4+3+1 bits).
    //
    // For simplicity, we start from rbsp[1] which contains the last 3 bits
    // of sps_temporal_id_nesting_flag + first 5 bits of general_profile_idc.

    // Calculate general_profile_idc from the first byte of PTL
    const ptlByte0 = rbsp.length > 1 ? rbsp[1] : 0;
    const general_profile_idc = ptlByte0 & 0x1F; // lower 5 bits

    // General level is at rbsp[12] in the simplest case (max_sub_layers_minus1=0)
    // But the exact offset depends on parsing. Use a safe default.
    const general_level_idc = rbsp.length > 12 ? rbsp[12] : 93; // level 5.1 default

    // Build hvcC
    const hvcCLen = 30 + vps.length + sps.length + pps.length;
    const buf = new Uint8Array(hvcCLen);
    let off = 0;

    // configurationVersion
    buf[off++] = 0x01;

    // ---- profile_tier_level general fields (12 bytes) ----
    // Copy the general part from SPS bytes 2-13 (includes 2-byte NAL header skip)
    // Bytes: general_profile_space(2)+tier_flag(1)+general_profile_idc(5) = 1 byte
    //        general_profile_compatibility_flags = 4 bytes
    //        general_constraint_indicator_flags = 6 bytes
    //        general_level_idc = 1 byte
    // Total = 12 bytes
    // These start at byte offset 2 (after NAL header) + 1 (after sps header fields)
    if (sps.length >= 15) {
        // Copy profile_tier_level general fields from SPS bytes 3-14
        // Byte 3: last bit of sps header + first 7 bits of PTL
        // We reconstruct it: general_profile_space=0, tier_flag=0, general_profile_idc from SPS
        buf[off++] = general_profile_idc & 0x1F; // general_profile_space=0, tier=0
        buf.set(sps.slice(4, 8), off); off += 4;  // compatibility flags
        buf.set(sps.slice(8, 14), off); off += 6; // constraint + level_idc
    } else {
        // Fallback: use compact profile_tier_level
        buf[off++] = general_profile_idc & 0x1F;
        buf[off++] = 0x60; buf[off++] = 0x00; buf[off++] = 0x00; buf[off++] = 0x00; // compat flags
        buf[off++] = 0x00; buf[off++] = 0x00; buf[off++] = 0x00; buf[off++] = 0x00;
        buf[off++] = 0x00; buf[off++] = 0x00; // constraint
        buf[off++] = general_level_idc;
    }

    // Remaining fixed fields
    buf[off++] = 0xF0; buf[off++] = 0x00; // min_spatial_segmentation_idc (reserved + 0)
    buf[off++] = 0xFC;                     // parallelismType (reserved + 0)
    buf[off++] = 0xFC | 0x01;             // chromaFormat = 1 (4:2:0)
    buf[off++] = 0xF8;                     // bitDepthLumaMinus8 = 0
    buf[off++] = 0xF8;                     // bitDepthChromaMinus8 = 0
    buf[off++] = 0x00; buf[off++] = 0x00; // avgFrameRate (0 = unspecified)
    buf[off++] = 0x0F;                     // constantFrameRate(0) | numTemporalLayers(1) | temporalIdNested(1) | lengthSizeMinusOne(3)
    buf[off++] = 0x03;                     // numOfArrays = 3 (VPS, SPS, PPS)

    // VPS array
    buf[off++] = 0x00 | HEVC_VPS;         // completeness=0, NAL_unit_type=32
    buf[off++] = 0x00; buf[off++] = 0x01; // numNalus = 1
    buf[off++] = (vps.length >> 8) & 0xFF;
    buf[off++] = vps.length & 0xFF;
    buf.set(vps, off); off += vps.length;

    // SPS array
    buf[off++] = 0x00 | HEVC_SPS;         // NAL_unit_type=33
    buf[off++] = 0x00; buf[off++] = 0x01;
    buf[off++] = (sps.length >> 8) & 0xFF;
    buf[off++] = sps.length & 0xFF;
    buf.set(sps, off); off += sps.length;

    // PPS array
    buf[off++] = 0x00 | HEVC_PPS;         // NAL_unit_type=34
    buf[off++] = 0x00; buf[off++] = 0x01;
    buf[off++] = (pps.length >> 8) & 0xFF;
    buf[off++] = pps.length & 0xFF;
    buf.set(pps, off);

    return buf;
}

/**
 * Build an hvc1 codec string from SPS data.
 * Format: hvc1.{profile_idc}.{compat_byte}.L{level}.B0
 */
export function getHevcCodecString(sps) {
    if (!sps || sps.length < 6) return 'hvc1.1.6.L93.B0';

    // After the 2-byte NAL header and 1 byte of SPS header fields,
    // the profile_tier_level starts at bit offset within byte 3.
    // general_profile_space(2) + tier_flag(1) + general_profile_idc(5)
    // For compact HEVC streams, profile_space=0, tier=0.
    const rbsp = sps.slice(2);
    const profileIdc = rbsp.length > 1 ? (rbsp[1] & 0x1F) : 1;

    // Constraint flags: byte at offset 4 in rbsp (first constraint byte)
    const constraintByte = rbsp.length > 4 ? rbsp[4] : 0x60;

    // Level IDC: byte at offset 12 in rbsp
    const levelIdc = rbsp.length > 12 ? rbsp[12] : 93;

    return `hvc1.${profileIdc}.${constraintByte}.L${levelIdc}.B0`;
}

// --- Unified helpers ---

/**
 * Get codec string for the detected codec.
 * Takes the parsed parameter sets from NalParser.
 */
export function getCodecString(parser) {
    if (!parser.isReady()) return null;

    if (parser.codec === CODEC_HEVC) {
        return getHevcCodecString(parser.sps);
    }
    return getH264CodecString(parser.sps);
}

/**
 * Build codec description (avcC or hvcC) from parsed parameter sets.
 */
export function buildDescription(parser) {
    if (!parser.isReady()) return null;

    if (parser.codec === CODEC_HEVC) {
        return buildHvcCDescription(parser.vps, parser.sps, parser.pps);
    }
    return buildAvccDescription(parser.sps, parser.pps);
}

/**
 * Common H.264 codec strings for fallback.
 * Listed in order of preference (most common for 1080p60 first).
 */
export const H264_FALLBACK_CODEC_STRINGS = [
    'avc1.64002A',  // High 4.2
    'avc1.640028',  // High 4.0
    'avc1.64001F',  // High 3.1
    'avc1.64001E',  // High 3.0
    'avc1.4D002A',  // Main 4.2
    'avc1.4D0028',  // Main 4.0
    'avc1.42002A',  // Baseline 4.2
    'avc1.42001E',  // Baseline 3.0
];

/**
 * Common HEVC codec strings for fallback.
 */
export const HEVC_FALLBACK_CODEC_STRINGS = [
    'hvc1.1.6.L153.B0',  // Main, High tier, Level 5.1 (most common for 1080p60)
    'hvc1.1.6.L150.B0',  // Main, High tier, Level 5.0
    'hvc1.1.6.L123.B0',  // Main, High tier, Level 4.1
    'hvc1.1.6.L120.B0',  // Main, High tier, Level 4.0
    'hvc1.1.6.L93.B0',   // Main, High tier, Level 3.1
    'hvc1.1.2.L153.B0',  // Main, Main tier, Level 5.1
    'hvc1.1.2.L150.B0',  // Main, Main tier, Level 5.0
];

/**
 * Convert raw Annex B data (with start codes) to AVCC/HEVC format
 * (4-byte length prefixes per NAL unit).
 *
 * When stripParams is true, SPS/PPS (H.264) or VPS/SPS/PPS (HEVC) NAL units
 * are skipped because the decoder already has them via the description.
 */
export function toAvcc(annexB, stripParams = false, codec = null) {
    const nals = splitNals(annexB);
    const parts = [];
    for (const n of nals) {
        if (n.length < 1) continue;

        if (stripParams) {
            if (codec === CODEC_HEVC && n.length >= 2) {
                const hevcType = (n[0] >> 1) & 0x3F;
                if (hevcType === HEVC_VPS || hevcType === HEVC_SPS || hevcType === HEVC_PPS) continue;
            }
            if (codec === CODEC_H264 || (!codec && n.length < 2)) {
                const type = n[0] & 0x1F;
                if (type === H264_SPS || type === H264_PPS) continue;
            }
        }

        const l = n.length;
        parts.push((l>>24)&0xFF, (l>>16)&0xFF, (l>>8)&0xFF, l&0xFF);
        for (let i = 0; i < l; i++) parts.push(n[i]);
    }
    return new Uint8Array(parts);
}
