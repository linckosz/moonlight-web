/*
 * MoonlightWeb — frontend TNR. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * A link straight to one app is made from the host's ⋮ menu (issue #63, Bruno's
 * decisions of 14/11): no button on the app cards any more; an entry after
 * Rename — "Create home app" on a phone or a tablet, "App shortcut…" on a
 * computer — opens a sheet listing the apps and displays, and the app picked
 * goes the way a link has always been made. The icon is named machine · app ·
 * MoonlightWeb.
 */
import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';

const bd = vi.hoisted(() => ({ touch: true, tv: false }));

vi.mock('../js/api/BackendClient.js', () => ({
    BackendClient: { adminLocked: false, getHosts: vi.fn(), getAppList: vi.fn() },
}));
vi.mock('../js/i18n/i18n.js', () => ({
    t: (key, vars) => (vars ? `${key} ${JSON.stringify(vars)}` : key),
}));
vi.mock('../js/util/BrowserDetect.js', async (importOriginal) => {
    const real = await importOriginal();
    return {
        ...real,
        get IS_TOUCH_DEVICE() {
            return bd.touch;
        },
        get IS_TV() {
            return bd.tv;
        },
    };
});

import { HostListView } from '../js/ui/HostListView.js';
import { Host } from '../js/models/Host.js';
import { BackendClient } from '../js/api/BackendClient.js';
import { Toast } from '../js/ui/Toast.js';
import { appIconName, appIconShortName, pickAppForShortcut } from '../js/ui/AppShortcutSheet.js';

const HOST = {
    uuid: 'h1',
    name: 'DualRTX — MoonlightWeb Host',
    state: 'online',
    pairState: 'paired',
};
const APPS = [
    { id: 1, displayName: 'MoonlightWeb Virtual Display' },
    { id: 2, displayName: 'Display 1' },
    { id: 3, displayName: 'Display 2', isOff: true },
    { id: 4, displayName: 'Steam Big Picture' },
];

const sheet = () => document.querySelector('.app-shortcut-sheet');

describe('the name of a link’s home-screen icon', () => {
    it.each([
        [
            'DualRTX — MoonlightWeb Host',
            'MoonlightWeb Virtual Display',
            'DualRTX · Virtual Display · MoonlightWeb',
        ],
        ['DualRTX — MoonlightWeb Host', 'Display 1', 'DualRTX · Display 1 · MoonlightWeb'],
        ['Living room', 'Steam Big Picture', 'Living room · Steam Big Picture · MoonlightWeb'],
        ['UM790Pro', 'MoonlightWeb', 'UM790Pro · MoonlightWeb · MoonlightWeb'],
        ['', 'Desktop', 'Desktop · MoonlightWeb'],
    ])('%s / %s → %s', (host, app, name) => {
        expect(appIconName({ displayName: host }, { displayName: app })).toBe(name);
    });

    it('keeps a name the user gave the machine as it is', () => {
        const host = new Host({ ...HOST, customName: 'Bureau — MoonlightWeb Host 2' });
        expect(appIconName(host, APPS[1])).toBe(
            'Bureau — MoonlightWeb Host 2 · Display 1 · MoonlightWeb',
        );
    });

    // I63SHORT: under the icon, the app alone, as it reads in the name.
    it('gives the app alone as the short name', () => {
        expect(appIconShortName(APPS[0])).toBe('Virtual Display');
        expect(appIconShortName(APPS[1])).toBe('Display 1');
        expect(appIconShortName({ displayName: 'MoonlightWeb' })).toBe('MoonlightWeb');
        expect(appIconShortName({ displayName: '  ' })).toBe('');
    });
});

