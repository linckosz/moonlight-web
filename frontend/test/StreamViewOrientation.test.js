/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */

/**
 * Mobile rotation → Fullscreen button visibility.
 *
 * Guards the one-rotation lag: the rotation signals (media query, legacy
 * orientationchange) fire while `screen.orientation.type` still reports the
 * orientation the device is leaving, so a synchronous read applied the previous
 * state — button hidden in landscape, shown in portrait.
 */
import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';

vi.mock('../js/util/BrowserDetect.js', () => ({
    IS_MOBILE_OR_TABLET: true,
    IS_IOS: false,
    IS_STANDALONE: false,
}));

import { StreamViewFullscreen } from '../js/ui/StreamViewFullscreen.js';

/** Build a StreamView-like object carrying the fullscreen subsystem's methods. */
function makeView() {
    const view = {};
    for (const name of Object.getOwnPropertyNames(StreamViewFullscreen.prototype)) {
        if (name === 'constructor') continue;
        Object.defineProperty(
            view,
            name,
            Object.getOwnPropertyDescriptor(StreamViewFullscreen.prototype, name),
        );
    }
    return view;
}

/**
 * Wire a mobile stream view whose device orientation is driven by hand, with
 * the rotation signals and the reported orientation deliberately decoupled.
 */
function setup(initial = 'portrait') {
    const mqlListeners = [];
    const orientationListeners = [];

    const mql = {
        matches: initial === 'landscape',
        addEventListener: (_type, fn) => mqlListeners.push(fn),
        removeEventListener: (_type, fn) => {
            const i = mqlListeners.indexOf(fn);
            if (i >= 0) mqlListeners.splice(i, 1);
        },
    };
    const orientation = {
        type: initial + '-primary',
        addEventListener: (_type, fn) => orientationListeners.push(fn),
        removeEventListener: (_type, fn) => {
            const i = orientationListeners.indexOf(fn);
            if (i >= 0) orientationListeners.splice(i, 1);
        },
    };

    vi.stubGlobal('matchMedia', () => mql);
    vi.stubGlobal('screen', { orientation });
    const addSpy = vi.spyOn(globalThis, 'addEventListener');
    const removeSpy = vi.spyOn(globalThis, 'removeEventListener');

    const view = makeView();
    view.videoEl = null;
    // Record what the subsystem decides instead of touching the real DOM.
    const applied = [];
    view._updateMobileFsButtonVisibility = () => applied.push(view._lastLandscape);
    view._exitMobileFullscreen = vi.fn();

    view._initMobileFullscreen();
    // The init pass sets the initial button state; only rotations matter here.
    applied.length = 0;

    return {
        view,
        applied,
        /** Fire the rotation signals — WITHOUT settling screen.orientation. */
        rotateSignal() {
            for (const fn of [...mqlListeners, ...orientationListeners]) fn();
        },
        /** The device finishes rotating: screen.orientation catches up. */
        settle(to) {
            orientation.type = to + '-primary';
            mql.matches = to === 'landscape';
        },
        counts: () => ({ mql: mqlListeners.length, orientation: orientationListeners.length }),
        addSpy,
        removeSpy,
    };
}

describe('StreamView mobile orientation sync', () => {
    beforeEach(() => vi.useFakeTimers());
    afterEach(() => {
        vi.useRealTimers();
        vi.unstubAllGlobals();
        vi.restoreAllMocks();
    });

    it('shows the button for the orientation the device settles on, not the one it left', () => {
        const { view, applied, rotateSignal, settle } = setup('portrait');

        // Signal arrives while screen.orientation still says portrait.
        rotateSignal();
        vi.advanceTimersByTime(50);
        expect(applied).toEqual([]); // stale read must not be applied

        settle('landscape');
        vi.advanceTimersByTime(600);
        expect(applied).toEqual([true]);
        expect(view._exitMobileFullscreen).not.toHaveBeenCalled();
    });

    it('leaves fullscreen once the device has settled back to portrait', () => {
        const { view, applied, rotateSignal, settle } = setup('landscape');

        rotateSignal();
        vi.advanceTimersByTime(50);
        expect(view._exitMobileFullscreen).not.toHaveBeenCalled();

        settle('portrait');
        vi.advanceTimersByTime(600);
        expect(applied).toEqual([false]);
        expect(view._exitMobileFullscreen).toHaveBeenCalledTimes(1);
    });

    it('applies a full rotation cycle in the right order', () => {
        const { applied, rotateSignal, settle } = setup('portrait');

        rotateSignal();
        settle('landscape');
        vi.advanceTimersByTime(600);

        rotateSignal();
        settle('portrait');
        vi.advanceTimersByTime(600);

        expect(applied).toEqual([true, false]);
    });

    it('ignores a viewport reshape that is not a rotation (soft keyboard)', () => {
        const { view, applied, rotateSignal } = setup('portrait');

        rotateSignal();
        vi.advanceTimersByTime(2000);

        expect(applied).toEqual([]);
        expect(view._exitMobileFullscreen).not.toHaveBeenCalled();
    });

    it('applies the state only once per rotation', () => {
        const { applied, rotateSignal, settle } = setup('portrait');

        settle('landscape');
        rotateSignal();
        vi.advanceTimersByTime(2000);

        expect(applied).toEqual([true]);
    });

    it('drops every listener and pending re-check on teardown', () => {
        const s = setup('portrait');
        expect(s.counts()).toEqual({ mql: 1, orientation: 1 });

        s.rotateSignal();
        s.view._teardownOrientation();
        s.settle('landscape');
        vi.advanceTimersByTime(2000);

        expect(s.counts()).toEqual({ mql: 0, orientation: 0 });
        expect(s.applied).toEqual([]);
        expect(s.removeSpy).toHaveBeenCalledWith('orientationchange', expect.any(Function));
    });
});
