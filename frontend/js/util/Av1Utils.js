/**
 * AV1 OBU utilities for WebCodecs-based decoding.
 *
 * AV1 uses Open Bitstream Units (OBUs) instead of H.264/HEVC NAL units.
 * OBUs have their own framing with built-in length fields, so no start
 * code parsing or AVCC-like conversion is needed.  The VideoDecoder
 * expects raw OBU data directly.
 *
 * Codec string format (per ISO 14496-15 AV1 mapping):
 *   av01.P.LLT.DD
 *     P  = profile (0=Main, 1=High, 2=Professional)
 *     LL = seq_level_idx (2 digits), T = tier (M=Main, H=High)
 *     DD = bit depth (08, 10, 12)
 */

export const CODEC_AV1 = 'av1';

/**
 * OBU header byte layout (AV1 spec 5.3.2), MSB first:
 *   bit 7:    obu_forbidden_bit
 *   bits 6-3: obu_type
 *   bit 2:    obu_extension_flag
 *   bit 1:    obu_has_size_field
 *   bit 0:    obu_reserved_1bit
 */
const OBU_SEQUENCE_HEADER = 1;
const OBU_TEMPORAL_DELIMITER = 2;
const OBU_PADDING = 15;

/**
 * AV1 codec strings to try, ordered by likelihood for 1080p60 streaming
 * with Sunshine (Main profile 8-bit, various levels).  Used as fallbacks
 * when the exact string cannot be derived from the Sequence Header OBU.
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

/** Extract the OBU type from the first byte of an OBU header (bits 6-3). */
export function getObuType(firstByte) {
    return (firstByte >> 3) & 0x0F;
}

/**
 * Heuristic: is this buffer likely AV1 OBU data (not Annex B)?
 *
 * AV1 data does NOT start with 0x00 0x00 0x01 or 0x00 0x00 0x00 0x01
 * Annex B start codes.
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
 * AV1 keyframes start with a Temporal Delimiter OBU followed by the
 * Sequence Header OBU, so we must walk past non-matching OBUs.
 */
export function findSequenceHeader(data) {
    if (!data || data.length < 2) return null;

    let offset = 0;
    while (offset < data.length - 1) {
        const obuHeader = data[offset];
        const obuType = (obuHeader >> 3) & 0x0F;
        const hasExtension = (obuHeader >> 2) & 0x01;
        const hasSizeField = (obuHeader >> 1) & 0x01;

        // OBU header length: 1 byte + optional extension (1 byte)
        let hdrLen = 1;
        if (hasExtension) hdrLen++;

        if (offset + hdrLen >= data.length) break;

        if (!hasSizeField) {
            // Without a size field we can't know where the OBU ends — abort.
            break;
        }

        // Read leb128 size field
        let pos = offset + hdrLen;
        let obuSize = 0;
        let sizeLen = 0;
        let sizeComplete = false;
        while (pos < data.length && sizeLen < 8) {
            const lebByte = data[pos];
            obuSize |= (lebByte & 0x7F) << (sizeLen * 7);
            sizeLen++;
            pos++;
            if (!(lebByte & 0x80)) { sizeComplete = true; break; }
        }
        if (!sizeComplete) break;

        const totalObuLen = hdrLen + sizeLen + obuSize;

        if (obuType === OBU_SEQUENCE_HEADER) {
            return data.slice(offset, offset + totalObuLen);
        }

        offset += totalObuLen;
    }

    return null;
}

/**
 * Strip Temporal Delimiter and Padding OBUs from a temporal unit.
 *
 * The WebCodecs AV1 registration uses the ISOBMFF low-overhead sample
 * format, where TD/padding OBUs must not be present.  Software decoders
 * (dav1d in Chrome) tolerate them, but Safari feeds chunks to VideoToolbox
 * which rejects the whole frame ("Decoder failure" on every keyframe).
 *
 * Returns the input untouched (no copy) when nothing needs stripping or
 * when the OBU stream cannot be parsed safely.
 */
