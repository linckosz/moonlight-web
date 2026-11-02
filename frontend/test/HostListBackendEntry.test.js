/*
 * MoonlightWeb — frontend TNR. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * A host's menu offers its backend to the administrator only: the server
 * refuses anyone else's changes, so the entry would lead nowhere for them.
 */
import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';

vi.mock('../js/api/BackendClient.js', () => ({
    BackendClient: { adminLocked: false, getHosts: vi.fn(), getAppList: vi.fn() },
}));
vi.mock('../js/i18n/i18n.js', () => ({ t: (key) => key }));

import { HostListView } from '../js/ui/HostListView.js';
import { Host } from '../js/models/Host.js';
import { BackendClient } from '../js/api/BackendClient.js';

const entry = (view, fields) => {
    const holder = document.createElement('div');
    holder.innerHTML = view.renderCard(new Host({ uuid: 'h1', name: 'One', ...fields }));
    return holder.querySelector('.btn-backend');
};

describe("the backend entry of a host's menu", () => {
    let view;

    beforeEach(() => {
        BackendClient.adminLocked = false;
        document.body.innerHTML = '<div id="root"></div>';
        view = new HostListView(document.getElementById('root'));
    });

    afterEach(() => {
        view.destroy();
        localStorage.clear();
    });

    it.each([
        ['a managed host', { backendType: 'wolf' }],
        ['a detected MultiSeat', { multiSeatDetected: true }],
        ['a host that may have a control API', { mayHaveControlApi: true }],
    ])('is offered to the administrator on %s, and to nobody else', (_name, fields) => {
        view.canAdminister = true;
        expect(entry(view, fields)).not.toBe(null);

        view.canAdminister = false;
        expect(entry(view, fields)).toBe(null);
    });

    it('stays away from a booth, administrator or not', () => {
        view.canAdminister = true;
        BackendClient.adminLocked = true;
        expect(entry(view, { backendType: 'wolf' })).toBe(null);
    });

    it('never appears on a plain Sunshine host', () => {
        view.canAdminister = true;
        expect(entry(view, {})).toBe(null);
    });
});
