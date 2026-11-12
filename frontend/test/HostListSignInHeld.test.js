/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 */
/*
 * A Windows host whose session Remote Desktop left, while somebody else is
 * signed in on the PC: the host would refuse the sign-in screen, so the card
 * says so in place of the app (never who), and a kiosk keeps the plain text.
 */
import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';

vi.mock('../js/api/BackendClient.js', () => ({
    BackendClient: {
        adminLocked: false,
        getAppList: vi.fn(async () => ({ status: 'ok', apps: [] })),
        getVirtualDisplay: vi.fn(),
    },
}));
vi.mock('../js/i18n/i18n.js', () => ({ t: (key) => key }));

import { HostListView } from '../js/ui/HostListView.js';
import { Host } from '../js/models/Host.js';
import { BackendClient } from '../js/api/BackendClient.js';

const PC = {
    uuid: 'moonlightweb-native-host',
    name: 'N95 — MoonlightWeb Host',
    state: 'online',
    pairState: 'paired',
    backendType: 'native',
};
const left = (extra) => ({
    ...PC,
    nativeDisplay: { state: 'session_disconnected', input_permission: true, ...extra },
});

describe('a session left by Remote Desktop, somebody signed in on the PC', () => {
    let view;
    let container;

    beforeEach(() => {
        vi.clearAllMocks();
        BackendClient.adminLocked = false;
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

    it('says somebody is signed in, and offers no app', () => {
        const card = mount(left({ sign_in_screen: false, sign_in_held: true }));
        expect(card.querySelector('.host-empty-display-text').textContent).toBe(
            'hosts.somebodySignedIn',
        );
        expect(card.querySelector('.host-apps')).toBeNull();
        expect(BackendClient.getAppList).not.toHaveBeenCalled();
    });

    it('a kiosk keeps the plain text', () => {
        BackendClient.adminLocked = true;
        const card = mount(left({ sign_in_screen: false, sign_in_held: true }));
        expect(card.querySelector('.host-empty-display-text').textContent).toBe(
            'hosts.sessionLeftByRemoteDesktop',
        );
    });

    it('nobody signed in and nothing offered: the plain text', () => {
        const card = mount(left({ sign_in_screen: false, sign_in_held: false }));
        expect(card.querySelector('.host-empty-display-text').textContent).toBe(
            'hosts.sessionLeftByRemoteDesktop',
        );
    });

    it('the sign-in screen offered: its app, no text', () => {
        const card = mount(left({ sign_in_screen: true, sign_in_held: false }));
        expect(card.querySelector('.host-empty-display-text')).toBeNull();
    });
});
