/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';

vi.mock('../js/api/BackendClient.js', () => ({
    BackendClient: { getAppList: vi.fn(), stopHostSession: vi.fn(), getRunningApp: vi.fn() },
}));

import { HostListView } from '../js/ui/HostListView.js';
import { Host } from '../js/models/Host.js';
import { BackendClient } from '../js/api/BackendClient.js';
import { othersOffChecked, othersOffOffer } from '../js/util/OthersOff.js';

/**
 * Issue #39 — the other screens off while a stream runs on the virtual display.
 * The admin allows it, each browser chooses it, and the displays a stream
 * turned off keep their cards, drawn dark, opening nothing.
 */
describe('the cards of the displays a stream turned off', () => {
    let view;
    let container;

    const device = (key, extra = {}) => ({ os: 'windows', display: 'external', key, ...extra });
    const APPS = [
        {
            id: 7,
            name: 'MoonlightWeb Virtual Display',
            device: device('moonlightweb-virtual-display'),
        },
        { id: 2, name: 'Display 2', device: device('d2', { off: true }) },
        { id: 3, name: 'Display 3', device: device('d3') },
    ];

    const mount = () => {
        localStorage.setItem(
            'mw-host-apps',
            JSON.stringify({ 'host-a': { ts: Date.now(), apps: APPS } }),
        );
        document.body.innerHTML = '<div id="root"></div>';
        container = document.getElementById('root');
        view = new HostListView(container);
        view.hosts = [
            new Host({
                uuid: 'host-a',
                name: 'BENCH-DESK',
                state: 'online',
                pairState: 'paired',
                backendType: 'native',
            }),
        ];
        view.renderList();
    };

    const cardOf = (id) => container.querySelector(`.app-card[data-app-id="${id}"]`);

    beforeEach(() => {
        vi.clearAllMocks();
        BackendClient.getAppList.mockReturnValue(new Promise(() => {}));
    });

    afterEach(() => {
        if (view) view.destroy();
        view = null;
        localStorage.clear();
        vi.restoreAllMocks();
    });

    it('draws an off display dark, with its note, and only that one', () => {
        mount();
        const off = cardOf(2);
        expect(off.classList.contains('app-card--off')).toBe(true);
        expect(off.getAttribute('aria-disabled')).toBe('true');
        expect(off.getAttribute('aria-label')).toBe('apps.displayOffAria');
        expect(off.querySelector('.app-card-off-note')).not.toBeNull();
        for (const id of [3, 7]) {
            expect(cardOf(id).classList.contains('app-card--off')).toBe(false);
            expect(cardOf(id).querySelector('.app-card-off-note')).toBeNull();
        }
    });

    it('opens nothing from an off display, and still opens the others', () => {
        mount();
        view.onLaunchApp = vi.fn();
        cardOf(2).click();
        expect(view.onLaunchApp).not.toHaveBeenCalled();
        cardOf(3).click();
        expect(view.onLaunchApp).toHaveBeenCalledTimes(1);
    });
});

describe('the other screens box of the streaming settings', () => {
    const native = (others_off) => ({ backendType: 'native', nativeDisplay: { others_off } });

    it('is offered only where a native host allows it', () => {
        expect(othersOffOffer(null)).toBeNull();
        expect(othersOffOffer([])).toBeNull();
        expect(othersOffOffer([native({ allowed: false, inherited: true })])).toBeNull();
        expect(
            othersOffOffer([{ backendType: '', nativeDisplay: { others_off: { allowed: true } } }]),
        ).toBeNull();
        expect(othersOffOffer([native({ allowed: true, inherited: false })])).toEqual({
            inherited: false,
        });
        expect(othersOffOffer([native({ allowed: true, inherited: true })])).toEqual({
            inherited: true,
        });
    });

    it('shows the browser its own choice, else the PC inherited default', () => {
        expect(othersOffChecked(true, { inherited: false })).toBe(true);
        expect(othersOffChecked(false, { inherited: true })).toBe(false);
        expect(othersOffChecked(null, { inherited: true })).toBe(true);
        expect(othersOffChecked(null, { inherited: false })).toBe(false);
        expect(othersOffChecked(undefined, null)).toBe(false);
    });
});
