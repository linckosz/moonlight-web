/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect, afterEach, vi } from 'vitest';

import { StreamView } from '../js/ui/StreamView.js';
import { Toast } from '../js/ui/Toast.js';
import { t } from '../js/i18n/i18n.js';

/**
 * The viewer's microphone button, in the stream header: right of the speaker,
 * between it and the Fullscreen button, as one sound group — and before Stop
 * where no Fullscreen button is laid out, so a viewer can always mute.
 */

/** A rect of `width` at `left`, laid out (or collapsed, width 0). */
function rect(left, width, height = 30) {
    return () => ({
        left,
        right: left + width,
        top: 10,
        bottom: 10 + height,
        width,
        height,
        x: left,
        y: 10,
    });
}

/**
 * Just what _mountMicButton and _positionGamingOverlay read: a header with
 * the controls this device has, and a microphone the host took.
 * @param {{volume?: boolean, fullscreen?: boolean}} [has]
 */
function view(has = {}) {
    const root = document.createElement('div');
    root.innerHTML = '<div class="stream-header"><button id="btn-stream-quit"></button></div>';
    // In the page, as the view's root is: a detached Fullscreen button is
    // never taken as laid out.
    document.body.appendChild(root);
    const header = /** @type {HTMLElement} */ (root.querySelector('.stream-header'));
    const quit = root.querySelector('#btn-stream-quit');
    header.getBoundingClientRect = rect(0, 1200, 54);
    let fs = null;
    if (has.fullscreen !== false) {
        fs = document.createElement('button');
        fs.id = 'btn-stream-fs';
        fs.getBoundingClientRect = rect(500, 100);
        header.insertBefore(fs, quit);
    }
    let vol = null;
    if (has.volume !== false) {
        vol = document.createElement('div');
        vol.className = 'stream-volume';
        vol.innerHTML = '<button type="button" class="stream-volume-btn"></button>';
        /** @type {HTMLElement} */ (vol.querySelector('.stream-volume-btn')).getBoundingClientRect =
            rect(0, 34);
        header.insertBefore(vol, fs || quit);
    }
    return {
        _rootEl: root,
        _volumeCtl: vol,
        _mobileFsBtn: fs,
        _micBtn: null,
        _gamingOverlay: null,
        _mic: {
            state: 'live',
            muted: false,
            toggleMuted() {
                this.muted = !this.muted;
                this.state = this.muted ? 'muted' : 'live';
                return Promise.resolve(!this.muted);
            },
        },
        _syncMicButton: StreamView.prototype._syncMicButton,
        _toggleMicSaid: StreamView.prototype._toggleMicSaid,
        _showMicHintOnce() {},
        _positionGamingOverlay: StreamView.prototype._positionGamingOverlay,
    };
}

/** Mounts the button, with its own laid-out width once it exists. */
function mount(v) {
    const header = v._rootEl.querySelector('.stream-header');
    const insertBefore = header.insertBefore.bind(header);
    // The button is created inside _mountMicButton: give it a width as soon
    // as it lands in the header, by whichever road.
    const sized = (node) => {
        if (node && node.id === 'btn-stream-mic') node.getBoundingClientRect = rect(0, 36);
    };
    header.insertBefore = (node, ref) => {
        sized(node);
        return insertBefore(node, ref);
    };
    const anchor = v._volumeCtl || v._mobileFsBtn;
    if (anchor) {
        const after = anchor.after.bind(anchor);
        anchor.after = (node) => {
            sized(node);
            after(node);
        };
    }
    StreamView.prototype._mountMicButton.call(v);
    return /** @type {HTMLButtonElement} */ (v._micBtn);
}

const ids = (v) =>
    [...v._rootEl.querySelector('.stream-header').children].map((el) => el.id || el.className);

