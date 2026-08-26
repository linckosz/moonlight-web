/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';

vi.mock('../js/api/BackendClient.js', () => ({
    BackendClient: { getAppList: vi.fn() },
}));

import { HostListView } from '../js/ui/HostListView.js';
import { Host } from '../js/models/Host.js';
import { BackendClient } from '../js/api/BackendClient.js';

const READY_HOST = { uuid: 'host-a', name: 'DUALRTX', state: 'online', pairState: 'paired' };

/**
 * What an app card says after the click.
 *
 * The card is the only thing on screen when a launch never starts, so it has to
 * carry the news itself — and it must stop carrying it, both the alert colour
 * and the focus ring, or the user is left staring at a card that still looks
 * busy with no cue that the thing to do is click it again.
 */
describe('app card launch feedback', () => {
    let view;
    let container;

    beforeEach(() => {
        localStorage.setItem(
            'mw-host-apps',
            JSON.stringify({ 'host-a': { ts: Date.now(), apps: [{ id: 1, name: 'Desktop' }] } }),
        );
        vi.clearAllMocks();
        BackendClient.getAppList.mockReturnValue(new Promise(() => {})); // never answers
        document.body.innerHTML = '<div id="root"></div>';
        container = document.getElementById('root');
        view = new HostListView(container);
        view.hosts = [new Host(READY_HOST)];
        view.renderList();
    });

    afterEach(() => {
        if (view) view.destroy();
        view = null;
        localStorage.clear();
        vi.useRealTimers();
    });

    const card = () => container.querySelector('.app-card');
    const name = () => container.querySelector('.app-card-name').textContent;

    // The click a real browser performs: the card takes focus, then goes busy.
    const launch = () => {
        const el = card();
        el.focus();
        view.setLaunching(el);
        return el;
    };

    it('marks the card busy while the launch is in flight', () => {
        const el = launch();
        expect(el.classList.contains('app-card--launching')).toBe(true);
        expect(el.getAttribute('aria-busy')).toBe('true');
        expect(name()).not.toBe('Desktop');
    });

    describe('when the launch never started', () => {
        it('flashes the alert marker and gives the name back', () => {
            const el = launch();
            view.clearLaunching({ failed: true });

            expect(el.classList.contains('app-card--launching')).toBe(false);
            expect(el.classList.contains('app-card--launch-failed')).toBe(true);
            expect(el.hasAttribute('aria-busy')).toBe(false);
            expect(name()).toBe('Desktop');
        });

        it('drops the focus the click left behind — the ring is the same yellow as "busy"', () => {
            const el = launch();
            expect(document.activeElement).toBe(el);

            view.clearLaunching({ failed: true });

            expect(document.activeElement).not.toBe(el);
        });

        it('leaves no colour behind once the fade is over', () => {
            vi.useFakeTimers();
            const el = launch();
            view.clearLaunching({ failed: true });

            vi.advanceTimersByTime(HostListView.LAUNCH_FAIL_FADE_MS + 50);

            expect(el.classList.contains('app-card--launch-failed')).toBe(false);
        });
    });

    describe('when the stream itself failed', () => {
        // The stream screen already showed the error. Alarming the card on the
        // way back would report the same failure twice, on the surface the user
        // has just turned to in order to pick something else.
        it('goes neutral, with no alert marker at all', () => {
            const el = launch();
            view.clearLaunching();

            expect(el.classList.contains('app-card--launching')).toBe(false);
            expect(el.classList.contains('app-card--launch-failed')).toBe(false);
            expect(name()).toBe('Desktop');
        });

        it('still refuses the focus', () => {
            const el = launch();
            view.clearLaunching();
            expect(document.activeElement).not.toBe(el);
        });
    });

    it('does not leave a fade timer pending past destroy()', () => {
        vi.useFakeTimers();
        launch();
        view.clearLaunching({ failed: true });
        expect(view._launchFailTimers.size).toBe(1);

        view.destroy();

        // Nothing left to fire against a view (and a grid) that are gone.
        expect(view._launchFailTimers.size).toBe(0);
        view = null;
    });
});
