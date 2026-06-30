/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect } from 'vitest';
import {
    isAv1HdrProfile,
    isAv1HdrFromSeq,
    getObuType,
    isAv1Buffer,
    findSequenceHeader,
    stripNonEssentialObus,
    parseSequenceHeader,
    buildAv1DecoderConfigs,
    AV1_FALLBACK_CODEC_STRINGS,
} from '../js/util/Av1Utils.js';

// MSB-first bit writer mirroring the BitReader the parser uses.
class BitWriter {
    constructor() {
        this.bits = [];
    }
    f(value, n) {
        for (let i = n - 1; i >= 0; i--) this.bits.push((value >> i) & 1);
        return this;
    }
    bytes() {
        const out = new Uint8Array(Math.ceil(this.bits.length / 8));
        this.bits.forEach((b, i) => {
            if (b) out[i >> 3] |= 1 << (7 - (i & 7));
        });
        return out;
    }
}

// Build a complete Sequence Header OBU (header + leb128 size + payload) for a
// reduced_still_picture_header stream (the simplest valid parse path).
function buildSeqHeaderObu({ profile = 0, level = 8, width = 1920, height = 1080 } = {}) {
    const w = new BitWriter();
    w.f(profile, 3).f(0, 1).f(1, 1); // seq_profile, still_picture, reduced=1
    w.f(level, 5); // seq_level_idx
    // 11 bits is enough for 1920/1080 (max 2048).
    const bits = 11;
    w.f(bits - 1, 4).f(bits - 1, 4);
    w.f(width - 1, bits).f(height - 1, bits);
    w.f(0, 1).f(0, 1).f(0, 1); // use_128x128, enable_filter_intra, enable_intra_edge_filter
    w.f(0, 1).f(0, 1).f(0, 1); // enable_superres, enable_cdef, enable_restoration
    w.f(0, 1); // high_bitdepth = 0 -> 8-bit
    const payload = w.bytes();
    const obu = new Uint8Array(2 + payload.length);
    obu[0] = (1 << 3) | (1 << 1); // type=1 (seq header), has_size=1 -> 0x0A
    obu[1] = payload.length; // leb128 size (single byte, < 128)
    obu.set(payload, 2);
    return obu;
}

describe('Av1Utils — codec string helpers', () => {
    it('detects HDR profiles from the codec string suffix', () => {
        expect(isAv1HdrProfile('av01.0.08M.10')).toBe(true);
        expect(isAv1HdrProfile('av01.0.08M.12')).toBe(true);
        expect(isAv1HdrProfile('av01.0.08M.08')).toBe(false);
        expect(isAv1HdrProfile('')).toBe(false);
        expect(isAv1HdrProfile(null)).toBe(false);
    });

    it('detects HDR from parsed sequence info', () => {
        expect(isAv1HdrFromSeq({ bitDepth: 10 })).toBe(true);
        expect(isAv1HdrFromSeq({ bitDepth: 8 })).toBe(false);
        expect(isAv1HdrFromSeq(null)).toBeFalsy();
    });

    it('extracts the OBU type from a header byte', () => {
        expect(getObuType(0x0a)).toBe(1); // sequence header
        expect(getObuType(0x12)).toBe(2); // temporal delimiter
    });
});

describe('Av1Utils — buffer classification', () => {
    it('rejects Annex B start codes as non-AV1', () => {
        expect(isAv1Buffer(new Uint8Array([0, 0, 1, 9]))).toBe(false); // 3-byte SC
        expect(isAv1Buffer(new Uint8Array([0, 0, 0, 1]))).toBe(false); // 4-byte SC
        expect(isAv1Buffer(new Uint8Array([0x12, 0, 0x0a, 0]))).toBe(true); // OBU-ish
        expect(isAv1Buffer(new Uint8Array([1, 2]))).toBe(false); // too short
        expect(isAv1Buffer(null)).toBe(false);
    });
});

describe('Av1Utils — OBU walking', () => {
    it('finds the sequence header past a temporal delimiter', () => {
        const seq = buildSeqHeaderObu();
        const td = new Uint8Array([0x12, 0x00]); // TD OBU, size 0
        const tu = new Uint8Array(td.length + seq.length);
        tu.set(td, 0);
        tu.set(seq, td.length);

        const found = findSequenceHeader(tu);
        expect(found).not.toBeNull();
        expect(getObuType(found[0])).toBe(1);
        expect(Array.from(found)).toEqual(Array.from(seq));
    });

    it('returns null when no sequence header is present', () => {
        expect(findSequenceHeader(new Uint8Array([0x12, 0x00]))).toBeNull();
        expect(findSequenceHeader(new Uint8Array([0x01]))).toBeNull();
    });

    it('strips temporal-delimiter and padding OBUs', () => {
        const seq = buildSeqHeaderObu();
        const td = new Uint8Array([0x12, 0x00]); // TD
        const tu = new Uint8Array(td.length + seq.length);
        tu.set(td, 0);
        tu.set(seq, td.length);

        const stripped = stripNonEssentialObus(tu);
        expect(Array.from(stripped)).toEqual(Array.from(seq)); // TD removed
    });

    it('returns the input untouched when nothing needs stripping', () => {
        const seq = buildSeqHeaderObu();
        expect(stripNonEssentialObus(seq)).toBe(seq); // same reference
        expect(stripNonEssentialObus(new Uint8Array([]))).toEqual(new Uint8Array([]));
    });
});

describe('Av1Utils — sequence header parsing', () => {
    it('parses profile/level/dimensions/bit depth from a crafted header', () => {
        const obu = buildSeqHeaderObu({ profile: 0, level: 8, width: 1920, height: 1080 });
        const info = parseSequenceHeader(obu);
        expect(info).toMatchObject({
            profile: 0,
            level: 8,
            tier: 0,
            bitDepth: 8,
            maxWidth: 1920,
            maxHeight: 1080,
        });
    });

    it('returns null on a malformed header', () => {
        expect(parseSequenceHeader(new Uint8Array([0x0a, 0x01, 0xff]))).toBeNull();
    });
});

describe('Av1Utils — decoder config building', () => {
    it('returns the fallback list when no sequence header is given', () => {
        const cfgs = buildAv1DecoderConfigs();
        expect(cfgs.length).toBe(AV1_FALLBACK_CODEC_STRINGS.length);
        expect(cfgs[0]).toMatchObject({ codec: AV1_FALLBACK_CODEC_STRINGS[0], optimizeForLatency: true });
    });

    it('puts the parsed exact codec string first, then fallbacks', () => {
        const obu = buildSeqHeaderObu({ profile: 0, level: 8, width: 1920, height: 1080 });
        const cfgs = buildAv1DecoderConfigs(obu);
        expect(cfgs[0].codec).toBe('av01.0.08M.08');
        expect(cfgs[0].codedWidth).toBe(1920);
        expect(cfgs[0].codedHeight).toBe(1080);
        // no duplicate of the same string in the fallback tail
        expect(cfgs.filter((c) => c.codec === 'av01.0.08M.08').length).toBe(1);
    });

    it('falls back to the default list when the header fails to parse', () => {
        const cfgs = buildAv1DecoderConfigs(new Uint8Array([0x0a, 0x01, 0xff]));
        expect(cfgs.length).toBe(AV1_FALLBACK_CODEC_STRINGS.length);
    });
});
