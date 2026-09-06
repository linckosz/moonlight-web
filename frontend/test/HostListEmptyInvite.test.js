/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';

vi.mock('../js/api/BackendClient.js', () => ({
    BackendClient: { getHosts: vi.fn(), getAppList: vi.fn() },
}));

import { HostListView } from '../js/ui/HostListView.js';
import { BackendClient } from '../js/api/BackendClient.js';

// An empty host list is the one place where the answer is not on the page:
// MoonlightWeb streams a machine, it does not make one streamable. So it names
// what is missing and links to Sunshine — a link, never an install.
describe('empty host list — the Sunshine invitation', () => {
    let view;
    let container;

    beforeEach(() => {
        vi.clearAllMocks();
        BackendClient.getAppList.mockReturnValue(new Promise(() => {}));
        document.body.innerHTML = '<div id="root"></div>';
        container = document.getElementById('root');
        view = new HostListView(container);
        view._active = true;
        view.renderShell();
    });

    afterEach(() => {
        if (view) view.destroy();
        view = null;
        localStorage.clear();
    });

    const cta = () => container.querySelector('.hosts-empty-cta');
    const link = () => container.querySelector('.hosts-empty-cta a');

    it('says nothing before the first answer has come back', () => {
        view.renderList();
        expect(container.querySelector('.hosts-empty')).not.toBe(null);
        expect(cta()).toBe(null);
    });

    it('invites once the list has come back empty', async () => {
        BackendClient.getHosts.mockResolvedValue({ hosts: [] });
        await view.refresh();
        expect(cta()).not.toBe(null);
        expect(link().getAttribute('href')).toBe(
            'https://github.com/LizardByte/Sunshine/releases/latest',
        );
        // Somebody else's site: opened away from this page, and told not to
        // hand it a handle back to us.
        expect(link().getAttribute('target')).toBe('_blank');
        expect(link().getAttribute('rel')).toContain('noopener');
    });

    // An empty answer leaves the fingerprint exactly as it was, so the case
    // that most needs a repaint is the one no change signals.
    it('repaints on the first answer even though nothing changed', async () => {
        BackendClient.getHosts.mockResolvedValue({ hosts: [] });
        const spy = vi.spyOn(view, 'renderList');
        await view.refresh();
        expect(spy).toHaveBeenCalled();
    });

    it('says nothing at all once a host is there', async () => {
        BackendClient.getHosts.mockResolvedValue({
            hosts: [{ uuid: 'h1', name: 'BENCH-DESK', state: 'online', pairState: 'paired' }],
        });
        await view.refresh();
        expect(container.querySelector('.hosts-empty')).toBe(null);
        expect(cta()).toBe(null);
    });

    // The invitation is a link and nothing else: no button that would make this
    // app fetch or run somebody else's installer.
    it('offers no install action of its own', async () => {
        BackendClient.getHosts.mockResolvedValue({ hosts: [] });
        await view.refresh();
        expect(cta().querySelector('button')).toBe(null);
        expect(link().hasAttribute('download')).toBe(false);
    });
});
