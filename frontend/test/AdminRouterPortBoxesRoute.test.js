/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 *
 * The box of admin › Internet, down to the request itself: with the real
 * BackendClient, Apply under the shared port box is a POST to
 * /api/admin/settings carrying that box's key alone, the route that writes it
 * on the server (server/RouterSwitchesWrite.h). Nothing posts ice_tcp_443: it
 * has no box. The mocked test beside this one
 * (AdminRouterPortBoxes.test.js) stops at the client's method.
 */
import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';

vi.mock('../js/ui/Toast.js', () => ({
    Toast: { success: vi.fn(), error: vi.fn(), warning: vi.fn(), info: vi.fn() },
}));
vi.mock('../js/i18n/i18n.js', () => ({
    t: (key) => 'text:' + key,
}));

import { AdminView } from '../js/ui/AdminView.js';
import { BackendClient } from '../js/api/BackendClient.js';
import { Toast } from '../js/ui/Toast.js';

describe('AdminView — the boxes of the router ports, as requests', () => {
    let view;
    let fetchMock;

    beforeEach(() => {
        document.body.innerHTML = '<div></div>';
        view = new AdminView(document.body.firstElementChild, () => {});
        view._hasAdminAccess = true;
        view._sharedPorts = true;
        view._sharedPortsRestartPending = false;
        vi.spyOn(BackendClient, '_adminKey').mockResolvedValue('admin-key');
        fetchMock = vi.fn(async (path, init) => {
            const answer =
                init && init.method === 'POST' && path === '/api/admin/settings'
                    ? { status: 'saved', restart: 'after_streams' }
                    : {};
            return new Response(JSON.stringify(answer), {
                status: 200,
                headers: { 'Content-Type': 'application/json' },
            });
        });
        vi.stubGlobal('fetch', fetchMock);
        Toast.error.mockReset();
    });

    afterEach(() => {
        view.destroy();
        vi.unstubAllGlobals();
        vi.restoreAllMocks();
    });

    const posts = () =>
        fetchMock.mock.calls
            .filter(([, init]) => init && init.method === 'POST')
            .map(([path, init]) => ({ path, body: JSON.parse(init.body), init }));

    const change = (selector, checked) => {
        view.render();
        view.bindEvents();
        const box = document.querySelector(selector);
        expect(box).not.toBeNull();
        box.checked = checked;
        box.dispatchEvent(new Event('change'));
    };

    it('unticking the shared port box posts nothing until Apply', async () => {
        change('#chk-shared-ports', false);
        await Promise.resolve();
        expect(posts()).toHaveLength(0);
        document.querySelector('#btn-shared-ports-cancel').click();
        expect(posts()).toHaveLength(0);
    });

    it('Apply posts the shared port box to /api/admin/settings, alone', async () => {
        change('#chk-shared-ports', false);
        document.querySelector('#btn-shared-ports-apply').click();
        await vi.waitFor(() => expect(posts()).toHaveLength(1));
        const [post] = posts();
        expect(post.path).toBe('/api/admin/settings');
        expect(post.body).toEqual({ ice_udp_mux: false });
        expect(post.init.headers['X-MW-Admin-Key']).toBe('admin-key');
        expect(posts().some((p) => p.path === '/api/settings/streaming')).toBe(false);
        expect(Toast.error).not.toHaveBeenCalled();
    });

    it('no request ever carries ice_tcp_443', async () => {
        view.render();
        view.bindEvents();
        expect(document.querySelector('#chk-tcp443')).toBeNull();
        expect(posts().some((p) => 'ice_tcp_443' in (p.body || {}))).toBe(false);
    });
});
