/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 *
 * Note: the module is named Mp4Muxer but exports NAL/AVCC/HVCC helpers used by
 * the WebCodecs decode path. We cover the deterministic bitstream logic.
 */
import { describe, it, expect } from 'vitest';
import {
    detectCodec,
    NalParser,
    removeEmulationPrevention,
    splitNals,
    buildAvccDescription,
    getH264CodecString,
    buildHvcCDescription,
    getHevcCodecString,
    getCodecString,
    buildDescription,
    isHevcHdrProfile,
    toAvcc,
    CODEC_H264,
    CODEC_HEVC,
} from '../js/util/Mp4Muxer.js';

const SC4 = [0, 0, 0, 1]; // 4-byte Annex B start code

// Concatenate NAL payloads into an Annex B stream (4-byte start codes).
function annexB(...nals) {
    const out = [];
    for (const n of nals) out.push(...SC4, ...n);
    return new Uint8Array(out);
}

// H.264 NAL: first byte low-5-bits = type. SPS=7 (0x67), PPS=8 (0x68), IDR=5 (0x65).
const H264_SPS = [0x67, 0x64, 0x00, 0x2a, 0xac, 0xb2];
const H264_PPS = [0x68, 0xee, 0x3c, 0x80];
const H264_IDR = [0x65, 0x88, 0x84, 0x00];

// HEVC NAL: 2-byte header, type = (b0>>1)&0x3f. VPS=32(0x40), SPS=33(0x42), PPS=34(0x44).
const HEVC_VPS = [0x40, 0x01, 0x0c, 0x01];
const HEVC_SPS = [0x42, 0x01, 0x01, 0x01, 0x60, 0x00, 0x00, 0x03, 0x00, 0x90, 0x00, 0x00, 0x78, 0x00, 0x00, 0x03, 0x00, 0x00];
const HEVC_PPS = [0x44, 0x01, 0xc1, 0x72];

describe('Mp4Muxer — splitNals', () => {
    it('splits a stream with 4-byte start codes', () => {
        const nals = splitNals(annexB(H264_SPS, H264_PPS));
        expect(nals.length).toBe(2);
        expect(Array.from(nals[0])).toEqual(H264_SPS);
        expect(Array.from(nals[1])).toEqual(H264_PPS);
    });

    it('handles 3-byte start codes', () => {
        const buf = new Uint8Array([0, 0, 1, 0x67, 0x10, 0, 0, 1, 0x68, 0x20]);
        const nals = splitNals(buf);
        expect(nals.length).toBe(2);
        expect(nals[0][0]).toBe(0x67);
        expect(nals[1][0]).toBe(0x68);
    });
});

describe('Mp4Muxer — codec detection', () => {
    it('detects H.264 from an SPS NAL', () => {
        expect(detectCodec(annexB(H264_SPS, H264_PPS))).toBe(CODEC_H264);
    });
    it('detects HEVC from a VPS NAL', () => {
        expect(detectCodec(annexB(HEVC_VPS, HEVC_SPS, HEVC_PPS))).toBe(CODEC_HEVC);
    });
    it('returns null for a too-short buffer', () => {
        expect(detectCodec(new Uint8Array([0, 0]))).toBeNull();
    });
});

describe('Mp4Muxer — NalParser', () => {
    it('collects H.264 SPS/PPS and reports ready', () => {
        const p = new NalParser();
        expect(p.feed(annexB(H264_IDR))).toBe(false); // no params yet
        expect(p.feed(annexB(H264_SPS, H264_PPS))).toBe(true);
        expect(p.codec).toBe(CODEC_H264);
        expect(p.isReady()).toBe(true);
        expect(getCodecString(p)).toBe('avc1.64002a');
        expect(buildDescription(p)).toBeInstanceOf(Uint8Array);
    });

    it('collects HEVC VPS/SPS/PPS', () => {
        const p = new NalParser();
        const ready = p.feed(annexB(HEVC_VPS, HEVC_SPS, HEVC_PPS));
        expect(p.codec).toBe(CODEC_HEVC);
        expect(ready).toBe(true);
        expect(getCodecString(p)).toMatch(/^hvc1\./);
    });

    it('reset() clears state; helpers no-op until ready', () => {
        const p = new NalParser();
        p.feed(annexB(H264_SPS, H264_PPS));
        p.reset();
        expect(p.isReady()).toBe(false);
        expect(getCodecString(p)).toBeNull();
        expect(buildDescription(p)).toBeNull();
    });
});

