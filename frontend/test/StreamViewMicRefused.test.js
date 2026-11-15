/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect, vi, beforeEach } from 'vitest';

vi.mock('../js/ui/Toast.js', () => ({
    Toast: { success: vi.fn(), error: vi.fn(), warning: vi.fn(), info: vi.fn() },
}));

import { StreamView } from '../js/ui/StreamView.js';
import { Toast } from '../js/ui/Toast.js';

/**
 * The host refused the viewer's microphone under the running stream (issue
 * #69, the administrator unticked "Accept viewers' microphones"): the page
 * closes it, so the browser's indicator goes out, takes the button away, and
 * says why once.
 */
const P = StreamView.prototype;

function view(over = {}) {
    const mic = { close: vi.fn() };
    const btn = { remove: vi.fn() };
    return {
        _mic: mic,
        _micBtn: btn,
        _standby: false,
        _positionGamingOverlay: vi.fn(),
        _micRefusedByHost: P._micRefusedByHost,
        ...over,
    };
}

describe('StreamView — the host refused the microphone mid-stream', () => {
    beforeEach(() => vi.clearAllMocks());

    it('closes the microphone, removes its button and says why', () => {
        const v = view();
        const { _mic: mic, _micBtn: btn } = v;
        v._micRefusedByHost("this host does not accept viewers' microphones");
        expect(mic.close).toHaveBeenCalledTimes(1);
        expect(btn.remove).toHaveBeenCalledTimes(1);
        expect(v._mic).toBe(null);
        expect(v._micBtn).toBe(null);
        expect(Toast.warning).toHaveBeenCalledTimes(1);
    });

    it('says nothing a second time, and nothing without a microphone', () => {
        const v = view();
        v._micRefusedByHost('x');
        v._micRefusedByHost('x');
        expect(Toast.warning).toHaveBeenCalledTimes(1);
        const none = view({ _mic: null });
        none._micRefusedByHost('x');
        expect(Toast.warning).toHaveBeenCalledTimes(1);
    });

    it('closes a standby leg quietly: the live one says it', () => {
        const v = view({ _standby: true });
        const { _mic: mic } = v;
        v._micRefusedByHost('x');
        expect(mic.close).toHaveBeenCalledTimes(1);
        expect(Toast.warning).not.toHaveBeenCalled();
    });
});
