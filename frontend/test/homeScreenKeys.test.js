/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * A home-screen shortcut carries one key per machine inside a single `#k=`
 * value, and never sends a key twice. Both are pinned here: the first because
 * the bootstrap only relays `k=` as it is, the second because a key sent back
 * spent is a refusal that counts against the abuse guard.
 */

import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';
import { createHash } from 'node:crypto';
import {
    packHandoffKeys,
    unpackHandoffKeys,
    isHomeScreenHandoff,
    isHandoffKeySpent,
    markHandoffKeySpent,
} from '../js/util/homeScreenKeys.js';

const A = 'hs-AAAAAAAAAAAAAAAAAAAAAA';
const B = 'hs-BBBBBBBBBBBBBBBBBBBBBB';
const C = 'hs-CCCCCCCCCCCCCCCCCCCCCC';
const idB = '0123456789abcdefghjkmnpqrs';
const idC = 'zyxwvtsrqpnmkjhgfedcba9876';

describe('packHandoffKeys / unpackHandoffKeys', () => {
    it('round-trips the own key and the others', () => {
        const packed = packHandoffKeys(A, [
            { id: idB, key: B },
            { id: idC, key: C },
        ]);
        expect(packed).toBe(`${A},${idB}:${B},${idC}:${C}`);
        expect(unpackHandoffKeys(packed)).toEqual({
            own: A,
            others: [
                { id: idB, key: B },
                { id: idC, key: C },
            ],
        });
    });

    it('is the bare key when there is nobody else', () => {
        expect(packHandoffKeys(A, [])).toBe(A);
        expect(unpackHandoffKeys(A)).toEqual({ own: A, others: [] });
    });

    it('keeps the others when the page had no key of its own', () => {
        const packed = packHandoffKeys('', [{ id: idB, key: B }]);
        expect(packed).toBe(`,${idB}:${B}`);
        const parsed = unpackHandoffKeys(packed);
        expect(parsed).toEqual({ own: '', others: [{ id: idB, key: B }] });
        expect(isHomeScreenHandoff(parsed)).toBe(true);
    });

    it('leaves out what is not shaped like a machine and a key', () => {
        expect(
            packHandoffKeys(A, [{ id: 'short', key: B }, { id: idB, key: 'not-a-key' }, null]),
        ).toBe(A);
        expect(unpackHandoffKeys(`${A},garbage,${idB}:nope,${idB}:${B}`).others).toEqual([
            { id: idB, key: B },
        ]);
    });

    it('passes a host key through untouched', () => {
        const parsed = unpackHandoffKeys('mwk-something-else');
        expect(parsed).toEqual({ own: 'mwk-something-else', others: [] });
        expect(isHomeScreenHandoff(parsed)).toBe(false);
        expect(unpackHandoffKeys(null)).toEqual({ own: '', others: [] });
        expect(isHomeScreenHandoff(unpackHandoffKeys(''))).toBe(false);
    });
});

describe('spent keys', () => {
    const STORE = 'mw-hs-spent';
    const sha = (text) => createHash('sha256').update(text).digest('hex');

    beforeEach(() => localStorage.clear());
    afterEach(() => vi.unstubAllGlobals());

    it('remembers a key once it has had its answer', async () => {
        expect(await isHandoffKeySpent(A)).toBe(false);
        await markHandoffKeySpent(A);
        expect(await isHandoffKeySpent(A)).toBe(true);
        expect(await isHandoffKeySpent(B)).toBe(false);
    });

    it('keeps a fingerprint of the key, never the key', async () => {
        await markHandoffKeySpent(A);
        await markHandoffKeySpent(B);
        const kept = localStorage.getItem(STORE);
        expect(kept).not.toContain(A.slice(3));
        expect(kept).not.toContain(B.slice(3));
        // The key's SHA-256: the same key gives the same entry at every launch.
        expect(JSON.parse(kept)).toEqual([sha(A), sha(B)]);
    });

    it('turns the keys an earlier version kept in clear into fingerprints, and still knows them', async () => {
        localStorage.setItem(STORE, JSON.stringify([A, B]));
        expect(await isHandoffKeySpent(A)).toBe(true);
        expect(JSON.parse(localStorage.getItem(STORE))).toEqual([sha(A), sha(B)]);
        expect(await isHandoffKeySpent(B)).toBe(true);
        expect(await isHandoffKeySpent(C)).toBe(false);
    });

    it('survives a broken record and an absent storage', async () => {
        localStorage.setItem(STORE, '{not json');
        expect(await isHandoffKeySpent(A)).toBe(false);
        await markHandoffKeySpent(A);
        expect(await isHandoffKeySpent(A)).toBe(true);
        expect(await isHandoffKeySpent(A, null)).toBe(false);
        await expect(markHandoffKeySpent(A, null)).resolves.toBeUndefined();
    });

    it('keeps the newest keys only', async () => {
        for (let i = 0; i < 80; i++) await markHandoffKeySpent(`hs-${String(i).padStart(20, '0')}`);
        expect(await isHandoffKeySpent('hs-00000000000000000000')).toBe(false);
        expect(await isHandoffKeySpent('hs-00000000000000000079')).toBe(true);
        expect(JSON.parse(localStorage.getItem(STORE))).toHaveLength(64);
    });

    it('keeps nothing in clear in a browser that cannot make a fingerprint', async () => {
        localStorage.setItem(STORE, JSON.stringify([A]));
        vi.stubGlobal('crypto', {});
        await markHandoffKeySpent(B);
        // Not remembered: the key is refused once more at the next launch, nothing worse.
        expect(await isHandoffKeySpent(B)).toBe(false);
        const kept = localStorage.getItem(STORE);
        expect(kept).not.toContain(A.slice(3));
        expect(kept).not.toContain(B.slice(3));
    });
});
