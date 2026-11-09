/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, expect, it } from 'vitest';
import { pairApprovalMessage, pairRefusalMessage } from '../js/util/pairingCrypto.js';

// What this page signs to let a device in by its QR code is what the host
// verifies: the same bytes as backend/tests/test_phone_pairing.cpp
// (testTheVectorThePageShares), field by field, each after its length.
const hex = (bytes) => Array.from(bytes, (b) => b.toString(16).padStart(2, '0')).join('');

describe('pairApprovalMessage', () => {
    it('frames the approval as the host does, byte for byte', () => {
        const expected = [
            '000000124d572d504149522d76317c617070726f7665', // MW-PAIR-v1|approve
            '0000000b686f73742d6b65792d6964', // host-key-id
            '000000104162436445664768496a4b6c4d6e4f70', // AbCdEfGhIjKlMnOp
            '00000004484a4453', // HJDS
        ].join('');
        expect(hex(pairApprovalMessage('host-key-id', 'AbCdEfGhIjKlMnOp', 'HJDS'))).toBe(expected);
    });

    it('frames the refusal the same way, without a code', () => {
        const expected = [
            '000000114d572d504149522d76317c726566757365', // MW-PAIR-v1|refuse
            '0000000b686f73742d6b65792d6964', // host-key-id
            '000000104162436445664768496a4b6c4d6e4f70', // AbCdEfGhIjKlMnOp
        ].join('');
        expect(hex(pairRefusalMessage('host-key-id', 'AbCdEfGhIjKlMnOp'))).toBe(expected);
    });
});
