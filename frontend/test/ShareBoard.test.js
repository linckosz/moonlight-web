/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 *
 * The sharing board. What is pinned down here is the part an owner acts on
 * without reading: the badge must say what the two boxes actually grant, a
 * permission moved mid-stream must reach the backend rather than waiting for a
 * re-share, and a link that was passed along must be visible on the row.
 *
 * Two of these guard regressions that already happened once: a tick undone a
 * second later by the board's own poll, and a row that hid the thing the owner
 * had opened the board to read.
 */
import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';

vi.mock('../js/api/BackendClient.js', () => ({
    BackendClient: {
        getShareStatus: vi.fn(),
        getAppList: vi.fn(async () => ({ apps: [{ id: 7, name: 'Desktop' }] })),
        shareActivate: vi.fn(async () => ({ url: 'https://x/p/tok', pin: '482917' })),
        shareCredentials: vi.fn(async () => ({
            available: true,
            url: 'https://x/p/tok',
            pin: '482917',
        })),
        sharePermissions: vi.fn(async () => ({})),
        shareDeactivate: vi.fn(async () => ({})),
        shareTtl: vi.fn(async () => ({})),
        shareRename: vi.fn(async () => ({})),
        shareFeedHeight: vi.fn(async (height) => ({ feed_height: height })),
        shareLinkAddress: vi.fn(async (host, id) => ({ link_address: id })),
    },
}));
// No locale is loaded in the TNR: render the key so the assertions can name it.
vi.mock('../js/i18n/i18n.js', () => ({
    t: (key, params) => (params ? key + ' ' + JSON.stringify(params) : key),
}));

import { ShareBoard } from '../js/ui/ShareBoard.js';
import { BackendClient } from '../js/api/BackendClient.js';

const slot = (n, over = {}) => ({
    slot: n,
    state: 'off',
    name: '',
    permissions: { gamepad: false, keyboardMouse: false },
    access_level: 'viewer',
    streaming: false,
    ttl_secs: 28800,
    expires_at: Math.floor(Date.now() / 1000) + 28800,
    devices: [],
    ...over,
});

const status = (rows, over = {}) => ({ slots: rows, streaming: 0, ...over });

/** The lifetimes the slider walks, in its own order. */
const TTL = [3600, 4 * 3600, 8 * 3600, 24 * 3600, 48 * 3600, 0];

