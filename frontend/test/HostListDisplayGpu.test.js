/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 *
 * The GPU under a native host's display cards (01/11/2026: DualRTX has two
 * GPUs of one name, and nothing said which screen was on which). Pinned down:
 * the line names the GPU, numbered between namesakes, and only where the host
 * sent one; on the virtual display's card its administrator picks the GPU in
 * a list, "Auto" first, which neither launches the stream nor stays on a
 * choice the server refused; anyone else sees the line only.
 */
import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';

vi.mock('../js/api/BackendClient.js', () => ({
    BackendClient: {
        adminLocked: false,
        getAppList: vi.fn(),
        setVirtualDisplayGpu: vi.fn(),
    },
}));
vi.mock('../js/i18n/i18n.js', () => ({
    t: (key, p = {}) => (key === 'apps.gpuNumbered' ? `${p.name} · n° ${p.n}` : key),
}));
vi.mock('../js/ui/Toast.js', () => ({ Toast: { error: vi.fn(), info: vi.fn() } }));

import { HostListView } from '../js/ui/HostListView.js';
import { App } from '../js/models/App.js';
import { BackendClient } from '../js/api/BackendClient.js';
import { Toast } from '../js/ui/Toast.js';

const UUID = 'moonlightweb-native-host';
const RTX = 'NVIDIA GeForce RTX 5060 Ti';
const AMD = 'AMD Radeon(TM) Graphics';

const display = (extra = {}) => ({
    id: 1,
    name: 'Display 1',
    boxArt: false,
    device: { os: 'windows', display: 'external', model: 'M27Q', key: 'k1', ...extra },
});

const virtualDisplay = (extra = {}) => ({
    id: 1000,
    name: 'MoonlightWeb Virtual Display',
    boxArt: false,
    device: {
        os: 'windows',
        display: 'virtual',
        model: 'MoonlightWeb Virtual Display',
        key: 'moonlightweb-virtual-display',
        owned: true,
        gpuChoice: { selected: '', options: [RTX, AMD] },
        ...extra,
    },
});

describe('the GPU under a native display card', () => {
    let view;
    let container;

    beforeEach(() => {
        vi.clearAllMocks();
        BackendClient.adminLocked = false;
        document.body.innerHTML = '<div id="root"></div>';
        container = document.getElementById('root');
        view = new HostListView(container);
    });

    afterEach(() => {
        if (view) view.destroy();
        view = null;
        localStorage.clear();
    });

    /** The grid of one native host, as _paintApps draws it. */
    function paint(apps) {
        container.innerHTML = `<div class="host-card" data-uuid="${UUID}"><div class="host-apps"></div></div>`;
        const cont = container.querySelector('.host-apps');
        view._paintApps(cont, { apps: apps.map((a) => new App(a, UUID)) }, UUID);
        return cont;
    }

    it('names the GPU, with its number between two of one name', () => {
        const cont = paint([
            display({ gpu: RTX, gpuNumber: 2 }),
            { ...display({ gpu: AMD, key: 'k2' }), id: 2, name: 'Display 2' },
        ]);
        const lines = [...cont.querySelectorAll('.app-card-gpu')].map((l) => l.textContent);
        expect(lines).toEqual([`RTX 5060 Ti · n° 2`, 'Radeon Graphics']);
    });

    it('keeps the full name in the tooltip', () => {
        const cont = paint([display({ gpu: RTX, gpuNumber: 2 })]);
        expect(cont.querySelector('.app-card-gpu').title).toBe(`${RTX} · n° 2`);
    });

    it('keeps full two different names that would read the same', () => {
        const other = 'NVIDIA RTX 5060 Ti';
        const cont = paint([
            display({ gpu: RTX }),
            { ...display({ gpu: other, key: 'k2' }), id: 2, name: 'Display 2' },
        ]);
        const lines = [...cont.querySelectorAll('.app-card-gpu')].map((l) => l.textContent);
        expect(lines).toEqual([RTX, other]);
    });

    it('says nothing on a PC with one GPU', () => {
        const cont = paint([display()]);
        expect(cont.querySelector('.app-card-gpu')).toBe(null);
    });

    it("offers its administrator the virtual display's GPU, Auto first", () => {
        view.canAdminister = true;
        const cont = paint([virtualDisplay({ gpuChoice: { selected: AMD, options: [RTX, AMD] } })]);
        const select = cont.querySelector('.app-card-gpu-select');
        expect([...select.options].map((o) => o.value)).toEqual(['', RTX, AMD]);
        expect(select.value).toBe(AMD);
        // Short to read, full to the driver.
        expect([...select.options].map((o) => o.textContent)).toEqual([
            'apps.gpuAuto',
            'RTX 5060 Ti',
            'Radeon Graphics',
        ]);
        expect(select.closest('label').title).toBe(`${AMD}
apps.gpuChoiceHint`);
    });

    it('says, on Auto, which GPU draws the display while it is on', () => {
        view.canAdminister = true;
        const cont = paint([virtualDisplay({ gpu: RTX, gpuNumber: 1 })]);
        const select = cont.querySelector('.app-card-gpu-select');
        expect(select.value).toBe('');
        expect(select.options[0].textContent).toBe(`apps.gpuAuto · RTX 5060 Ti · n° 1`);
    });

    it('shows anyone else, and a booth, the line only', () => {
        const cont = paint([virtualDisplay({ gpu: RTX })]);
        expect(cont.querySelector('select')).toBe(null);
        expect(cont.querySelector('.app-card-gpu').textContent).toBe('RTX 5060 Ti');

        view.canAdminister = true;
        BackendClient.adminLocked = true;
        expect(paint([virtualDisplay({ gpu: RTX })]).querySelector('select')).toBe(null);
    });

    it('never starts the stream from the list', () => {
        view.canAdminister = true;
        view.onLaunchApp = vi.fn();
        const cont = paint([virtualDisplay()]);
        const select = cont.querySelector('.app-card-gpu-select');
        select.click();
        select.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', bubbles: true }));
        expect(view.onLaunchApp).not.toHaveBeenCalled();
    });

    it('saves a choice, then asks the host for the card again', async () => {
        view.canAdminister = true;
        BackendClient.setVirtualDisplayGpu.mockResolvedValue({ gpu: AMD });
        BackendClient.getAppList.mockReturnValue(new Promise(() => {}));
        const select = paint([virtualDisplay()]).querySelector('.app-card-gpu-select');
        select.value = AMD;
        select.dispatchEvent(new Event('change', { bubbles: true }));
        expect(BackendClient.setVirtualDisplayGpu).toHaveBeenCalledWith(UUID, AMD);
        await vi.waitFor(() => expect(BackendClient.getAppList).toHaveBeenCalledWith(UUID));
    });

    it('goes back to what it showed when the server refuses', async () => {
        view.canAdminister = true;
        BackendClient.setVirtualDisplayGpu.mockRejectedValue(new Error('409'));
        const select = paint([virtualDisplay()]).querySelector('.app-card-gpu-select');
        select.value = RTX;
        select.dispatchEvent(new Event('change', { bubbles: true }));
        await vi.waitFor(() => expect(Toast.error).toHaveBeenCalledWith('apps.gpuSaveFailed'));
        expect(select.value).toBe('');
        expect(select.disabled).toBe(false);
        expect(BackendClient.getAppList).not.toHaveBeenCalled();
    });
});