describe('Mp4Muxer — emulation prevention', () => {
    it('removes 00 00 03 escape bytes', () => {
        expect(Array.from(removeEmulationPrevention(new Uint8Array([0, 0, 3, 1, 5])))).toEqual([
            0, 0, 1, 5,
        ]);
        // 00 00 03 03 -> 00 00 03
        expect(Array.from(removeEmulationPrevention(new Uint8Array([0, 0, 3, 3])))).toEqual([0, 0, 3]);
    });
});

describe('Mp4Muxer — H.264 description + codec string', () => {
    it('builds an avcC record', () => {
        const sps = new Uint8Array(H264_SPS);
        const pps = new Uint8Array(H264_PPS);
        const avcc = buildAvccDescription(sps, pps);
        expect(avcc[0]).toBe(0x01); // configurationVersion
        expect(avcc[1]).toBe(sps[1]); // profile
        expect(avcc.length).toBe(11 + sps.length + pps.length);
        expect(buildAvccDescription(null, pps)).toBeNull();
    });
    it('builds the avc1.* codec string, with a safe default for short SPS', () => {
        expect(getH264CodecString(new Uint8Array(H264_SPS))).toBe('avc1.64002a');
        expect(getH264CodecString(new Uint8Array([1, 2]))).toBe('avc1.64002A');
    });
});

describe('Mp4Muxer — HEVC description + codec string', () => {
    it('builds an hvcC record from VPS/SPS/PPS', () => {
        const hvcc = buildHvcCDescription(
            new Uint8Array(HEVC_VPS),
            new Uint8Array(HEVC_SPS),
            new Uint8Array(HEVC_PPS),
        );
        expect(hvcc).toBeInstanceOf(Uint8Array);
        expect(hvcc[0]).toBe(0x01);
        expect(buildHvcCDescription(null, null, null)).toBeNull();
    });
    it('returns a default codec string for a short SPS', () => {
        expect(getHevcCodecString(new Uint8Array([0x42, 0x01]))).toBe('hvc1.1.6.L93.B0');
    });
    it('detects HDR (Main10) HEVC profiles', () => {
        expect(isHevcHdrProfile('hvc1.2.4.L153.B0')).toBe(true);
        expect(isHevcHdrProfile('hev1.2.4.L153.B0')).toBe(true);
        expect(isHevcHdrProfile('hvc1.1.6.L153.B0')).toBe(false);
        expect(isHevcHdrProfile('')).toBe(false);
    });
});

describe('Mp4Muxer — toAvcc', () => {
    it('length-prefixes all NALs in AVCC mode', () => {
        const out = toAvcc(annexB(H264_IDR), false, CODEC_H264, false);
        // 4-byte length prefix + the NAL
        expect(out.length).toBe(4 + H264_IDR.length);
        const len = (out[0] << 24) | (out[1] << 16) | (out[2] << 8) | out[3];
        expect(len).toBe(H264_IDR.length);
    });

    it('strips SPS/PPS when requested (params already in the description)', () => {
        const out = toAvcc(annexB(H264_SPS, H264_PPS, H264_IDR), true, CODEC_H264, false);
        // Only the IDR remains: 4-byte prefix + IDR
        expect(out.length).toBe(4 + H264_IDR.length);
    });

    it('emits Annex B start codes for HEVC when useAnnexB is set', () => {
        const out = toAvcc(annexB(HEVC_VPS), false, CODEC_HEVC, true);
        expect(Array.from(out.slice(0, 4))).toEqual([0, 0, 0, 1]);
    });
});
