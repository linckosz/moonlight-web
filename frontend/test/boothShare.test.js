/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect, vi, beforeEach } from 'vitest';

vi.mock('../js/ui/ShareMenu.js', () => ({
    ShareMenu: class {
        mount() {
            return Promise.resolve(true);
        }
    },
}));
vi.mock('../js/ui/BoothPinMenu.js', () => ({
    BoothPinMenu: class {
        constructor() {
            this.isBoothPin = true;
        }
        mount() {
            return Promise.resolve(true);
        }
    },
}));

import { BackendClient } from '../js/api/BackendClient.js';
import { StreamView } from '../js/ui/StreamView.js';

/**
 * Booth mode (--noadmin) and Share (issue #40).
 *
 * A booth lets no guest in, so the stream offers no Share — unless the owner
 * set share_in_booth_mode, which the server reports as booth_share. Outside
 * booth mode the field means nothing and Share is there as before.
 */

function authStatus(body) {
    vi.stubGlobal(
        'fetch',
        vi.fn().mockResolvedValue({
            ok: true,
            status: 200,
            json: async () => body,
            text: async () => JSON.stringify(body),
        }),
    );
    return BackendClient.getAuthStatus();
}

/** Just what _mountShareMenu reads: a header and the Quit button beside it. */
function view() {
    const root = document.createElement('div');
    root.innerHTML = '<div class="stream-header"><button id="btn-stream-quit"></button></div>';
    return {
        _playerMode: false,
        _shareMenu: null,
        _rootEl: root,
        host: { supportsLobbies: false },
    };
}

const mount = (v) => StreamView.prototype._mountShareMenu.call(v);

describe('booth mode and Share', () => {
    beforeEach(() => {
        BackendClient._hadSession = false;
    });

    it('reads booth_share only under admin_locked', async () => {
        await authStatus({ authenticated: true, admin_locked: true });
        expect(BackendClient.boothShare).toBe(false);
        await authStatus({ authenticated: true, admin_locked: true, booth_share: false });
        expect(BackendClient.boothShare).toBe(false);
        await authStatus({ authenticated: true, admin_locked: true, booth_share: true });
        expect(BackendClient.boothShare).toBe(true);
        await authStatus({ authenticated: true, admin_locked: false, booth_share: true });
        expect(BackendClient.boothShare).toBe(false);
    });

    it('a booth offers no Share by default', async () => {
        await authStatus({ authenticated: true, admin_locked: true });
        const v = view();
        mount(v);
        expect(v._shareMenu).toBeNull();
    });

    it('a booth with share_in_booth_mode offers Share', async () => {
        await authStatus({ authenticated: true, admin_locked: true, booth_share: true });
        const v = view();
        mount(v);
        expect(v._shareMenu).not.toBeNull();
    });

    it('outside booth mode, Share is there as before', async () => {
        await authStatus({ authenticated: true, admin_locked: false });
        const v = view();
        mount(v);
        expect(v._shareMenu).not.toBeNull();
        expect(v._shareMenu.isBoothPin).toBeUndefined();
    });

    // booth_stream_pin (issue #68): the stream's PIN in place of Share.
    it('reads booth_pin only under admin_locked, and keeps boothPin() a call', async () => {
        await authStatus({ authenticated: true, admin_locked: true, booth_pin: true });
        expect(BackendClient.boothPinMode).toBe(true);
        expect(typeof BackendClient.boothPin).toBe('function');
        await authStatus({ authenticated: true, admin_locked: false, booth_pin: true });
        expect(BackendClient.boothPinMode).toBe(false);
        await authStatus({ authenticated: true, admin_locked: true });
        expect(BackendClient.boothPinMode).toBe(false);
        expect(typeof BackendClient.boothPin).toBe('function');
    });

    it('a booth with stream PINs shows the PIN in place of Share, share_in_booth_mode or not', async () => {
        for (const boothShare of [false, true]) {
            await authStatus({
                authenticated: true,
                admin_locked: true,
                booth_pin: true,
                booth_share: boothShare,
            });
            const v = view();
            mount(v);
            expect(v._shareMenu?.isBoothPin).toBe(true);
        }
    });

    // A take-over notice names the machine that took the stream (issue #68):
    // whatever it says is drawn as text.
    it("draws a forced exit's words as text, never as markup", () => {
        vi.useFakeTimers();
        const root = document.createElement('div');
        const v = {
            _rootEl: root,
            _quitting: false,
            _takenOver: false,
            connected: true,
            _playPowerOff() {},
            quit() {},
        };
        StreamView.prototype._handleForcedExit.call(
            v,
            'T<i>x</i>',
            '<img src=x onerror="alert(1)">',
        );
        const el = root.querySelector('.stream-takeover-overlay');
        expect(el.querySelector('img')).toBe(null);
        expect(el.querySelector('i')).toBe(null);
        expect(el.querySelector('.takeover-sub').textContent).toBe(
            '<img src=x onerror="alert(1)">',
        );
        expect(el.querySelector('.takeover-title').textContent).toBe('T<i>x</i>');
        expect(el.querySelector('.takeover-title').getAttribute('data-text')).toBe('T<i>x</i>');
        vi.useRealTimers();
    });
});
