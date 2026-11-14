/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 *
 * A stream the host's tray disconnects (issue #42, the Mi TV bench of
 * 14/11/2026): the page was told nothing and showed "DataChannel input closed
 * unexpectedly". The host now sends "session-ended" first, and the page says
 * calmly that the stream was stopped on the host. A guest keeps its own words.
 */
import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';

vi.mock('../js/i18n/i18n.js', () => ({
    t: (key, vars) => (vars ? `${key}:${JSON.stringify(vars)}` : key),
}));

import { StreamView } from '../js/ui/StreamView.js';

const P = StreamView.prototype;

function view(overrides = {}) {
    const root = document.createElement('div');
    root.className = 'stream-overlay';
    document.body.appendChild(root);
    return {
        _rootEl: root,
        _quitting: false,
        _takenOver: false,
        _manualQuitting: false,
        _standby: false,
        _playerMode: false,
        connected: true,
        webrtc: { markStopping: vi.fn() },
        onQuitStart: vi.fn(),
        quit: vi.fn(),
        _playPowerOff: (done) => done(),
        _handleSessionEnded: P._handleSessionEnded,
        _handleHostEnded: P._handleHostEnded,
        _handleCalmExit: P._handleCalmExit,
        _handleForcedExit: vi.fn(),
        ...overrides,
    };
}

beforeEach(() => {
    document.body.innerHTML = '';
});

afterEach(() => {
    vi.useRealTimers();
});

describe('a viewer the host stopped', () => {
    it('says the stream was stopped on the host, without an alarm, then leaves', async () => {
        vi.useFakeTimers();
        const v = view();
        v._handleSessionEnded();
        const overlay = document.querySelector('.stream-takeover-overlay.is-quit');
        expect(overlay).not.toBeNull();
        expect(overlay?.textContent).toContain('stream.stoppedOnHostTitle');
        expect(overlay?.textContent).toContain('stream.stoppedOnHostBody');
        // Its channels close next: not an unexpected disconnect.
        expect(v.webrtc.markStopping).toHaveBeenCalled();
        expect(v._manualQuitting).toBe(true);
        expect(v.connected).toBe(false);
        expect(v._handleForcedExit).not.toHaveBeenCalled();
        await vi.advanceTimersByTimeAsync(1200);
        expect(v.quit).toHaveBeenCalledWith({ takenOver: true });
    });

    it('a guest keeps its own words, and its join page', () => {
        const v = view({ _playerMode: true });
        v._handleSessionEnded();
        expect(v._sessionEndedByOwner).toBe(true);
        expect(v._handleForcedExit).toHaveBeenCalledWith('player.endedTitle', 'player.endedBody');
        expect(document.querySelector('.stream-takeover-overlay')).toBeNull();
    });

    it('says nothing while the page is leaving already, or is a hidden standby', () => {
        view({ _manualQuitting: true })._handleSessionEnded();
        view({ _quitting: true })._handleSessionEnded();
        view({ _standby: true })._handleSessionEnded();
        expect(document.querySelector('.stream-takeover-overlay')).toBeNull();
    });

    it('an app closed on the host still says that', () => {
        vi.useFakeTimers();
        const v = view();
        v._handleHostEnded();
        const overlay = document.querySelector('.stream-takeover-overlay.is-quit');
        expect(overlay?.textContent).toContain('stream.hostEndedTitle');
        expect(overlay?.textContent).toContain('stream.hostEndedBody');
    });
});
