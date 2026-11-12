/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect } from 'vitest';
import { App, sortApps } from '../js/models/App.js';
import { Host } from '../js/models/Host.js';
import { t } from '../js/i18n/i18n.js';

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

    it('knows the screens made for the stream: the virtual display and gamescope apps', () => {
        const card = (key) => new App({ id: 3, device: { key } });
        expect(card('moonlightweb-virtual-display').isMadeForStream).toBe(true);
        expect(card('moonlightweb-virtual-display').isGamescope).toBe(false);
        expect(card('gamescope-steam').isMadeForStream).toBe(true);
        expect(card('gamescope-steam').isGamescope).toBe(true);
        expect(card('gamescope-app-Cube').isGamescope).toBe(true);
        expect(card('gamescope-app-Cube').isVirtualDisplay).toBe(false);
        expect(card('HDMI-A-1').isMadeForStream).toBe(false);
        expect(new App({ id: 3 }).isMadeForStream).toBe(false);
    });

    it("knows the native host's apps, and the screen each opens on", () => {
        const app = (device) => new App({ id: 2001, device }, 'moonlightweb-native-host');
        const onVirtual = app({ key: 'host-app-x', screen: 'virtual' });
        expect(onVirtual.isHostApp).toBe(true);
        expect(onVirtual.screen).toBe('virtual');
        expect(onVirtual.isMadeForStream).toBe(true);
        const onPrimary = app({ key: 'host-app-y', screen: 'primary' });
        expect(onPrimary.isMadeForStream).toBe(false);
        expect(app({ key: 'gamescope-app-z', screen: 'gamescope' }).isGamescope).toBe(true);
        expect(app({ key: 'HDMI-A-1' }).isHostApp).toBe(false);
        expect(app({ key: 'HDMI-A-1' }).screen).toBeNull();
    });

    it("puts an app image's hash in its URL, so each version is a URL of its own", () => {
        const withArt = new App(
            { id: 2001, boxArt: true, device: { screen: 'virtual', art: 'ab12' } },
            'moonlightweb-native-host',
        );
        expect(withArt.boxArtUrl).toBe(
            '/api/hosts/moonlightweb-native-host/appasset?appid=2001&v=ab12',
        );
        const without = new App({ id: 2002, boxArt: false, device: { screen: 'virtual' } }, 'n');
        expect(without.boxArtUrl).toBeNull();
    });
});

describe('sortApps', () => {
    const virtual = { key: 'moonlightweb-virtual-display' };

    it('puts the virtual display first, then the names, numbers counted as numbers', () => {
        const apps = [
            new App({ id: 1, name: 'Screen 10' }),
            new App({ id: 2, name: 'screen 2' }),
            new App({ id: 3, name: 'MoonlightWeb Virtual Display', device: virtual }),
            new App({ id: 4, name: 'Desktop' }),
        ];
        expect(sortApps(apps).map((a) => a.id)).toEqual([3, 4, 2, 1]);
    });

    it("sorts the host's raw entries the same way, a nameless one included", () => {
        const raw = [{ id: 1, name: 'Zed' }, { id: 2 }, { id: 3, name: 'Any', device: virtual }];
        expect(sortApps(raw).map((a) => a.id)).toEqual([3, 2, 1]);
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

    it('is not available while its certificate is not the one of the pairing', () => {
        const refused = new Host({
            state: 'online',
            pairState: 'paired',
            certificateChanged: true,
        });
        expect(refused.isAvailable).toBe(false);
        expect(refused.isLocked).toBe(false);
        expect(refused.statusClass).toBe('locked');
        expect(refused.statusLabel).toBe(t('hosts.statusCertificateChanged'));
        // Off the network it is simply offline: nothing answers to refuse.
        const gone = new Host({ state: 'offline', pairState: 'paired', certificateChanged: true });
        expect(gone.statusClass).toBe('offline');
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
