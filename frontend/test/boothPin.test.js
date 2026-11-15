/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 *
 * A booth's stream PIN in the header (booth_stream_pin, issue #68). Pinned
 * down: the owner sees the PIN where Share was, with the count of who watches;
 * its panel lists only the guests who joined with the PIN, moves their level
 * and removes them; a new PIN is asked for and shown; the owner hears when
 * wrong tries moved the PIN, and only then; a guest who joined with the PIN
 * sees it too, and nobody sees anything that is not a PIN.
 */
import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';

vi.mock('../js/api/BackendClient.js', () => ({
    BackendClient: {
        boothPin: vi.fn(),
        getShareStatus: vi.fn(),
        sharePermissions: vi.fn(),
        shareDeactivate: vi.fn(),
        playerPresence: vi.fn(),
    },
}));
vi.mock('../js/ui/Toast.js', () => ({
    Toast: { info: vi.fn(), warning: vi.fn() },
}));
// No locale is loaded in the TNR: render the key so the assertions can name it.
vi.mock('../js/i18n/i18n.js', () => ({
    t: (key, params) => (params ? key + ' ' + JSON.stringify(params) : key),
}));

import { BoothPinMenu, BOOTH_LEVELS, permissionsFor } from '../js/ui/BoothPinMenu.js';
import { GuestPresence, formatPin } from '../js/ui/SharePresence.js';
import { BackendClient } from '../js/api/BackendClient.js';
import { Toast } from '../js/ui/Toast.js';

const flush = () => new Promise((r) => setTimeout(r, 0));

function header() {
    document.body.innerHTML =
        '<div class="stream-header"><button id="btn-stream-quit">stop</button></div>';
    return {
        headerEl: /** @type {HTMLElement} */ (document.querySelector('.stream-header')),
        quit: /** @type {HTMLElement} */ (document.getElementById('btn-stream-quit')),
    };
}

const status = (slots) => ({ slots });

describe('the PIN as the header shows it', () => {
    it('is six digits in two groups, and nothing else', () => {
        expect(formatPin('482913')).toBe('482 913');
        for (const bad of ['48291', '4829130', 'abcdef', '', null, undefined, 482913])
            expect(formatPin(/** @type {any} */ (bad))).toBe('');
    });

    it('names the four levels as the backend reads them', () => {
        expect(BOOTH_LEVELS).toEqual(['viewer', 'gamer', 'desktop', 'full']);
        expect(permissionsFor('viewer')).toEqual({ gamepad: false, keyboardMouse: false });
        expect(permissionsFor('gamer')).toEqual({ gamepad: true, keyboardMouse: false });
        expect(permissionsFor('desktop')).toEqual({ gamepad: false, keyboardMouse: true });
        expect(permissionsFor('full')).toEqual({ gamepad: true, keyboardMouse: true });
        expect(permissionsFor('admin')).toEqual({ gamepad: false, keyboardMouse: false });
    });
});

