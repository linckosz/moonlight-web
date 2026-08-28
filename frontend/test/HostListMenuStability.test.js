/*
 * MoonlightWeb — frontend TNR. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * The kebab menu must survive the list refreshing under it.
 *
 * Cards are replaced whole when their data changes, and a host's data changes on
 * its own every few seconds: a poll that fails, a session starting, a status
 * flapping. Replacing a card whose menu is open took the menu with it, and the
 * click already travelling towards "Share" landed on the card underneath — which
 * is not a race anybody has to be unlucky to hit, since the menu stays open for
 * as long as it takes to read.
 *
 * So: no card is rebuilt while its own menu is open, and the rebuild that was
 * skipped happens as soon as the menu closes. Both halves matter — without the
 * second the card would sit stale until the next time something changed.
 */
import { describe, it, expect, beforeEach, vi } from 'vitest';

vi.mock('../js/api/BackendClient.js', () => ({
    BackendClient: { getHosts: vi.fn(), getAppList: vi.fn() },
}));
vi.mock('../js/i18n/i18n.js', () => ({ t: (key) => key }));

import { HostListView } from '../js/ui/HostListView.js';

/** The two states a card is rendered from, differing only in what it says. */
const card = (uuid, fingerprint, menuOpen) =>
    `<div class="host-card" data-uuid="${uuid}" data-fingerprint="${fingerprint}">
        <div class="host-card-menu">
            <button class="btn-icon btn-host-menu" data-uuid="${uuid}"
                    aria-expanded="${menuOpen ? 'true' : 'false'}"></button>
            <div class="host-menu"${menuOpen ? '' : ' hidden'}>
                <button class="host-menu-item btn-share" data-uuid="${uuid}"></button>
            </div>
        </div>
     </div>`;

describe('the host list refreshing under an open menu', () => {
    let view;
    let container;

    /** A view with one host on screen, rendered from a stale fingerprint. */
    const build = (menuOpen) => {
        container = document.createElement('div');
        document.body.innerHTML = '';
        document.body.appendChild(container);

        view = Object.create(HostListView.prototype);
        view.container = container;
        view.hosts = [{ uuid: 'h1', isPaired: true, isAvailable: false }];
        view.appsByHost = {};
        view._staleCards = false;
        view._destroyed = false;
        view._cardFingerprint = () => 'NEW';
        view.renderCard = (h) => card(h.uuid, 'NEW', false);

        container.innerHTML = `<div class="hosts-list" id="hosts-list">${card('h1', 'OLD', menuOpen)}</div>`;
    };

    const menu = () => container.querySelector('.host-menu');
    const shareItem = () => container.querySelector('.btn-share');

    beforeEach(() => {
        vi.useFakeTimers();
    });

    it('leaves the card alone while its menu is open', () => {
        build(true);
        const before = shareItem();

        view.renderList();

        // The very node the click is heading for is still the node in the page.
        expect(shareItem()).toBe(before);
        expect(menu().hasAttribute('hidden')).toBe(false);
        expect(container.querySelector('.host-card').dataset.fingerprint).toBe('OLD');
    });

    it('rebuilds it the moment the menu closes', () => {
        build(true);
        view.renderList();
        expect(view._staleCards).toBe(true);

        view._closeAllMenus();
        vi.runAllTimers();

        expect(container.querySelector('.host-card').dataset.fingerprint).toBe('NEW');
        expect(view._staleCards).toBe(false);
    });

    it('rebuilds a card whose menu is shut, as it always did', () => {
        build(false);

        view.renderList();

        expect(container.querySelector('.host-card').dataset.fingerprint).toBe('NEW');
        expect(view._staleCards).toBe(false);
    });

    // Opening a kebab closes every other one first. A rebuild in that gap would
    // detach the menu about to be shown, so the flush has to look again.
    it('does not rebuild when a menu is open again by the time it runs', () => {
        build(true);
        view.renderList();

        view._closeAllMenus(); // queues the flush
        menu().removeAttribute('hidden'); // …and something is open again
        vi.runAllTimers();

        expect(container.querySelector('.host-card').dataset.fingerprint).toBe('OLD');
        // Still owed, so the next close pays it.
        expect(view._staleCards).toBe(true);
    });
});
