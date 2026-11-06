/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';

// "Apps on this PC": the native host's apps, a card each — a name, the command
// or link that opens it, an image — on every OS the native host streams on. A
// section of its own, only where the server says the native host streams, each
// change one request whose answer is the whole section, and the list shown
// afterwards always the server's. Launchers found on the PC are badged, and
// one the owner removed is offered back.
vi.mock('../js/api/BackendClient.js', () => ({
    BackendClient: {
        getHostApps: vi.fn(),
        saveHostApps: vi.fn(),
        restoreHostApp: vi.fn(),
        setHostAppImage: vi.fn(),
        removeHostAppImage: vi.fn(),
    },
}));
vi.mock('../js/ui/Toast.js', () => ({
    Toast: { success: vi.fn(), error: vi.fn(), warning: vi.fn() },
}));

import { AdminView } from '../js/ui/AdminView.js';
import { BackendClient } from '../js/api/BackendClient.js';
import { Toast } from '../js/ui/Toast.js';

const A = { id: 'a-id', app_id: 2000, name: 'A', command: 'a', source: 'user' };
const STEAM = {
    id: 's-id',
    app_id: 2001,
    name: 'Steam Big Picture',
    command: 'steam://open/bigpicture',
    source: 'steam',
    image: '0123456789abcdef',
};