describe("the owner's PIN in place of Share", () => {
    /** @type {BoothPinMenu|null} */
    let menu = null;

    beforeEach(() => {
        vi.clearAllMocks();
        BackendClient.boothPin.mockResolvedValue({
            pin: '482913',
            generation: 0,
            changed_by_failures: false,
        });
        BackendClient.getShareStatus.mockResolvedValue(
            status([
                {
                    slot: 2,
                    booth: true,
                    streaming: true,
                    name: 'LAPTOP-SAM',
                    access_level: 'viewer',
                },
                {
                    slot: 3,
                    booth: true,
                    streaming: false,
                    name: 'IPAD-ALEX',
                    access_level: 'gamer',
                },
                { slot: 4, streaming: true, name: 'board guest' },
            ]),
        );
    });
    afterEach(() => {
        menu?.destroy();
        menu = null;
    });

    it('shows the PIN and who watches, the owner included, before Stop', async () => {
        const { headerEl, quit } = header();
        menu = new BoothPinMenu(headerEl, quit, { uuid: 'host-A', name: 'Gaming PC' });
        expect(await menu.mount()).toBe(true);
        expect(BackendClient.boothPin).toHaveBeenCalledWith('host-A');
        const chip = /** @type {HTMLElement} */ (headerEl.querySelector('.booth-pin'));
        expect(chip.nextElementSibling).toBe(null);
        expect(headerEl.lastElementChild).toBe(quit);
        expect(chip.querySelector('.booth-pin-digits').textContent).toBe('482 913');
        // The owner, and the one booth guest streaming; a board row is not one.
        expect(chip.querySelector('.booth-pin-n').textContent).toBe('2');
        expect(chip.title).toContain('LAPTOP-SAM');
        expect(chip.title).not.toContain('board guest');
        expect(menu.hasActiveShare()).toBe(true);
    });

    it('says when wrong tries moved the PIN, and not when a join did', async () => {
        const { headerEl, quit } = header();
        menu = new BoothPinMenu(headerEl, quit, { uuid: 'host-A' });
        await menu.mount();
        BackendClient.boothPin.mockResolvedValueOnce({
            pin: '730158',
            generation: 1,
            changed_by_failures: false,
        });
        await menu.refresh();
        expect(Toast.warning).not.toHaveBeenCalled();
        expect(headerEl.querySelector('.booth-pin-digits').textContent).toBe('730 158');
        BackendClient.boothPin.mockResolvedValueOnce({
            pin: '111222',
            generation: 2,
            changed_by_failures: true,
        });
        await menu.refresh();
        expect(Toast.warning).toHaveBeenCalledTimes(1);
        expect(Toast.warning).toHaveBeenCalledWith('booth.pinChangedFailures');
        // The same generation seen again is no news.
        BackendClient.boothPin.mockResolvedValueOnce({
            pin: '111222',
            generation: 2,
            changed_by_failures: true,
        });
        await menu.refresh();
        expect(Toast.warning).toHaveBeenCalledTimes(1);
    });

    it('keeps the last PIN through a failed look', async () => {
        const { headerEl, quit } = header();
        menu = new BoothPinMenu(headerEl, quit, { uuid: 'host-A' });
        await menu.mount();
        BackendClient.boothPin.mockRejectedValueOnce(new Error('offline'));
        const warn = vi.spyOn(console, 'warn').mockImplementation(() => {});
        await menu.refresh();
        warn.mockRestore();
        expect(headerEl.querySelector('.booth-pin-digits').textContent).toBe('482 913');
    });

    it('opens a panel with the booth guests only, their level, Remove and New PIN', async () => {
        const { headerEl, quit } = header();
        menu = new BoothPinMenu(headerEl, quit, { uuid: 'host-A' });
        await menu.mount();
        /** @type {HTMLElement} */ (headerEl.querySelector('.booth-pin')).click();
        const panel = /** @type {HTMLElement} */ (headerEl.querySelector('.booth-panel'));
        expect(panel).not.toBe(null);
        expect(panel.querySelector('.booth-panel-pin').textContent).toBe('482 913');
        const rows = [...panel.querySelectorAll('.booth-panel-row')];
        expect(rows.map((r) => r.querySelector('.booth-panel-name').textContent)).toEqual([
            'LAPTOP-SAM',
            'IPAD-ALEX',
        ]);
        const levels = rows.map(
            (r) => /** @type {HTMLSelectElement} */ (r.querySelector('select')).value,
        );
        expect(levels).toEqual(['viewer', 'gamer']);

        // Move a level.
        const select = /** @type {HTMLSelectElement} */ (rows[0].querySelector('select'));
        select.value = 'full';
        select.dispatchEvent(new Event('change'));
        await flush();
        expect(BackendClient.sharePermissions).toHaveBeenCalledWith(2, {
            gamepad: true,
            keyboardMouse: true,
        });

        // Remove one.
        /** @type {HTMLElement} */ (rows[1].querySelector('.booth-panel-remove')).click();
        await flush();
        expect(BackendClient.shareDeactivate).toHaveBeenCalledWith(3);

        // A new PIN, asked for and shown at once.
        BackendClient.boothPin.mockResolvedValueOnce({
            pin: '555666',
            generation: 3,
            changed_by_failures: false,
        });
        /** @type {HTMLElement} */ (
            headerEl.querySelector('.booth-panel .booth-panel-new')
        ).click();
        await flush();
        await flush();
        expect(BackendClient.boothPin).toHaveBeenCalledWith('host-A', 'new');
        expect(headerEl.querySelector('.booth-pin-digits').textContent).toBe('555 666');

        // A second click on the chip closes it.
        /** @type {HTMLElement} */ (headerEl.querySelector('.booth-pin')).click();
        expect(headerEl.querySelector('.booth-panel')).toBe(null);
    });

    it('says nobody joined yet, and announces whoever comes', async () => {
        BackendClient.getShareStatus.mockResolvedValue(status([]));
        const { headerEl, quit } = header();
        menu = new BoothPinMenu(headerEl, quit, { uuid: 'host-A' });
        await menu.mount();
        expect(menu.hasActiveShare()).toBe(false);
        /** @type {HTMLElement} */ (headerEl.querySelector('.booth-pin')).click();
        expect(headerEl.querySelector('.booth-panel-empty')).not.toBe(null);
        BackendClient.getShareStatus.mockResolvedValue(
            status([{ slot: 2, booth: true, streaming: true, name: 'LAPTOP-SAM' }]),
        );
        await menu.refresh();
        expect(Toast.info).toHaveBeenCalledWith('sharing.presenceJoined {"name":"LAPTOP-SAM"}', {
            durationMs: 4000,
        });
        expect(headerEl.querySelectorAll('.booth-panel-row').length).toBe(1);
    });
});

