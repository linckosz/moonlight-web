/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect, beforeEach, vi } from 'vitest';
import {
    loadCachedApps,
    saveCachedApps,
    forgetCachedApps,
    appsSignature,
    boxArtWasSeen,
    noteBoxArtSeen,
} from '../js/util/appCache.js';

// The per-host app-list memory that lets a host card paint its grid the moment
// it goes "ready" instead of after an HTTPS round trip to the host.
describe('app-list memory', () => {
    beforeEach(() => {
        localStorage.clear();
        vi.useRealTimers();
    });

    it('gives back what a host last answered', () => {
        saveCachedApps('host-a', [{ id: 7, name: 'Desktop', hdrSupported: true }]);
        expect(loadCachedApps('host-a')).toEqual([
            { id: 7, name: 'Desktop', hdrSupported: true, boxArt: true },
        ]);
    });

    it('carries the box-art flag, so a grid painted from memory asks for nothing extra', () => {
        saveCachedApps('host-native', [{ id: 3, name: 'DISPLAY1', boxArt: false }]);
        expect(loadCachedApps('host-native')[0].boxArt).toBe(false);
    });

    it('keeps each host apart', () => {
        saveCachedApps('host-a', [{ id: 1, name: 'A' }]);
        saveCachedApps('host-b', [{ id: 2, name: 'B' }]);
        expect(loadCachedApps('host-a').map((a) => a.name)).toEqual(['A']);
        expect(loadCachedApps('host-b').map((a) => a.name)).toEqual(['B']);
        expect(loadCachedApps('host-c')).toBeNull();
    });

    it('never remembers an empty list — "no applications" is the one stale state with no way out', () => {
        saveCachedApps('host-a', [{ id: 1, name: 'A' }]);
        saveCachedApps('host-a', []);
        expect(loadCachedApps('host-a')).toBeNull();
    });

    it('forgets a host that was removed', () => {
        saveCachedApps('host-a', [{ id: 1, name: 'A' }]);
        forgetCachedApps('host-a');
        expect(loadCachedApps('host-a')).toBeNull();
    });

    it('drops a list nobody has confirmed for a month', () => {
        saveCachedApps('host-a', [{ id: 1, name: 'A' }]);
        const raw = JSON.parse(localStorage.getItem('mw-host-apps'));
        raw['host-a'].ts = Date.now() - 31 * 24 * 60 * 60 * 1000;
        localStorage.setItem('mw-host-apps', JSON.stringify(raw));
        expect(loadCachedApps('host-a')).toBeNull();
    });

    it('re-dates a list the host confirmed unchanged, instead of ageing it out', () => {
        saveCachedApps('host-a', [{ id: 1, name: 'A' }]);
        const raw = JSON.parse(localStorage.getItem('mw-host-apps'));
        raw['host-a'].ts = Date.now() - 29 * 24 * 60 * 60 * 1000;
        localStorage.setItem('mw-host-apps', JSON.stringify(raw));

        saveCachedApps('host-a', [{ id: 1, name: 'A' }]);
        const after = JSON.parse(localStorage.getItem('mw-host-apps'));
        expect(Date.now() - after['host-a'].ts).toBeLessThan(5000);
    });

    it('caps the number of hosts, keeping the most recently confirmed', () => {
        for (let i = 0; i < 30; i++) {
            saveCachedApps(`host-${i}`, [{ id: 1, name: `App ${i}` }]);
            const raw = JSON.parse(localStorage.getItem('mw-host-apps'));
            raw[`host-${i}`].ts = 1000 + i; // strictly increasing, oldest first
            localStorage.setItem('mw-host-apps', JSON.stringify(raw));
        }
        // The write above is what applies the cap, so trigger one more.
        saveCachedApps('host-29', [{ id: 1, name: 'App 29' }]);
        const stored = JSON.parse(localStorage.getItem('mw-host-apps'));
        expect(Object.keys(stored)).toHaveLength(24);
        expect(stored['host-29']).toBeDefined();
        expect(stored['host-0']).toBeUndefined();
    });

    it('survives a corrupted store instead of breaking the host list', () => {
        const warn = vi.spyOn(console, 'warn').mockImplementation(() => {});
        localStorage.setItem('mw-host-apps', 'not json at all');
        expect(loadCachedApps('host-a')).toBeNull();
        saveCachedApps('host-a', [{ id: 1, name: 'A' }]);
        expect(loadCachedApps('host-a')).not.toBeNull();
        warn.mockRestore();
    });

    it('rejects entries the grid could not draw', () => {
        localStorage.setItem(
            'mw-host-apps',
            JSON.stringify({ 'host-a': { ts: Date.now(), apps: [{ id: 0 }, { name: 42 }] } }),
        );
        expect(loadCachedApps('host-a')).toBeNull();
    });

    // Which covers were actually seen: the difference between "this host has no
    // art for that app" and "the request failed just now", which is what lets
    // the grid retry instead of settling for the generic pad.
    describe('covers already seen', () => {
        it('knows nothing until a cover has actually reached the screen', () => {
            saveCachedApps('host-a', [{ id: 1, name: 'A' }]);
            expect(boxArtWasSeen('host-a', 1)).toBe(false);
            noteBoxArtSeen('host-a', 1);
            expect(boxArtWasSeen('host-a', 1)).toBe(true);
            expect(boxArtWasSeen('host-a', 2)).toBe(false);
            expect(boxArtWasSeen('host-b', 1)).toBe(false);
        });

        it('keeps what it knows when the host adds a game', () => {
            saveCachedApps('host-a', [{ id: 1, name: 'A' }]);
            noteBoxArtSeen('host-a', 1);
            saveCachedApps('host-a', [
                { id: 1, name: 'A' },
                { id: 2, name: 'B' },
            ]);
            expect(boxArtWasSeen('host-a', 1)).toBe(true);
        });

        it('drops it with the app it belonged to', () => {
            saveCachedApps('host-a', [
                { id: 1, name: 'A' },
                { id: 2, name: 'B' },
            ]);
            noteBoxArtSeen('host-a', 2);
            saveCachedApps('host-a', [{ id: 1, name: 'A' }]);
            expect(boxArtWasSeen('host-a', 2)).toBe(false);
            expect(JSON.parse(localStorage.getItem('mw-host-apps'))['host-a'].art).toBeUndefined();
        });

        it('forgets it with the host', () => {
            saveCachedApps('host-a', [{ id: 1, name: 'A' }]);
            noteBoxArtSeen('host-a', 1);
            forgetCachedApps('host-a');
            expect(boxArtWasSeen('host-a', 1)).toBe(false);
        });
    });

    it('signs a list by ids, names and order — the three things the grid draws', () => {
        const a = [
            { id: 1, name: 'A' },
            { id: 2, name: 'B' },
        ];
        expect(appsSignature(a)).toBe(
            appsSignature([
                { id: 1, name: 'A' },
                { id: 2, name: 'B' },
            ]),
        );
        // A renamed app, a new app, and a reordered list all read as a change.
        expect(appsSignature(a)).not.toBe(
            appsSignature([
                { id: 1, name: 'A2' },
                { id: 2, name: 'B' },
            ]),
        );
        expect(appsSignature(a)).not.toBe(appsSignature([...a, { id: 3, name: 'C' }]));
        expect(appsSignature(a)).not.toBe(appsSignature([a[1], a[0]]));
        // Two names that concatenate to the same string must not collide.
        expect(appsSignature([{ id: 1, name: 'A B' }])).not.toBe(
            appsSignature([
                { id: 1, name: 'A' },
                { id: 1, name: 'B' },
            ]),
        );
    });
});