describe('the host’s ⋮ menu entry', () => {
    let view;
    const menu = (fields = {}) => {
        const holder = document.createElement('div');
        holder.innerHTML = view.renderCard(new Host({ ...HOST, ...fields }));
        return [...holder.querySelectorAll('.host-menu .host-menu-item')];
    };
    const shortcut = (fields) =>
        menu(fields).find((b) => b.classList.contains('btn-app-shortcut')) || null;

    beforeEach(() => {
        bd.touch = true;
        bd.tv = false;
        BackendClient.adminLocked = false;
        document.body.innerHTML = '<div id="root"></div>';
        view = new HostListView(document.getElementById('root'));
        view.onAppLink = vi.fn(async () => {});
    });
    afterEach(() => {
        view.destroy();
        localStorage.clear();
        vi.restoreAllMocks();
    });

    it('says "Create home app" on a phone or a tablet, right after Rename', () => {
        const items = menu();
        expect(items[0].classList.contains('btn-rename')).toBe(true);
        expect(items[1].classList.contains('btn-app-shortcut')).toBe(true);
        expect(items[1].textContent).toBe('appLink.menuTouch');
    });

    it('says "App shortcut…" on a computer', () => {
        bd.touch = false;
        expect(shortcut().textContent).toBe('appLink.menuDesktop');
    });

    it.each([
        ['a booth', () => (BackendClient.adminLocked = true), {}],
        ['a TV', () => (bd.tv = true), {}],
        ['a Wolf host', () => {}, { backendType: 'wolf' }],
        ['a MultiSeat host', () => {}, { backendType: 'multiseat' }],
        ['a host offline', () => {}, { state: 'offline' }],
        ['a host not paired', () => {}, { pairState: 'unpaired' }],
        ['a page that makes no link', () => (view.onAppLink = null), {}],
    ])('is not offered on %s', (_name, set, fields) => {
        set();
        expect(shortcut(fields)).toBe(null);
    });

    it('is not offered on a TV over plain HTTP, whose menu is hidden', () => {
        view.plainHttp = true;
        const holder = document.createElement('div');
        holder.innerHTML = view.renderCard(new Host(HOST));
        expect(holder.querySelector('.host-card-menu').style.display).toBe('none');
    });

    it('leaves the app cards with no link button', () => {
        view.hosts = [new Host(HOST)];
        expect(view.renderApp(APPS[1], [], 0)).not.toContain('app-card-link');
    });

    it('opens the sheet on the apps a card would open, then makes the link of the one picked', async () => {
        view.hosts = [new Host(HOST)];
        view.appsByHost.h1 = { apps: APPS };
        const made = view._makeAppShortcut(view.hosts[0]);
        expect(sheet()).not.toBe(null);
        const options = [...sheet().querySelectorAll('option')].map((o) => o.textContent);
        expect(options).toEqual(['MoonlightWeb Virtual Display', 'Display 1', 'Steam Big Picture']);
        expect(sheet().querySelector('[data-name]').textContent).toBe(
            'DualRTX · Virtual Display · MoonlightWeb',
        );

        const select = sheet().querySelector('select');
        select.value = '2';
        select.dispatchEvent(new Event('change'));
        expect(sheet().querySelector('[data-name]').textContent).toBe(
            'DualRTX · Display 1 · MoonlightWeb',
        );
        sheet().querySelector('[data-answer="yes"]').click();
        await made;
        expect(sheet()).toBe(null);
        expect(view.onAppLink).toHaveBeenCalledTimes(1);
        expect(view.onAppLink.mock.calls[0][0].uuid).toBe('h1');
        expect(view.onAppLink.mock.calls[0][1].id).toBe(2);
    });

    it('makes nothing when the sheet is closed', async () => {
        view.hosts = [new Host(HOST)];
        view.appsByHost.h1 = { apps: APPS };
        for (const close of [
            () => sheet().querySelector('[data-answer="no"]').click(),
            () => document.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape' })),
            () => document.querySelector('.app-shortcut-overlay').click(),
        ]) {
            const made = view._makeAppShortcut(view.hosts[0]);
            close();
            await made;
            expect(sheet()).toBe(null);
        }
        expect(view.onAppLink).not.toHaveBeenCalled();
    });

    it('says so when the host has no app to offer yet', async () => {
        const error = vi.spyOn(Toast, 'error').mockImplementation(() => {});
        view.hosts = [new Host(HOST)];
        await view._makeAppShortcut(view.hosts[0]);
        expect(error).toHaveBeenCalledWith('appLink.sheetNoApps');
        expect(sheet()).toBe(null);
    });

    it('opens from the menu entry itself', async () => {
        view.hosts = [new Host(HOST)];
        view.appsByHost.h1 = { apps: APPS };
        const spy = vi.spyOn(view, '_makeAppShortcut');
        document.getElementById('root').innerHTML = view.renderCard(view.hosts[0]);
        document.querySelector('.btn-app-shortcut').click();
        expect(spy).toHaveBeenCalledWith(view.hosts[0]);
        document.querySelector('.app-shortcut-overlay')?.click();
    });
});

describe('the sheet on a computer', () => {
    afterEach(() => {
        document.body.innerHTML = '';
    });

    it('copies a link rather than naming an icon', async () => {
        const picked = pickAppForShortcut({
            host: { displayName: 'DualRTX' },
            apps: APPS.slice(0, 2),
            touch: false,
        });
        expect(sheet().querySelector('[data-name]')).toBe(null);
        expect(sheet().querySelector('[data-answer="yes"]').textContent).toBe('appLink.sheetCopy');
        expect(sheet().textContent).toContain('appLink.sheetIntroDesktop {"host":"DualRTX"}');
        sheet().querySelector('[data-answer="yes"]').click();
        expect((await picked).id).toBe(1);
    });

    it('writes app names as text, never as markup', async () => {
        const picked = pickAppForShortcut({
            host: { displayName: '<b>PC</b>' },
            apps: [{ id: 9, displayName: '<img src=x onerror=alert(1)>' }],
            touch: true,
        });
        expect(sheet().querySelector('img')).toBe(null);
        expect(sheet().querySelector('b:not(strong)')).toBe(null);
        expect(sheet().querySelector('option').textContent).toBe('<img src=x onerror=alert(1)>');
        sheet().querySelector('[data-answer="no"]').click();
        expect(await picked).toBe(null);
    });
});