describe('ShareBoard', () => {
    let board;

    const rowFor = (n) => document.querySelector(`.share-brow[data-slot="${n}"]`);
    const badgeFor = (n) => rowFor(n).querySelector('.share-access-badge');

    const openBoard = async (
        rows,
        ctx = { streaming: true, hostUuid: 'h', hostName: 'BENCH-DESK' },
        statusOver = {},
    ) => {
        BackendClient.getShareStatus.mockResolvedValue(status(rows, statusOver));
        board = new ShareBoard(ctx);
        await board.open();
    };

    beforeEach(() => {
        vi.clearAllMocks();
        document.body.innerHTML = '';
    });

    afterEach(() => {
        if (board) board.close();
        board = null;
    });

    it('reads the access badge off both boxes, not just the keyboard', async () => {
        await openBoard([slot(2), slot(3), slot(4)]);
        const row = rowFor(2);
        const gamepad = row.querySelector('.share-perm-gamepad');
        const km = row.querySelector('.share-perm-km');

        expect(badgeFor(2).dataset.level).toBe('viewer');

        gamepad.checked = true;
        gamepad.dispatchEvent(new Event('change'));
        expect(badgeFor(2).dataset.level).toBe('gamer');

        // Keyboard and mouse without a gamepad is "desktop" — it used to report
        // "full", promising a gamepad the owner had not granted.
        gamepad.checked = false;
        km.checked = true;
        km.dispatchEvent(new Event('change'));
        expect(badgeFor(2).dataset.level).toBe('desktop');

        gamepad.checked = true;
        gamepad.dispatchEvent(new Event('change'));
        expect(badgeFor(2).dataset.level).toBe('full');
    });

    it('sends a permission change straight down while the guest is streaming', async () => {
        await openBoard([
            slot(2, {
                state: 'binded',
                streaming: true,
                devices: [{ bound_at: 1, user_agent: 'Chrome' }],
            }),
            slot(3),
            slot(4),
        ]);
        const km = rowFor(2).querySelector('.share-perm-km');
        km.checked = true;
        km.dispatchEvent(new Event('change'));
        await Promise.resolve();

        expect(BackendClient.sharePermissions).toHaveBeenCalledWith(2, {
            gamepad: false,
            keyboardMouse: true,
            hid: false,
        });
    });

    it('offers the HID box only with the gamepad, and sends it', async () => {
        await openBoard([slot(2, { state: 'shared' }), slot(3), slot(4)]);
        const gamepad = rowFor(2).querySelector('.share-perm-gamepad');
        const hid = rowFor(2).querySelector('.share-perm-hid');
        // A wheel sent as itself is a game controller: no gamepad, no box.
        expect(hid.disabled).toBe(true);
        expect(hid.checked).toBe(false);

        gamepad.checked = true;
        gamepad.dispatchEvent(new Event('change'));
        expect(hid.disabled).toBe(false);
        hid.checked = true;
        hid.dispatchEvent(new Event('change'));
        await vi.waitFor(() =>
            expect(BackendClient.sharePermissions).toHaveBeenLastCalledWith(2, {
                gamepad: true,
                keyboardMouse: false,
                hid: true,
            }),
        );

        // Taking the gamepad back takes the wheel with it.
        gamepad.checked = false;
        gamepad.dispatchEvent(new Event('change'));
        expect(hid.checked).toBe(false);
        expect(hid.disabled).toBe(true);
        await vi.waitFor(() =>
            expect(BackendClient.sharePermissions).toHaveBeenLastCalledWith(2, {
                gamepad: false,
                keyboardMouse: false,
                hid: false,
            }),
        );
    });

    it('paints a granted HID box from the server', async () => {
        await openBoard([
            slot(2, {
                state: 'shared',
                permissions: { gamepad: true, keyboardMouse: false, hid: true },
                access_level: 'gamer',
            }),
            slot(3),
            slot(4),
        ]);
        const hid = rowFor(2).querySelector('.share-perm-hid');
        expect(hid.checked).toBe(true);
        expect(hid.disabled).toBe(false);
    });

    it('records an idle row’s choice at once, so the poll cannot undo it', async () => {
        await openBoard([slot(2), slot(3), slot(4)]);
        const gamepad = rowFor(2).querySelector('.share-perm-gamepad');
        gamepad.checked = true;
        gamepad.dispatchEvent(new Event('change'));

        // The backend remembers it as what START will mint. Keeping it local
        // was the bug: five seconds later the poll repainted the row from a
        // server that had never been told, and the tick came back off.
        await vi.waitFor(() =>
            expect(BackendClient.sharePermissions).toHaveBeenCalledWith(2, {
                gamepad: true,
                keyboardMouse: false,
                hid: false,
            }),
        );
    });

    it('leaves the DOM alone when a poll brings back the same state', async () => {
        await openBoard([slot(2, { state: 'shared' }), slot(3), slot(4)]);
        const link = document.querySelector('.share-link-input');

        await board.refresh();
        // Same node, not an identical replacement: rebuilding it every few
        // seconds wiped the selection, so the link could not be swiped by hand.
        expect(document.querySelector('.share-link-input')).toBe(link);

        // A real change still redraws.
        BackendClient.getShareStatus.mockResolvedValue(
            status([slot(2, { state: 'binded' }), slot(3), slot(4)]),
        );
        await board.refresh();
        expect(document.querySelector('.share-link-input')).not.toBe(link);
    });

    it('ages the countdown without rebuilding the row', async () => {
        const now = Math.floor(Date.now() / 1000);
        await openBoard([
            slot(2, { state: 'shared', ttl_secs: 28800, expires_at: now + 3600 }),
            slot(3),
            slot(4),
        ]);
        const label = document.querySelector('.share-brow-detail[data-slot="2"] .share-expiry');
        const before = label.textContent;

        BackendClient.getShareStatus.mockResolvedValue(
            status([
                slot(2, { state: 'shared', ttl_secs: 28800, expires_at: now + 1800 }),
                slot(3),
                slot(4),
            ]),
        );
        await board.refresh();

        expect(document.querySelector('.share-brow-detail[data-slot="2"] .share-expiry')).toBe(
            label,
        );
        expect(label.textContent).not.toBe(before);
    });

    it('holds a dragged lifetime until START or close, and caps what it shows', async () => {
        const now = Math.floor(Date.now() / 1000);
        // 24 h chosen, 20 h 20 still to run.
        await openBoard([
            slot(2, { state: 'shared', ttl_secs: 86400, expires_at: now + 73200 }),
            slot(3),
            slot(4),
        ]);
        const detail = document.querySelector('.share-brow-detail[data-slot="2"]');
        const slider = detail.querySelector('.share-ttl-slider');
        const expiry = detail.querySelector('.share-expiry');

        slider.value = String(TTL.indexOf(8 * 3600));
        slider.dispatchEvent(new Event('input'));
        slider.dispatchEvent(new Event('change'));

        // Shown at once, sent to nobody: dragging through 1 h on the way to
        // 48 h must not cut the invitation short in passing.
        expect(expiry.textContent).toContain('{"h":8,"m":0}');
        expect(BackendClient.shareTtl).not.toHaveBeenCalled();

        // Closing is what commits it.
        board.close();
        await vi.waitFor(() => expect(BackendClient.shareTtl).toHaveBeenCalledWith(2, 8 * 3600));
        board = null;
    });

    it('never shows more time left than the invitation actually has', async () => {
        const now = Math.floor(Date.now() / 1000);
        // 24 h chosen, but only ~3 h left of it. The spare 90 s keeps the
        // assertion off a minute boundary the test's own runtime would cross.
        await openBoard([
            slot(2, { state: 'shared', ttl_secs: 86400, expires_at: now + 3 * 3600 + 90 }),
            slot(3),
            slot(4),
        ]);
        const detail = document.querySelector('.share-brow-detail[data-slot="2"]');
        const slider = detail.querySelector('.share-ttl-slider');

        slider.value = String(TTL.indexOf(8 * 3600));
        slider.dispatchEvent(new Event('input'));

        // 8 h is more than is left, so it stays at 3 h. A lifetime only ever
        // shortens; the backend applies the same cap on commit.
        expect(detail.querySelector('.share-expiry').textContent).toContain('"h":3');

        // And the bar is that much of the chosen span, not all of it: 8 h sits
        // at 40% of the track, ~3 h of it leaves the solid part near 15%.
        const live = parseFloat(
            detail.querySelector('.share-ttl-track').style.getPropertyValue('--live'),
        );
        expect(live).toBeGreaterThan(13);
        expect(live).toBeLessThan(17);
    });

    it('sends an idle row’s lifetime with START, not before it', async () => {
        await openBoard([slot(2), slot(3), slot(4)]);
        const slider = document.querySelector(
            '.share-brow-detail[data-slot="2"] .share-ttl-slider',
        );
        slider.value = String(TTL.indexOf(48 * 3600));
        slider.dispatchEvent(new Event('change'));
        expect(BackendClient.shareTtl).not.toHaveBeenCalled();

        document.querySelector('.share-brow[data-slot="2"] .share-toggle-btn').click();
        await vi.waitFor(() =>
            expect(BackendClient.shareActivate).toHaveBeenCalledWith(2, {
                ttl_secs: 48 * 3600,
                host_uuid: 'h',
            }),
        );
        // It rode along with the activation, so no second write says the same.
        expect(BackendClient.shareTtl).not.toHaveBeenCalled();
    });

    it('does not repaint from a poll while a write is in flight', async () => {
        await openBoard([slot(2), slot(3), slot(4)]);
        let release;
        BackendClient.sharePermissions.mockImplementationOnce(
            () => new Promise((r) => (release = r)),
        );
        const gamepad = rowFor(2).querySelector('.share-perm-gamepad');
        gamepad.checked = true;
        gamepad.dispatchEvent(new Event('change'));
        BackendClient.getShareStatus.mockClear();

        // The answer in flight is newer than anything this poll can return.
        await board.refresh();
        expect(BackendClient.getShareStatus).not.toHaveBeenCalled();

        release({});
        await vi.waitFor(() => expect(rowFor(2)).toBeTruthy());
    });

    it('binds a cold invitation to the picked host and app', async () => {
        await openBoard([slot(2), slot(3), slot(4)], {
            streaming: false,
            hostUuid: 'host-A',
            hostName: 'bench-mini',
        });
        rowFor(2).querySelector('.share-toggle-btn').click();
        await vi.waitFor(() => expect(BackendClient.shareActivate).toHaveBeenCalled());

        expect(BackendClient.shareActivate).toHaveBeenCalledWith(2, {
            ttl_secs: 28800,
            host_uuid: 'host-A',
            app_id: 7,
        });
    });

    it('lists the apps in the hosts page order, the host’s first one still picked', async () => {
        // The native host lists its primary display first; the hosts page
        // puts the virtual display first, then the rest alphabetically.
        BackendClient.getAppList.mockResolvedValueOnce({
            apps: [
                { id: 3, name: 'Main Screen' },
                { id: 9, name: 'beamer' },
                {
                    id: 5,
                    name: 'MoonlightWeb Virtual Display',
                    device: { key: 'moonlightweb-virtual-display' },
                },
                { id: 4, name: 'Desk 10' },
                { id: 6, name: 'Desk 2' },
            ],
        });
        await openBoard([slot(2), slot(3), slot(4)], {
            streaming: false,
            hostUuid: 'host-A',
            hostName: 'bench-mini',
        });

        const select = /** @type {HTMLSelectElement} */ (
            document.getElementById('share-app-select')
        );
        expect([...select.options].map((o) => o.value)).toEqual(['5', '9', '6', '4', '3']);
        expect(select.value).toBe('3');

        rowFor(2).querySelector('.share-toggle-btn').click();
        await vi.waitFor(() => expect(BackendClient.shareActivate).toHaveBeenCalled());
        expect(BackendClient.shareActivate).toHaveBeenCalledWith(2, {
            ttl_secs: 28800,
            host_uuid: 'host-A',
            app_id: 3,
        });
    });

    it('names its host but leaves the app to the stream that decides it', async () => {
        // The host tells the backend which of this device's streams the guest
        // joins, when the owner streams more than one machine.
        await openBoard([slot(2), slot(3), slot(4)]);
        rowFor(2).querySelector('.share-toggle-btn').click();
        await vi.waitFor(() => expect(BackendClient.shareActivate).toHaveBeenCalled());

        expect(BackendClient.shareActivate).toHaveBeenCalledWith(2, {
            ttl_secs: 28800,
            host_uuid: 'h',
        });
    });

    it('lets a link go once its row was opened again elsewhere', async () => {
        const live = (opening) => slot(2, { state: 'shared', opening, devices: [] });
        BackendClient.shareCredentials.mockResolvedValue({
            available: true,
            url: 'https://x/p/old',
            pin: '111111',
            opening: 'first',
        });
        await openBoard([live('first'), slot(3), slot(4)]);
        expect(BackendClient.shareCredentials).toHaveBeenCalledTimes(1);

        // The same opening on the next poll: nothing is asked again.
        await board.refresh();
        expect(BackendClient.shareCredentials).toHaveBeenCalledTimes(1);

        // Closed and opened again from another device between two polls: the
        // pair held is a dead one, and the new one is read.
        BackendClient.shareCredentials.mockResolvedValue({
            available: true,
            url: 'https://x/p/new',
            pin: '222222',
            opening: 'second',
        });
        BackendClient.getShareStatus.mockResolvedValue(status([live('second'), slot(3), slot(4)]));
        await board.refresh();
        expect(BackendClient.shareCredentials).toHaveBeenCalledTimes(2);
        const link = () =>
            document.querySelector('.share-brow-detail[data-slot="2"] .share-link-input').value;
        await vi.waitFor(() => expect(link()).toBe('https://x/p/new'));
    });

    it('shows a forwarded link on the row it was forwarded from', async () => {
        // Real User-Agent shapes: the browser is only ever named with its
        // version attached, which is what the parser keys on.
        const chrome =
            'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 ' +
            '(KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36';
        const firefox = 'Mozilla/5.0 (X11; Linux x86_64; rv:133.0) Gecko/20100101 Firefox/133.0';
        await openBoard([
            slot(2, {
                state: 'binded',
                devices: [{ bound_at: 1756290000, user_agent: chrome }],
                last_refused: { at: 1756300000, user_agent: firefox },
            }),
            slot(3),
            slot(4),
        ]);

        // No unfolding: the board never hides a row's own half.
        const text = document.querySelector('.share-refused').textContent;
        expect(text).toContain('sharing.refused');
        // Named well enough for the owner to tell it apart from the machine
        // they meant to invite — which is on the same row, reading "Chrome".
        expect(text).toContain('Firefox · Linux');
        expect(rowFor(2).querySelector('.share-brow-hint').textContent).toContain(
            'Chrome · Windows',
        );
    });

    it('shows the guests and nobody else', async () => {
        await openBoard([slot(2), slot(3), slot(4)]);
        // No owner line: it was a label with nothing to set, taking the top of
        // a board whose whole subject is the other three.
        const rows = document.querySelectorAll('.share-brow');
        expect(rows).toHaveLength(3);
        expect([...rows].every((r) => r.dataset.slot)).toBe(true);
    });

    it('stops a row through the backend, not just in the view', async () => {
        await openBoard([slot(2, { state: 'binded' }), slot(3), slot(4)]);
        rowFor(2).querySelector('.share-toggle-btn').click();
        await vi.waitFor(() => expect(BackendClient.shareDeactivate).toHaveBeenCalledWith(2));
    });

    it('shows every row’s half at all times, greyed until it is started', async () => {
        await openBoard([slot(2, { state: 'shared' }), slot(3), slot(4)]);

        // Three rows, three detail blocks: nothing folds.
        expect(document.querySelectorAll('.share-brow-detail')).toHaveLength(3);

        const started = document.querySelector('.share-brow-detail[data-slot="2"]');
        const idle = document.querySelector('.share-brow-detail[data-slot="3"]');
        expect(started.dataset.live).toBe('1');
        expect(idle.dataset.live).toBe('0');
        // An idle row's link and PIN do not exist yet, so they cannot be copied
        // or regenerated — but its lifetime is a choice made before START, and
        // stays live.
        expect(idle.querySelector('.share-copy-btn').disabled).toBe(true);
        expect(idle.querySelector('.share-regen-btn').disabled).toBe(true);
        expect(idle.querySelector('.share-ttl-slider').disabled).toBe(false);
        expect(started.querySelector('.share-copy-btn').disabled).toBe(false);
    });

    // How far a link reaches. Two causes, and the board must never mix them up:
    // one is a standing fact the owner has to act on, the other is the weather.
    describe('says how far the link actually reaches', () => {
        const detailFor = (n) => document.querySelector(`.share-brow-detail[data-slot="${n}"]`);
        const reachOf = (n) => detailFor(n).querySelector('.share-reach');
        const live = () => slot(2, { state: 'shared' });

        // clearAllMocks() clears the calls, not the implementation — a
        // mockResolvedValue set by one of these tests would otherwise decide
        // what the next one sees.
        beforeEach(() => {
            BackendClient.shareCredentials.mockResolvedValue({
                available: true,
                url: 'https://x/p/tok',
                pin: '482917',
                local_only: false,
            });
        });

        it('says nothing at all when the link reaches the internet', async () => {
            await openBoard([live(), slot(3), slot(4)], undefined, { remote_reachable: true });
            expect(reachOf(2)).toBeNull();
        });

        it('warns when the link cannot leave this network', async () => {
            BackendClient.shareCredentials.mockResolvedValue({
                available: true,
                url: 'https://192.168.1.24:18443/p/tok',
                pin: '482917',
                local_only: true,
            });
            await openBoard([live(), slot(3), slot(4)], undefined, { remote_reachable: true });
            expect(reachOf(2).dataset.reach).toBe('lanOnly');
            expect(reachOf(2).textContent).toContain('sharing.lanOnly');
        });

        it('says the machine is not answering, without touching the link', async () => {
            await openBoard([live(), slot(3), slot(4)], undefined, { remote_reachable: false });
            expect(reachOf(2).dataset.reach).toBe('offline');
            // The link is good and stays good: an owner who reads this must not
            // be nudged into regenerating a perfectly valid invitation.
            expect(detailFor(2).querySelector('.share-link-input').value).toBe('https://x/p/tok');
        });

        // The confusion this whole block exists to forbid. A LAN-only link is
        // ALSO unreachable from outside, so both conditions are true at once —
        // and only the one the owner can act on may be shown.
        it('never says both at once', async () => {
            BackendClient.shareCredentials.mockResolvedValue({
                available: true,
                url: 'https://192.168.1.24:18443/p/tok',
                pin: '482917',
                local_only: true,
            });
            await openBoard([live(), slot(3), slot(4)], undefined, { remote_reachable: false });
            expect(detailFor(2).querySelectorAll('.share-reach')).toHaveLength(1);
            expect(reachOf(2).dataset.reach).toBe('lanOnly');
        });

        // Issue #41: the owner's own domain, or the name they opened the board
        // on, reaches this PC without the rendezvous line. Whether it answers
        // is not something the board can see, so it claims nothing.
        it.each(['domain', 'direct'])(
            'says nothing about a %s link, even with no line out',
            async (where) => {
                BackendClient.shareCredentials.mockResolvedValue({
                    available: true,
                    url: 'https://moonlight.example.org/p/tok',
                    pin: '482917',
                    reach: where,
                    local_only: false,
                });
                await openBoard([live(), slot(3), slot(4)], undefined, { remote_reachable: false });
                expect(reachOf(2)).toBeNull();
            },
        );

        it('says nothing on a row that was never opened', async () => {
            await openBoard([slot(2), slot(3), slot(4)], undefined, { remote_reachable: false });
            expect(reachOf(2)).toBeNull();
        });
    });

    describe('lets the owner pick the address a link carries', () => {
        const menu = () => document.querySelector('.share-address-select');
        const detailFor = (n) => document.querySelector(`.share-brow-detail[data-slot="${n}"]`);
        const networks = [
            { id: 'auto' },
            { id: 'lan', kind: 'lan', address: '192.168.1.66', adapter: 'vEthernet (LAN)' },
            { id: 'tailscale', kind: 'tailscale', address: '100.116.41.43', adapter: 'Tailscale' },
        ];
        const tailscaleCreds = {
            available: true,
            url: 'https://100.116.41.43:8443/p/tok',
            pin: '482917',
            reach: 'tailscale',
            local_only: true,
        };

        beforeEach(() => {
            BackendClient.shareCredentials.mockResolvedValue({
                available: true,
                url: 'https://x/p/tok',
                pin: '482917',
                reach: 'public',
                local_only: false,
            });
        });

        it('offers no menu when Automatic is the only choice', async () => {
            await openBoard([slot(2), slot(3), slot(4)], undefined, {
                link_addresses: [{ id: 'auto' }],
                link_address: 'auto',
            });
            expect(menu()).toBeNull();
        });

        it('lists the networks, with the pick of this host selected', async () => {
            await openBoard([slot(2), slot(3), slot(4)], undefined, {
                link_addresses: networks,
                link_address: 'tailscale',
            });
            expect([...menu().options].map((o) => o.value)).toEqual(['auto', 'lan', 'tailscale']);
            expect(menu().value).toBe('tailscale');
        });

        it("offers the owner's own domain first, under its name", async () => {
            await openBoard([slot(2), slot(3), slot(4)], undefined, {
                link_addresses: [
                    networks[0],
                    { id: 'domain', kind: 'domain', address: 'moonlight.example.org', adapter: '' },
                    ...networks.slice(1),
                ],
                link_address: 'domain',
            });
            expect([...menu().options].map((o) => o.value)).toEqual([
                'auto',
                'domain',
                'lan',
                'tailscale',
            ]);
            expect(menu().value).toBe('domain');
            expect(menu().options[1].textContent).toContain('sharing.linkAddressDomain');
        });

        it('stores the pick for this host and reads the live link again', async () => {
            await openBoard([slot(2, { state: 'shared' }), slot(3), slot(4)], undefined, {
                link_addresses: networks,
                link_address: 'auto',
            });
            BackendClient.shareCredentials.mockClear();
            BackendClient.shareCredentials.mockResolvedValue(tailscaleCreds);
            menu().value = 'tailscale';
            menu().dispatchEvent(new Event('change'));
            await vi.waitFor(() => expect(BackendClient.shareCredentials).toHaveBeenCalledWith(2));
            expect(BackendClient.shareLinkAddress).toHaveBeenCalledWith('h', 'tailscale');
            // The token and the PIN stay; only the address in front of them moved.
            await vi.waitFor(() =>
                expect(detailFor(2).querySelector('.share-link-input').value).toBe(
                    'https://100.116.41.43:8443/p/tok',
                ),
            );
        });

        it('says a Tailscale link is for the tailnet, with the direct-path hint', async () => {
            BackendClient.shareCredentials.mockResolvedValue(tailscaleCreds);
            await openBoard([slot(2, { state: 'shared' }), slot(3), slot(4)], undefined, {
                link_addresses: networks,
                link_address: 'tailscale',
            });
            const reach = detailFor(2).querySelector('.share-reach[data-reach]');
            expect(reach.dataset.reach).toBe('reachTailscale');
            expect(detailFor(2).querySelector('.share-tailscale-hint').textContent).toContain(
                'sharing.tailscaleHint',
            );
        });

        it('shows the hint on no other link', async () => {
            BackendClient.shareCredentials.mockResolvedValue({
                available: true,
                url: 'https://192.168.1.66:8443/p/tok',
                pin: '482917',
                reach: 'lan',
                local_only: true,
            });
            await openBoard([slot(2, { state: 'shared' }), slot(3), slot(4)], undefined, {
                link_addresses: networks,
                link_address: 'lan',
            });
            // Pinned to the LAN on purpose: no nudge towards internet access.
            expect(detailFor(2).querySelector('.share-reach').dataset.reach).toBe('reachLan');
            expect(detailFor(2).querySelector('.share-tailscale-hint')).toBeNull();
        });

        it('keeps a pick whose network is gone, and says so', async () => {
            await openBoard([slot(2), slot(3), slot(4)], undefined, {
                link_addresses: networks.slice(0, 2),
                link_address: 'tailscale',
                link_address_missing: true,
            });
            const picked = [...menu().options].find((o) => o.selected);
            expect(picked.value).toBe('tailscale');
            expect(picked.disabled).toBe(true);
            expect(
                document.querySelector('.share-board-address .is-warning').textContent,
            ).toContain('sharing.linkAddressMissing');
        });
    });

    it('keeps the board up when the backdrop is clicked', async () => {
        await openBoard([slot(2), slot(3), slot(4)]);
        const overlay = document.querySelector('.share-board-overlay');
        overlay.dispatchEvent(new MouseEvent('click', { bubbles: true }));
        // Only the ✕ (and Escape) close it: a stray click next to a PIN being
        // read should not take the whole board away.
        expect(document.querySelector('.share-board-overlay')).toBeTruthy();

        document.querySelector('.share-board-close').click();
        expect(document.querySelector('.share-board-overlay')).toBeNull();
    });

    it('does not repaint under a name the owner is typing', async () => {
        vi.useFakeTimers();
        try {
            await openBoard([slot(2), slot(3), slot(4)]);
            const input = rowFor(2).querySelector('.share-name-input');
            input.focus();
            input.value = 'Léo';
            BackendClient.getShareStatus.mockClear();

            vi.advanceTimersByTime(6000);
            // A repaint here would take the caret with it, mid-word.
            expect(BackendClient.getShareStatus).not.toHaveBeenCalled();
        } finally {
            vi.useRealTimers();
        }
    });
    // A native host's guests watch one picture: its height is one choice for
    // the whole board, never a row's (two rows, two heights, one stream).
    it('offers one picture height for every guest on a native host', async () => {
        await openBoard([slot(2), slot(3), slot(4)], undefined, {
            feed_height: 1080,
            owner_height: true,
            shared_feed: true,
        });
        expect(BackendClient.getShareStatus).toHaveBeenCalledWith('h');
        const buttons = document.querySelectorAll('.share-board-feed .share-feed-btn');
        expect([...buttons].map((b) => b.textContent)).toEqual(['720p', '1080p', '1440p']);
        expect(document.querySelector('.share-feed-btn.is-selected').textContent).toBe('1080p');
        // Only one, above the rows: no row carries a height of its own.
        expect(document.querySelectorAll('.share-board-feed').length).toBe(1);
        expect(document.querySelector('.share-player .share-feed-btn')).toBeNull();
        expect(document.querySelector('.share-board-feed').textContent).toContain(
            'sharing.feedHintShared',
        );

        buttons[0].dispatchEvent(new Event('click'));
        await vi.waitFor(() => expect(BackendClient.shareFeedHeight).toHaveBeenCalledWith(720));
        await vi.waitFor(() =>
            expect(document.querySelector('.share-feed-btn.is-selected').textContent).toBe('720p'),
        );
    });

    it('puts the height back when the backend refuses it', async () => {
        await openBoard([slot(2), slot(3), slot(4)], undefined, {
            feed_height: 1080,
            owner_height: true,
        });
        BackendClient.shareFeedHeight.mockRejectedValueOnce(new Error('400'));
        document.querySelectorAll('.share-feed-btn')[2].dispatchEvent(new Event('click'));
        await vi.waitFor(() => expect(BackendClient.shareFeedHeight).toHaveBeenCalledWith(1440));
        await vi.waitFor(() =>
            expect(document.querySelector('.share-feed-btn.is-selected').textContent).toBe('1080p'),
        );
    });

    it('leaves the height to each guest elsewhere', async () => {
        await openBoard([slot(2), slot(3), slot(4)], undefined, {
            feed_height: 1080,
            owner_height: false,
        });
        expect(document.querySelector('.share-board-feed')).toBeNull();
    });
});
