/*
 * MoonlightWeb — frontend TNR. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * The browser half of the rendezvous control-channel format.
 *
 * The other half is C++ (backend/src/server/TunnelFrame.h), and nothing catches
 * a disagreement between the two: not the compiler, not a deploy, not a review.
 * The first symptom would be a user whose application never loads, with no error
 * naming the cause.
 *
 * So both sides are pinned against the SAME vectors. Every hex string below also
 * appears in backend/tests/test_tunnel_frame.cpp. If one side is changed alone,
 * one of the two suites fails — which is the only mechanism there is here.
 */

import { describe, it, expect } from 'vitest';
import { encodeFrame, encodeHead } from '../../bootstrap/tunnel.js';

const FRAME_REQUEST = 0x01;
const FRAME_END = 0x04;
const FRAME_WS_TEXT = 0x06;

function hex(bytes) {
    return [...bytes].map((b) => b.toString(16).padStart(2, '0')).join('');
}

describe('tunnel frame encoding', () => {
    it('writes a five-byte, big-endian header', () => {
        // The identifier is big-endian. A backend reading it the other way round
        // would answer request 16777216 and the two sides would talk past each
        // other for every request in flight.
        expect(hex(encodeFrame(FRAME_REQUEST, 1, null))).toBe('0100000001');
        expect(hex(encodeFrame(FRAME_END, 0x01020304, null))).toBe('0401020304');
    });

    it('appends the payload verbatim', () => {
        const payload = new TextEncoder().encode('hi');
        expect(hex(encodeFrame(FRAME_WS_TEXT, 7, payload))).toBe('06000000076869');
    });

    it('carries an empty payload without confusing it for a failure', () => {
        // END and WS_OPENED carry nothing at all, so five bytes is a complete
        // and meaningful frame.
        expect(encodeFrame(FRAME_END, 1, null)).toHaveLength(5);
    });

    it('length-prefixes the head so the body starts where it says', () => {
        const encoded = encodeHead({ m: 'GET' }, new TextEncoder().encode('BODY'));
        // 4-byte length, then {"m":"GET"} (11 bytes), then the body verbatim.
        expect(hex(encoded)).toBe('0000000b7b226d223a22474554227d424f4459');
    });

    it('encodes a head with no body', () => {
        const encoded = encodeHead({ m: 'GET' }, null);
        expect(hex(encoded)).toBe('0000000b7b226d223a22474554227d');
    });

    it('measures the head in BYTES, not characters', () => {
        // A path with a non-ASCII character encodes to more bytes than it has
        // characters. Using the string length would make the far end read the
        // head short and take the remainder as a body — a silent corruption
        // rather than an error, and only for users whose host names or app
        // titles are not plain ASCII.
        const encoded = encodeHead({ p: '/apps/é' }, null);
        const declared = new DataView(encoded.buffer).getUint32(0, false);
        expect(declared).toBe(encoded.length - 4);
        expect(declared).toBeGreaterThan(JSON.stringify({ p: '/apps/é' }).length);
    });
});