describe('AdminView — apps on this PC', () => {
    let view;

    const $ = (sel) => document.querySelector(sel);
    const mount = () => {
        document.body.innerHTML = `<div>${view._renderHostApps()}</div>`;
        view.container = document.body;
        view._bindHostApps();
    };
    const type = (sel, value) => {
        $(sel).value = value;
        $(sel).dispatchEvent(new Event('input'));
    };
    const state = (apps, extra = {}) => ({
        supported: true,
        max: 16,
        screen: 'virtual',
        apps,
        detected: [],
        ...extra,
    });

    beforeEach(() => {
        document.body.innerHTML = '<div></div>';
        view = new AdminView(document.body, () => {});
        for (const fn of Object.values(BackendClient)) fn.mockReset();
        Toast.success.mockReset();
        Toast.error.mockReset();
    });

    afterEach(() => {
        view.destroy();
    });

    it('reads the apps, the limit, where they open and what this PC has', async () => {
        BackendClient.getHostApps.mockResolvedValue(
            state([A], {
                max: 8,
                screen: 'gamescope',
                detected: [{ source: 'steam', name: 'Steam', listed: false, dismissed: true }],
            }),
        );
        await view._loadHostApps();
        expect(view._hostApps).toEqual([A]);
        expect(view._hostAppsSupported).toBe(true);
        expect(view._hostAppsMax).toBe(8);
        expect(view._hostAppsScreen).toBe('gamescope');
        expect(view._hostAppsDetected.length).toBe(1);
    });

    it('stays hidden when the server has no such route (an older backend)', async () => {
        BackendClient.getHostApps.mockRejectedValue(new Error('Not found'));
        await view._loadHostApps();
        expect(view._hostAppsSupported).toBe(false);
        expect(view._hostApps).toEqual([]);
    });

    it('says where the apps open', () => {
        for (const [screen, key] of [
            ['virtual', 'admin.hostAppsDescVirtual'],
            ['gamescope', 'admin.hostAppsDescGamescope'],
            ['primary', 'admin.hostAppsDescPrimary'],
        ]) {
            view._hostAppsScreen = screen;
            mount();
            expect($('.settings-section-desc').textContent).toBe(key);
        }
    });

    it('lists each app escaped, its image, and a launcher found on this PC badged', () => {
        view._hostApps = [{ ...A, name: '<b>Emu</b>', command: 'emu --x "a&b"' }, STEAM];
        mount();
        const rows = document.querySelectorAll('.host-app-row');
        expect(rows[0].querySelector('.host-app-name').textContent).toBe('<b>Emu</b>');
        expect(rows[0].querySelector('.host-app-command').textContent).toBe('emu --x "a&b"');
        expect(rows[0].querySelector('b')).toBeNull();
        expect(rows[0].querySelector('img')).toBeNull();
        expect(rows[0].querySelector('.host-app-badge')).toBeNull();
        expect(rows[0].querySelector('[data-host-app-image-remove]')).toBeNull();
        expect(rows[1].querySelector('img').getAttribute('src')).toBe(
            '/api/hosts/moonlightweb-native-host/appasset?appid=2001&v=0123456789abcdef',
        );
        expect(rows[1].querySelector('.host-app-badge').textContent).toBe('admin.hostAppAuto');
        expect(rows[1].querySelector('[data-host-app-image-remove]')).not.toBeNull();
    });

    it('is the last part of the MoonlightWeb Host section, not under Advanced', () => {
        view._hostAppsSupported = true;
        document.body.innerHTML = view._renderAdvanced();
        expect($('#admin-section-host-apps')).toBeNull();
        document.body.innerHTML = view._renderNativeHost();
        const host = $('#admin-section-native-host');
        expect(host.lastElementChild.id).toBe('admin-section-host-apps');
        mount();
        const section = $('#admin-section-host-apps');
        expect(section.querySelector('.admin-subsection-title').textContent).toBe('admin.hostApps');
        expect($('#host-app-command').placeholder).toBe('admin.hostAppCommand');
        expect(section.textContent).toContain('admin.hostAppsHint');
    });

    it('says so when there is none yet', () => {
        mount();
        expect($('.host-app-list')).toBeNull();
        expect(document.body.textContent).toContain('admin.hostAppsEmpty');
    });

    it('adds an app once both fields are filled, sending the ids it keeps', async () => {
        view._hostApps = [A];
        const added = {
            id: 'h-id',
            app_id: 2002,
            name: 'Heroic',
            command: 'heroic',
            source: 'user',
        };
        BackendClient.saveHostApps.mockResolvedValue(state([A, added]));
        mount();
        expect($('#btn-host-app-add').disabled).toBe(true);
        type('#host-app-name', '  Heroic ');
        expect($('#btn-host-app-add').disabled).toBe(true);
        type('#host-app-command', 'heroic');
        expect($('#btn-host-app-add').disabled).toBe(false);
        $('#btn-host-app-add').click();
        await vi.waitFor(() => expect(Toast.success).toHaveBeenCalled());
        expect(Toast.success).toHaveBeenCalledWith('admin.hostAppAdded');
        expect(BackendClient.saveHostApps).toHaveBeenCalledWith([
            { id: 'a-id', name: 'A', command: 'a' },
            { name: 'Heroic', command: 'heroic' },
        ]);
        expect(BackendClient.setHostAppImage).not.toHaveBeenCalled();
        expect(view._hostApps).toEqual([A, added]);
        // Drawn again from what the server kept, the form empty.
        expect(document.querySelectorAll('.host-app-row').length).toBe(2);
        expect($('#host-app-name').value).toBe('');
    });

    it('sends the picked image for the new entry, as the card draws it', async () => {
        const added = { id: 'h-id', app_id: 2002, name: 'H', command: 'h', source: 'user' };
        BackendClient.saveHostApps.mockResolvedValue(state([added]));
        BackendClient.setHostAppImage.mockResolvedValue(
            state([{ ...added, image: 'ff00ff00ff00ff00' }]),
        );
        view._hostAppPng = vi.fn().mockResolvedValue('UE5H');
        await view._addHostApp({ name: 'H', command: 'h' }, new Blob(['x']));
        expect(BackendClient.setHostAppImage).toHaveBeenCalledWith('h-id', 'UE5H');
        expect(view._hostApps[0].image).toBe('ff00ff00ff00ff00');
    });

    it('removes an app', async () => {
        view._hostApps = [A, STEAM];
        BackendClient.saveHostApps.mockResolvedValue(state([STEAM]));
        mount();
        document.querySelector('[data-host-app-remove="0"]').click();
        await vi.waitFor(() => expect(Toast.success).toHaveBeenCalled());
        expect(Toast.success).toHaveBeenCalledWith('admin.hostAppRemoved');
        expect(BackendClient.saveHostApps).toHaveBeenCalledWith([
            { id: 's-id', name: 'Steam Big Picture', command: 'steam://open/bigpicture' },
        ]);
        expect(document.querySelectorAll('.host-app-row').length).toBe(1);
    });

    it('removes an image', async () => {
        view._hostApps = [STEAM];
        BackendClient.removeHostAppImage.mockResolvedValue(state([{ ...STEAM, image: undefined }]));
        mount();
        document.querySelector('[data-host-app-image-remove="0"]').click();
        await vi.waitFor(() => expect(Toast.success).toHaveBeenCalled());
        expect(BackendClient.removeHostAppImage).toHaveBeenCalledWith('s-id');
        expect(Toast.success).toHaveBeenCalledWith('admin.hostAppImageRemoved');
    });

    it('offers back a launcher found on this PC that the owner removed', async () => {
        view._hostAppsDetected = [
            { source: 'steam', name: 'Steam Big Picture', listed: false, dismissed: true },
            { source: 'playnite', name: 'Playnite', listed: true, dismissed: false },
        ];
        BackendClient.restoreHostApp.mockResolvedValue(state([STEAM]));
        mount();
        const buttons = document.querySelectorAll('[data-host-app-restore]');
        expect(buttons.length).toBe(1);
        buttons[0].click();
        await vi.waitFor(() => expect(Toast.success).toHaveBeenCalled());
        expect(BackendClient.restoreHostApp).toHaveBeenCalledWith('steam');
        expect(Toast.success).toHaveBeenCalledWith('admin.hostAppRestored');
        expect(document.querySelectorAll('.host-app-row').length).toBe(1);
    });

    it('keeps the list as it was when the server refuses it', async () => {
        view._hostApps = [A];
        BackendClient.saveHostApps.mockRejectedValue(new Error('16 apps at most'));
        mount();
        document.querySelector('[data-host-app-remove="0"]').click();
        await vi.waitFor(() => expect(Toast.error).toHaveBeenCalled());
        expect(view._hostApps).toEqual([A]);
        expect(document.querySelectorAll('.host-app-row').length).toBe(1);
        expect(Toast.success).not.toHaveBeenCalled();
    });

    it('takes no more once the list is full', () => {
        view._hostAppsMax = 2;
        view._hostApps = [A, STEAM];
        mount();
        expect($('#host-app-name').disabled).toBe(true);
        expect($('#host-app-command').disabled).toBe(true);
        expect($('#host-app-file').disabled).toBe(true);
        expect(document.body.textContent).toContain('admin.hostAppsFull');
    });
});