describe("a guest's header", () => {
    beforeEach(() => vi.clearAllMocks());

    it('shows the PIN a guest who joined with it reads, beside the count', async () => {
        BackendClient.playerPresence.mockResolvedValue({
            self: 2,
            owner: true,
            players: [{ slot: 2, name: 'LAPTOP-SAM' }],
            booth_pin: '482913',
            booth_pin_generation: 1,
        });
        const { headerEl, quit } = header();
        const p = new GuestPresence(headerEl, quit);
        p.mount();
        await flush();
        const chip = /** @type {HTMLElement} */ (headerEl.querySelector('.booth-pin-guest'));
        expect(chip).not.toBe(null);
        expect(chip.querySelector('.booth-pin-digits').textContent).toBe('482 913');
        expect(chip.nextElementSibling).toBe(p.btn);

        // It moves with the PIN, and goes when there is none.
        BackendClient.playerPresence.mockResolvedValue({
            self: 2,
            owner: true,
            players: [{ slot: 2, name: 'LAPTOP-SAM' }],
            booth_pin: '730158',
        });
        await p.refresh();
        expect(chip.querySelector('.booth-pin-digits').textContent).toBe('730 158');
        BackendClient.playerPresence.mockResolvedValue({ self: 2, owner: true, players: [] });
        await p.refresh();
        expect(headerEl.querySelector('.booth-pin-guest')).toBe(null);
        p.destroy();
    });

    it('never shows anything that is not a PIN', async () => {
        BackendClient.playerPresence.mockResolvedValue({
            self: 3,
            owner: true,
            players: [],
            booth_pin: '<img src=x>',
        });
        const { headerEl, quit } = header();
        const p = new GuestPresence(headerEl, quit);
        p.mount();
        await flush();
        expect(headerEl.querySelector('.booth-pin-guest')).toBe(null);
        expect(headerEl.innerHTML).not.toContain('<img');
        p.destroy();
    });
});
