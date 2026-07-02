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

    it('offers Wake-on-LAN only for offline hosts with a real MAC', () => {
        expect(new Host({ state: 'offline', macAddress: 'AA:BB:CC:DD:EE:FF' }).canWake).toBe(true);
        expect(new Host({ state: 'offline', macAddress: '00:00:00:00:00:00' }).canWake).toBe(false);
        expect(new Host({ state: 'offline' }).canWake).toBe(false);
        expect(new Host({ state: 'online', macAddress: 'AA:BB:CC:DD:EE:FF' }).canWake).toBe(false);
    });

    it('picks a display name from name, never from the address', () => {
        expect(new Host({ name: 'Desk' }).displayName).toBe('Desk');
        expect(new Host({ name: 'UNKNOWN', activeAddress: '10.0.0.5' }).displayName).toBe(
            'Unknown Host',
        );
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

    it('formats the best display mode resolution', () => {
        expect(new Host({}).resolutionText).toBe('');
        const h = new Host({ displayModes: [{ width: 1920, height: 1080, refreshRate: 120 }] });
        expect(h.resolutionText).toBe('1920×1080 @ 120Hz');
        // refreshRate defaults to 60 when absent
        expect(new Host({ displayModes: [{ width: 1280, height: 720 }] }).resolutionText).toBe(
            '1280×720 @ 60Hz',
        );
    });
});
