/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, expect, it } from 'vitest';
import { tvApprovalMessage, tvRefusalMessage } from '../js/util/pairingCrypto.js';

// What a paired device signs to let a TV in for a short while (MW-LANTV-v1) is
// what the host verifies: the same bytes as backend/tests/test_lan_http_pairing.cpp
// (the vector the page shares), field by field, each after its length.
const hex = (bytes) => Array.from(bytes, (b) => b.toString(16).padStart(2, '0')).join('');

const HOST = '0000000b686f73742d6b65792d6964'; // host-key-id
const ID = '000000164162436445664768496a4b6c4d6e4f70517253745576'; // AbCdEfGhIjKlMnOpQrStUv

describe('tvApprovalMessage', () => {
    it('frames the approval as the host does, byte for byte', () => {
        const expected = [
            '000000134d572d4c414e54562d76317c617070726f7665', // MW-LANTV-v1|approve
            HOST,
            ID,
            '00000006484a44533458', // HJDS4X
        ].join('');
        expect(hex(tvApprovalMessage('host-key-id', 'AbCdEfGhIjKlMnOpQrStUv', 'HJDS4X'))).toBe(
            expected,
        );
    });

    it('frames the refusal the same way, without a code', () => {
        const expected = [
            '000000124d572d4c414e54562d76317c726566757365', // MW-LANTV-v1|refuse
            HOST,
            ID,
        ].join('');
        expect(hex(tvRefusalMessage('host-key-id', 'AbCdEfGhIjKlMnOpQrStUv'))).toBe(expected);
    });
});