describe('the microphone button beside the speaker', () => {
    afterEach(() => {
        document.body.innerHTML = '';
        vi.restoreAllMocks();
    });

    it('says on screen that a click cut it, then opened it', async () => {
        const info = vi.spyOn(Toast, 'info').mockImplementation(() => {});
        const v = view();
        const btn = mount(v);
        btn.click();
        await Promise.resolve();
        expect(info).toHaveBeenLastCalledWith(t('stream.micOff'), { durationMs: 1500 });
        btn.click();
        await Promise.resolve();
        expect(info).toHaveBeenLastCalledWith(t('stream.micOn'), { durationMs: 1500 });
        expect(info).toHaveBeenCalledTimes(2);
    });

    it('says nothing when a click did not open it: a refusal has its own message', async () => {
        const info = vi.spyOn(Toast, 'info').mockImplementation(() => {});
        const v = view();
        v._mic.state = 'denied';
        v._mic.muted = true;
        v._mic.toggleMuted = () => Promise.resolve(false);
        const btn = mount(v);
        btn.click();
        await Promise.resolve();
        expect(info).not.toHaveBeenCalled();
    });

    it('comes right after the volume, before Fullscreen', () => {
        const v = view();
        mount(v);
        expect(ids(v)).toEqual([
            'stream-volume',
            'btn-stream-mic',
            'btn-stream-fs',
            'btn-stream-quit',
        ]);
    });

    it('is docked right of the speaker, the volume moved left to make room', () => {
        const v = view();
        const btn = mount(v);
        expect(btn.classList.contains('is-docked')).toBe(true);
        // Fullscreen at 500 (its transform aside), a 12 px gap, anchored by
        // the right edge: the microphone ends at 488, the volume 6 px before
        // the microphone's 36.
        expect(btn.style.left).toBe('488px');
        expect(v._volumeCtl.style.left).toBe('446px');
        expect(v._volumeCtl.hidden).toBe(false);
    });

    it('goes back before Stop when Fullscreen is not laid out', () => {
        const v = view();
        const btn = mount(v);
        v._mobileFsBtn.getBoundingClientRect = rect(500, 0, 0);
        v._positionGamingOverlay();
        expect(btn.classList.contains('is-docked')).toBe(false);
        expect(btn.style.left).toBe('');
        expect(v._volumeCtl.hidden).toBe(true);
    });

    it('follows Fullscreen on a device without the volume (a phone)', () => {
        const v = view({ volume: false });
        const btn = mount(v);
        expect(ids(v)).toEqual(['btn-stream-fs', 'btn-stream-mic', 'btn-stream-quit']);
        expect(btn.style.left).toBe('488px');
    });

    it('waits before Stop where the header has neither', () => {
        const v = view({ volume: false, fullscreen: false });
        const btn = mount(v);
        expect(ids(v)).toEqual(['btn-stream-mic', 'btn-stream-quit']);
        expect(btn.classList.contains('is-docked')).toBe(false);
    });

    it('mutes and says so', () => {
        const v = view();
        const btn = mount(v);
        expect(btn.getAttribute('aria-pressed')).toBe('false');
        btn.click();
        expect(v._mic.muted).toBe(true);
        expect(btn.classList.contains('is-off')).toBe(true);
        expect(btn.getAttribute('aria-pressed')).toBe('true');
    });

    it('is lit only once the microphone really sends', () => {
        const v = view();
        // Open asked for, the permission prompt still up.
        v._mic.state = 'muted';
        v._mic.muted = false;
        const btn = mount(v);
        expect(btn.classList.contains('is-off')).toBe(true);
        expect(btn.getAttribute('aria-pressed')).toBe('true');
        v._mic.state = 'live';
        v._syncMicButton();
        expect(btn.classList.contains('is-off')).toBe(false);
    });

    it('stays clickable after a refusal, to try again', () => {
        const v = view();
        v._mic.state = 'denied';
        v._mic.muted = true;
        const btn = mount(v);
        expect(btn.disabled).toBe(false);
        expect(btn.classList.contains('is-off')).toBe(true);
        btn.click();
        expect(v._mic.muted).toBe(false);
    });
});
