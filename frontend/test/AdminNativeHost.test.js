/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 *
 * The admin's "MoonlightWeb Host" section: what only this PC's own host uses,
 * gathered apart from what serves every host. Placed after the server's
 * configuration, Apps on this PC last, and shown only when it has something
 * to offer.
 */
import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';

vi.mock('../js/api/BackendClient.js', () => ({
    BackendClient: { getGamepadDriver: vi.fn(async () => ({})) },
}));
vi.mock('../js/ui/Toast.js', () => ({
    Toast: { success: vi.fn(), error: vi.fn(), warning: vi.fn() },
}));
vi.mock('../js/i18n/i18n.js', () => ({
    t: (key) => 'text:' + key,
}));

import { AdminView } from '../js/ui/AdminView.js';

describe('AdminView — the MoonlightWeb Host section', () => {
    let view;
    const $ = (sel) => document.querySelector(sel);

    beforeEach(() => {
        document.body.innerHTML = '<div></div>';
        view = new AdminView(document.body.firstElementChild, () => {});
        view._hasAdminAccess = true;
        view._vdExclusiveSupported = true;
        view._videoPipelineSupported = true;
        view._hostAppsSupported = true;
    });

    afterEach(() => view.destroy());

    it('holds the four native-only items, Apps on this PC last', () => {
        view.render();
        const host = $('#admin-section-native-host');
        expect(host.querySelector('.settings-section-title').textContent).toBe(
            'text:admin.nativeHost',
        );
        expect(host.textContent).toContain('text:admin.nativeHostDesc');
        for (const sel of [
            '#chk-vd-exclusive',
            '#chk-stream-notifications',
            '#select-video-pipeline',
            '#admin-section-host-apps',
        ]) {
            expect(host.querySelector(sel)).not.toBeNull();
        }
        expect(host.lastElementChild.id).toBe('admin-section-host-apps');
    });

    it('sits after the server configuration and before Advanced', () => {
        view.render();
        const sections = [...document.querySelectorAll('.settings-section')];
        const host = sections.indexOf($('#admin-section-native-host'));
        const server = sections.findIndex(
            (s) =>
                s.querySelector('.settings-section-title')?.textContent ===
                'text:admin.serverConfig',
        );
        expect(server).toBeGreaterThanOrEqual(0);
        expect(host).toBe(server + 1);
        expect(sections[host + 1].id).toBe('admin-section-advanced');
    });

    it('leaves nothing behind where the items were', () => {
        view.render();
        const server = [...document.querySelectorAll('.settings-section')].find(
            (s) =>
                s.querySelector('.settings-section-title')?.textContent ===
                'text:admin.serverConfig',
        );
        expect(server.querySelector('#chk-vd-exclusive')).toBeNull();
        expect($('#admin-section-sessions').querySelector('#chk-stream-notifications')).toBeNull();
        expect($('#admin-section-advanced').querySelector('#select-video-pipeline')).toBeNull();
        // What serves every host stays with the server.
        expect(server.querySelector('#chk-packet-marking')).not.toBeNull();
    });

    it('keeps each box working where it now is', () => {
        view._vdExclusive = true;
        view._streamNotifications = false;
        view.render();
        view.bindEvents();
        expect($('#chk-vd-exclusive').checked).toBe(true);
        expect($('#chk-stream-notifications').checked).toBe(false);
    });

    it('shows only the items offered, and nothing when none is', () => {
        view._vdExclusiveSupported = false;
        view._videoPipelineSupported = false;
        view._hostAppsSupported = false;
        const html = view._renderNativeHost();
        expect(html).toContain('chk-stream-notifications');
        expect(html).not.toContain('chk-vd-exclusive');
        expect(html).not.toContain('select-video-pipeline');
        expect(html).not.toContain('admin-section-host-apps');

        view._hasAdminAccess = false;
        vi.spyOn(view, '_isLocalhost').mockReturnValue(false);
        expect(view._renderNativeHost()).toBe('');
    });
});
