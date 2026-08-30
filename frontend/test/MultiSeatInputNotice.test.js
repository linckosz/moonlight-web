/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */

/**
 * The MultiSeat "your input may be refused" notice.
 *
 * A seat hands input rights to the first device that pairs and says nothing to
 * the ones after, so the warning is given up front rather than inferred from
 * the stream. Three things have to hold: it is scoped to MultiSeat hosts; it is
 * shown once — a quality change builds a whole new StreamView, and a notice that
 * re-pops on every degradation is worse than no notice at all; and both "once"
 * and "never again" are per host, since each MultiSeat machine's seats hand out
 * their own permissions.
 *
 * The address it prints is the other half: a seat's Apollo interface answers on
 * its stream port plus one, over https, and the whole point of the notice is
 * that the user can go there without being told to work the port out.
 *
 * The mute key is guarded too: the popin is shared with the AWDL stutter
 * notice, and a popin that wrote its sibling's key would silence the wrong one.
 */
import { describe, it, expect, vi, beforeEach } from 'vitest';

// Keys, plus their interpolation values so the printed address is assertable.
vi.mock('../js/i18n/i18n.js', () => ({
    t: (key, params) => (params ? `${key} ${JSON.stringify(params)}` : key),
    applyTranslations: () => {},
}));

const getHostSeats = vi.fn();
vi.mock('../js/api/BackendClient.js', () => ({
    BackendClient: { getHostSeats: (...args) => getHostSeats(...args) },
}));

/** Fresh module instance: the "shown once" latch lives at module scope. */
async function loadStreamView() {
    vi.resetModules();
    const mod = await import('../js/ui/StreamView.js');
    return mod.StreamView;
}

/** A StreamView-like object carrying just the notice methods. */
function makeView(StreamView, host) {
    const view = { host, _standby: false, _rootEl: null, _stallPopinOverlay: null };
    for (const name of ['_maybeShowMultiSeatInputNotice', '_showNoticePopin', '_seatWebUiUrls']) {
        view[name] = StreamView.prototype[name];
    }
    return view;
}

const popin = () => document.querySelector('.share-popin-overlay');
const text = () => popin().textContent;

const seatHost = (uuid) => ({ uuid, backendType: 'multiseat' });
const muteKey = (uuid) => `mw_multiseat_input_notice:${uuid}`;
const seat = (name, httpPort, busy = false) => ({
    id: name,
    name,
    address: '192.168.1.9',
    httpPort,
    busy,
});

describe('MultiSeat input notice', () => {
    beforeEach(() => {
        document.body.innerHTML = '';
        localStorage.clear();
        getHostSeats.mockReset();
        getHostSeats.mockResolvedValue({ seats: [seat('luka', 48100)] });
    });

    it('opens on a MultiSeat host', async () => {
        const StreamView = await loadStreamView();
        await makeView(StreamView, seatHost('um790'))._maybeShowMultiSeatInputNotice();

        expect(popin()).not.toBeNull();
        expect(text()).toContain('stream.multiSeatInputBody');
    });

    it('stays away on every other backend', async () => {
        const StreamView = await loadStreamView();
        const others = [
            { uuid: 'wolf2', backendType: 'wolf' },
            { uuid: 'dualrtx', backendType: '' },
            null,
        ];
        for (const host of others) {
            await makeView(StreamView, host)._maybeShowMultiSeatInputNotice();
            expect(popin()).toBeNull();
        }
        expect(getHostSeats).not.toHaveBeenCalled();
    });

    it('does not re-open on the new view a quality change builds', async () => {
        const StreamView = await loadStreamView();
        const host = seatHost('um790');
        await makeView(StreamView, host)._maybeShowMultiSeatInputNotice();
        popin().remove(); // the retiring view takes its DOM with it

        await makeView(StreamView, host)._maybeShowMultiSeatInputNotice();
        expect(popin()).toBeNull();
    });

    it('speaks again for a second MultiSeat host, whose seats are its own', async () => {
        const StreamView = await loadStreamView();
        await makeView(StreamView, seatHost('um790'))._maybeShowMultiSeatInputNotice();
        popin().remove();

        await makeView(StreamView, seatHost('minis2'))._maybeShowMultiSeatInputNotice();
        expect(popin()).not.toBeNull();
    });

    it('stays away on a standby view, which nobody is looking at', async () => {
        const StreamView = await loadStreamView();
        const view = makeView(StreamView, seatHost('um790'));
        view._standby = true;
        await view._maybeShowMultiSeatInputNotice();
        expect(popin()).toBeNull();
    });

    it('mutes one host without muting the next, nor its AWDL sibling', async () => {
        let StreamView = await loadStreamView();
        await makeView(StreamView, seatHost('um790'))._maybeShowMultiSeatInputNotice();
        popin().querySelector('.share-stall-mute').click();

        expect(localStorage.getItem(muteKey('um790'))).toBe('off');
        expect(localStorage.getItem(muteKey('minis2'))).toBeNull();
        expect(localStorage.getItem('mw_awdl_notice')).toBeNull();

        // A later session (fresh module, so the latch is clear): the muted host
        // stays quiet, the other one still gets its warning.
        StreamView = await loadStreamView();
        await makeView(StreamView, seatHost('um790'))._maybeShowMultiSeatInputNotice();
        expect(popin()).toBeNull();

        await makeView(StreamView, seatHost('minis2'))._maybeShowMultiSeatInputNotice();
        expect(popin()).not.toBeNull();
    });

    it('closes on "Got it" without muting anything', async () => {
        const StreamView = await loadStreamView();
        await makeView(StreamView, seatHost('um790'))._maybeShowMultiSeatInputNotice();
        popin().querySelector('.share-stall-ok').click();

        expect(popin()).toBeNull();
        expect(localStorage.getItem(muteKey('um790'))).toBeNull();
    });

    describe('the address it sends the user to', () => {
        it('is the streaming seat, on its stream port plus one, over https', async () => {
            getHostSeats.mockResolvedValue({
                seats: [seat('luka', 48100, true), seat('leo', 48130)],
            });
            const StreamView = await loadStreamView();
            await makeView(StreamView, seatHost('um790'))._maybeShowMultiSeatInputNotice();

            expect(text()).toContain('stream.multiSeatInputWhere');
            expect(text()).toContain('https://192.168.1.9:48101');
            expect(text()).not.toContain('48131');
        });

        it('lists every seat rather than guess when none stands out', async () => {
            getHostSeats.mockResolvedValue({ seats: [seat('luka', 48100), seat('leo', 48130)] });
            const StreamView = await loadStreamView();
            await makeView(StreamView, seatHost('um790'))._maybeShowMultiSeatInputNotice();

            expect(text()).toContain('https://192.168.1.9:48101');
            expect(text()).toContain('https://192.168.1.9:48131');
        });

        it('describes the address when the host will not say, and still warns', async () => {
            getHostSeats.mockRejectedValue(new Error('backend unreachable'));
            const StreamView = await loadStreamView();
            await makeView(StreamView, seatHost('um790'))._maybeShowMultiSeatInputNotice();

            expect(text()).toContain('stream.multiSeatInputBody');
            expect(text()).toContain('stream.multiSeatInputWhereUnknown');
        });

        it('skips a seat with no port yet rather than print :1', async () => {
            getHostSeats.mockResolvedValue({ seats: [seat('luka', 0), seat('leo', 48130)] });
            const StreamView = await loadStreamView();
            await makeView(StreamView, seatHost('um790'))._maybeShowMultiSeatInputNotice();

            expect(text()).toContain('https://192.168.1.9:48131');
            expect(text()).not.toContain(':1"');
        });
    });
});
