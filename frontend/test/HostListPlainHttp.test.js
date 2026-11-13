/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';

vi.mock('../js/api/BackendClient.js', () => ({
    BackendClient: { getHosts: vi.fn(), getAppList: vi.fn() },
}));

import { HostListView } from '../js/ui/HostListView.js';

// A TV on plain HTTP (issue #42) reads the hosts and their apps, nothing of the
// administration: no « Add manually », whose routes it does not reach. The
// shell is drawn as the view is made, so the view knows it is a TV's from the
// start: told afterwards, the Mi TV still showed the button (bench of
// 13/11/2026).
describe('the host list of a TV on plain HTTP', () => {
    let view;
    let container;

    beforeEach(() => {
        document.body.innerHTML = '<div id="root"></div>';
        container = document.getElementById('root');
    });

    afterEach(() => {
        if (view) view.destroy();
        view = null;
        localStorage.clear();
    });

    it('has no « Add manually » from its very first paint', () => {
        view = new HostListView(container, { plainHttp: true });
        expect(view.plainHttp).toBe(true);
        expect(container.querySelector('#btn-manual')).toBe(null);
    });

    it('any other page still offers it', () => {
        view = new HostListView(container);
        expect(view.plainHttp).toBe(false);
        expect(container.querySelector('#btn-manual')).not.toBe(null);
    });
});