export function stripNonEssentialObus(data) {
    if (!data || data.length < 1) return data;

    let offset = 0;
    const kept = [];      // [start, end] ranges of OBUs to keep
    let removed = false;

    while (offset < data.length) {
        const obuHeader = data[offset];
        const obuType = (obuHeader >> 3) & 0x0F;
        const hasExtension = (obuHeader >> 2) & 0x01;
        const hasSizeField = (obuHeader >> 1) & 0x01;

        const hdrLen = 1 + (hasExtension ? 1 : 0);
        // Without a size field the OBU extent is unknown — bail out untouched.
        if (!hasSizeField || offset + hdrLen >= data.length) return data;

        let pos = offset + hdrLen;
        let obuSize = 0;
        let sizeLen = 0;
        let sizeComplete = false;
        while (pos < data.length && sizeLen < 8) {
            const lebByte = data[pos];
            obuSize |= (lebByte & 0x7F) << (sizeLen * 7);
            sizeLen++;
            pos++;
            if (!(lebByte & 0x80)) { sizeComplete = true; break; }
        }
        if (!sizeComplete) return data;

        const totalObuLen = hdrLen + sizeLen + obuSize;
        if (offset + totalObuLen > data.length) return data;

        if (obuType === OBU_TEMPORAL_DELIMITER || obuType === OBU_PADDING) {
            removed = true;
        } else if (kept.length > 0 && kept[kept.length - 1][1] === offset) {
            kept[kept.length - 1][1] = offset + totalObuLen;  // extend contiguous range
        } else {
            kept.push([offset, offset + totalObuLen]);
        }

        offset += totalObuLen;
    }

    if (!removed || kept.length === 0) return data;
    // Single contiguous range (the common case: TD at the head) — zero-copy view.
    if (kept.length === 1) return data.subarray(kept[0][0], kept[0][1]);

    let totalLen = 0;
    for (const [s, e] of kept) totalLen += e - s;
    const out = new Uint8Array(totalLen);
    let outPos = 0;
    for (const [s, e] of kept) {
        out.set(data.subarray(s, e), outPos);
        outPos += e - s;
    }
    return out;
}

/** MSB-first bit reader over a Uint8Array. */
class BitReader {
    constructor(bytes) {
        this.bytes = bytes;
        this.bitPos = 0;
    }
    f(n) {
        let v = 0;
        for (let i = 0; i < n; i++) {
            const byteIdx = this.bitPos >> 3;
            if (byteIdx >= this.bytes.length) throw new Error('BitReader overrun');
            const bit = (this.bytes[byteIdx] >> (7 - (this.bitPos & 7))) & 1;
            v = (v * 2) + bit;
            this.bitPos++;
        }
        return v;
    }
    uvlc() {
        let leadingZeros = 0;
        while (this.f(1) === 0) {
            leadingZeros++;
            if (leadingZeros > 32) throw new Error('uvlc overrun');
        }
        if (leadingZeros === 0) return 0;
        return this.f(leadingZeros) + (1 << leadingZeros) - 1;
    }
}

/**
 * Parse a Sequence Header OBU (header + size + payload) and extract the
 * fields needed to build an exact codec string: profile, level, tier,
 * bit depth, and max frame dimensions.  Returns null on any parse error.
 */
