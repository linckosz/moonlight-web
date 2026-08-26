/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect } from 'vitest';
import { App } from '../js/models/App.js';
import { Host } from '../js/models/Host.js';

describe('App model', () => {
    it('applies defaults for missing fields', () => {
        const a = new App({});
        expect(a.id).toBe(0);
        expect(a.name).toBe('Unknown App');
        expect(a.hdrSupported).toBe(false);
        expect(a.hostUuid).toBeNull();
        expect(a.displayName).toBe('Unknown App');
    });

    it('builds a boxArtUrl only when host + id are known', () => {
        expect(new App({ id: 12 }).boxArtUrl).toBeNull(); // no host
        expect(new App({ id: 0 }, 'uuid').boxArtUrl).toBeNull(); // no id
        const a = new App({ id: 7, name: 'Game' }, 'host/uuid');
        expect(a.boxArtUrl).toBe('/api/hosts/host%2Fuuid/appasset?appid=7');
    });
});

describe('Host model', () => {
    it('defaults the port and address fields', () => {
        const h = new Host({});
        expect(h.port).toBe(47989);
        expect(h.state).toBe('unknown');
        expect(h.displayName).toBe('Unknown Host');
    });

    it('derives online/paired/locked/available state', () => {
        const online = new Host({ state: 'online', pairState: 'paired' });
        expect(online.isOnline).toBe(true);
        expect(online.isAvailable).toBe(true);
        expect(online.isLocked).toBe(false);

        const locked = new Host({ state: 'online', pairState: 'unpaired' });
        expect(locked.isLocked).toBe(true);
        expect(locked.isAvailable).toBe(false);

        const offline = new Host({ state: 'offline' });
        expect(offline.isOnline).toBe(false);
    });

    // The MAC never reaches the browser — the backend sends wakeSupported, which
    // already accounts for an unknown or all-zero address.
    it('offers Wake-on-LAN only for offline hosts the server can wake', () => {
        expect(new Host({ state: 'offline', wakeSupported: true }).canWake).toBe(true);
        expect(new Host({ state: 'offline', wakeSupported: false }).canWake).toBe(false);
        expect(new Host({ state: 'offline' }).canWake).toBe(false);
        expect(new Host({ state: 'online', wakeSupported: true }).canWake).toBe(false);
        // Service down but machine reachable: nothing to wake.
        expect(new Host({ state: 'offline', reachable: true, wakeSupported: true }).canWake).toBe(
            false,
        );
    });

    it('never exposes host addresses on the model', () => {
        const h = new Host({ name: 'Desk', activeAddress: '10.0.0.5', macAddress: 'AA:BB' });
        expect(h.activeAddress).toBeUndefined();
        expect(h.macAddress).toBeUndefined();
        expect(h.localAddress).toBeUndefined();
    });

    it('picks a display name from name, never from the address', () => {
        expect(new Host({ name: 'Desk' }).displayName).toBe('Desk');
        expect(new Host({ name: 'UNKNOWN' }).displayName).toBe('Unknown Host');
        expect(new Host({ name: '' }).displayName).toBe('Unknown Host');
    });

    it('exposes the GPU model when known', () => {
        expect(new Host({}).displayGpu).toBe('');
        expect(new Host({ gpuModel: 'RTX' }).displayGpu).toBe('RTX');
    });

    it('maps status to class/icon and a translated label', () => {
        const offline = new Host({ state: 'offline' });
        expect(offline.statusClass).toBe('offline');
        expect(typeof offline.statusIcon).toBe('string');
        expect(typeof offline.statusLabel).toBe('string');

        expect(new Host({ state: 'online', pairState: 'paired' }).statusClass).toBe('ready');
        expect(new Host({ state: 'online', pairState: 'no' }).statusClass).toBe('locked');
    });
});
