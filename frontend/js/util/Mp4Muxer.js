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
export const CODEC_AV1 = 'av1';

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
        const h264Type = n[0] & 0x1f;
        if (h264Type === H264_SPS || h264Type === H264_PPS) return CODEC_H264;

        if (n.length >= 2) {
            const hevcType = (n[0] >> 1) & 0x3f;
            if (hevcType === HEVC_VPS || hevcType === HEVC_SPS || hevcType === HEVC_PPS)
                return CODEC_HEVC;
        }
    }
    // Default to H.264 (most common fallback)
    return CODEC_H264;
}

export class NalParser {
    constructor() {
        this.codec = null; // CODEC_H264 or CODEC_HEVC
        this.sps = null;
        this.pps = null;
        this.vps = null; // HEVC only
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
                if ((n[0] & 0x1f) === H264_SPS) {
                    this.codec = CODEC_H264;
                    break;
                }
                if (n.length >= 2 && ((n[0] >> 1) & 0x3f) === HEVC_VPS) {
                    this.codec = CODEC_HEVC;
                    break;
                }
                if (n.length >= 2 && ((n[0] >> 1) & 0x3f) === HEVC_SPS) {
                    this.codec = CODEC_HEVC;
                    break;
                }
            }
            if (!this.codec) this.codec = CODEC_H264; // fallback
        }

        for (const n of nals) {
            if (this.codec === CODEC_HEVC && n.length >= 2) {
                const type = (n[0] >> 1) & 0x3f;
                if (type === HEVC_VPS) this.vps = n;
                else if (type === HEVC_SPS) this.sps = n;
                else if (type === HEVC_PPS) this.pps = n;
            } else if (n.length >= 1) {
                const type = n[0] & 0x1f;
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
 * Remove H.264/HEVC emulation prevention bytes (00 00 03) from a NAL unit.
 *
 * Annex B requires inserting 0x03 after any 0x00 0x00 pair when the next
 * byte could form a start code (0x00, 0x01, 0x02, 0x03).  The decoder
 * must remove these before interpreting the RBSP.
 *
 * This function returns a NEW Uint8Array without the 0x03 bytes.
 * It correctly handles the escape case 00 00 03 03 → 00 00 03.
 */
export function removeEmulationPrevention(buffer) {
    const result = [];
    let i = 0;
    while (i < buffer.length) {
        if (
            i + 2 < buffer.length &&
            buffer[i] === 0 &&
            buffer[i + 1] === 0 &&
            buffer[i + 2] === 3
        ) {
            // Skip the emulation prevention byte (0x03), keep the two zeros
            result.push(0, 0);
            i += 3;
        } else {
            result.push(buffer[i]);
            i++;
        }
    }
    return new Uint8Array(result);
}

/**
 * Split an Annex B byte stream into individual NAL units (without start codes).
 * Handles both 3-byte (00 00 01) and 4-byte (00 00 00 01) start codes.
 */
export function splitNals(annexB) {
    const nals = [];
    let i = 0;
    const len = annexB.length;
    while (i < len - 3) {
        if (annexB[i] === 0 && annexB[i + 1] === 0) {
            let sc = 0;
            if (annexB[i + 2] === 1) sc = 3;
            else if (i + 3 < len && annexB[i + 2] === 0 && annexB[i + 3] === 1) sc = 4;
            if (sc) {
                const start = i + sc;
                let end = len;
                for (let j = start; j < len - 3; j++) {
                    if (annexB[j] === 0 && annexB[j + 1] === 0) {
                        if (
                            annexB[j + 2] === 1 ||
                            (j + 3 < len && annexB[j + 2] === 0 && annexB[j + 3] === 1)
                        ) {
                            end = j;
                            break;
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

    buf[off++] = 0x01; // configurationVersion
    buf[off++] = sps[1]; // AVCProfileIndication
    buf[off++] = sps[2]; // profile_compatibility
    buf[off++] = sps[3]; // AVCLevelIndication
    buf[off++] = 0xff; // lengthSizeMinusOne = 3 (4-byte length prefixes)
    buf[off++] = 0xe1; // numOfSequenceParameterSets = 1

    buf[off++] = (sps.length >> 8) & 0xff;
    buf[off++] = sps.length & 0xff;
    buf.set(sps, off);
    off += sps.length;

    buf[off++] = 0x01; // numOfPictureParameterSets = 1

    buf[off++] = (pps.length >> 8) & 0xff;
    buf[off++] = pps.length & 0xff;
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
    if (!vps || !sps || !pps || sps.length < 15) return null;

    // Build hvcC (HEVCDecoderConfigurationRecord) per ISO 14496-15.
    //
    // SPS layout (after 2-byte NAL header, with emulation prevention REMOVED):
    //   byte 2: sps_video_parameter_set_id(4) | sps_max_sub_layers_minus1(3) | temporal_id_nesting(1)
    //   bytes 3-14: profile_tier_level general fields (12 bytes, always present):
    //     byte 3:  general_profile_space(2) | general_tier_flag(1) | general_profile_idc(5)
    //     bytes 4-7:  general_profile_compatibility_flags (4)
    //     bytes 8-13: general_constraint_indicator_flags (6)
    //     byte 14: general_level_idc
    //
    // Fixed hvcC header: 22 bytes (configurationVersion + PTL + fixed fields).
    // Each NAL array entry: 5 (1 type + 2 count + 2 length) + nal_unit.
    //
    // IMPORTANT: The raw NAL units in the bitstream may contain emulation
    // prevention bytes (00 00 03).  The hvcC header PTL fields MUST be read
    // after removing emulation prevention bytes (which shift byte offsets).
    //
    // The NAL arrays in the hvcC, per ISO 14496-15, keep emulation prevention
    // bytes.  However, Chromium's WebCodecs implementation for HEVC validates
    // the hvcC by re-parsing the SPS from the NAL array and comparing the PTL
    // values against the header.  If the raw SPS has emulation prevention bytes
    // in the PTL region, the re-parsed PTL values shift and do NOT match the
    // cleaned header PTL, causing isConfigSupported() to reject the config.
    //
    // Workaround: use de-emulated NAL units in the NAL arrays too.  This is
    // technically non-compliant with ISO 14496-15 but is required for Chrome
    // compatibility when the bitstream has emulation prevention in the PTL
    // byte range (bytes 3-14 of the SPS).

    // Strip emulation prevention from all parameter sets for consistency
    // between the PTL header and the NAL array data.
    const cleanVps = removeEmulationPrevention(vps);
    const cleanSps = removeEmulationPrevention(sps);
    const cleanPps = removeEmulationPrevention(pps);

    // Extract HDR info from clean SPS: chroma_format_idc and bit depth.
    const spsInfo = parseHevcSpsInfo(cleanSps);

    // Fixed header: 1 (version) + 12 (PTL) + 2 + 1 + 1 + 1 + 1 + 2 + 1 + 1 = 23
    // Each NAL array entry: 5 (1 type + 2 count + 2 length) + NAL data
    const hvcCLen = 23 + 5 + cleanVps.length + 5 + cleanSps.length + 5 + cleanPps.length;
    const buf = new Uint8Array(hvcCLen);
    let off = 0;

    // configurationVersion
    buf[off++] = 0x01;

    // profile_tier_level general fields — extract from de-emulated SPS.
    // Emulation prevention bytes (00 00 03) in the SPS would corrupt the
    // fixed-field offsets, so we clean them first.
    buf.set(cleanSps.slice(3, 15), off);
    off += 12;

    // min_spatial_segmentation_idc: reserved(4) + 0
    buf[off++] = 0xf0;
    buf[off++] = 0x00;

    // parallelismType: reserved(6) + 0
    buf[off++] = 0xfc;

    // chromaFormat: reserved(6) + chroma_format_idc (0=monochrome, 1=4:2:0, 2=4:2:2, 3=4:4:4)
    buf[off++] = 0xfc | (spsInfo.chromaFormat & 0x03);

    // bitDepthLumaMinus8: reserved(5) + bitDepthLumaMinus8
    buf[off++] = 0xf8 | (spsInfo.bitDepthLumaMinus8 & 0x07);

    // bitDepthChromaMinus8: reserved(5) + bitDepthChromaMinus8
    buf[off++] = 0xf8 | (spsInfo.bitDepthChromaMinus8 & 0x07);

    // avgFrameRate (0 = unspecified)
    buf[off++] = 0x00;
    buf[off++] = 0x00;

    // constantFrameRate(2)=0 | numTemporalLayers(3)=1 | temporalIdNested(1)=1 | lengthSizeMinusOne(2)=3
    buf[off++] = 0x0f;

    // numOfArrays = 3 (VPS, SPS, PPS)
    buf[off++] = 0x03;

    // VPS array (de-emulated)
    buf[off++] = 0x00 | HEVC_VPS;
    buf[off++] = 0x00;
    buf[off++] = 0x01;
    buf[off++] = (cleanVps.length >> 8) & 0xff;
    buf[off++] = cleanVps.length & 0xff;
    buf.set(cleanVps, off);
    off += cleanVps.length;

    // SPS array (de-emulated)
    buf[off++] = 0x00 | HEVC_SPS;
    buf[off++] = 0x00;
    buf[off++] = 0x01;
    buf[off++] = (cleanSps.length >> 8) & 0xff;
    buf[off++] = cleanSps.length & 0xff;
    buf.set(cleanSps, off);
    off += cleanSps.length;

    // PPS array (de-emulated)
    buf[off++] = 0x00 | HEVC_PPS;
    buf[off++] = 0x00;
    buf[off++] = 0x01;
    buf[off++] = (cleanPps.length >> 8) & 0xff;
    buf[off++] = cleanPps.length & 0xff;
    buf.set(cleanPps, off);

    return buf;
}

/** MSB-first bit reader with Exp-Golomb support (HEVC/H.264 RBSP). */
class SpsBitReader {
    constructor(bytes) {
        this.bytes = bytes;
        this.bitPos = 0;
    }
    u(n) {
        let v = 0;
        for (let i = 0; i < n; i++) {
            const byteIdx = this.bitPos >> 3;
            if (byteIdx >= this.bytes.length) throw new Error('SpsBitReader overrun');
            const bit = (this.bytes[byteIdx] >> (7 - (this.bitPos & 7))) & 1;
            v = (v << 1) | bit;
            this.bitPos++;
        }
        return v;
    }
    ue() {
        let zeros = 0;
        while (this.u(1) === 0) {
            zeros++;
            if (zeros > 31) throw new Error('ue overrun');
        }
        if (zeros === 0) return 0;
        return (1 << zeros) - 1 + this.u(zeros);
    }
}

/**
 * Parse chroma_format_idc and bit depths from a de-emulated HEVC SPS NAL unit
 * (full NAL including the 2-byte header). Returns SDR 4:2:0 8-bit defaults on
 * any parse error.
 *
 * Walks the SPS up to bit_depth_*_minus8 per ITU-T H.265 §7.3.2.2.1, handling
 * the variable-length profile_tier_level and sub-layer ordering loops.
 */
function parseHevcSpsInfo(spsNal) {
    const result = { chromaFormat: 1, bitDepthLumaMinus8: 0, bitDepthChromaMinus8: 0 };
    if (!spsNal || spsNal.length < 16) return result;

    try {
        // Skip the 2-byte HEVC NAL header.
        const br = new SpsBitReader(spsNal.subarray(2));

        br.u(4); // sps_video_parameter_set_id
        const maxSubLayersMinus1 = br.u(3); // sps_max_sub_layers_minus1
        br.u(1); // sps_temporal_id_nesting_flag

        // profile_tier_level(1, maxSubLayersMinus1)
        // general profile/tier/level (88 bits) + general_level_idc (8 bits)
        br.u(2); // general_profile_space
        br.u(1); // general_tier_flag
        br.u(5); // general_profile_idc
        br.u(32); // general_profile_compatibility_flag[32]
        // 48 bits of general constraint/source flags
        br.u(24);
        br.u(24);
        br.u(8); // general_level_idc

        // sub_layer_profile_present_flag / sub_layer_level_present_flag
        const subProfile = [];
        const subLevel = [];
        for (let i = 0; i < maxSubLayersMinus1; i++) {
            subProfile.push(br.u(1));
            subLevel.push(br.u(1));
        }
        if (maxSubLayersMinus1 > 0) {
            for (let i = maxSubLayersMinus1; i < 8; i++) br.u(2); // reserved_zero_2bits
        }
        for (let i = 0; i < maxSubLayersMinus1; i++) {
            if (subProfile[i]) {
                br.u(2);
                br.u(1);
                br.u(5);
                br.u(32);
                br.u(24);
                br.u(24);
            }
            if (subLevel[i]) br.u(8);
        }

        br.ue(); // sps_seq_parameter_set_id
        const chromaFormatIdc = br.ue();
        result.chromaFormat = chromaFormatIdc;
        if (chromaFormatIdc === 3) br.u(1); // separate_colour_plane_flag

        br.ue(); // pic_width_in_luma_samples
        br.ue(); // pic_height_in_luma_samples
        if (br.u(1)) {
            // conformance_window_flag
            br.ue(); // conf_win_left_offset
            br.ue(); // conf_win_right_offset
            br.ue(); // conf_win_top_offset
            br.ue(); // conf_win_bottom_offset
        }

        result.bitDepthLumaMinus8 = br.ue();
        result.bitDepthChromaMinus8 = br.ue();
    } catch (e) {
        // Parse failure: keep SDR 4:2:0 8-bit defaults.
    }

    return result;
}

/**
 * Build an hvc1 codec string from SPS data.
 * Format: hvc1.{profile_idc}.{compat_byte}.L{level}.B0
 */
export function getHevcCodecString(sps) {
    if (!sps || sps.length < 6) return 'hvc1.1.6.L93.B0';

    // The SPS from the bitstream may contain emulation prevention bytes
    // (00 00 03).  These must be removed before reading byte-offset fields
    // because they shift the RBSP layout.  The NAL header (2 bytes) is not
    // affected.
    const rbsp = removeEmulationPrevention(sps.slice(2));

    // After the 2-byte NAL header and 1 byte of SPS header fields:
    // rbsp[0]: sps_video_parameter_set_id(4) | sps_max_sub_layers_minus1(3) | temporal_id_nesting(1)
    // rbsp[1]: general_profile_space(2) + tier_flag(1) + general_profile_idc(5)
    // rbsp[2..5]: general_profile_compatibility_flags (4 bytes)
    // rbsp[6..11]: general_constraint_indicator_flags (6 bytes)
    // rbsp[12]: general_level_idc

    const profileIdc = rbsp.length > 1 ? rbsp[1] & 0x1f : 1;

    // First constraint/feature byte: rbsp[6] = first constraint indicator byte
    const constraintByte = rbsp.length > 6 ? rbsp[6] : 0x60;

    // Level IDC: byte at offset 12 in rbsp
    const levelIdc = rbsp.length > 12 ? rbsp[12] : 93;

    // Validate: level_idc < 30 (3.0) is invalid for HEVC.  This catches
    // the case where emulation prevention shifting produces level=0.
    // A valid level_idc >= 30 (Level 3.0) and <= 186 (Level 6.2).
    if (levelIdc < 30) {
        // Fall back to a safe default: Main, Level 5.1 (common for 1080p60)
        return 'hvc1.1.6.L153.B0';
    }

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
 * Returns true if the HEVC codec string indicates a 10-bit (HDR-capable) profile.
 * Profile IDC 2 = Main10, 4 = Main10 Still Picture, etc.
 */
export function isHevcHdrProfile(codecString) {
    if (!codecString) return false;
    // hvc1.2.x, hvc1.4.x, hev1.2.x, hev1.4.x → Main10
    return /^hvc1\.[24]\./.test(codecString) || /^hev1\.[24]\./.test(codecString);
}

/**
 * Common H.264 codec strings for fallback.
 * Listed in order of preference (most common for 1080p60 first).
 */
export const H264_FALLBACK_CODEC_STRINGS = [
    'avc1.64002A', // High 4.2
    'avc1.640028', // High 4.0
    'avc1.64001F', // High 3.1
    'avc1.64001E', // High 3.0
    'avc1.4D002A', // Main 4.2
    'avc1.4D0028', // Main 4.0
    'avc1.42002A', // Baseline 4.2
    'avc1.42001E', // Baseline 3.0
];

/**
 * Common HEVC codec strings for fallback (hvc1 — AVCC format with description).
 * HDR Main10 profiles are listed first (hvc1.2.x, hev1.2.x) so that the
 * primary codec string is HDR when negotiated.
 */
export const HEVC_FALLBACK_CODEC_STRINGS = [
    'hvc1.2.4.L153.B0', // Main10 (HDR), High tier, Level 5.1
    'hvc1.2.4.L150.B0', // Main10 (HDR), High tier, Level 5.0
    'hvc1.1.6.L153.B0', // Main, High tier, Level 5.1 (most common for 1080p60)
    'hvc1.1.6.L150.B0', // Main, High tier, Level 5.0
    'hvc1.1.6.L123.B0', // Main, High tier, Level 4.1
    'hvc1.1.6.L120.B0', // Main, High tier, Level 4.0
    'hvc1.1.6.L93.B0', // Main, High tier, Level 3.1
    'hvc1.1.2.L153.B0', // Main, Main tier, Level 5.1
    'hvc1.1.2.L150.B0', // Main, Main tier, Level 5.0
];

/**
 * HEVC codec strings with hev1 brand — used for no-description configs.
 * hev1 allows Annex B stream data (start codes), which Chromium's
 * keyframe validator (AnalyzeAnnexB) requires.
 */
export const HEVC_ANNEXB_CODEC_STRINGS = [
    'hev1.2.4.L153.B0', // Main10 (HDR), High tier, Level 5.1
    'hev1.2.4.L150.B0', // Main10 (HDR), High tier, Level 5.0
    'hev1.1.6.L153.B0',
    'hev1.1.6.L150.B0',
    'hev1.1.6.L123.B0',
    'hev1.1.6.L120.B0',
    'hev1.1.6.L93.B0',
    'hev1.1.2.L153.B0',
    'hev1.1.2.L150.B0',
];

/**
 * Convert raw Annex B data (with start codes) to the format expected by
 * the VideoDecoder's EncodedVideoChunk.
 *
 * Two output modes:
 *   Annex B (useAnnexB=true):  4-byte start codes, all NALs kept.
 *      Used when no codec description is provided — the decoder
 *      auto-detects the format.  Also required by Chromium's keyframe
 *      validator (AnalyzeAnnexB) which only handles Annex B.
 *   AVCC (useAnnexB=false):    4-byte big-endian length prefix per NAL unit.
 *      Used when a codec description (avcC/hvcC) is provided.
 *
 * When stripParams is true AND useAnnexB is false, SPS/PPS (H.264) or
 * VPS/SPS/PPS + pre-IRAP NALs (HEVC) are stripped — the decoder already
 * has them via the description.  The pre-IRAP strip is CRITICAL for
 * Chrome/Edge which validate that the first NAL in AVCC data is an IRAP.
 */
export function toAvcc(annexB, stripParams = false, codec = null, useAnnexB = false) {
    const nals = splitNals(annexB);
    // Two-pass build: select the NALs to keep, then copy them into one
    // preallocated buffer with set() (memcpy). This runs per frame on the
    // decode path — appending byte-by-byte to a dynamic Array costs
    // milliseconds on large keyframes and thrashes the GC on mobile.
    const kept = [];
    let total = 0;
    let hevcFoundIrap = false;

    for (const n of nals) {
        if (n.length < 1) continue;

        // Stripping logic: only active for AVCC mode (not Annex B).
        // In Annex B mode, all NALs are kept — the decoder auto-detects
        // the format and AnalyzeAnnexB can parse start codes directly.
        if (stripParams && !useAnnexB) {
            if (codec === CODEC_HEVC && n.length >= 2) {
                const hevcType = (n[0] >> 1) & 0x3f;
                // Strip VPS/SPS/PPS (always safe — they're in the description)
                if (hevcType === HEVC_VPS || hevcType === HEVC_SPS || hevcType === HEVC_PPS)
                    continue;
                // Strip non-IRAP NALs before the first IRAP (types 16-21).
                // Chrome/Edge check the first NAL in AVCC data: if it's not
                // an IRAP, decode() synchronously rejects the chunk.
                if (!hevcFoundIrap) {
                    const isIrap = hevcType >= 16 && hevcType <= 21;
                    if (!isIrap) continue;
                    hevcFoundIrap = true;
                }
            }
            if (codec === CODEC_H264 || (!codec && n.length < 2)) {
                const type = n[0] & 0x1f;
                if (type === H264_SPS || type === H264_PPS) continue;
            }
        }

        kept.push(n);
        total += 4 + n.length; // 4-byte start code or length prefix + NAL
    }

    if (kept.length === 0 && nals.length > 0) {
        console.warn(
            '[toAvcc] PARTS IS EMPTY after stripping ' +
                nals.length +
                ' NALs, ' +
                'stripParams=' +
                stripParams +
                ' codec=' +
                codec +
                ' useAnnexB=' +
                useAnnexB,
        );
    }

    const out = new Uint8Array(total);
    let off = 0;
    for (const n of kept) {
        const l = n.length;
        if (useAnnexB && codec === CODEC_HEVC) {
            // Annex B: 4-byte start codes
            out[off + 3] = 1; // bytes 0-2 already zero
        } else {
            // AVCC: 4-byte big-endian length prefix
            out[off] = (l >> 24) & 0xff;
            out[off + 1] = (l >> 16) & 0xff;
            out[off + 2] = (l >> 8) & 0xff;
            out[off + 3] = l & 0xff;
        }
        out.set(n, off + 4);
        off += 4 + l;
    }

    return out;
}