export function parseSequenceHeader(obu) {
    try {
        // Skip OBU header byte (+ extension) and leb128 size field
        const hasExtension = (obu[0] >> 2) & 0x01;
        let payloadStart = 1 + (hasExtension ? 1 : 0);
        while (obu[payloadStart] & 0x80) payloadStart++;
        payloadStart++;

        const br = new BitReader(obu.subarray(payloadStart));

        const seqProfile = br.f(3);
        br.f(1); // still_picture
        const reduced = br.f(1);

        let level = 0;
        let tier = 0;
        let decoderModelInfo = 0;
        let bufferDelayLen = 0;

        if (reduced) {
            level = br.f(5);
        } else {
            const timingInfoPresent = br.f(1);
            if (timingInfoPresent) {
                br.f(32); // num_units_in_display_tick
                br.f(32); // time_scale
                if (br.f(1)) br.uvlc(); // equal_picture_interval -> num_ticks
                decoderModelInfo = br.f(1);
                if (decoderModelInfo) {
                    bufferDelayLen = br.f(5) + 1;
                    br.f(32); // num_units_in_decoding_tick
                    br.f(5);  // buffer_removal_time_length_minus_1
                    br.f(5);  // frame_presentation_time_length_minus_1
                }
            }
            const initialDisplayDelay = br.f(1);
            const opCount = br.f(5) + 1;
            for (let i = 0; i < opCount; i++) {
                br.f(12); // operating_point_idc
                const lvl = br.f(5);
                const tr = (lvl > 7) ? br.f(1) : 0;
                if (i === 0) { level = lvl; tier = tr; }
                if (decoderModelInfo && br.f(1)) {
                    br.f(bufferDelayLen); // decoder_buffer_delay
                    br.f(bufferDelayLen); // encoder_buffer_delay
                    br.f(1);              // low_delay_mode_flag
                }
                if (initialDisplayDelay && br.f(1)) {
                    br.f(4); // initial_display_delay_minus_1
                }
            }
        }

        const widthBits = br.f(4) + 1;
        const heightBits = br.f(4) + 1;
        const maxWidth = br.f(widthBits) + 1;
        const maxHeight = br.f(heightBits) + 1;

        if (!reduced && br.f(1)) { // frame_id_numbers_present_flag
            br.f(4); // delta_frame_id_length_minus_2
            br.f(3); // additional_frame_id_length_minus_1
        }
        br.f(1); // use_128x128_superblock
        br.f(1); // enable_filter_intra
        br.f(1); // enable_intra_edge_filter

        let enableOrderHint = 0;
        if (!reduced) {
            br.f(1); // enable_interintra_compound
            br.f(1); // enable_masked_compound
            br.f(1); // enable_warped_motion
            br.f(1); // enable_dual_filter
            enableOrderHint = br.f(1);
            if (enableOrderHint) {
                br.f(1); // enable_jnt_comp
                br.f(1); // enable_ref_frame_mvs
            }
            // seq_choose_screen_content_tools / seq_force_screen_content_tools
            const forceSct = br.f(1) ? 2 : br.f(1);
            if (forceSct > 0) {
                if (!br.f(1)) br.f(1); // seq_choose_integer_mv / seq_force_integer_mv
            }
            if (enableOrderHint) br.f(3); // order_hint_bits_minus_1
        }
        br.f(1); // enable_superres
        br.f(1); // enable_cdef
        br.f(1); // enable_restoration

        // color_config: just enough for bit depth
        const highBitdepth = br.f(1);
        let bitDepth = 8;
        if (seqProfile === 2 && highBitdepth) {
            bitDepth = br.f(1) ? 12 : 10;
        } else if (highBitdepth) {
            bitDepth = 10;
        }

        return { profile: seqProfile, level, tier, bitDepth, maxWidth, maxHeight };
    } catch (e) {
        return null;
    }
}

/** Build "av01.P.LLT.DD" from parsed sequence header fields. */
function codecStringFromSeqInfo(info) {
    const ll = String(info.level).padStart(2, '0');
    const t = info.tier ? 'H' : 'M';
    const dd = String(info.bitDepth).padStart(2, '0');
    return 'av01.' + info.profile + '.' + ll + t + '.' + dd;
}

/**
 * Build VideoDecoderConfig candidates for AV1.
 *
 * If a Sequence Header OBU is provided, the exact codec string (and real
 * coded dimensions) derived from it is tried FIRST, then the fallbacks.
 * Per the WebCodecs AV1 registration the bitstream is self-describing:
 * no description is ever set (the in-band sequence header configures the
 * decoder), the seq header is only used to derive accurate config values.
 */
export function buildAv1DecoderConfigs(seqHeaderObu = null) {
    let width = 1920;
    let height = 1080;
    const codecs = [];

    if (seqHeaderObu) {
        const info = parseSequenceHeader(seqHeaderObu);
        if (info) {
            codecs.push(codecStringFromSeqInfo(info));
            width = info.maxWidth;
            height = info.maxHeight;
            console.log('[Av1Utils] Seq header parsed: profile=' + info.profile +
                ' level=' + info.level + ' tier=' + info.tier +
                ' bitDepth=' + info.bitDepth + ' ' + width + 'x' + height +
                ' -> ' + codecs[0]);
        } else {
            console.warn('[Av1Utils] Sequence Header parse failed, using fallback codec list');
        }
    }

    for (const c of AV1_FALLBACK_CODEC_STRINGS) {
        if (!codecs.includes(c)) codecs.push(c);
    }

    return codecs.map(codecStr => ({
        codec: codecStr,
        codedWidth: width,
        codedHeight: height,
        optimizeForLatency: true,
    }));
}
