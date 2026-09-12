/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect } from 'vitest';
import { bootstrapAddressFor } from '../js/net/tunnelBridge.js';

// Going back through the bootstrap must keep what the link arrived with. This
// is how the installer's "open the admin page" landed on the PIN screen: the
// page bounced on a bare /<id>, and the single-use host key went with the
// fragment it dropped.
describe('bootstrapAddressFor — the bounce keeps the fragment', () => {
    const ID = 'a'.repeat(26);

    it('is a bare identifier when the page carried nothing', () => {
        expect(bootstrapAddressFor(ID, { hash: '', pathname: '/' })).toBe(`/${ID}`);
        expect(bootstrapAddressFor(ID, { hash: `#${ID}`, pathname: '/' })).toBe(`/${ID}`);
    });

    it('keeps the host key and the requested page, without the identifier', () => {
        expect(
            bootstrapAddressFor(ID, { hash: `#${ID}&k=SEKRIT&p=/admin`, pathname: '/admin' }),
        ).toBe(`/${ID}#k=SEKRIT&p=/admin`);
    });

    it('writes the page back when the router had moved it to the path', () => {
        // After startup the fragment holds the identifier alone and the page
        // asked for is the path; the bootstrap reads only the fragment.
        expect(bootstrapAddressFor(ID, { hash: `#${ID}&k=SEKRIT`, pathname: '/admin' })).toBe(
            `/${ID}#k=SEKRIT&p=%2Fadmin`,
        );
        expect(bootstrapAddressFor(ID, { hash: `#${ID}`, pathname: '/settings' })).toBe(
            `/${ID}#p=%2Fsettings`,
        );
    });

    it('keeps an invitation token', () => {
        expect(bootstrapAddressFor(ID, { hash: `#${ID}&t=TOKEN`, pathname: '/p' })).toBe(
            `/${ID}#t=TOKEN&p=%2Fp`,
        );
    });

    it('survives a location without a hash or a path', () => {
        expect(bootstrapAddressFor(ID, {})).toBe(`/${ID}`);
    });
});
