/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 */
/*
 * The native host card and "MoonlightWeb Virtual Display".
 *
 * A PC installed through a TV and then left without a screen used to lose its
 * card altogether: the engine answered "no display", ComputerManager dropped
 * the host, and the owner was left with a list that no longer mentioned the
 * machine serving it. Now the card stays. With "MoonlightWeb Virtual Display"
 * installed it is an ordinary card whose one app is that display — the server
 * lists it, on or off, and opening it turns it on. Without it, the card says
 * what is missing. What is verified here is the card's side of that contract:
 *
 *   - a headless host with the display installed paints its app grid and asks
 *     for its apps, like any host;
 *   - a headless host without it paints the empty state, from the server's
 *     `nativeDisplay` verdict, and asks for no apps;
 *   - the kebab never offers to add or remove the display: the installer
 *     owns it, and there is no such button anywhere;
 *   - a display appearing or leaving changes the card's fingerprint, so the
 *     poll repaints it.
 */
import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';

vi.mock('../js/api/BackendClient.js', () => ({
    BackendClient: {
        getAppList: vi.fn(async () => ({ status: 'ok', apps: [] })),
        getVirtualDisplay: vi.fn(),
    },
}));
vi.mock('../js/i18n/i18n.js', () => ({ t: (key) => key }));

import { HostListView } from '../js/ui/HostListView.js';
import { Host } from '../js/models/Host.js';
import { BackendClient } from '../js/api/BackendClient.js';

const NATIVE = {
    uuid: 'moonlightweb-native-host',
    name: 'UM790 — MoonlightWeb Host',
    state: 'online',
    pairState: 'paired',
    backendType: 'native',
};

const VD = {
    supported: true,
    installed: true,
    enabled: false,
    active: false,
    can_manage: true,
    method: 'task',
    name: 'MoonlightWeb Virtual Display',
};

const headless = (vd = {}) => ({
    ...NATIVE,
    nativeDisplay: { state: 'no_display', virtual_display: { ...VD, ...vd } },
});

const withDisplay = (vd = {}) => ({
    ...NATIVE,
    nativeDisplay: { state: 'ok', virtual_display: { ...VD, ...vd } },
});

describe('the native host card and the virtual display', () => {
    let view;
    let container;

    beforeEach(() => {
        localStorage.clear();
        vi.clearAllMocks();
        document.body.innerHTML = '<div id="root"></div>';
        container = document.getElementById('root');
    });

    afterEach(() => {
        if (view) view.destroy();
        view = null;
    });

    const mount = (data) => {
        view = new HostListView(container);
        view.hosts = [new Host(data)];
        view.renderList();
        return container.querySelector('.host-card');
    };

    it('headless with the display installed: an ordinary card, grid and all', () => {
        const card = mount(headless());
        expect(card).not.toBeNull();
        expect(card.querySelector('.host-empty-display')).toBeNull();
        expect(card.querySelector('.host-apps')).not.toBeNull();
        expect(card.querySelector('.status-badge').textContent).not.toBe('hosts.statusNoDisplay');
        expect(BackendClient.getAppList).toHaveBeenCalled();
    });

    it('headless without it: the empty state, and no apps asked for', () => {
        const card = mount(headless({ installed: false }));
        expect(card).not.toBeNull();
        expect(card.querySelector('.host-empty-display')).not.toBeNull();
        expect(card.querySelector('.host-empty-display-text').textContent).toBe(
            'vdisplay.emptyBody',
        );
        expect(card.querySelector('.host-apps')).toBeNull();
        expect(card.querySelector('.status-badge').textContent).toBe('hosts.statusNoDisplay');
        expect(BackendClient.getAppList).not.toHaveBeenCalled();
    });

    it('headless where no virtual display is shipped: never names the installer', () => {
        // Windows ARM64 (evo, Discord, 06/11): the card sent the owner to an
        // installer that has no virtual display to offer there.
        const card = mount(headless({ supported: false, installed: false, can_manage: false }));
        expect(card.querySelector('.host-empty-display-text').textContent).toBe(
            'vdisplay.emptyBodyNoDriver',
        );
        expect(card.querySelector('.host-apps')).toBeNull();
        expect(card.querySelector('.status-badge').textContent).toBe('hosts.statusNoDisplay');
        expect(BackendClient.getAppList).not.toHaveBeenCalled();
    });

    it('never offers to add or remove the display — the installer owns it', () => {
        for (const data of [headless(), headless({ installed: false }), withDisplay()]) {
            const card = mount(data);
            expect(card.querySelector('.btn-vdisplay-add')).toBeNull();
            expect(card.querySelector('.btn-vdisplay-remove')).toBeNull();
            expect(card.querySelector('.host-menu').textContent).not.toContain('vdisplay.');
            view.destroy();
            view = null;
        }
    });

    it('is untouched on a host that is not the native one', () => {
        const card = mount({
            uuid: 'sunshine-1',
            name: 'BENCH',
            state: 'online',
            pairState: 'paired',
        });
        expect(card.querySelector('.host-empty-display')).toBeNull();
        expect(card.querySelector('.host-apps')).not.toBeNull();
    });

    it('changes fingerprint when the display state moves, so the poll repaints', () => {
        view = new HostListView(container);
        const before = view._cardFingerprint(new Host(headless({ installed: false })));
        const after = view._cardFingerprint(new Host(withDisplay()));
        expect(before).not.toBe(after);
        const on = view._cardFingerprint(new Host(headless({ enabled: true, active: true })));
        expect(on).not.toBe(view._cardFingerprint(new Host(headless())));
    });
});

describe('the Host model', () => {
    it('needs a virtual display only when headless AND none is installed', () => {
        expect(new Host(headless({ installed: false })).needsVirtualDisplay).toBe(true);
        expect(new Host(headless()).needsVirtualDisplay).toBe(false);
        expect(new Host(withDisplay()).needsVirtualDisplay).toBe(false);
        // A Sunshine host carrying the same object by accident is still not native.
        expect(
            new Host({ ...headless({ installed: false }), backendType: '' }).needsVirtualDisplay,
        ).toBe(false);
        expect(new Host(NATIVE).needsVirtualDisplay).toBe(false);
    });

    it('can have a virtual display unless the server says none is shipped', () => {
        expect(new Host(headless({ installed: false })).canHaveVirtualDisplay).toBe(true);
        expect(new Host(headless({ supported: false })).canHaveVirtualDisplay).toBe(false);
        // An answer without the field keeps the old text rather than hiding it.
        expect(new Host(NATIVE).canHaveVirtualDisplay).toBe(true);
    });
});
