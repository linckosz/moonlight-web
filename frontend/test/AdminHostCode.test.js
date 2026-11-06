/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 *
 * The host's code on the admin page. The Connect page of the site and the TV
 * app both ask for it: the 26 characters the rendezvous link ends with. It is
 * shown on its own, in capitals, read-only, with a copy button, and only when
 * the link really ends with a code.
 */
import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';

vi.mock('../js/api/BackendClient.js', () => ({
    BackendClient: { getInternetStatus: vi.fn(), getGamepadDriver: vi.fn(async () => ({})) },
}));
vi.mock('../js/ui/Toast.js', () => ({
    Toast: { success: vi.fn(), error: vi.fn(), warning: vi.fn() },
}));
vi.mock('../js/i18n/i18n.js', () => ({
    t: (key) => 'text:' + key,
}));

import { AdminView } from '../js/ui/AdminView.js';

const ID = 'a1b2c3d4e5f6g7h8j9k0m1n2p3';

describe('AdminView — the host code', () => {
    let view;

    beforeEach(() => {
        document.body.innerHTML = '<div></div>';
        view = new AdminView(document.body.firstElementChild, () => {});
    });

    afterEach(() => view.destroy());

    it('is the end of the link, in capitals', () => {
        view._rendezvousUrl = `https://stream.moonlightweb.top/${ID}`;
        expect(view._hostCode()).toBe(ID.toUpperCase());
        // A test rendezvous, or a trailing slash, changes nothing.
        view._rendezvousUrl = `https://stream.dev.moonlightweb.top/${ID}/`;
        expect(view._hostCode()).toBe(ID.toUpperCase());
    });

    it('is not shown without a link, or for a link that ends with no code', () => {
        for (const url of [
            '',
            'not a url',
            'https://stream.moonlightweb.top/',
            'https://stream.moonlightweb.top/abc',
            // i, l, o and u are not in the alphabet.
            `https://stream.moonlightweb.top/${ID.slice(0, 25)}u`,
        ]) {
            view._rendezvousUrl = url;
            expect(view._hostCode()).toBe('');
            expect(view._renderHostCode()).toBe('');
        }
    });

    it('is a read-only field under the link, and its button copies it', async () => {
        const writeText = vi.fn(async () => {});
        Object.defineProperty(navigator, 'clipboard', {
            value: { writeText },
            configurable: true,
        });
        view._hasAdminAccess = true;
        view._internetEnabled = true;
        view._active = true;
        view._rendezvousUrl = `https://stream.moonlightweb.top/${ID}`;
        view._rendezvousOnline = true;
        view.render();
        view.bindEvents();

        const input = document.querySelector('#admin-host-code-input');
        expect(input).not.toBeNull();
        expect(input.readOnly).toBe(true);
        expect(input.value).toBe(ID.toUpperCase());
        // In the Internet section, after the link.
        const section = document.querySelector('#admin-section-internet');
        expect(section.contains(input)).toBe(true);
        const link = document.querySelector('#admin-rdv-url');
        expect(link.compareDocumentPosition(input) & Node.DOCUMENT_POSITION_FOLLOWING).toBeTruthy();

        document.querySelector('#btn-copy-host-code').click();
        await vi.waitFor(() => expect(writeText).toHaveBeenCalledWith(ID.toUpperCase()));
    });
});
