/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 *
 * Who is in a shared session (issue #47). Pinned down: each arrival and each
 * departure is announced once, by name, to everyone else on the stream; the
 * people already there when a stream starts are not; a guest is never told of
 * their own arrival; and both headers name everyone on hover.
 */
import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';

vi.mock('../js/api/BackendClient.js', () => ({
    BackendClient: {
        getShareStatus: vi.fn(),
        playerPresence: vi.fn(),
    },
}));
vi.mock('../js/ui/Toast.js', () => ({
    Toast: { info: vi.fn() },
}));
// No locale is loaded in the TNR: render the key so the assertions can name it.
vi.mock('../js/i18n/i18n.js', () => ({
    t: (key, params) => (params ? key + ' ' + JSON.stringify(params) : key),
}));

import {
    GuestPresence,
    guestName,
    guestSidePeople,
    ownerSidePeople,
    presenceChanges,
} from '../js/ui/SharePresence.js';
import { ShareMenu } from '../js/ui/ShareMenu.js';
import { BackendClient } from '../js/api/BackendClient.js';
import { Toast } from '../js/ui/Toast.js';

const toasts = () => Toast.info.mock.calls.map((c) => c[0]);

describe('SharePresence, the people of one look', () => {
    it("names a guest as the board does: the owner's label, else Player N", () => {
        expect(guestName({ slot: 3, name: 'Léo' })).toBe('Léo');
        expect(guestName({ slot: 2, name: '' })).toBe('sharing.playerN {"n":1}');
    });

    it('the owner counts the guests streaming, not the links out', () => {
        const people = ownerSidePeople([
            { slot: 2, state: 'binded', streaming: true, name: 'Léo' },
            { slot: 3, state: 'shared', streaming: false },
            { slot: 4, state: 'binded', streaming: true },
        ]);
        expect([...people.values()]).toEqual(['Léo', 'sharing.playerN {"n":3}']);
    });

    it('a guest counts the host when watching, then every guest', () => {
        const people = guestSidePeople({
            self: 3,
            owner: true,
            players: [{ slot: 2, name: 'Léo' }, { slot: 3 }],
        });
        expect([...people.keys()]).toEqual(['owner', 'p2', 'p3']);
        expect(people.get('owner')).toBe('sharing.presenceHost');
    });

    it('tells who came and who went, never the one skipped', () => {
        const before = new Map([
            ['owner', 'Host'],
            ['p2', 'Léo'],
        ]);
        const after = new Map([
            ['p2', 'Léo'],
            ['p3', 'Sam'],
            ['p4', 'Me'],
        ]);
        expect(presenceChanges(before, after, 'p4')).toEqual({ joined: ['Sam'], left: ['Host'] });
    });
});

describe('GuestPresence, a guest’s header', () => {
    let header;
    let quit;

    beforeEach(() => {
        vi.useFakeTimers();
        Toast.info.mockClear();
        header = document.createElement('div');
        quit = document.createElement('button');
        header.appendChild(quit);
        document.body.appendChild(header);
    });

    afterEach(() => {
        vi.useRealTimers();
        header.remove();
    });

    it('shows everyone, this guest marked, and announces only later changes', async () => {
        BackendClient.playerPresence.mockResolvedValueOnce({
            self: 3,
            owner: true,
            players: [{ slot: 2, name: 'Léo' }, { slot: 3 }],
        });
        const p = new GuestPresence(header, quit);
        p.mount();
        await vi.advanceTimersByTimeAsync(0);

        const btn = header.querySelector('.stream-presence');
        expect(btn.nextSibling).toBe(quit);
        expect(btn.hidden).toBe(false);
        expect(btn.querySelector('.stream-presence-count').textContent).toBe('3');
        expect(btn.title.split('\n')).toEqual([
            'sharing.presenceCount {"count":3}',
            'sharing.presenceHost',
            'Léo',
            'sharing.presenceYou {"name":"sharing.playerN {\\"n\\":2}"}',
        ]);
        // Who was there first is the scene, not news.
        expect(Toast.info).not.toHaveBeenCalled();

        BackendClient.playerPresence.mockResolvedValueOnce({
            self: 3,
            owner: false,
            players: [{ slot: 3 }, { slot: 4, name: 'Sam' }],
        });
        await vi.advanceTimersByTimeAsync(5000);
        expect(toasts()).toEqual([
            'sharing.presenceJoined {"name":"Sam"}',
            'sharing.presenceLeft {"name":"sharing.presenceHost"}',
            'sharing.presenceLeft {"name":"Léo"}',
        ]);
        expect(btn.querySelector('.stream-presence-count').textContent).toBe('2');

        p.destroy();
        expect(header.querySelector('.stream-presence')).toBeNull();
    });

    it('keeps the last count through a failed look', async () => {
        BackendClient.playerPresence.mockResolvedValueOnce({
            self: 2,
            owner: true,
            players: [{ slot: 2 }],
        });
        const p = new GuestPresence(header, quit);
        p.mount();
        await vi.advanceTimersByTimeAsync(0);
        BackendClient.playerPresence.mockRejectedValueOnce(new Error('offline'));
        await vi.advanceTimersByTimeAsync(5000);
        expect(header.querySelector('.stream-presence-count').textContent).toBe('2');
        expect(Toast.info).not.toHaveBeenCalled();
        p.destroy();
    });
});

describe("ShareMenu, the owner's header", () => {
    beforeEach(() => {
        vi.useFakeTimers();
        Toast.info.mockClear();
    });
    afterEach(() => vi.useRealTimers());

    it('names the guests on hover and announces each arrival and departure', async () => {
        const header = document.createElement('div');
        const quit = document.createElement('button');
        header.appendChild(quit);
        BackendClient.getShareStatus.mockResolvedValueOnce({
            slots: [{ slot: 2, state: 'binded', streaming: true, name: 'Léo' }],
        });
        const menu = new ShareMenu(header, quit, { uuid: 'h' });
        await menu.mount();
        const btn = header.querySelector('.share-btn');
        expect(btn.title.split('\n')).toEqual([
            'sharing.presenceCount {"count":2}',
            'sharing.presenceYouHost',
            'Léo',
        ]);
        expect(Toast.info).not.toHaveBeenCalled();

        BackendClient.getShareStatus.mockResolvedValueOnce({
            slots: [
                { slot: 2, state: 'binded', streaming: false, name: 'Léo' },
                { slot: 3, state: 'binded', streaming: true },
            ],
        });
        await menu.refresh();
        expect(toasts()).toEqual([
            'sharing.presenceJoined {"name":"sharing.playerN {\\"n\\":2}"}',
            'sharing.presenceLeft {"name":"Léo"}',
        ]);

        BackendClient.getShareStatus.mockResolvedValueOnce({ slots: [] });
        await menu.refresh();
        expect(btn.title).toBe('');
        menu.destroy();
    });
});
